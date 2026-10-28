// make-icon.swift <out.icns> <sf-symbol> <hex-top> <hex-bottom>
// Draws a macOS-style rounded-square icon (vertical gradient + white SF Symbol) and packs it with iconutil.
// Used by the build scripts of the menu-bar tools: swift make-icon.swift AppIcon.icns thermometer.medium FF9F0A FF375F
import AppKit

let a = CommandLine.arguments
guard a.count == 5 else { print("usage: make-icon <out.icns> <symbol> <hexTop> <hexBottom>"); exit(1) }
func color(_ h: String) -> NSColor {
    let v = UInt32(h, radix: 16) ?? 0
    return NSColor(srgbRed: CGFloat(v >> 16 & 0xFF) / 255, green: CGFloat(v >> 8 & 0xFF) / 255, blue: CGFloat(v & 0xFF) / 255, alpha: 1)
}
let top = color(a[3]), bottom = color(a[4])

func render(_ px: Int) -> Data? {
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                                     hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return nil }
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)
    // Apple's grid: 824/1024 body, centred, corner radius ~ 185/1024
    let inset = s * 100 / 1024, body = NSRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let path = NSBezierPath(roundedRect: body, xRadius: s * 185 / 1024, yRadius: s * 185 / 1024)
    NSGraphicsContext.saveGraphicsState()
    let sh = NSShadow(); sh.shadowColor = .black.withAlphaComponent(0.3); sh.shadowBlurRadius = s * 12 / 1024; sh.shadowOffset = NSSize(width: 0, height: -s * 6 / 1024)
    sh.set()
    path.fill()
    NSGraphicsContext.restoreGraphicsState()
    NSGradient(starting: top, ending: bottom)?.draw(in: path, angle: -90)
    // soft highlight on the upper half
    NSGradient(starting: .white.withAlphaComponent(0.22), ending: .white.withAlphaComponent(0))?
        .draw(in: NSBezierPath(roundedRect: body, xRadius: s * 185 / 1024, yRadius: s * 185 / 1024), angle: -90)
    let cfg = NSImage.SymbolConfiguration(pointSize: s * 0.42, weight: .semibold).applying(.init(paletteColors: [.white]))
    if let sym = NSImage(systemSymbolName: a[2], accessibilityDescription: nil)?.withSymbolConfiguration(cfg) {
        let sz = sym.size, r = NSRect(x: (s - sz.width) / 2, y: (s - sz.height) / 2, width: sz.width, height: sz.height)
        sym.draw(in: r)
    } else { print("unknown symbol \(a[2])"); exit(1) }
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])
}

let set = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("icon-\(getpid()).iconset")
try? FileManager.default.createDirectory(at: set, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
        try? render(base * scale)?.write(to: set.appendingPathComponent(name))
    }
}
let p = Process()
p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
p.arguments = ["-c", "icns", set.path, "-o", a[1]]
try p.run(); p.waitUntilExit()
try? FileManager.default.removeItem(at: set)
exit(p.terminationStatus)
