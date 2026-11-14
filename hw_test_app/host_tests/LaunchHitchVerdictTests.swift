import HWTestCore
import XCTest

final class LaunchHitchVerdictTests: XCTestCase {
    /// ca_generic_keys.txt's lines for 24B119's three generic states boot 22 drew
    static let states27 = GenericStates.parse(
        "b0f4f3daabb01e2e PBGRAXmw_A3Xghfc\n64ed02fd5d7a1865 PBGRAXm_A2Xghfc\n12016b9f2603fab8 PRGhAXm_A2Xghfc\n")

    /// Boot 22's two generic spans on ui.launch_hitch's return (b22.txt 13:47:09.113 and .163, each in a 50.5 ms
    /// command buffer) as the kernel's notes hold them: the state has no name on its label, so the span says "draws"
    static let boot22 =
        "job_max_us=50535 sync_subs=1 syncs=1 sync_us=0 behind=0 behind_sync_us=0 sync_max_us=0 spans=2; "
        + "backboardd: 41339 us draws kb0f4f3daabb01e2e 63696 instr x2 921k variant 1559kpx e5211 c0 80/720x1280/bl; "
        + "backboardd: 41270 us draws kb0f4f3daabb01e2e 63696 instr x2 921k variant 1559kpx e5203 c0 80/720x1280/bl"

    func testBoot22sGenericSpansOnTheReturnAreCountedByKeyAndFail() {
        let verdict = LaunchHitchVerdict(
            reading: SpanReading.parse(Self.boot22), states: Self.states27, jobBoundUs: 50_000)
        XCTAssertEqual(verdict.generic.count, 2, "a span is a generic state's by its key, whatever its name says")
        XCTAssertEqual(verdict.generic.map(\.key), [0xb0f4_f3da_abb0_1e2e, 0xb0f4_f3da_abb0_1e2e])
        XCTAssertEqual(verdict.firstGenericMs, 41.339, accuracy: 1e-9)
        XCTAssertEqual(verdict.genericTotalMs, 82.609, accuracy: 1e-9)
        XCTAssertEqual(
            verdict.genericText,
            "kb0f4f3daabb01e2e PBGRAXmw_A3Xghfc 41339 us, kb0f4f3daabb01e2e PBGRAXmw_A3Xghfc 41270 us")
        let metrics = verdict.metrics
        XCTAssertEqual(metrics["generic_spans"], .integer(2))
        XCTAssertEqual(metrics["first_generic_ms"], .number(41.339, digits: 2))
        XCTAssertEqual(metrics["generic_total_ms"], .number(82.609, digits: 2))
        XCTAssertEqual(metrics["job_max_us"], .integer(50_535))
        let failure = verdict.failure ?? ""
        XCTAssertTrue(failure.hasPrefix("the compositor's longest job took 50535 us; generic states: kb0f4"), failure)
    }

    func testTheKeyIsTheThirdFieldOfASpanFormattedNow() {
        let span = LongSpan.parse("backboardd: 41339 us kb0f4f3daabb01e2e draws 63696 instr x2 921k")
        XCTAssertEqual(span, LongSpan(owner: "backboardd", us: 41_339, key: 0xb0f4_f3da_abb0_1e2e))
        XCTAssertEqual(
            LongSpan.parse("backboardd: 2000 us k0000000000000000 pass prologue 0 instr x1 3686km"),
            LongSpan(owner: "backboardd", us: 2000, key: 0))
        let named = "backboardd: 5574 us PBGRAXm_TimgA2Xhf_IxrgN3Oc3mtc4nlnlnl k4c33d27bde9b894a 624 instr x2 925k"
        XCTAssertEqual(
            LongSpan.parse(named), LongSpan(owner: "backboardd", us: 5574, key: 0x4c33_d27b_de9b_894a),
            "a line from before the key moved")
        XCTAssertNil(LongSpan.parse("backboardd: no span"))
    }

    func testAnotherBuildsTableCountsNoneOfThem() {
        let states17 = GenericStates.parse("c0718fe9aaad0d5f PBGRAXmw_A3Xghfc\n")
        let verdict = LaunchHitchVerdict(reading: SpanReading.parse(Self.boot22), states: states17, jobBoundUs: 50_000)
        XCTAssertEqual(verdict.generic, [], "iOS 17's PBGRAXmw_A3Xghfc is another state, under another key")
    }

    func testAWindowWithinTheBoundPassesAndStillNamesItsGenericSpans() {
        let line =
            "job_max_us=31000 sync_subs=0 spans=1; "
            + "backboardd: 30500 us k64ed02fd5d7a1865 PBGRAXm_A2Xghfc 64208 instr x1 937k"
        let verdict = LaunchHitchVerdict(reading: SpanReading.parse(line), states: Self.states27, jobBoundUs: 50_000)
        XCTAssertNil(verdict.failure)
        XCTAssertEqual(verdict.metrics["generic"], .text("k64ed02fd5d7a1865 PBGRAXm_A2Xghfc 30500 us"))
    }

    /// The kernel keeps the notes one line has no room for: a launch's notes over two asks read as one window
    func testTheAsksOfOneDrainReadAsOneWindow() {
        let first = SpanReading.parse(
            "job_max_us=12000 sync_subs=2 sync_us=10 sync_max_us=7 spans=3; backboardd: 6000 us k1 a 1 instr x1 1k; "
                + "backboardd: 7000 us kb0f4f3daabb01e2e draws 63696 instr x2 921k")
        XCTAssertFalse(first.complete)
        let second = SpanReading.parse(
            "job_max_us=50535 sync_subs=1 sync_us=4 sync_max_us=4 spans=1; "
                + "backboardd: 41339 us draws kb0f4f3daabb01e2e 1 instr x2 921k")
        XCTAssertTrue(second.complete)
        let merged = SpanReading.merged([first, second])
        XCTAssertEqual(merged.jobMaxUs, 50_535)
        XCTAssertEqual(merged.counts["sync_subs"], 3)
        XCTAssertEqual(merged.counts["sync_us"], 14)
        XCTAssertEqual(merged.counts["sync_max_us"], 7)
        XCTAssertEqual(merged.spans.count, 3)
        XCTAssertTrue(merged.complete)
        let verdict = LaunchHitchVerdict(reading: merged, states: Self.states27, jobBoundUs: 50_000)
        XCTAssertEqual(verdict.generic.map(\.us), [7000, 41_339])
        XCTAssertNotNil(verdict.failure)
    }
}
