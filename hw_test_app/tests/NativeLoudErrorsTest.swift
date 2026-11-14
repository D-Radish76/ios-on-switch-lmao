import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A client error the raw path now fails its command buffer for (drivers/TegraGM20BMetal/raw/runtime/RawMetalPredicates.h
/// predFailure) instead of replaying it on m2v: the command buffer completes with MTLCommandBufferErrorDomain's
/// InvalidResource, its path is error:<predicate>, the predicate's failed count rises by one and m2v's does not. The
/// raw path logs one refusal line a process (`<predicate>: the command buffer fails`). Every wait is gpu.run's.
protocol NativeLoudErrorTest: HWTest {
    var predicate: String { get }
    func encode(_ gpu: GPU, _ commands: MTLCommandBuffer) throws
}

extension NativeLoudErrorTest {
    var subsystem: Subsystem { .metal }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let before = RawFacade.counters(of: gpu.device)
        guard !before.isEmpty else { return .skip("a release runtime: no counters") }
        var metrics = Metrics()
        var failures: [String] = []
        var commandBuffer: MTLCommandBuffer?
        _ = try await gpu.run(tolerateError: true) { commands in
            commandBuffer = commands
            try encode(gpu, commands)
        }
        let after = RawFacade.counters(of: gpu.device)
        let failed = (after["failed.\(predicate)"] ?? 0) - (before["failed.\(predicate)"] ?? 0)
        let m2v = (after["m2v"] ?? 0) - (before["m2v"] ?? 0)
        let path = commandBuffer.map(RawFacade.path(of:)) ?? "-"
        let error = commandBuffer?.error.map { $0 as NSError }
        metrics.set("path", path)
        metrics.set("domain", error?.domain ?? "-")
        metrics.set("code", error?.code ?? 0)
        metrics.set("failed", failed)
        metrics.set("m2v", m2v)
        if commandBuffer?.status != .error { failures.append("the command buffer did not fail") }
        if error?.domain != MTLCommandBufferErrorDomain || error?.code != MTLCommandBufferError.invalidResource.rawValue
        {
            failures.append("its error is not MTLCommandBufferErrorDomain InvalidResource")
        }
        if path != "error:\(predicate)" { failures.append("its path is \(path)") }
        if failed != 1 { failures.append("failed.\(predicate) rose by \(failed)") }
        if m2v != 0 { failures.append("\(m2v) command buffers went to m2v") }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof)
            : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}

enum LoudErrors {
    static func target(_ gpu: GPU, _ format: MTLPixelFormat, samples: Int = 1) throws -> MTLTexture {
        try gpu.texture(format, 32, 32, usage: [.renderTarget], samples: samples)
    }

    static func pass(_ t: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = t
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = t.sampleCount > 1 ? .dontCare : .store
        return pass
    }

    static func draw(
        _ gpu: GPU, _ commands: MTLCommandBuffer, _ pass: MTLRenderPassDescriptor, _ state: MTLRenderPipelineState
    ) {
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setRenderPipelineState(state)
        GPU.draw(e, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
        e.endEncoding()
    }
}

/// A blit copy of 1 MiB out of a 4 KiB buffer
struct NativeErrorBufferCopyTest: NativeLoudErrorTest {
    let id = "metal.native.error_bufcopy"
    let name = "A buffer copy past its buffer fails its command buffer with InvalidResource, not m2v"
    let predicate = "call.bufcopy"

    func encode(_ gpu: GPU, _ commands: MTLCommandBuffer) throws {
        let a = try gpu.buffer(4096), b = try gpu.buffer(4096)
        let blit = commands.makeBlitCommandEncoder()!
        blit.copy(from: a, sourceOffset: 0, to: b, destinationOffset: 0, size: 1 << 20)
        blit.endEncoding()
    }
}

/// An indirect draw whose arguments start 4 bytes into a 16-byte buffer
struct NativeErrorIndexRangeTest: NativeLoudErrorTest {
    let id = "metal.native.error_idxrange"
    let name = "Indirect draw arguments past their buffer fail the command buffer with InvalidResource, not m2v"
    let predicate = "enc.idxrange"

