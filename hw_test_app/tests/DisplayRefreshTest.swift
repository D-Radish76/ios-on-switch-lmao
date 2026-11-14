import CoreFoundation
import Darwin
import HWTestCore
import UIKit

/// IOMFBDisplayRefresh on the internal display (AppleCLCD), which QuartzCore's AppleDisplay::update_display_limits
/// reads (24B119) and IOMobileFramebufferLegacy::setDisplayRefreshProperties publishes. Its shortest refresh interval
/// is the panel's period at its highest rate, and on a fixed-rate internal panel the longest is the same (the family
/// answers 1/24 s for an external display); *_mach fields are mach ticks, period_mach the period expected.
struct DisplayRefreshTest: HWTest {
    let id = "display.refresh_dictionary"
    let name = "The display publishes IOMFBDisplayRefresh"
    let subsystem = Subsystem.compositor

    static let keys: [(key: String, field: String)] = [
        ("displayMinRefreshInterval", "min"), ("displayMinRefreshIntervalMachTime", "min_mach"),
        ("displayMaxRefreshInterval", "max"), ("displayMaxRefreshIntervalMachTime", "max_mach"),
        ("displayRefreshStep", "step"), ("displayRefreshStepMachTime", "step_mach"),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let fps = await MainActor.run { UIScreen.main.maximumFramesPerSecond }
        let period = Self.machTicks(seconds: 1 / Double(max(fps, 1)))
        let found = Self.dictionary()
        var metrics = Metrics()
        metrics.set("present", found != nil)
        metrics.set("period_mach", Int(period))
        var values: [String: Int64] = [:]
        for (key, field) in Self.keys {
            guard let number = found?[key] as? NSNumber else { continue }
            values[field] = number.int64Value
            metrics.set(field, Int(number.int64Value))
        }
        let minMach = Double(values["min_mach"] ?? 0)
        let internalPanel = values["max_mach"].map { $0 == values["min_mach"] } ?? false
        return .judged(
            [
                ("IOMFBDisplayRefresh on AppleCLCD (present)", found != nil),
                ("all six keys", values.count == Self.keys.count),
                ("min_mach within 1% of period_mach", abs(minMach - period) <= period / 100),
                ("max_mach == min_mach (an internal panel)", internalPanel),
            ], metrics)
    }

    static func machTicks(seconds: Double) -> Double {
        var base = mach_timebase_info_data_t()
        mach_timebase_info(&base)
        return seconds * 1e9 * Double(base.denom) / Double(base.numer)
    }

    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> CFMutableDictionary?
    typealias Service = @convention(c) (mach_port_t, CFDictionary?) -> mach_port_t
    typealias Property = @convention(c) (mach_port_t, CFString, CFAllocator?, UInt32) -> Unmanaged<CFTypeRef>?
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t

    /// AppleCLCD's IOMFBDisplayRefresh, nil when the service or the property is missing.
    static func dictionary() -> [String: Any]? {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let property = dlsym(iokit, "IORegistryEntryCreateCFProperty"),
            let release = dlsym(iokit, "IOObjectRelease")
        else { return nil }
        let display = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("AppleCLCD"))
        guard display != 0 else { return nil }
        defer { _ = unsafeBitCast(release, to: Release.self)(display) }
        let value = unsafeBitCast(property, to: Property.self)(display, "IOMFBDisplayRefresh" as CFString, nil, 0)
        return value?.takeRetainedValue() as? [String: Any]
    }
}
