// Compile SC6's recorded pipelines with Metal (notes/native-metal.md, step 3).
//
// Usage: compile-pipelines <capture dir>
//
// The capture dir holds what the sc6vrr DLL recorded (bindings-*.tsv, layouts/,
// blend/, dxbc/) and the airconv output (convert-airconv/metallib/). For each
// unique recorded combination with a vertex and a pixel shader, this builds the
// Metal render pipeline that the game's state describes and asks Metal to
// compile it to GPU code:
//   - vertex and fragment functions from the converted libraries,
//   - a vertex descriptor from the input layout and the recorded strides,
//   - color and depth attachment formats from the bound views,
//   - blending from the blend state, and the primitive class.
// A combination with a geometry shader becomes a mesh pipeline, as in DXMT: the
// vertex shader converted for that geometry shader is the object function, the
// geometry shader is the mesh function, and the pixel shader (if any) is the
// fragment function. tests/gs_variants.py converts each variant, with its input
// layout and strip flag, into gs-variants/.
// Results go to <capture dir>/convert-airconv/pipelines.tsv.
import Foundation
import Metal

let args = CommandLine.arguments
guard args.count == 2 else { print("usage: compile-pipelines <capture dir>"); exit(2) }
let root = URL(fileURLWithPath: args[1])
let libDir = root.appendingPathComponent("convert-airconv/metallib")
guard let device = MTLCreateSystemDefaultDevice() else { print("no Metal device"); exit(1) }

func lines(_ url: URL) -> [Substring] {
  // The DLL writes Windows line endings; split on any newline character.
  ((try? String(contentsOf: url, encoding: .utf8)) ?? "").split(whereSeparator: \.isNewline)
}

// MARK: DXBC input signature: (semantic, index) -> input register

func inputRegisters(_ dxbc: Data) -> [String: Int] {
  let b = [UInt8](dxbc)
  func u32(_ o: Int) -> Int {
    guard o + 4 <= b.count else { return 0 }
    return Int(b[o]) | Int(b[o + 1]) << 8 | Int(b[o + 2]) << 16 | Int(b[o + 3]) << 24
  }
  guard b.count >= 32, b[0] == 0x44, b[1] == 0x58 else { return [:] }  // "DX"
  var result: [String: Int] = [:]
  for i in 0..<u32(28) {
    let off = u32(32 + 4 * i)
    guard off + 8 <= b.count else { continue }
    let tag = String(bytes: b[off..<off + 4], encoding: .ascii) ?? ""
    guard tag == "ISGN" || tag == "ISG1" else { continue }
    let data = off + 8
    let count = u32(data)
    // ISGN entries are 6 words: name, index, system value, component type, register, masks.
    // ISG1 entries are 8 words and start with a stream number.
    let size = tag == "ISGN" ? 24 : 32, first = tag == "ISGN" ? 0 : 4
    for e in 0..<count {
      let entry = data + 8 + e * size + first
      var name = "", p = data + u32(entry)
      while p < b.count, b[p] != 0 { name.append(Character(UnicodeScalar(b[p]))); p += 1 }
      result["\(name.uppercased())#\(u32(entry + 4))"] = u32(entry + 16)
    }
  }
  return result
}

// MARK: Format and enum tables (D3D11 numeric values -> Metal)

let vertexFormats: [Int: MTLVertexFormat] = [
  2: .float4, 3: .uint4, 6: .float3, 7: .uint3, 10: .half4, 11: .ushort4Normalized, 12: .ushort4,
  13: .short4Normalized, 14: .short4, 16: .float2, 17: .uint2, 24: .uint1010102Normalized,
  28: .uchar4Normalized, 30: .uchar4, 31: .char4Normalized, 32: .char4, 34: .half2, 35: .ushort2Normalized,
  36: .ushort2, 37: .short2Normalized, 38: .short2, 41: .float, 42: .uint, 43: .int, 49: .uchar2Normalized,
  87: .uchar4Normalized_bgra,
]
let pixelFormats: [Int: MTLPixelFormat] = [
  2: .rgba32Float, 10: .rgba16Float, 11: .rgba16Unorm, 16: .rg32Float, 24: .rgb10a2Unorm, 26: .rg11b10Float,
  28: .rgba8Unorm, 29: .rgba8Unorm_srgb, 34: .rg16Float, 35: .rg16Unorm, 41: .r32Float, 42: .r32Uint,
  49: .rg8Unorm, 54: .r16Float, 56: .r16Unorm, 57: .r16Uint, 61: .r8Unorm, 87: .bgra8Unorm,
  91: .bgra8Unorm_srgb,
]
let integerPixelFormats: Set<Int> = [42, 57]
// Apple GPUs have no 24-bit depth format; DXMT uses 32-bit float depth with 8-bit stencil.
let depthFormats: [Int: (MTLPixelFormat, Bool)] = [
  45: (.depth32Float_stencil8, true), 20: (.depth32Float_stencil8, true), 40: (.depth32Float, false),
  55: (.depth16Unorm, false),
]
let blendFactors: [Int: MTLBlendFactor] = [
  1: .zero, 2: .one, 3: .sourceColor, 4: .oneMinusSourceColor, 5: .sourceAlpha, 6: .oneMinusSourceAlpha,
  7: .destinationAlpha, 8: .oneMinusDestinationAlpha, 9: .destinationColor, 10: .oneMinusDestinationColor,
  11: .sourceAlphaSaturated, 14: .blendColor, 15: .oneMinusBlendColor, 16: .source1Color,
  17: .oneMinusSource1Color, 18: .source1Alpha, 19: .oneMinusSource1Alpha,
]
let blendOps: [Int: MTLBlendOperation] = [1: .add, 2: .subtract, 3: .reverseSubtract, 4: .min, 5: .max]

