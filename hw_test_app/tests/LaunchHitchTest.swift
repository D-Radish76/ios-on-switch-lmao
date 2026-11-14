import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// An app launch's compositor cost, there and back: Settings is opened (LSApplicationWorkspace, with FrontBoard's
/// launch entitlement), then this app again, and the debug kernel answers the compositor's longest GPU job and every
/// span of 5 ms or more of one state the render server's raw path noted (TegraGM20BPathsABI.h spans op;
/// raw_span_marks.h), each "<us> us k<key> <state> <instructions> instr x<marks> <target>k[m]". The return counts: boot
/// 22's generic spans came after this app was opened again. A job longer than three frames fails; the spans of Core
/// Animation's generic merge states are named by key from the build's own table (ca_generic_keys.txt), never by a
/// state's name, which a span may lack.
struct LaunchHitchTest: HWTest {
    let id = "ui.launch_hitch"
    let name = "An app launch and the return keep the compositor's GPU jobs within three frames"
    let subsystem = Subsystem.compositor

    static let spansOp: UInt32 = 3
    static let launched = "com.apple.Preferences"
    static let settleSeconds: UInt64 = 4
    static let returnSettleSeconds: UInt64 = 2
    static let jobBoundUs = 50_000
    /// SwitchGpuSpanNotes.h kNotes: a line per ask at least, so asking once more than it keeps empties it
    static let keptNotes = 16

    func run(_ context: TestContext) async throws -> Outcome {
        guard let connection = GPUConnection() else { return .skip("no TegraGM20B connection") }
        guard Self.drain(connection) != nil else {
            return .skip("the kernel keeps no compositor spans (a release kernel)")
        }
        guard let url = Bundle.main.url(forResource: "ca_generic_keys", withExtension: "txt"),
            let table = try? String(contentsOf: url, encoding: .utf8)
        else { return .fail("ca_generic_keys.txt is not in the app's bundle") }
        let states = GenericStates.parse(table)
        let task = await MainActor.run { UIApplication.shared.beginBackgroundTask(withName: id) }
        defer { Task { @MainActor in UIApplication.shared.endBackgroundTask(task) } }
        guard Self.open(Self.launched) else { return .skip("LSApplicationWorkspace refused to open \(Self.launched)") }
        try await Task.sleep(nanoseconds: Self.settleSeconds * NSEC_PER_SEC)
        let launch = Self.drain(connection)
        let returned = Self.open(Bundle.main.bundleIdentifier ?? "")
        for _ in 0..<50 where await MainActor.run(body: { UIApplication.shared.applicationState != .active }) {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        try await Task.sleep(nanoseconds: Self.returnSettleSeconds * NSEC_PER_SEC)
        let back = Self.drain(connection)
        let verdict = LaunchHitchVerdict(
            reading: SpanReading.merged([launch, back].compactMap { $0 }), states: states, jobBoundUs: Self.jobBoundUs)
        var metrics = verdict.metrics
        metrics.set("returned", returned)
        metrics.set("generic_states", states.names.count)
        if let failure = verdict.failure { return .fail(failure, metrics) }
        return .pass(metrics)
    }

    /// One spans line; nil off a debug kernel
    static func spans(_ connection: GPUConnection) -> String? {
        let (kr, reply) = connection.call(GPUPaths.method, GPUConnection.words([spansOp, 0]), outputSize: 512)
        guard kr == KERN_SUCCESS else { return nil }
        return String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self)
    }

    /// Every note waiting and the totals since the last ask, as one reading: the kernel keeps the notes a line has no
    /// room for, so it is asked until a line holds all it had; nil off a debug kernel
    static func drain(_ connection: GPUConnection) -> SpanReading? {
        var readings: [SpanReading] = []
        for _ in 0...keptNotes {
            guard let line = spans(connection) else { break }
            let reading = SpanReading.parse(line)
            readings.append(reading)
            if reading.complete { break }
        }
        return readings.isEmpty ? nil : SpanReading.merged(readings)
    }

    /// -[LSApplicationWorkspace openApplicationWithBundleID:], which answers a BOOL
    static func open(_ bundle: String) -> Bool {
        let selector = NSSelectorFromString("openApplicationWithBundleID:")
        guard let type = NSClassFromString("LSApplicationWorkspace") as? NSObject.Type,
            let workspace = type.perform(NSSelectorFromString("defaultWorkspace"))?.takeUnretainedValue() as? NSObject,
            workspace.responds(to: selector)
        else { return false }
        typealias Open = @convention(c) (NSObject, Selector, NSString) -> Bool
        let call = unsafeBitCast(workspace.method(for: selector), to: Open.self)
        return call(workspace, selector, bundle as NSString)
    }
}
