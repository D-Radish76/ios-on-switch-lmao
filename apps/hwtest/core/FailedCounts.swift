import Foundation

/// The raw facade's failed command buffers between two reads of its counters (switchRawCounters, debug builds):
/// `failed` counts every command buffer that completed with an error, whoever made it (RenderBox, Core Image, MPS,
/// Core Animation or the test itself), and `failed.<predicate>` / `failed.none` name what gave each its error, the
/// predicate its path names too (error:<predicate>, error:none). Fields: failed_cbs= and failed_why=
/// ("call.sampler/1,none/2", most first).
public struct FailedCounts: Equatable, Sendable {
    /// Whether the later read answered `failed` at all: a facade whose counters lack it judges nothing.
    public let answered: Bool
    public let total: Int
    public let predicates: [String: Int]

    public init(before: [String: Int], after: [String: Int]) {
        answered = after["failed"] != nil
        total = (after["failed"] ?? 0) - (before["failed"] ?? 0)
        predicates = after.reduce(into: [:]) { out, entry in
            guard entry.key.hasPrefix("failed.") else { return }
            let change = entry.value - (before[entry.key] ?? 0)
            if change != 0 { out[String(entry.key.dropFirst("failed.".count))] = change }
        }
    }

    public var text: String { Self.text(predicates) }

    public var metrics: Metrics {
        var metrics = Metrics()
        metrics.set("failed_cbs", answered ? .integer(total) : .text("unanswered"))
        if !predicates.isEmpty { metrics.set("failed_why", text) }
        return metrics
    }

    /// The check that exactly `expected` failed between the reads (each predicate its count, "none" for an error no
    /// predicate gave) and nothing else, and, given the test's own command buffers off the raw path (PathTally.offRaw),
    /// that they are those failures, error:<predicate> each. It fails where the counters answer no `failed`.
    public func check(expecting expected: [String: Int] = [:], offRaw: PathTally? = nil) -> (String, Bool) {
        let want = expected.filter { $0.value != 0 }
        let label = want.isEmpty ? "no command buffer failed" : "exactly \(Self.text(want)) failed"
        guard answered else { return (label + ": the facade's counters answer no failed count", false) }
        var problems: [String] = []
        if total != want.values.reduce(0, +) || predicates != want { problems.append("failed \(text)") }
        if let offRaw, offRaw.paths != want.reduce(into: [:], { $0["error:" + $1.key] = $1.value }) {
            problems.append("off the raw path \(offRaw.isEmpty ? "-" : offRaw.text)")
        }
        return (problems.isEmpty ? label : "\(label), not \(problems.joined(separator: " and "))", problems.isEmpty)
    }

    /// The failure fields on `metrics` and their check (`check`) over the counters `before` and `after`.
    public static func judged(
        before: [String: Int], after: [String: Int], expecting expected: [String: Int] = [:], offRaw: PathTally? = nil,
        _ metrics: inout Metrics
    ) -> (String, Bool) {
        let counts = FailedCounts(before: before, after: after)
        metrics.merge(counts.metrics)
        return counts.check(expecting: expected, offRaw: offRaw)
    }

    static func text(_ counts: [String: Int]) -> String {
        guard !counts.isEmpty else { return "-" }
        return counts.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            .map { "\($0.key)/\($0.value)" }.joined(separator: ",")
    }
}