// MARK: Recorded state

struct Blend { var enable = false; var v = [2, 1, 1, 2, 1, 1, 15] }  // src dst op srcA dstA opA mask
func loadBlend(_ hash: String) -> [Blend] {
  var rts = Array(repeating: Blend(), count: 8)
  guard hash != "none" else { return rts }
  var independent = false
  for line in lines(root.appendingPathComponent("blend/\(hash).txt")) {
    let f = line.split(separator: "\t").map(String.init)
    if f.first == "independent_blend" { independent = f[1] == "1" }
    if let n = f.first, n.hasPrefix("rt"), let i = Int(n.dropFirst(2)), f.count == 9 {
      rts[i] = Blend(enable: f[1] == "1", v: f[2...].compactMap { Int($0) })
    }
  }
  // Without independent blending, D3D applies render target 0's settings to all targets.
  return independent ? rts : Array(repeating: rts[0], count: 8)
}

struct Element { var semantic: String; var index, format, slot, offset, perInstance, stepRate: Int }
func loadLayout(_ hash: String) -> [Element] {
  lines(root.appendingPathComponent("layouts/\(hash).txt")).compactMap { line in
    let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    guard f.count == 7, !f[0].hasPrefix("#"), f[0] != "semantic", let i = Int(f[1]), let fmt = Int(f[2]),
          let slot = Int(f[3]), let off = Int(f[4]), let cls = Int(f[5]), let step = Int(f[6]) else { return nil }
    return Element(semantic: f[0].uppercased(), index: i, format: fmt, slot: slot, offset: off, perInstance: cls,
                   stepRate: step)
  }
}

let columns = ["seconds", "thread", "topology", "input_layout", "vs", "gs", "ps", "blend", "rt_formats",
               "dsv_format", "multisample", "strides"]
var combos = Set<[String]>()
for file in try FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)
where file.lastPathComponent.hasPrefix("bindings-") {
  for line in lines(file) {
    let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    guard f.count == columns.count, f[0] != "seconds" else { continue }
    combos.insert(Array(f[2...]))
  }
}

// MARK: Compile

var libraries: [String: MTLFunction] = [:]
var vsInputs: [String: [String: Int]] = [:]
/// Loads the one function of libDir/<file>.metallib.
func function(_ file: String) -> MTLFunction? {
  if let f = libraries[file] { return f }
  guard let data = try? Data(contentsOf: libDir.appendingPathComponent("\(file).metallib")),
        let lib = try? data.withUnsafeBytes({ try device.makeLibrary(data: DispatchData(bytes: $0)) }),
        let name = lib.functionNames.first, let f = lib.makeFunction(name: name) else { return nil }
  libraries[file] = f
  return f
}

