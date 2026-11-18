import CoreGraphics
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The App Library's icon corners. `rb<side>_<storage>_*`: RenderBox in this process draws the icon mask at the small
/// pod icons' 54 pixels and the large ones' 136 (27 and 68 points at scale 2) on the default device, into a private and
/// a shared target, against its own CPU drawing (HWTestGPU RenderBoxCorner: `beyond`, `max_diff`, `partial` and
/// `ref_partial`), with the command buffers of RenderBox's that failed (`<arm>_failed_cbs`, `<arm>_failed_why`) and the
/// fetch routes of those draws. `strip_<storage>_*` and `strip_restart_<storage>_*`: RenderBox's draw shape alone, on
/// the raw path; `replay_scissor_*` the same draw culled and scissored; `derivative_<storage>_*` that shape's dfdx,
/// dfdy and fwidth in a fetching program, which RenderBox's coverage takes (HWTestGPU FetchStrip; an M4 Max: 0 pixels
/// off in every strip cell). `icon*`: IconServices' icons and its rendered-icon cache (IconServicesCache). FAIL: a
/// RenderBox or strip arm off its reference, off the raw path or failing (`<arm>_error=`), a RenderBox command buffer
/// that failed, a RenderBox arm off the Mac's drawing of its side (`<arm>_off_mac=`, RenderBoxCorner.macReference),
/// RenderBox's calls absent (`rb_api=`), or an IconServices arm's own failure.
struct IconCornersTest: HWTest {
    let id = "gpu.raw.icon_corners"
    let name = "Icon corners: RenderBox's mask and IconServices' icons at the App Library's sizes against references"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let iconRadius = 0.2237

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        var wrong: [String] = []
        metrics.set("dev", RawFacade.kind(of: gpu.device))
        for cell in try await FetchStrip.cells(gpu) {
            metrics.set("\(cell.name)_path", cell.path)
            metrics.set("\(cell.name)_routes", cell.routes)
            metrics.set("\(cell.name)_wrong", cell.wrong)
            if !cell.error.isEmpty { metrics.set("\(cell.name)_error", cell.error) }
            let offPath = cell.path != "raw" && cell.path != "device"
            if cell.wrong > 0 || offPath || !cell.error.isEmpty { wrong.append(cell.name) }
        }
        if RenderBoxCorner.available {
            for side in [54, 136] {
                for storage in [MTLStorageMode.private, .shared] {
                    let before = RawFacade.counters(of: gpu.device)
                    let name = "rb\(side)_\(storage == .private ? "private" : "shared")"
                    let cell: RenderBoxCorner.Cell
                    do {
                        cell = try await RenderBoxCorner.cell(
                            gpu, side: side, radius: Double(side) * Self.iconRadius, storage: storage)
                    } catch {
                        metrics.set("\(name)_error", "\(error)")
                        wrong.append(name)
                        continue
                    }
                    let after = RawFacade.counters(of: gpu.device)
                    metrics.set("\(name)_beyond", cell.beyond)
                    metrics.set("\(name)_max_diff", cell.maxDiff)
                    metrics.set("\(name)_partial", cell.partial)
                    metrics.set("\(name)_ref_partial", cell.referencePartial)
                    let failed = FailedCounts(before: before, after: after)
                    metrics.merge(failed.metrics, prefix: name + "_")
                    metrics.set("\(name)_routes", RawFacade.fetchRoutes(before: before, after: after))
                    let offMac = RenderBoxCorner.offMac(cell)
                    if let offMac { metrics.set("\(name)_off_mac", offMac) }
                    if cell.beyond > 0 || !failed.check().1 || offMac != nil { wrong.append(name) }
                }
            }
        } else {
            metrics.set("rb_api", "absent")
            wrong.append("rb_api")
        }
        IconServicesCache.measure(&metrics, &wrong)
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
