import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// What the raw device answers about function pointers. The raw path makes no visible function table and no function
/// handle (newVisibleFunctionTableWithDescriptor: and functionHandleWithFunction: are refused: raw/test/refused.txt),
/// so each answer must be NO: a YES sends 24B119's RenderBox uber_fragment states (a function table behind a shader
/// constant), Core Image's ci_ubershader kernels and CoreRE's vertex functions to tables the device answers nil for,
/// where a NO keeps them on their linked-function routes. 21H461 ships no shader that takes one, and its answer must be
/// NO as well. An M4 Max answers YES to all three. Fields: function_pointers, function_pointers_from_render,
/// function_pointers_from_mesh (-1 where the device has no such method, which is no YES).
struct FunctionPointerAnswersTest: HWTest {
    let id = "gpu.raw.function_pointer_answers"
    let name = "The raw device answers NO to function pointers, having no visible function tables"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (key, answer) in FunctionPointerAnswers.answers(gpu.device) {
            metrics.set(key, answer)
            checks.append(("\(key)=\(answer): YES with no function table support", answer != 1))
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