/// Sets attachment formats and blending; returns a reason when a format is unmapped.
/// A format of 0 is an empty render target slot.
func configureTargets(_ colors: MTLRenderPipelineColorAttachmentDescriptorArray, _ rtList: String,
                      _ blendHash: String) -> String? {
  let blends = loadBlend(blendHash)
  guard rtList != "none" else { return nil }
  for (i, item) in rtList.split(separator: ",").enumerated() {
    guard let code = Int(item), code != 0 else { continue }
    guard let format = pixelFormats[code] else { return "target format \(code)" }
    let a = colors[i]!
    a.pixelFormat = format
    let b = blends[i]
    guard b.v.count == 7 else { continue }
    let mask = b.v[6]
    a.writeMask = MTLColorWriteMask(rawValue: UInt((mask & 1) << 3 | (mask & 2) << 1 | (mask & 4) >> 1 | (mask & 8) >> 3))
    // D3D does not blend integer targets, and Metal rejects blending on them.
    if b.enable && !integerPixelFormats.contains(code) {
      a.isBlendingEnabled = true
      a.sourceRGBBlendFactor = blendFactors[b.v[0]] ?? .one
      a.destinationRGBBlendFactor = blendFactors[b.v[1]] ?? .zero
      a.rgbBlendOperation = blendOps[b.v[2]] ?? .add
      a.sourceAlphaBlendFactor = blendFactors[b.v[3]] ?? .one
      a.destinationAlphaBlendFactor = blendFactors[b.v[4]] ?? .zero
      a.alphaBlendOperation = blendOps[b.v[5]] ?? .add
    }
  }
  return nil
}

enum Outcome { case compiled, failed(String), skipped(String) }

func build(_ c: [String]) -> Outcome {
  let (topology, layoutHash, vs, gs, ps, blendHash) = (Int(c[0]) ?? 0, c[1], c[2], c[3], c[4], c[5])
  let (rtList, dsv, multisample, strideList) = (c[6], Int(c[7]) ?? 0, c[8], c[9])
  if multisample != "0x0" { return .skipped("multisampling") }
  if gs != "none" {
    // The variant name matches tests/gs_variants.py: D3D11 topologies 3 and 5 are strips.
    let key = "\(vs)_\(gs)_\(layoutHash.prefix(16))_\([3, 5].contains(topology) ? "strip" : "list")"
    return buildMesh(key: key, ps: ps, rtList: rtList, dsv: dsv, blendHash: blendHash)
  }
  guard let vfn = function(vs) else { return .skipped("vertex shader not converted") }
  guard let ffn = function(ps) else { return .skipped("pixel shader not converted") }
  let d = MTLRenderPipelineDescriptor()
  d.vertexFunction = vfn
  d.fragmentFunction = ffn
  d.inputPrimitiveTopology = [1: .point, 2: .line, 3: .line, 4: .triangle, 5: .triangle][topology] ?? .unspecified

  // Vertex descriptor. DXMT numbers each attribute by the shader's input register,
  // so each layout element goes to the register that its semantic names.
  if layoutHash != "none" {
    if vsInputs[vs] == nil {
      vsInputs[vs] = inputRegisters((try? Data(contentsOf: root.appendingPathComponent("dxbc/\(vs).dxbc"))) ?? Data())
    }
    var strides: [Int: Int] = [:]
    for pair in strideList.split(separator: ",") {
      let p = pair.split(separator: ":")
      if p.count == 2, let s = Int(p[0]), let v = Int(p[1]) { strides[s] = v }
    }
    let vd = MTLVertexDescriptor()
    for e in loadLayout(layoutHash) {
      guard let reg = vsInputs[vs]?["\(e.semantic)#\(e.index)"] else { continue }
      guard let format = vertexFormats[e.format] else { return .skipped("vertex format \(e.format)") }
      let buffer = 30 - e.slot  // high buffer indices, away from the shader's own buffers (assumed)
      vd.attributes[reg].format = format
      vd.attributes[reg].offset = e.offset
      vd.attributes[reg].bufferIndex = buffer
      let stride = strides[e.slot] ?? 0
      vd.layouts[buffer].stride = stride
      if stride == 0 {
        // D3D allows stride 0: every vertex reads the same element. Metal expresses
        // that as a constant step, but it still requires a nonzero stride value.
        vd.layouts[buffer].stepFunction = .constant
        vd.layouts[buffer].stepRate = 0
        vd.layouts[buffer].stride = max(vd.layouts[buffer].stride, (e.offset + 16 + 3) & ~3)
      } else if e.perInstance == 1 {
        vd.layouts[buffer].stepFunction = .perInstance
        vd.layouts[buffer].stepRate = max(e.stepRate, 1)
      }
    }
    d.vertexDescriptor = vd
  }

  if let reason = configureTargets(d.colorAttachments, rtList, blendHash) { return .skipped(reason) }
  if let (format, stencil) = depthFormats[dsv] {
    d.depthAttachmentPixelFormat = format
    if stencil { d.stencilAttachmentPixelFormat = format }
  } else if dsv != 0 {
    return .skipped("depth format \(dsv)")
  }
  do {
    _ = try device.makeRenderPipelineState(descriptor: d)
    return .compiled
  } catch {
    return .failed(error.localizedDescription.replacingOccurrences(of: "\n", with: " "))
  }
}

