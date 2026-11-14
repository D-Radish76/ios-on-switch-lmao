import CoreGraphics
import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// One icon as iconservicesagent draws it at one size and scale, uncached (setIgnoreCache:) and as the cache serves it,
/// for the icon_* cases. Fields, prefixed `<icon><points>x<scale in hundredths>` (icons: set Settings, pc Passcode, cal
/// Calendar):
///  - `_side`: the image's pixels;
///  - `_edge=right/bottom`: IconEdgeBand's asymmetric pixels of the uncached image's last two columns and rows (raw's
///    `edge miss` shape); Apple's GPU draws every icon symmetric (0/0 on an M4 Max, iconpassoracle sizes);
///  - `_cached_diff`: the pixels where the cached image is more than IconServicesCache.codes off the uncached one,
///    `unmatched` where the sizes differ: a cache that holds another build's or another day's picture;
///  - `_placeholder`: the uncached image is the placeholder IconServices makes when the agent refuses the request (a
///    missing app's or type's icon at the same size, by the request's kind), `unknown` without one.
/// A failure names the field.
enum IconArm {
    static func placeholder(
        _ source: IconServicesCache.IconSource, points: CGFloat, scale: CGFloat
    )
        -> IconServicesCache.Image?
    {
        IconServicesCache.icon(source.missing, points: points, scale: scale, ignoreCache: true)
    }

    static func judge(
        _ name: String, _ source: IconServicesCache.IconSource, points: CGFloat, scale: CGFloat,
        placeholder: IconServicesCache.Image?, _ metrics: inout Metrics
    ) -> [String] {
        let prefix = "\(name)\(Int(points))x\(Int((scale * 100).rounded()))"
        let cached = IconServicesCache.icon(source, points: points, scale: scale, ignoreCache: false)
        guard let fresh = IconServicesCache.icon(source, points: points, scale: scale, ignoreCache: true) else {
            metrics.set("\(prefix)_api", "absent")
            return ["\(prefix)_api"]
        }
        var failures: [String] = []
        metrics.set("\(prefix)_side", fresh.side)
        let edge = IconEdgeBand.asymmetry(IconEdgeBand.alpha(fresh.rgba), side: fresh.side)
        metrics.set("\(prefix)_edge", edge.field)
        if max(edge.right, edge.bottom) > fresh.side / 32 { failures.append("\(prefix)_edge") }
        if let cached, cached.side == fresh.side {
            let diff = IconServicesCache.differing(cached, fresh)
            metrics.set("\(prefix)_cached_diff", diff)
            if diff > 0 { failures.append("\(prefix)_cached_diff") }
        } else if cached != nil {
            metrics.set("\(prefix)_cached_diff", "unmatched")
            failures.append("\(prefix)_cached_diff")
        }
        if let placeholder {
            let isPlaceholder = IconServicesCache.differing(placeholder, fresh) == 0
            metrics.set("\(prefix)_placeholder", isPlaceholder)
            if isPlaceholder { failures.append("\(prefix)_placeholder") }
        } else {
            metrics.set("\(prefix)_placeholder", "unknown")
        }
        return failures
    }

    /// The device's own scale and the 1.25 Settings asks its named images at (boot 22's `scaleFactor:1.250000`)
    static func scales() async -> [CGFloat] {
        let screen = await MainActor.run { UIScreen.main.scale }
        return screen == 1.25 ? [screen] : [screen, 1.25]
    }
}

/// Settings' icon at the App Library's sizes (68 and 28 points, boot 22's 136x136 and 56x56 surfaces at scale 2, 40 and
/// 16) at the device's scale and at 1.25, where every size is whole pixels: IconArm's fields. FAIL: an arm's edge
/// bands asymmetric, its cached picture another, a placeholder, or IconServices' calls absent.
struct IconSmallSizesTest: HWTest {
    let id = "gpu.raw.icon_small_sizes"
    let name = "IconServices' Settings icon at the App Library's sizes: edges, cache and placeholder"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var failures: [String] = []
        for scale in await IconArm.scales() {
            for points: CGFloat in [68, 40, 28, 16] {
                let placeholder = IconArm.placeholder(IconServicesCache.settings, points: points, scale: scale)
                failures += IconArm.judge(
                    "set", IconServicesCache.settings, points: points, scale: scale, placeholder: placeholder,
                    &metrics)
            }
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: ","), metrics)
    }
}

