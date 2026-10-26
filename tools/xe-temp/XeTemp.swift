// xe-temp: menu-bar CPU temperature and power for the XPS 13 Plus (i7-1360P) on macOS Tahoe.
// Reads sysctl debug.xepm.sensors from NootedGreen (XE-PM): per-core and package temperatures (TjMax minus the
// thermal-status readout) and the RAPL energy counters (package / IA cores / GPU). Battery temperature from
// AppleSmartBattery. No root needed. Build: zsh tools/xe-temp/build.sh
import SwiftUI
import IOKit

struct Sensors {
    var pkg = 0, tjmax = 100
    var pTemps: [Int] = [], eTemps: [Int] = []
    var pkgW = 0.0, cpuW = 0.0, gpuW = 0.0
    var battery: Double? = nil
    var ok = false
}

final class Monitor: ObservableObject {
    @Published var s = Sensors()
    @Published var history: [Double] = []
    private var last: (pkg: UInt64, ia: UInt64, gt: UInt64, t: Date)? = nil
    private var timer: Timer?

    init() {
        poll()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.poll() }
    }

    private func readRaw() -> [UInt8]? {
        var len = 0
        guard sysctlbyname("debug.xepm.sensors", nil, &len, nil, 0) == 0, len >= 112 else { return nil }
        var buf = [UInt8](repeating: 0, count: len)
        guard sysctlbyname("debug.xepm.sensors", &buf, &len, nil, 0) == 0 else { return nil }
        return buf
    }
    private func u32(_ b: [UInt8], _ o: Int) -> UInt32 { b[o..<o + 4].enumerated().reduce(0) { $0 | UInt32($1.element) << (8 * $1.offset) } }
    private func u64(_ b: [UInt8], _ o: Int) -> UInt64 { b[o..<o + 8].enumerated().reduce(0) { $0 | UInt64($1.element) << (8 * $1.offset) } }

    private func batteryTemp() -> Double? {
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"))
        guard svc != 0 else { return nil }
        defer { IOObjectRelease(svc) }
        guard let v = IORegistryEntryCreateCFProperty(svc, "Temperature" as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? Int, v > 0 else { return nil }
        return Double(v) / 10.0 - 273.15   // 0.1 K
    }

    func poll() {
        var n = Sensors()
        n.battery = batteryTemp()
        if let b = readRaw() {
            // struct { u32 ver, ncpu, tjmax, pkg; u8 core[32], type[32]; u32 eunit, pad; u64 epkg, eia, egt }
            n.ok = u32(b, 0) == 1
            n.tjmax = Int(u32(b, 8)); n.pkg = Int(u32(b, 12))
            for c in 0..<32 {
                let t = Int(b[16 + c]), ty = b[48 + c]
                if t == 0 { continue }
                if ty == 0x40 { if c % 2 == 0 { n.pTemps.append(t) } }   // one value per P-core (HT pairs share it)
                else if ty == 0x20 { n.eTemps.append(t) }
            }
            let unit = pow(0.5, Double(u32(b, 80)))
            let ep = u64(b, 88), ei = u64(b, 96), eg = u64(b, 104), now = Date()
            if let l = last {
                let dt = now.timeIntervalSince(l.t)
                func w(_ a: UInt64, _ b: UInt64) -> Double { Double((b &- a) & 0xffffffff) * unit / dt }
                if dt > 0.2 { n.pkgW = w(l.pkg, ep); n.cpuW = w(l.ia, ei); n.gpuW = w(l.gt, eg) }
            }
            last = (ep, ei, eg, now)
        }
        s = n
        if n.ok { history.append(Double(n.pkg)); if history.count > 90 { history.removeFirst(history.count - 90) } }
    }
}

func tempColor(_ t: Int) -> Color {
    switch t {
    case ..<60: return .green
    case 60..<75: return .mint
    case 75..<85: return .yellow
    case 85..<93: return .orange
    default: return .red
    }
}

