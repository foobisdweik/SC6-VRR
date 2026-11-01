// pad-watch: log every controller input change that macOS reports, outside
// Wine and outside the game, to check whether one physical press reaches
// macOS as one press.
//
// It listens at two levels of the macOS input pipeline:
//   HID  IOHIDManager input values: the raw element values of the device.
//        Wine's controller driver (winebus) reads this layer, so XInput in
//        the game gets its data from here.
//   GC   The GameController framework: Apple's high-level controller layer.
// Each line has the time since start in milliseconds (0.1 ms resolution),
// taken from the event's own timestamp.
//
// A "BOUNCE" line means a button or D-pad direction went down, up, and down
// again within --bounce-ms (default 60 ms). A clean single press never makes one.
//
// Build: tools/pad-watch/build.sh
// Usage: pad-watch [--seconds N] [--axes] [--bounce-ms N]
//   --seconds N  stop after N seconds (default: run until Ctrl-C)
//   --axes       also log stick and trigger axes (they change constantly)
import Foundation
import GameController
import IOKit.hid

setvbuf(stdout, nil, _IOLBF, 0) // one line at a time, also when piped to tee

var seconds: Double = 0
var showAxes = false
var bounceMs: Double = 60
var argv = CommandLine.arguments.dropFirst()
while let a = argv.popFirst() {
  switch a {
  case "--seconds": seconds = Double(argv.popFirst() ?? "") ?? 0
  case "--axes": showAxes = true
  case "--bounce-ms": bounceMs = Double(argv.popFirst() ?? "") ?? 60
  default:
    FileHandle.standardError.write("usage: pad-watch [--seconds N] [--axes] [--bounce-ms N]\n".data(using: .utf8)!)
    exit(2)
  }
}

var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
let start = mach_absolute_time()
func ms(_ t: UInt64) -> Double {
  let d = t >= start ? t - start : 0
  return Double(d) * Double(timebase.numer) / Double(timebase.denom) / 1e6
}
func stamp(_ t: UInt64) -> String { String(format: "%10.1f", ms(t)) }

// Bounce detector: per input name, the times of its last release and press.
var lastRelease: [String: Double] = [:]
var lastPress: [String: Double] = [:]
func edge(_ name: String, down: Bool, at t: UInt64, layer: String) {
  let now = ms(t)
  if down {
    if let r = lastRelease[name], let p = lastPress[name], now - p <= bounceMs, r >= p {
      print(String(format: "%@ %@ BOUNCE %@: down again %.1f ms after the previous press (up for %.1f ms)",
                   stamp(t), layer, name, now - p, now - r))
    }
    lastPress[name] = now
  } else {
    lastRelease[name] = now
  }
}

// ---------- HID level -------------------------------------------------------

let hatNames = ["up", "up-right", "right", "down-right", "down", "down-left", "left", "up-left"]
func hatDirections(_ v: Int, _ min: Int, _ max: Int) -> Set<String> {
  // A hat reports min...max for the 8 directions and a value outside that range for "centered".
  guard v >= min && v <= max, max - min == 7 else { return [] }
  let n = hatNames[v - min]
  return Set(n.split(separator: "-").map(String.init))
}
var hatState: [String: Set<String>] = [:]

func elementName(_ page: UInt32, _ usage: UInt32) -> String? {
  switch (page, usage) {
  case (0x09, _): return "button \(usage)"
  case (0x01, 0x39): return "hat"
  case (0x01, 0x90): return "dpad up"
  case (0x01, 0x91): return "dpad down"
  case (0x01, 0x92): return "dpad right"
  case (0x01, 0x93): return "dpad left"
  case (0x01, 0x30...0x35): return showAxes ? "axis 0x\(String(usage, radix: 16))" : nil
  case (0x02, 0xC4), (0x02, 0xC5): return showAxes ? "trigger 0x\(String(usage, radix: 16))" : nil
  case (0x0C, _): return "consumer 0x\(String(usage, radix: 16))" // for example the Xbox/menu button
  default: return showAxes ? "page 0x\(String(page, radix: 16)) usage 0x\(String(usage, radix: 16))" : nil
  }
}

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
let matches: [[String: Int]] = [
  [kIOHIDDeviceUsagePageKey: 0x01, kIOHIDDeviceUsageKey: 0x05], // game pad
  [kIOHIDDeviceUsagePageKey: 0x01, kIOHIDDeviceUsageKey: 0x04], // joystick
  [kIOHIDDeviceUsagePageKey: 0x01, kIOHIDDeviceUsageKey: 0x08], // multi-axis controller
]
IOHIDManagerSetDeviceMatchingMultiple(manager, matches as CFArray)