/// Settings' Passcode pane icon (com.apple.graphic-icon.passcode, which Settings shows on a device without Face ID or
/// Touch ID) at Settings' 29-point row size and 68 points, at the device's scale and at 1.25: IconArm's fields. FAIL as
/// gpu.raw.icon_small_sizes.
struct IconPasscodeTest: HWTest {
    let id = "gpu.raw.icon_passcode"
    let name = "IconServices' Passcode pane icon: edges, cache and placeholder"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var failures: [String] = []
        let passcode = IconServicesCache.IconSource.type("com.apple.graphic-icon.passcode")
        for scale in await IconArm.scales() {
            for points: CGFloat in [29, 68] where (points * scale).rounded() == points * scale {
                let placeholder = IconArm.placeholder(passcode, points: points, scale: scale)
                failures += IconArm.judge(
                    "pc", passcode, points: points, scale: scale, placeholder: placeholder, &metrics)
            }
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: ","), metrics)
    }
}

/// Calendar's icon (com.apple.mobilecal, drawn with today's date, so a cached picture of another day is another
/// picture) at the App Library's 68 and 28 points, at the device's scale and at 1.25: IconArm's fields. FAIL as
/// gpu.raw.icon_small_sizes.
struct IconCalendarTest: HWTest {
    let id = "gpu.raw.icon_calendar"
    let name = "IconServices' Calendar icon: edges, today's cache and placeholder"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var failures: [String] = []
        let calendar = IconServicesCache.IconSource.bundle("com.apple.mobilecal")
        for scale in await IconArm.scales() {
            for points: CGFloat in [68, 28] {
                let placeholder = IconArm.placeholder(calendar, points: points, scale: scale)
                failures += IconArm.judge(
                    "cal", calendar, points: points, scale: scale, placeholder: placeholder,
                    &metrics)
            }
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: ","), metrics)
    }
}

/// What iconservicesagent's rendered-icon cache holds (IconServicesCache.cacheInfo): `container` (the group container's
/// folder, a UUID or the group's name), `folders` and `first_folder` (24B119 keeps its cache in a salt folder), `info_at` (top, salt or none), `info_build` (__cache_info__'s ProductBuildVersion, which the
/// agent resets the cache at launch without), `reset_this_boot`, `system_build`, and `served`: whether a cached request
/// for Settings' icon left the store index as it was (cache) or grew it (generated). FAIL: no __cache_info__ the app can
/// read, a ProductBuildVersion other than the system's, or no store index beside it.
struct IconCacheTest: HWTest {
    let id = "system.icon_cache"
    let name = "IconServices' rendered-icon cache: where it is, its build, and whether it serves"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        let system = IconServicesCache.systemBuild() ?? "unreadable"
        let found = IconServicesCache.cacheInfo()
        metrics.set("system_build", system)
        metrics.set("container", found.container.isEmpty ? "-" : found.container)
        metrics.set("folders", found.folders.count)
        if let first = found.folders.first { metrics.set("first_folder", first) }
        metrics.set("info_at", found.at)
        metrics.set("info_build", found.build)
        if let reset = found.resetThisBoot { metrics.set("reset_this_boot", reset) }
        var checks: [(String, Bool)] = [
            ("__cache_info__ is readable", found.at != "none"),
            ("__cache_info__ holds the system's build", found.build == system),
        ]
        guard let store = found.store else { return .judged(checks, metrics) }
        let before = Self.stamp(store)
        _ = IconServicesCache.icon(IconServicesCache.settings, points: 52, scale: 2, ignoreCache: false)
        let after = Self.stamp(store)
        metrics.set("served", before == nil ? "no_store" : before == after ? "cache" : "generated")
        checks.append(("the store index is beside __cache_info__", before != nil))
        return .judged(checks, metrics)
    }

    static func stamp(_ url: URL) -> [Int]? {
        guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey]),
            let size = values.fileSize, let date = values.contentModificationDate
        else { return nil }
        return [size, Int(date.timeIntervalSince1970 * 1000)]
    }
}
