import Foundation
import HWTestCore
import HWTestGPU
import Metal
import OSLog

/// Pipeline reflection per binding kind (drivers/TegraGM20BMetal/raw/runtime/RawMetalReflectionObjects.mm), two arms
/// judged apart. kinds: reflection_kinds's compute state and reflection_vs/reflection_fs's render state (BGRA8Unorm),
/// made with binding and buffer type info, and on the versions whose QuartzCore has them its compute_average_luma and
/// compute_sum_luma (qc_kernels): every binding answers type, access, index, arrayLength, isUsed, isActive, isArgument
/// and its own kind's selectors as an M4 Max's Metal does (ReflectionKindsGoldens; mismatches, of them
/// array_length_mismatches, first_mismatch), responds to MTLBindingInternal's 19 selectors and not to dataType, offset,
/// elementType or members (responds_mismatches); a state not made is named apart (unmade); the pointer and struct types
/// every binding has a method for are reported (kinds_reported_off). wrong_kind: the first binding of each kind of each
/// state asked the other kinds' selectors, where Metal fails an assertion and aborts the process: each answers
/// Self.wrongKind's answer and logs its error lines ("raw: <process>: <selector> asked of binding <name> of type <n>,
/// where Metal asserts"), counted in this process's log since the test began (wrong_kind_asks, wrong_kind_off,
/// wrong_kind_lines, wrong_kind_lines_other).
struct ReflectionKindsTest: HWTest {
    let id = "gpu.raw.reflection_kinds"
    let name = "Every binding kind's reflection answers as Metal's"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    /// What a binding answers another kind's selector, and the error lines each such ask logs
    static let wrongKind = (answer: 0, linesPerAsk: 1)
    /// The QuartzCore compute kernels each iOS version's library has and this test reflects
    static let systemKernels: [Int: [String]] = [27: ["compute_average_luma", "compute_sum_luma"]]
    static let assertSuffix = ", where Metal asserts"

    struct Ask {
        let key: String, line: String
        let answer: Int
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let start = Date()
        let system = Self.systemKernels[context.ios] ?? []
        let made = ReflectionKinds.made(gpu.device, gpu.library, system: system) {
            try gpu.device.makeLibrary(URL: CAState.library)
        }
        let asks = Self.wrongKinds(made.seen)
        let compared = ReflectionKinds.compare(made.seen, ReflectionKindsGoldens.metal, functions: made.functions)
        var metrics = Metrics()
        metrics.set("unmade", made.unmade.isEmpty ? "-" : made.unmade.joined(separator: ","))
        metrics.set(
            "qc_kernels",
            system.isEmpty ? "absent" : system.allSatisfy(made.functions.contains) ? "compared" : "unmade")
        metrics.set("bindings", compared.bindings)
        metrics.set("mismatches", compared.mismatches)
        metrics.set("array_length_mismatches", compared.arrayLengthMismatches)
        metrics.set("responds_mismatches", compared.respondsMismatches)
        metrics.set("first_mismatch", compared.firstMismatch)
        metrics.set("kinds_reported_off", compared.reportedOff)
        var checks: [(String, Bool)] = [
            ("kinds: a state not made (\(made.unmade.joined(separator: ",")))", made.unmade.isEmpty),
            (
                "kinds: \(compared.mismatches) answers off Metal's over \(compared.bindings) bindings, first "
                    + compared.firstMismatch, compared.mismatches == 0 && compared.bindings > 0
            ),
            ("kinds: \(compared.respondsMismatches) respondsToSelector answers off", compared.respondsMismatches == 0),
        ]

        let off = asks.filter { $0.answer != Self.wrongKind.answer }
        let lines = Self.assertLines(since: start)
        var left = lines ?? []
        var matched = 0
        for ask in asks {
            guard let at = left.firstIndex(where: { $0.hasPrefix("raw: ") && $0.hasSuffix(": " + ask.line) }) else {
                continue
            }
            left.remove(at: at)
            matched += 1
        }
        let wantLines = asks.count * Self.wrongKind.linesPerAsk
        metrics.set("wrong_kind_asks", asks.count)
        metrics.set("wrong_kind_off", off.count)
        metrics.set("wrong_kind_first_off", off.first.map { "\($0.key):\($0.answer)" } ?? "-")
        metrics.set("wrong_kind_lines", lines == nil ? -1 : matched)
        metrics.set("wrong_kind_lines_other", left.count)
        checks += [
            (
                "wrong_kind: \(off.count) of \(asks.count) asks answered other than \(Self.wrongKind.answer)",
                off.isEmpty && !asks.isEmpty
            ),
            (
                lines == nil
                    ? "wrong_kind: this process's log is unreadable"
                    : "wrong_kind: \(matched) of \(wantLines) lines, \(left.count) others",
                lines != nil && matched == wantLines && left.isEmpty
            ),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The first binding of each kind of each state, asked every other kind's selector
    static func wrongKinds(_ seen: [ReflectionKinds.Seen]) -> [Ask] {
        var asked: Set<String> = []
        var asks: [Ask] = []
        let guarded = ReflectionKinds.own.keys.sorted().flatMap { ReflectionKinds.own[$0]! }
        for binding in seen {
            guard asked.insert("\(binding.key.prefix { $0 != "." })/\(binding.type)").inserted else { continue }
            let own = ReflectionKinds.own[binding.type] ?? []
            for selector in guarded where !own.contains(selector) {
                asks.append(
                    Ask(
                        key: "\(binding.key).\(selector)",
                        line: "\(selector) asked of binding \(binding.name) of type \(binding.type)\(assertSuffix)",
                        answer: ReflectionKinds.answer(binding.object, selector)))
            }
        }
        return asks
    }

    /// This process's facade lines since `start` that end as a wrong-kind ask's does; nil when its log cannot be read
    static func assertLines(since start: Date) -> [String]? {
        let subsystem = "com.brunostjohn.switch.TegraGM20BMetal"
        guard let store = try? OSLogStore(scope: .currentProcessIdentifier),
            let entries = try? store.getEntries(
                at: store.position(date: start),
                matching: NSPredicate(
                    format: "subsystem == %@ AND category == %@ AND composedMessage CONTAINS %@", subsystem, "raw",
                    assertSuffix))
        else { return nil }
        return entries.compactMap { $0 as? OSLogEntryLog }.filter {
            $0.date >= start && $0.subsystem == subsystem && $0.category == "raw"
                && $0.composedMessage.hasSuffix(assertSuffix)
        }.map(\.composedMessage)
    }
}
