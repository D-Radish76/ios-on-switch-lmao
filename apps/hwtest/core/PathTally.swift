import Foundation

/// Which GPU path drew a test's work, for its log line: each of its own command buffers' path as the raw facade
/// reports it ("raw", or "error:<predicate>" / "error:none" for one that failed), and the process's counters before and
/// after (they also count command buffers a framework made, such as Core Image's).
public struct PathTally: Equatable, Sendable {
    public private(set) var paths: [String: Int] = [:]

    public init() {}

    public mutating func add(_ path: String) { paths[path, default: 0] += 1 }

    public mutating func merge(_ other: PathTally) {
        for (path, count) in other.paths { paths[path, default: 0] += count }
    }

    public var isEmpty: Bool { paths.isEmpty }

    /// "raw/4,error:call.sampler/1": most frequent first.
    public var text: String {
        paths.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }
            .map { "\($0.key)/\($0.value)" }.joined(separator: ",")
    }

    /// The command buffers the facade did not complete on the raw path, each under its path, which names the failure
    /// ("device": a release runtime, which answers no path, is not counted).
    public var offRaw: PathTally {
        var off = PathTally()
        for (path, count) in paths where path != "raw" && path != "device" { off.paths[path] = count }
        return off
    }

    /// Counter deltas (the facade's switchRawCounters) as log fields: raw_cbs, and failed_cbs and failed_why
    /// (FailedCounts).
    public static func counterMetrics(before: [String: Int], after: [String: Int]) -> Metrics {
        var metrics = Metrics()
        metrics.set("raw_cbs", (after["raw"] ?? 0) - (before["raw"] ?? 0))
        metrics.merge(FailedCounts(before: before, after: after).metrics)
        return metrics
    }
}
