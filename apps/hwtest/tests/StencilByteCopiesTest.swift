import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// X32_Stencil8 copied into R8Uint and back with the blit encoder (StencilByteCopies.swift): out_wrong= R8Uint bytes
/// that are not the stencil, in_wrong= stencil bytes that are not R8Uint's, depth_changed= depths the copy in moved,
/// paths=, m2v_cbs=. An M4 Max: 0, 0, 0.
struct StencilByteCopiesTest: HWTest {
    let id = "gpu.raw.stencil_byte_copies"
    let name = "Blit copies between an X32_Stencil8 view and R8Uint move the stencil bytes as they are, on raw"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = RawFacade.counters(of: gpu.device)
        let result = try await StencilByteCopies.run(gpu)
        var metrics = Metrics()
        metrics.set("out_wrong", result.outWrong)
        metrics.set("in_wrong", result.inWrong)
        metrics.set("depth_changed", result.depthChanged)
        metrics.set("paths", result.paths.map { String($0.prefix(60)) }.joined(separator: ","))
        metrics.merge(PathTally.counterMetrics(before: before, after: RawFacade.counters(of: gpu.device)))
        return .judged(
            [
                ("X32_Stencil8 into R8Uint keeps every byte", result.outWrong == 0),
                ("R8Uint into X32_Stencil8 keeps every byte", result.inWrong == 0),
                ("the copy in keeps the depth", result.depthChanged == 0),
                ("both copies on the raw path", result.paths.allSatisfy { $0 == "raw" }),
                ("no m2v command buffer", metrics["m2v_cbs"] == .integer(0)),
            ], metrics, proof: gpu.proof)
    }
}
