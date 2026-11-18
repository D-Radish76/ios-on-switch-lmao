import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// DynamicStrides.swift on the raw path, which once refused strides past 4095 (raw:draw.stride_twin, its own refusal
/// line, held to none by tools/devicerun refusal.hwtest-m2v): wrong= pixels off the colour (0 on an M4 Max), path=,
/// failed_cbs=.
struct DynamicStridesTest: HWTest {
    let id = "gpu.raw.draw_five_dynamic_strides"
    let name = "Five dynamic-stride vertex buffers at strides past 4095 bytes draw on raw"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = RawFacade.counters(of: gpu.device)
        let (wrong, path) = try await DynamicStrides.run(gpu)
        var metrics = Metrics()
        metrics.set("wrong", wrong)
        metrics.set("path", String(path.prefix(80)))
        let counted = RawFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        return .judged(
            [
                ("every pixel at the buffers' colour", wrong == 0), ("on the raw path", path == "raw"),
                FailedCounts(before: before, after: counted).check(),
            ], metrics, proof: gpu.proof)
    }
}