    func encode(_ gpu: GPU, _ commands: MTLCommandBuffer) throws {
        let state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let arguments = try gpu.buffer(16)
        let e = commands.makeRenderCommandEncoder(descriptor: LoudErrors.pass(try LoudErrors.target(gpu, .rgba8Unorm)))!
        e.setRenderPipelineState(state)
        var corners = GPU.fullScreen
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        e.drawPrimitives(type: .triangleStrip, indirectBuffer: arguments, indirectBufferOffset: 4)
        e.endEncoding()
    }
}

/// An RGBA8 state drawn into a BGRA8 pass
struct NativeErrorColourFormatTest: NativeLoudErrorTest {
    let id = "metal.native.error_colorfmt"
    let name = "A state of another colour format than its pass fails the command buffer with InvalidResource, not m2v"
    let predicate = "enc.colorfmt"

    func encode(_ gpu: GPU, _ commands: MTLCommandBuffer) throws {
        let state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        LoudErrors.draw(gpu, commands, LoudErrors.pass(try LoudErrors.target(gpu, .bgra8Unorm)), state)
    }
}

/// A one-sample state drawn into a four-sample pass
struct NativeErrorSamplesTest: NativeLoudErrorTest {
    let id = "metal.native.error_samples"
    let name = "A state of other samples than its pass fails the command buffer with InvalidResource, not m2v"
    let predicate = "enc.samples"

    func encode(_ gpu: GPU, _ commands: MTLCommandBuffer) throws {
        let state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        LoudErrors.draw(gpu, commands, LoudErrors.pass(try LoudErrors.target(gpu, .rgba8Unorm, samples: 4)), state)
    }
}

/// Four sample positions on a two-sample pass (Metal's setter takes a count of 4; the pass's samples do not)
struct NativeErrorPositionsTest: NativeLoudErrorTest {
    let id = "metal.native.error_positions"
    let name = "Sample positions past the pass's samples fail the command buffer with InvalidResource, not m2v"
    let predicate = "pass.positions"

    func encode(_ gpu: GPU, _ commands: MTLCommandBuffer) throws {
        let pass = LoudErrors.pass(try LoudErrors.target(gpu, .rgba8Unorm, samples: 2))
        let positions = [
            MTLSamplePosition(x: 0.25, y: 0.25), MTLSamplePosition(x: 0.75, y: 0.25),
            MTLSamplePosition(x: 0.25, y: 0.75), MTLSamplePosition(x: 0.75, y: 0.75),
        ]
        pass.setSamplePositions(positions)
        commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
    }
}

/// What is left of pass.other: rasterization rate maps. The device answers supportsRasterizationRateMapWithLayerCount:
/// YES for 1 and 2 layers (raw_device_answers.h) and newRasterizationRateMapWithDescriptor: is _MTLDevice's own, which
/// no device run has read. Logs both; where a rate map is made, a pass of it must draw on the raw path, which it does not
/// yet (pass.other sends it to m2v), so this fails until raw draws rate maps or the device answers NO.
struct NativeRateMapAnswerTest: NativeObjectTest {
    let id = "metal.native.rate_map"
    let name = "A rate map the device answers it makes draws its pass on the raw path"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let answers = gpu.device.supportsRasterizationRateMap(layerCount: 1)
        metrics.set("answers", answers ? "yes" : "no")
        let layer = MTLRasterizationRateLayerDescriptor(sampleCount: MTLSize(width: 2, height: 2, depth: 0))
        let descriptor = MTLRasterizationRateMapDescriptor(
            screenSize: MTLSize(width: 32, height: 32, depth: 0), layer: layer)
        let map = gpu.device.makeRasterizationRateMap(descriptor: descriptor)
        metrics.set("made", map.map { NSStringFromClass(type(of: $0 as AnyObject)) } ?? "nil")
        if answers != (map != nil) {
            work.failures.append(
                "the device answers \(answers ? "YES" : "NO") and \(map == nil ? "made none" : "made one")")
        }
        guard let map else { return work }
        let target = try gpu.texture(.rgba8Unorm, 32, 32)
        work.objects.append(target)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = LoudErrors.pass(target)
            pass.rasterizationRateMap = map
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        work.paths.append(timing.path)
        return work
    }
}
