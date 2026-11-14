import Foundation

/// HW Test ui.launch_hitch's judgement of one launch and the return to the test app, read as one SpanReading: it fails
/// on a compositor job over the bound and names the generic merge states' spans (GenericStates) by key, name and time.
public struct LaunchHitchVerdict: Sendable {
    public let reading: SpanReading
    public let generic: [LongSpan]
    public let jobBoundUs: Int
    private let states: GenericStates

    public init(reading: SpanReading, states: GenericStates, jobBoundUs: Int) {
        self.reading = reading
        self.generic = states.spans(in: reading)
        self.jobBoundUs = jobBoundUs
        self.states = states
    }

    public var firstGenericMs: Double { Double(generic.first?.us ?? 0) / 1000 }
    public var genericTotalMs: Double { Double(generic.reduce(0) { $0 + $1.us }) / 1000 }

    /// "k<key> <name> <us> us" for each generic span, in order
    public var genericText: String {
        generic.map { "k\(Self.hex($0.key)) \(states.names[$0.key] ?? "?") \($0.us) us" }.joined(separator: ", ")
    }

    static func hex(_ key: UInt64) -> String {
        let digits = String(key, radix: 16)
        return String(repeating: "0", count: max(0, 16 - digits.count)) + digits
    }

    /// Nil where every compositor job of the window kept within the bound
    public var failure: String? {
        guard reading.jobMaxUs > jobBoundUs else { return nil }
        let named = reading.spans.isEmpty ? "no span of 5 ms named" : reading.spans.joined(separator: " | ")
        let generics = generic.isEmpty ? "" : "; generic states: \(genericText)"
        return "the compositor's longest job took \(reading.jobMaxUs) us\(generics): \(named)"
    }

    public var metrics: Metrics {
        var metrics = Metrics()
        metrics.set("job_max_us", reading.jobMaxUs)
        metrics.set("spans", reading.spans.count)
        metrics.set("generic_spans", generic.count)
        metrics.set("first_generic_ms", firstGenericMs)
        metrics.set("generic_total_ms", genericTotalMs)
        if !generic.isEmpty { metrics.set("generic", genericText) }
        for key in ["sync_subs", "sync_us", "behind", "behind_sync_us", "sync_max_us"] {
            metrics.set(key, reading.counts[key] ?? 0)
        }
        // os_log keeps 1,024 characters of the result line: the generic spans are named above, later spans in the report
        for (i, span) in reading.spans.prefix(6).enumerated() {
            metrics.set("span\(i)", span)
            if i >= 3 { metrics.keepInReportOnly("span\(i)") }
        }
        return metrics
    }
}
