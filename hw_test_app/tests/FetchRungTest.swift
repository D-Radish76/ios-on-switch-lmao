import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Fetch rungs 2 and 3 (drivers/TegraGM20BMetal/raw/runtime/raw_fetch_free.h) on the chip: Core Animation states that
/// blend by fetching their colour (FetchRungStates), each drawn first as the rules draw it (the probe channel at 0), then
/// by its storage form (rung 4: the fetch and the blend inside the pixel interlock) and by its fetch-free form (rung 2:
/// the ROP's 8-bit blender with the form's factors; rung 3: the iterated blend unit running the form's program, the
/// colour targets narrowed to the blended one), both forced through the facade's probe channel; the colour matrix (vcm)
/// takes its program and constants from the GPU's macro each draw. Per state, by its key:
///   unforced_<key>  the draws the facade took on the rung with the probe at 0 (2 where the rules take the form)
///   worst_<key>     the largest difference in codes between the two forced routes' first attachments (rule
///                   fetch.blend allows 2; rung 3's programs are exact in QuartzCore's arithmetic, so 2 too)
///   us_rung4_<key>, us_rung<N>_<key>  one full-screen draw's GPU time each way; us_pass_<key> per pass of the program
///   error_<key>     a draw the GPU refused (the channel reopens and the next state runs)
///   noform_<key>    the forced draw took no fetch-free form: left out where the pack holds none on this IPSW (21H461's
///                   pdi), else formless= and FAIL
/// proven= the programs every state of which was within the bound and faster; on= the programs the rules drew on the
/// rung (raw_chip_facts.h kIteratedProven for rung 3). r<N>= the draws the facade's counter says took the fetch-free
/// form. Core Animation's clear (clr, clr_m1 with its memoryless image) loads no colour, so it draws plain:
/// fetched_<key> the fetching draws the facade counted over its draws (0), path_<key> its path.
/// verdict=PASS: on equals proven, no draw refused, every forced draw counted and every plain state drawn raw with no
/// fetch; on_unproven= and proven_off= name the programs the rules take wrongly either way. A state this IPSW's
/// QuartzCore lacks is skipped. Rung 3 programs a unit no other driver here drives: a wrong setup kills the channel
/// (class errors 0x59, 0x5c, 0x5d, INVALID_VALUE at 0x0df4 in the kernel log), so it runs last.
struct FetchRungTest: HWTest {
    let id: String
    let rung: Int
    var name: String { "Fetch rung \(rung) against the storage form: codes and GPU time, Core Animation's states" }
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 600 }

    static let bound = 2
    static let draws = 10
    static let runs = 5
    static let fetchCounters = ["fetch_sequential", "fetch_interlocked", "fetch_rung2", "fetch_rung3"]

    struct Measured {
        var unforced = 0
        /// the forced draw took no fetch-free form: the pack holds none of this state on this IPSW
        var formless = false
        var worst = 0
        var differing = 0
        var paths = ""
        var us: [Double] = []
    }

    /// The state as the rules draw it, then forced each way: nil where the facade has no probe channel
    func measure(_ line: PipelineLine, library: MTLLibrary, gpu: GPU) async throws -> Measured? {
        var m = Measured()
        let counter = "fetch_rung\(rung)"
        let before = RawFacade.counters(of: gpu.device)[counter] ?? 0
        _ = try await FetchRungScene.bytes(line, library: library, gpu: gpu)
        m.unforced = (RawFacade.counters(of: gpu.device)[counter] ?? 0) - before
        let draw = { try await FetchRungScene.bytes(line, library: library, gpu: gpu) }
        guard let storage = try await FetchRungScene.forcing(4, gpu.device, draw) else { return nil }
        let forcedFrom = RawFacade.counters(of: gpu.device)[counter] ?? 0
        guard let free = try await FetchRungScene.forcing(rung, gpu.device, draw) else { return nil }
        m.formless = (RawFacade.counters(of: gpu.device)[counter] ?? 0) == forcedFrom
        if m.formless { return m }
        (m.worst, m.differing) = FetchRungScene.difference(storage.1, free.1)
        m.paths = "\(storage.0)/\(free.0)"
        for forced in [4, rung] {
            let timed = try await FetchRungScene.forcing(forced, gpu.device) {
                try await FetchRungScene.microseconds(
                    line, library: library, gpu: gpu, draws: Self.draws, runs: Self.runs)
            }
            m.us.append(timed?.0 ?? 0)
        }
        return m
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else { return .skip("the default device is not the raw facade") }
        guard let statesURL = Bundle.main.url(forResource: "ca_states", withExtension: "log"),
            let text = try? String(contentsOf: statesURL, encoding: .utf8)
        else { return .fail("ca_states.log is not in the app's bundle") }
        let states = Dictionary(uniqueKeysWithValues: (FetchRungStates.fetching[rung] ?? []).map { ($0.label, $0) })
        let plainKeys = Dictionary(uniqueKeysWithValues: (FetchRungStates.plain[rung] ?? []).map { ($0.label, $0.key) })
        let all = CAStateSweep.states(text, everyState: true)
        let lines = all.filter { states[$0.shortLabel] != nil }
        let plainLines = all.filter { plainKeys[$0.shortLabel] != nil }
        if lines.isEmpty && plainLines.isEmpty { return .skip("this QuartzCore makes none of the rung's states") }
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        var metrics = Metrics()
        var beyond: [String] = []
        var slower: [String] = []
        var refused: [String] = []
        var formless: [String] = []
        var outcomes: [(program: String, ok: Bool)] = []
        var on = Set<String>()
        let key = "fetch_rung\(rung)"
        let before = RawFacade.counters(of: gpu.device)[key] ?? 0
        var forcedDraws = 0
        for line in lines {
            let state = states[line.shortLabel]!
            let program = FetchRungStates.program(state)
            let measured: Measured?
            do {
                measured = try await measure(line, library: library, gpu: gpu)
            } catch {
                metrics.set("error_\(state.key)", String(GPU.describe(error).prefix(60)))
                refused.append(state.key)
                outcomes.append((program, false))
                continue
            }
            guard let m = measured else { return .skip("the facade has no probe channel (a release build)") }
            metrics.set("unforced_\(state.key)", m.unforced)
            if m.formless {
                metrics.set("noform_\(state.key)", 1)
                if !FetchRungStates.formless(state) { formless.append(state.key) }
                continue
            }
            forcedDraws += 2 + Self.draws * (Self.runs + 1)
            metrics.set("worst_\(state.key)", m.worst)
            metrics.set("differing_\(state.key)", m.differing)
            metrics.set("path_\(state.key)", m.paths)
            metrics.set("us_rung4_\(state.key)", m.us[0], digits: 1)
            metrics.set("us_rung\(rung)_\(state.key)", m.us[1], digits: 1)
            if state.passes > 0 { metrics.set("us_pass_\(state.key)", m.us[1] / Double(state.passes), digits: 2) }
            let within = m.worst <= Self.bound && m.paths == "raw/raw"
            let faster = m.us[1] > 0 && m.us[1] < m.us[0]
            if !within { beyond.append(state.key) }
            if !faster { slower.append(state.key) }
            if m.unforced > 0 { on.insert(program) }
            outcomes.append((program, within && faster))
        }
        var fetching: [String] = []
        let fetchedDraws = {
            let counters = RawFacade.counters(of: gpu.device)
            return Self.fetchCounters.reduce(0) { $0 + (counters[$1] ?? 0) }
        }
        for line in plainLines {
            let label = plainKeys[line.shortLabel]!
            let start = fetchedDraws()
            let (path, _) = try await FetchRungScene.bytes(line, library: library, gpu: gpu)
            let fetched = fetchedDraws() - start
            metrics.set("fetched_\(label)", fetched)
            metrics.set("path_\(label)", path)
            if fetched != 0 || path != "raw" { fetching.append(label) }
        }
        let taken = (RawFacade.counters(of: gpu.device)[key] ?? 0) - before
        metrics.set("r\(rung)", taken)
        metrics.set("forced_draws", forcedDraws)
        metrics.set("states", lines.count + plainLines.count)
        let failing = Set(outcomes.filter { !$0.ok }.map(\.program))
        let proven = Set(outcomes.map(\.program)).subtracting(failing)
        let onUnproven = on.subtracting(proven).sorted()
        let provenOff = proven.subtracting(on).sorted()
        let named = { (names: [String]) in names.isEmpty ? "-" : names.joined(separator: ",") }
        metrics.set("proven", named(proven.sorted()))
        metrics.set("on", named(on.sorted()))
        let counted = taken >= forcedDraws
        let verdict =
            refused.isEmpty && formless.isEmpty && onUnproven.isEmpty && provenOff.isEmpty && counted
                && fetching.isEmpty ? "PASS" : "FAIL"
        metrics.set("verdict", verdict)
        for (field, names) in [
            ("refused", refused), ("formless", formless), ("on_unproven", onUnproven), ("proven_off", provenOff),
            ("beyond", beyond), ("slower", slower), ("fetching", fetching),
        ] where !names.isEmpty {
            metrics.set(field, named(names))
        }
        if verdict == "PASS" { return .pass(metrics, proof: gpu.proof) }
        let why =
            !refused.isEmpty
            ? "the GPU refused a draw"
            : !formless.isEmpty
                ? "a forced draw took no fetch-free form where the pack should hold one"
                : !onUnproven.isEmpty
                    ? "the rules draw a program on rung \(rung) that is beyond the bound or slower"
                    : !provenOff.isEmpty
                        ? "a program within the bound and faster is not drawn on rung \(rung)"
                        : !counted
                            ? "the facade drew \(taken) of the forced draws on rung \(rung)"
                            : "a state that loads no colour fetched"
        return .fail(why, metrics)
    }
}
