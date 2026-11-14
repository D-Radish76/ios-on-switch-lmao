import Foundation

/// HW Test cpu.dfll_landing's verdict on the kernel's line (drivers/SwitchPower/SwitchThermalDfllLanding.h): case i is
/// "i=<from MHz>><to MHz>", its readings "i.<ms>=<probe kHz>,<FREQ_REQ>,<TUNE1>,<OUTPUT_CFG>,<MONITOR_DATA>,<I2C_STS>".
/// Judged at 200 ms, once clk-dfll.c's Vmin calibration has run: every row at most 0.8 % under the rate asked and the
/// same from above and from below within 0.5 %, and over it by no more than the DFLL resolves where its loop stood.
/// A loop pinned at DFLL_OUTPUT_CFG MIN runs the DVCO floor skipped down: 0.25 %, so a landing a skipper step fast
/// (boot 21: 0.48 % and 0.56 % over) fails. A loop above MIN regulates the DVCO, and T210B01's runs without its
/// integral term (cf 6, ci 0, cg 2: TegraDFLL/platform.c; Tegra X1 TRM 6.1.3 CI_PARAM clears a cycle surplus only when
/// set), so the surplus it leaves is bounded by what the DFLL measures: one DFLL_MONITOR_DATA unit of REF_CLK / 4
/// (clk-dfll.c REF_CLK_CYC_PER_DVCO_SAMPLE) at the skipper's (SCALE + 1) / 256, and its monitored count within that
/// unit of the MULT x REF_CLK / 2 asked. Boot 22 held 1581 MHz (MULT 62, SCALE 255) at output 14, MIN 0, the monitor
/// one unit over: 0.36 % over by the probe, inside 0.81 %.
public enum DfllLanding {
    public static let settledMs = 200

    /// One reading's probe and registers (Tegra X1 TRM 6.1.6 DFLL_FREQ_REQ, 6.1.9 DFLL_OUTPUT_CFG; clk-dfll.c's
    /// DFLL_MONITOR_DATA and DFLL_I2C_STS fields).
    struct Reading {
        static let monitorUnitKhz = 51000.0 / 4

        let khz: Double
        let mult, scale, outMin, monitor, lastOutput: UInt32

        init?(_ value: String) {
            let parts = value.split(separator: ",")
            guard parts.count == 6, let khz = Double(parts[0]) else { return nil }
            let registers = parts.dropFirst().compactMap { UInt32($0, radix: 16) }
            guard registers.count == 5 else { return nil }
            self.khz = khz
            mult = registers[0] & 0x7f
            scale = (registers[0] >> 8) & 0xff
            outMin = (registers[2] >> 8) & 0x3f
            monitor = registers[3] & 0xffff
            lastOutput = (registers[4] >> 1) & 0x3f
        }

        var pinned: Bool { lastOutput <= outMin }

        func overBoundPct(asked mhz: Int) -> Double {
            pinned ? 0.25 : Self.monitorUnitKhz * Double(scale + 1) / 256 / Double(mhz * 1000) * 100
        }

        var monitorHoldsTarget: Bool { abs(Int(monitor) - 2 * Int(mult)) <= 1 }
    }

    public static func judge(_ fields: [String: String]) -> (checks: [(String, Bool)], metrics: Metrics) {
        var metrics = Metrics()
        metrics.set("dfll", fields["dfll"] ?? "-")
        if let reason = fields["reason"] { metrics.set("reason", reason) }
        if let restore = fields["restore"] { metrics.set("restore", restore) }
        var landed: [Int: [Landed]] = [:]
        var errors: [String] = []
        var index = 0
        while let rates = fields["\(index)"] {
            let pair = rates.split(separator: ">").compactMap { Int($0) }
            if let error = fields["\(index).err"] { errors.append("\(rates): \(error)") }
            metrics.set("c\(index)", rates.replacingOccurrences(of: ">", with: "_to_"))
            for (key, value) in fields.sorted(by: { $0.key < $1.key }) where key.hasPrefix("\(index).") {
                let ms = key.dropFirst("\(index).".count)
                guard ms.allSatisfy(\.isNumber) else { continue }
                metrics.set("c\(index)_\(ms)", value)
                if ms != "\(settledMs)" { metrics.keepInReportOnly("c\(index)_\(ms)") }
            }
            if pair.count == 2, let settled = fields["\(index).\(settledMs)"],
                let khz = Double(settled.split(separator: ",").first ?? "")
            {
                landed[pair[1], default: []].append((khz, Reading(settled)))
            }
            index += 1
        }
        var checks: [(String, Bool)] = [
            ("the probe ran on the DFLL (dfll=ran)", fields["dfll"] == "ran"),
            ("every rate was taken (\(errors.joined(separator: "; ")))", errors.isEmpty),
            ("every case was measured", index > 0 && landed.values.map(\.count).reduce(0, +) == index),
        ]
        for (asked, readings) in landed.sorted(by: { $0.key < $1.key }) {
            checks += judge(asked, readings, &metrics)
        }
        return (checks, metrics)
    }

    typealias Landed = (khz: Double, reading: Reading?)

    static func judge(_ asked: Int, _ readings: [Landed], _ metrics: inout Metrics) -> [(String, Bool)] {
        let target = Double(asked)
        let mhz = readings.map { $0.khz / 1000 }
        let overs = mhz.map { ($0 - target) / target * 100 }
        let under = (target - (mhz.min() ?? 0)) / target * 100
        let spread = ((mhz.max() ?? 0) - (mhz.min() ?? 0)) / target * 100
        metrics.set("over_pct_\(asked)", overs.max() ?? 0, digits: 2)
        metrics.set("under_pct_\(asked)", under, digits: 2)
        metrics.set("spread_pct_\(asked)", spread, digits: 2)
        let bounds = readings.map { $0.reading?.overBoundPct(asked: asked) ?? 0.25 }
        let worst = overs.indices.max { overs[$0] - bounds[$0] < overs[$1] - bounds[$1] } ?? 0
        let pinned = readings[worst].reading?.pinned ?? true
        let how = pinned ? "pinned at OUTPUT_CFG MIN" : "one DFLL monitor unit, the loop above MIN"
        let bound = String(format: "%.2f", bounds[worst])
        var checks = [
            (
                "\(asked) MHz at most \(bound)% over the rate asked at \(settledMs) ms (\(how))",
                zip(overs, bounds).allSatisfy { $0 <= $1 }
            ),
            ("\(asked) MHz at most 0.8% under the rate asked at \(settledMs) ms", under <= 0.8),
            ("\(asked) MHz the same from above and below (within 0.5%)", spread <= 0.5),
        ]
        for reading in readings.compactMap(\.reading) where !reading.pinned && !reading.monitorHoldsTarget {
            checks.append(
                (
                    "\(asked) MHz: the DVCO within one monitor unit of MULT x 2 = \(2 * reading.mult) by the DFLL's "
                        + "monitor (read \(reading.monitor))", false
                ))
        }
        return checks
    }
}
