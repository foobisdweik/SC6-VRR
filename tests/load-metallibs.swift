// Load converted SC6 shader libraries with the Metal runtime (notes/native-metal.md, step 3).
//
// Usage: load-metallibs <results.tsv> <metallib dir> <capture dir>
//
// For each library it records three levels, because Metal checks shaders in stages:
//   1. The device accepts the library data (newLibraryWithData).
//   2. The library returns its entry function.
//   3. Metal compiles the function to GPU code inside a pipeline state.
//      Compute shaders get a compute pipeline. Vertex shaders get a depth-only
//      render pipeline, which needs no fragment shader.
//      Pixel shaders need a vertex shader whose outputs match their inputs,
//      and the captures do not record those pairs, so level 3 skips them.
import Foundation
import Metal

let args = CommandLine.arguments
guard args.count == 4 else {
  print("usage: load-metallibs <results.tsv> <metallib dir> <capture dir>")
  exit(2)
}
let resultsURL = URL(fileURLWithPath: args[1])
let libDir = URL(fileURLWithPath: args[2])
let captureDir = URL(fileURLWithPath: args[3])
guard let device = MTLCreateSystemDefaultDevice() else { print("no Metal device"); exit(1) }

// Stage of each shader, from the capture index files.
var stageOf: [String: String] = [:]
for file in try FileManager.default.contentsOfDirectory(at: captureDir, includingPropertiesForKeys: nil)
where file.lastPathComponent.hasPrefix("index-") {
  // The DLL writes Windows line endings. Swift treats "\r\n" as one character,
  // so split on any newline character.
  for line in try String(contentsOf: file, encoding: .utf8).split(whereSeparator: \.isNewline).dropFirst() {
    let f = line.split(separator: "\t", omittingEmptySubsequences: false)
    if f.count > 5, stageOf[String(f[5])] == nil { stageOf[String(f[5])] = String(f[2]) }
  }
}

// Builds a vertex descriptor from the shader's own attributes: one attribute
// after another in buffer 30, which the converted shaders do not use (assumed).
// It only lets Metal compile the shader; the game's real layouts differ.
func descriptor(for function: MTLFunction) -> MTLVertexDescriptor {
  let formats: [MTLDataType: (MTLVertexFormat, Int)] = [
    .float: (.float, 4), .float2: (.float2, 8), .float3: (.float3, 12), .float4: (.float4, 16),
    .int: (.int, 4), .int2: (.int2, 8), .int3: (.int3, 12), .int4: (.int4, 16),
    .uint: (.uint, 4), .uint2: (.uint2, 8), .uint3: (.uint3, 12), .uint4: (.uint4, 16),
    .half: (.half, 2), .half2: (.half2, 4), .half3: (.half3, 6), .half4: (.half4, 8),
  ]
  let d = MTLVertexDescriptor()
  var offset = 0
  for attribute in function.vertexAttributes ?? [] where attribute.isActive {
    let (format, size) = formats[attribute.attributeType] ?? (.float4, 16)
    d.attributes[attribute.attributeIndex].format = format
    d.attributes[attribute.attributeIndex].bufferIndex = 30
    d.attributes[attribute.attributeIndex].offset = offset
    offset += (size + 3) & ~3
  }
  d.layouts[30].stride = max(offset, 4)
  return d
}

struct Count { var total = 0, loaded = 0, function = 0, compiled = 0, compileTried = 0 }
var counts: [String: Count] = [:]
var failures: [String] = []

for line in try String(contentsOf: resultsURL, encoding: .utf8).split(whereSeparator: \.isNewline) {
  let f = line.split(separator: "\t")
  guard f.count == 3, f[2] == "ok" else { continue }
  let name = String(f[0])
  let stage = stageOf[name] ?? "?"
  var c = counts[stage, default: Count()]
  c.total += 1
  defer { counts[stage] = c }
  let data: Data
  do { data = try Data(contentsOf: libDir.appendingPathComponent(name + ".metallib")) } catch {
    failures.append("\(stage) \(name.prefix(16)) read: \(error.localizedDescription)"); continue
  }
  let library: MTLLibrary
  do {
    library = try data.withUnsafeBytes { try device.makeLibrary(data: DispatchData(bytes: $0)) }
  } catch {
    failures.append("\(stage) \(name.prefix(16)) load: \(error.localizedDescription)"); continue
  }
  c.loaded += 1
  guard let fname = library.functionNames.first, let function = library.makeFunction(name: fname) else {
    failures.append("\(stage) \(name.prefix(16)) no function"); continue
  }
  c.function += 1
  do {
    switch function.functionType {
    case .kernel:
      c.compileTried += 1
      _ = try device.makeComputePipelineState(function: function)
      c.compiled += 1
    case .vertex:
      c.compileTried += 1
      let d = MTLRenderPipelineDescriptor()
      // A pipeline without a fragment function is valid (depth-only drawing),
      // so the vertex shader compiles without a matching pixel shader.
      d.vertexFunction = function
      d.depthAttachmentPixelFormat = .depth32Float
      d.vertexDescriptor = descriptor(for: function)
      _ = try device.makeRenderPipelineState(descriptor: d)
      c.compiled += 1
    default:
      break
    }
  } catch {
    failures.append("\(stage) \(name.prefix(16)) compile: \(error.localizedDescription.prefix(160))")
  }
}

print("device: \(device.name)")
print("stage   total  loaded  function  compiled/tried")
var all = Count()
for (stage, c) in counts.sorted(by: { $0.key < $1.key }) {
  print(String(format: "%-6@ %6d %7d %9d %9d/%d", stage as NSString, c.total, c.loaded, c.function, c.compiled, c.compileTried))
  all.total += c.total; all.loaded += c.loaded; all.function += c.function
  all.compiled += c.compiled; all.compileTried += c.compileTried
}
print(String(format: "%-6@ %6d %7d %9d %9d/%d", "all" as NSString, all.total, all.loaded, all.function, all.compiled, all.compileTried))
if !failures.isEmpty {
  print("\(failures.count) failures; first ones:")
  failures.prefix(10).forEach { print("  " + $0) }
}