func deviceName(_ d: IOHIDDevice) -> String {
  let p = IOHIDDeviceGetProperty(d, kIOHIDProductKey as CFString) as? String ?? "unknown device"
  let v = IOHIDDeviceGetProperty(d, kIOHIDVendorIDKey as CFString) as? Int ?? 0
  let i = IOHIDDeviceGetProperty(d, kIOHIDProductIDKey as CFString) as? Int ?? 0
  let t = IOHIDDeviceGetProperty(d, kIOHIDTransportKey as CFString) as? String ?? "?"
  return String(format: "\"%@\" (vid %04x pid %04x, %@)", p, v, i, t)
}

IOHIDManagerRegisterDeviceMatchingCallback(manager, { _, _, _, device in
  print("\(stamp(mach_absolute_time())) HID  connected \(deviceName(device))")
}, nil)
IOHIDManagerRegisterDeviceRemovalCallback(manager, { _, _, _, device in
  print("\(stamp(mach_absolute_time())) HID  disconnected \(deviceName(device))")
}, nil)

IOHIDManagerRegisterInputValueCallback(manager, { _, _, _, value in
  let e = IOHIDValueGetElement(value)
  let page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e)
  guard let name = elementName(page, usage) else { return }
  let v = IOHIDValueGetIntegerValue(value)
  let t = IOHIDValueGetTimeStamp(value)
  let deviceID = String(UInt(bitPattern: Unmanaged.passUnretained(IOHIDElementGetDevice(e)).toOpaque().hashValue))
  if name == "hat" {
    let mn = IOHIDElementGetLogicalMin(e), mx = IOHIDElementGetLogicalMax(e)
    let dirs = hatDirections(v, mn, mx)
    let key = deviceID
    let before = hatState[key] ?? []
    print("\(stamp(t)) HID  hat \(v) (\(dirs.isEmpty ? "centered" : dirs.sorted().joined(separator: "+")))")
    for d in dirs.subtracting(before) { edge("hat \(d)", down: true, at: t, layer: "HID ") }
    for d in before.subtracting(dirs) { edge("hat \(d)", down: false, at: t, layer: "HID ") }
    hatState[key] = dirs
  } else {
    print("\(stamp(t)) HID  \(name) = \(v)")
    if page == 0x09 || (page == 0x01 && (0x90...0x93).contains(usage)) {
      edge(name, down: v != 0, at: t, layer: "HID ")
    }
  }
}, nil)

IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
let openResult = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
if openResult != kIOReturnSuccess {
  print(String(format: "HID: IOHIDManagerOpen failed (0x%08x); if macOS asks, allow Input Monitoring for this terminal", openResult))
}

// ---------- GameController level --------------------------------------------

// A command-line tool is never the frontmost app, so it must ask for events
// that arrive while another app (the terminal, or the game) is in front.
GCController.shouldMonitorBackgroundEvents = true

func watch(_ c: GCController) {
  let name = c.vendorName ?? "controller"
  print("\(stamp(mach_absolute_time())) GC   connected \"\(name)\" (\(c.productCategory))")
  guard let pad = c.extendedGamepad else {
    print("\(stamp(mach_absolute_time())) GC   \"\(name)\" has no extended gamepad profile")
    return
  }
  let dirs: [(String, GCControllerButtonInput)] = [
    ("dpad up", pad.dpad.up), ("dpad down", pad.dpad.down), ("dpad left", pad.dpad.left), ("dpad right", pad.dpad.right),
  ]
  let buttons: [(String, GCControllerButtonInput)] = [
    ("A", pad.buttonA), ("B", pad.buttonB), ("X", pad.buttonX), ("Y", pad.buttonY),
    ("LB", pad.leftShoulder), ("RB", pad.rightShoulder), ("menu", pad.buttonMenu),
  ]
  for (n, b) in dirs + buttons {
    b.pressedChangedHandler = { _, _, pressed in
      let t = mach_absolute_time() // GameController gives no event timestamp here
      print("\(stamp(t)) GC   \(n) \(pressed ? "down" : "up")")
      edge(n, down: pressed, at: t, layer: "GC  ")
    }
  }
  if showAxes {
    pad.leftThumbstick.valueChangedHandler = { _, x, y in
      print("\(stamp(mach_absolute_time())) GC   left stick \(String(format: "%.3f %.3f", x, y))")
    }
  }
}
NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { n in
  if let c = n.object as? GCController { watch(c) }
}
NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { n in
  let c = n.object as? GCController
  print("\(stamp(mach_absolute_time())) GC   disconnected \"\(c?.vendorName ?? "controller")\"")
}
for c in GCController.controllers() { watch(c) }

print("pad-watch: listening (HID and GameController). Press Ctrl-C to stop.")
if seconds > 0 {
  DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { exit(0) }
}
signal(SIGINT) { _ in exit(0) }
CFRunLoopRun()
