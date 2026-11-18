import Foundation
import HWTestCore
import HWTestGPU
import Metal
import SwitchBench

/// A 500 texture of 64x512 over a 64x8 '420v' of 768 bytes, its rows running past the allocation's last page
/// (gpu/YcbcrRowsPastPage.swift): an M4 Max makes it, and every one of its 512 rows reads as a reference surface whose
/// planes hold the surface's bytes, then 0, in their own rows (rows_matched 512): rows past the last page read the
/// conversion of code 0 (0, 0.5312, 0). With the kernel's TegraGM20BInfoSparse (sparse_info_bit 1) the raw path makes it
/// the same way, the pages past the allocation sparse; without it the texture is nil (made 0, one raw:tex.surf.form
/// line). sparse_faults: the test's command buffers that failed (0).
struct YcbcrRowsPastPageTest: HWTest {
    let id = "gpu.raw.ycbcr_rows_past_page"
    let name = "A YCbCr texture whose rows run past its surface's last page reads them as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let sparse = switch_bench_kernel_flags() & 1
        let before = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        metrics.set("sparse_info_bit", Int(sparse))
        do {
            let r = try await YcbcrRowsPastPage.observe(gpu)
            metrics.set("made", r.made)
            if r.made { metrics.set("rows_matched", r.rowsMatched) }
            metrics.set("first_off", r.first)
            metrics.set("sparse_faults", r.paths.filter { $0.hasPrefix("error:") }.count)
            if sparse == 1 {
                checks.append(("made over sparse pages", r.made))
                checks.append(("rows_matched=\(r.rowsMatched) of \(YcbcrRowsPastPage.height)", r.rowsMatched == 512))
                checks.append(("no failed read: \(r.paths.joined(separator: ","))", !r.paths.contains { $0.hasPrefix("error:") }))
            } else {
                checks.append(("nil without TegraGM20BInfoSparse", !r.made))
            }
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = RawFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the raw path", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