func buildMesh(key: String, ps: String, rtList: String, dsv: Int, blendHash: String) -> Outcome {
  guard let object = function("../gs-variants/\(key).vs") else { return .skipped("vertex+geometry variant not converted") }
  guard let mesh = function("../gs-variants/\(key).gs") else { return .skipped("vertex+geometry variant not converted") }
  let d = MTLMeshRenderPipelineDescriptor()
  d.objectFunction = object
  d.meshFunction = mesh
  if ps != "none" {
    guard let fragment = function(ps) else { return .skipped("pixel shader not converted") }
    d.fragmentFunction = fragment
  }
  d.payloadMemoryLength = 16256  // the value DXMT uses (d3d11_pipeline_gs.cpp)
  if let reason = configureTargets(d.colorAttachments, rtList, blendHash) { return .skipped(reason) }
  if let (format, stencil) = depthFormats[dsv] {
    d.depthAttachmentPixelFormat = format
    if stencil { d.stencilAttachmentPixelFormat = format }
  } else if dsv != 0 {
    return .skipped("depth format \(dsv)")
  }
  do {
    _ = try device.makeRenderPipelineState(descriptor: d, options: [])
    return .compiled
  } catch {
    return .failed(error.localizedDescription.replacingOccurrences(of: "\n", with: " "))
  }
}

// Shader loading stays serial (the caches are plain dictionaries); compiling is the slow part.
let ordered = combos.sorted { $0.joined() < $1.joined() }
var outcomes: [Outcome] = []
let started = Date()
for c in ordered { outcomes.append(build(c)) }
let seconds = Date().timeIntervalSince(started)

// MARK: Report

var report = "vs\tps\tresult\tdetail\n"
var compiled = 0, failedCount = 0
var skipped: [String: Int] = [:], errors: [String: Int] = [:]
var psSeen = Set<String>(), psCompiled = Set<String>()
var meshCompiled = 0, meshFailed = 0, meshDepthOnly = 0
for (c, o) in zip(ordered, outcomes) {
  let (vs, gs, ps) = (c[2], c[3], c[4])
  if gs != "none" && vs != "none" && vs != "unknown" {
    if case .compiled = o { meshCompiled += 1 } else if case .failed = o { meshFailed += 1 }
    if ps == "none" { meshDepthOnly += 1 }
  }
  if vs != "none" && vs != "unknown" && ps != "none" && ps != "unknown" { psSeen.insert(ps) }
  else if !(gs != "none" && ps == "none") { continue }
  switch o {
  case .compiled:
    compiled += 1
    if ps != "none" { psCompiled.insert(ps) }
    report += "\(vs)\t\(ps)\tcompiled\t\n"
  case .failed(let m):
    failedCount += 1
    // Group messages by their text with names and numbers removed.
    let key = m.replacingOccurrences(of: "`[^`]*`", with: "`…`", options: .regularExpression)
      .replacingOccurrences(of: "[0-9]+", with: "N", options: .regularExpression)
    errors[String(key.prefix(150)), default: 0] += 1
    report += "\(vs)\t\(ps)\tfailed\t\(m)\n"
  case .skipped(let why):
    skipped[why, default: 0] += 1; report += "\(vs)\t\(ps)\tskipped\t\(why)\n"
  }
}
try report.write(to: root.appendingPathComponent("convert-airconv/pipelines.tsv"), atomically: true, encoding: .utf8)

print("device: \(device.name)")
print("recorded combinations: \(combos.count)")
print("tested (a vertex shader with a pixel shader, a geometry shader, or both): \(compiled + failedCount + skipped.values.reduce(0, +))")
print("  compiled: \(compiled)")
print("  failed:   \(failedCount)")
for (why, n) in skipped.sorted(by: { $0.value > $1.value }) { print("  skipped (\(why)): \(n)") }
print("mesh pipelines (with a geometry shader): \(meshCompiled) compiled, \(meshFailed) failed, \(meshDepthOnly) of them depth-only")
print("pixel shaders compiled in at least one real pipeline: \(psCompiled.count) of \(psSeen.count) seen in pairs")
print(String(format: "time: %.1f s", seconds))
if !errors.isEmpty {
  print("failure messages, most common first:")
  for (m, n) in errors.sorted(by: { $0.value > $1.value }).prefix(8) { print("  \(n)× \(m)") }
}