struct Sparkline: View {
    let values: [Double]
    var body: some View {
        GeometryReader { g in
            let lo = max(20, (values.min() ?? 40) - 5), hi = min(105, (values.max() ?? 60) + 5)
            let span = max(hi - lo, 1)
            Path { p in
                for (i, v) in values.enumerated() {
                    let x = g.size.width * CGFloat(i) / CGFloat(max(values.count - 1, 1))
                    let y = g.size.height * (1 - CGFloat((v - lo) / span))
                    i == 0 ? p.move(to: CGPoint(x: x, y: y)) : p.addLine(to: CGPoint(x: x, y: y))
                }
            }
            .stroke(LinearGradient(colors: [.mint, .yellow, .orange], startPoint: .bottom, endPoint: .top),
                    style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
        }
    }
}

struct CoreBar: View {
    let t: Int, tjmax: Int
    var body: some View {
        VStack(spacing: 3) {
            GeometryReader { g in
                ZStack(alignment: .bottom) {
                    Capsule().fill(.white.opacity(0.08))
                    Capsule().fill(tempColor(t).gradient)
                        .frame(height: g.size.height * CGFloat(min(max(t - 25, 2), tjmax - 25)) / CGFloat(tjmax - 25))
                }
            }
            Text("\(t)").font(.system(size: 9, weight: .medium, design: .rounded)).foregroundStyle(.secondary)
        }
    }
}

struct Stat: View {
    let title: String, value: String, tint: Color
    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.caption2).foregroundStyle(.secondary)
            Text(value).font(.system(.title3, design: .rounded).weight(.semibold)).foregroundStyle(tint).monospacedDigit()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14))
    }
}

struct Panel: View {
    @ObservedObject var m: Monitor
    var body: some View {
        let s = m.s
        VStack(alignment: .leading, spacing: 12) {
            if !s.ok {
                Label("Sensors not available", systemImage: "exclamationmark.triangle")
                Text("Needs NootedGreen with XE-PM sensors (sysctl debug.xepm.sensors).")
                    .font(.caption).foregroundStyle(.secondary)
            } else {
                HStack(alignment: .firstTextBaseline) {
                    Text("\(s.pkg)°").font(.system(size: 44, weight: .semibold, design: .rounded))
                        .foregroundStyle(tempColor(s.pkg).gradient).monospacedDigit()
                    VStack(alignment: .leading) {
                        Text("CPU package").font(.headline)
                        Text("i7-1360P · limit \(s.tjmax)°").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                }
                Sparkline(values: m.history).frame(height: 44)
                    .padding(8).glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14))
                HStack(spacing: 8) {
                    Stat(title: "Package", value: String(format: "%.1f W", s.pkgW), tint: .primary)
                    Stat(title: "CPU", value: String(format: "%.1f W", s.cpuW), tint: .cyan)
                    Stat(title: "GPU", value: String(format: "%.1f W", s.gpuW), tint: .purple)
                }
                VStack(alignment: .leading, spacing: 6) {
                    Text("P-cores").font(.caption).foregroundStyle(.secondary)
                    HStack(spacing: 6) { ForEach(Array(s.pTemps.enumerated()), id: \.offset) { CoreBar(t: $0.element, tjmax: s.tjmax) } }
                        .frame(height: 54)
                    Text("E-cores").font(.caption).foregroundStyle(.secondary)
                    HStack(spacing: 4) { ForEach(Array(s.eTemps.enumerated()), id: \.offset) { CoreBar(t: $0.element, tjmax: s.tjmax) } }
                        .frame(height: 54)
                }
                .padding(10).glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14))
                if let bt = s.battery {
                    HStack {
                        Label("Battery", systemImage: "battery.75percent")
                        Spacer()
                        Text(String(format: "%.0f°", bt)).monospacedDigit().foregroundStyle(tempColor(Int(bt)))
                    }
                    .padding(10).glassEffect(.regular, in: RoundedRectangle(cornerRadius: 14))
                }
            }
            HStack {
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }.buttonStyle(.glass)
            }
        }
        .padding(16)
        .frame(width: 320)
    }
}

@main
struct XeTempApp: App {
    @StateObject private var m = Monitor()
    var body: some Scene {
        MenuBarExtra {
            Panel(m: m)
        } label: {
            HStack(spacing: 3) {
                Image(systemName: "thermometer.medium")
                Text(m.s.ok ? "\(m.s.pkg)°" : "--°").monospacedDigit()
            }
        }
        .menuBarExtraStyle(.window)
    }
}
