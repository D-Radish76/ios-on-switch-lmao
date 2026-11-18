import Foundation
import HWTestCore
import HWTestGPU
import Metal
import OSLog

/// Blits of textures held through planes ('420v' 500, 'x420' 505, 'p420' 508, '2vuy' 501, 'b3a8' 550 and the one-plane
/// 504 and 524 over 'v410', 562 over '2vuy') complete on the raw path and move exactly the bytes their rule names, every
/// other byte of each surface (to its last page's end), buffer and plain texture as it was (PlanesBlitCases.swift): A a
/// buffer into plane 0, B plane 0 into a buffer (24x8 surfaces: the region (6,2) 12x3 runs past 16), C plane 0 into a
/// plain texture as one run in its texel, through row padding, plane 1 and the last page's tail (also at an exact
/// pitch, so no padding answers for it), D a 16x8 plain texture into plane 0 where its texel is plane 0's unit, E
/// planes into planes, F a colour fill, G a run from a surface of exactly one page, within it and past it.
///
/// A to D, E's plane 0 and G within the page are an M4 Max's bytes under macOS 27.2.1, validation off (probes
/// blitgroups.mm, bound.mm, units.mm). E's plane 1 is the raw path's rule (raw_blit_planes.h planeOneSpan), not Apple's:
/// an M4 Max moves plane 1 at plane 0's region in bytes and rows, past the plane's rows, a defect not copied. D of
/// another texel size (Apple lands it transposed) and G past the page (Apple reads zeros there) expect nothing written
/// and raw:blit.invalid; F (Apple writes plane 0 with a value that ignores the colour) nothing and raw:blit.undefined.
/// Each line is logged once a process, so the process's log must hold each exactly once afterwards (invalid_lines=,
/// undefined_lines=). cases= and cases_off=; for a case off its rule or the raw path only (a line of every case would
/// pass os_log's limit), <case>_changed= (bytes off the rule) and <case>_path=; m2v_cbs=.
struct PlanesBlitsTest: HWTest {
    let id = "gpu.raw.planes_blits"
    let name = "Blits of textures held through YCbCr and RGB10A8_2P_XR10 planes move only their rule's bytes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var off = 0
        let cases = PlanesBlitCases.all
        for (key, run) in cases {
            do {
                let result = try await run(gpu)
                if result.changed != 0 || result.path != "raw" {
                    off += 1
                    metrics.set("\(key)_changed", result.changed)
                    metrics.set("\(key)_path", String(result.path.prefix(60)))
                }
                checks.append(("\(key): on the raw path", result.path == "raw"))
                checks.append(("\(key): \(result.changed) bytes off its rule (\(result.text))", result.changed == 0))
            } catch {
                off += 1
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("cases", cases.count)
        metrics.set("cases_off", off)
        for (field, reason) in [("invalid_lines", "blit.invalid"), ("undefined_lines", "blit.undefined")] {
            let lines = Self.refusalLines(reason)
            metrics.set(field, lines ?? -1)
            checks.append(
                ("raw:\(reason) once in the process's log (\(lines.map(String.init) ?? "no log"))", lines == 1))
        }
        metrics.merge(PathTally.counterMetrics(before: before, after: RawFacade.counters(of: gpu.device)))
        checks.append(("every command buffer on the raw path", gpu.paths.offRaw.isEmpty))
        checks.append(("no m2v command buffer", metrics["m2v_cbs"] == .integer(0)))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The process's raw:<reason> lines (kits/SwitchRefusal: subsystem com.brunostjohn.switch.refusal, category raw,
    /// "<process>[<pid>] <reason>: <text>"); nil when its log cannot be read
    static func refusalLines(_ reason: String) -> Int? {
        let subsystem = "com.brunostjohn.switch.refusal", text = " \(reason): "
        guard let store = try? OSLogStore(scope: .currentProcessIdentifier),
            let entries = try? store.getEntries(
                matching: NSPredicate(
                    format: "subsystem == %@ AND category == %@ AND composedMessage CONTAINS %@", subsystem, "raw", text
                ))
        else { return nil }
        return entries.compactMap { $0 as? OSLogEntryLog }.filter {
            $0.subsystem == subsystem && $0.category == "raw" && $0.composedMessage.contains(text)
        }.count
    }
}
