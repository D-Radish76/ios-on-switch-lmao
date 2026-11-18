import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// generateMipmapsForTexture: of extended-range, sRGB, 16-bit packed and shared-exponent words on the raw path, every
/// level exactly an M4 Max's levels of the same chain (WordMipmapCases.swift, WordMipGoldens.swift): 16x16 2D chains of
/// 552 to 555, 101x63 2D and 45x37x7 3D chains of the rest, where the taps weight texels and planes off 1/2 (the mip
/// rule package: 0 texels differ from Apple's on every format). Per format <format>_max_diff= the largest difference in
/// codes from Apple's (RGB9E5: mantissa steps), <format>_off= texels that differ, <format>_path=; m2v_cbs=.
struct WordMipmapsTest: HWTest {
    let id = "gpu.raw.word_mipmaps"
    let name = "Mipmaps of extended-range, sRGB, packed and shared-exponent words as an M4 Max makes them, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for format in WordMipFormat.all {
            guard let text = WordMipmapCases.apple[format.key],
                let apple = Data(base64Encoded: text, options: .ignoreUnknownCharacters)
            else {
                checks.append(("\(format.key): an M4 Max's chain", false))
                continue
            }
            do {
                let (levels, path) = try await WordMipmapCases.run(format, gpu)
                let (worst, off) = WordMipmapCases.judged(format, levels, apple: [UInt8](apple))
                metrics.set("\(format.key)_max_diff", worst)
                metrics.set("\(format.key)_off", off)
                metrics.set("\(format.key)_path", String(path.prefix(80)))
                checks.append(("\(format.key): its mipmaps on the raw path", path == "raw"))
                checks.append(("\(format.key): every texel Apple's", worst == 0))
            } catch {
                checks.append(("\(format.key): \(GPU.describe(error))", false))
            }
        }
        metrics.merge(PathTally.counterMetrics(before: before, after: RawFacade.counters(of: gpu.device)))
        checks.append(("no m2v command buffer", metrics["m2v_cbs"] == .integer(0)))
        metrics.fitLog(within: 720, keeping: ["m2v_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
