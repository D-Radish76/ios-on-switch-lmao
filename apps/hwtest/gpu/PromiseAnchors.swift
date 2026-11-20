import Foundation
import HWTestCore
import IOSurface
import Metal

/// gpu.kernel.fence_page_regions's Metal work: a command buffer held behind a shared event while it renders into an
/// IOSurface, then a render pass's four stage-boundary timestamp samples resolved on the CPU and by a blit
public enum PromiseAnchors {
    public static let side = 64, samples = 4

    public struct Held: Sendable {
        /// The writer's status when its waitUntilScheduled returned, then the head's and the writer's at the end
        public var whenScheduled = -1, head = -1, writer = -1
        public var headPath = "-", writerPath = "-"
        public var held: Bool { whenScheduled >= 0 && whenScheduled < Int(MTLCommandBufferStatus.scheduled.rawValue) }
    }

    /// On a queue of its own, a head that waits for an unsignalled shared event and behind it a command buffer that
    /// clears and draws a 64x64 BGRA IOSurface's texture; the event is signalled once the writer's waitUntilScheduled
    /// returns.
    public static func held(_ gpu: GPU) throws -> Held {
        let rowBytes = side * 4
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .bytesPerElement: 4, .bytesPerRow: rowBytes, .allocSize: rowBytes * side,
            .pixelFormat: 0x4247_5241,
        ]
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let surface = IOSurface(properties: properties),
            let target = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0),
            let queue = gpu.device.makeCommandQueue(), let event = gpu.device.makeSharedEvent(),
            let head = queue.makeCommandBuffer(), let writer = queue.makeCommandBuffer()
        else { throw TestFailure("no surface texture, queue, shared event or command buffer") }
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        head.encodeWaitForEvent(event, value: 1)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        let encoder = writer.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
        encoder.endEncoding()
        head.commit()
        writer.commit()
        writer.waitUntilScheduled()
        var out = Held()
        out.whenScheduled = Int(writer.status.rawValue)
        event.signaledValue = 1
        writer.waitUntilCompleted()
        head.waitUntilCompleted()
        out.head = Int(head.status.rawValue)
        out.writer = Int(writer.status.rawValue)
        out.headPath = RawFacade.path(of: head)
        out.writerPath = RawFacade.path(of: writer)
        return out
    }

    public struct Sampled: Sendable {
        /// The four samples as the CPU's resolve and the blit's give them
        public var samples: [UInt64] = [], blitted: [UInt64] = []
        /// sampleTimestamps' GPU values before the commit and after the completion
        public var before: UInt64 = 0, after: UInt64 = 0
        public var gpuMicroseconds = 0.0
        public var paths: [String] = []

        /// Samples outside [before, after], a sample the CPU's resolve did not give counted outside
        public var outside: Int {
            samples.filter { $0 < before || $0 > after }.count + max(0, PromiseAnchors.samples - samples.count)
        }
        public var differ: Int { zip(samples, blitted).filter { $0 != $1 }.count + abs(samples.count - blitted.count) }
        public var leadMicroseconds: Double { samples.min().map { (Double($0) - Double(before)) / 1000 } ?? -1 }
    }

    /// A render pass of one draw into a 32x32 RGBA8 target sampling its vertex and fragment stages' start and end into
    /// a 4-sample timestamp buffer, between two sampleTimestamps; then its samples resolved by the CPU and by a blit
    public static func sampled(_ gpu: GPU) throws -> Sampled {
        guard let set = gpu.device.counterSets?.first(where: { $0.name == MTLCommonCounterSet.timestamp.rawValue })
        else { throw TestFailure("no timestamp counter set") }
        let descriptor = MTLCounterSampleBufferDescriptor()
        descriptor.counterSet = set
        descriptor.sampleCount = samples
        descriptor.storageMode = .shared
        let counters = try gpu.device.makeCounterSampleBuffer(descriptor: descriptor)
        let target = try gpu.texture(.rgba8Unorm, 32, 32)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        var out = Sampled()
        out.before = gpu.device.sampleTimestamps().gpu
        let sampling = try gpu.commit { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let attachment = pass.sampleBufferAttachments[0]!
            attachment.sampleBuffer = counters
            attachment.startOfVertexSampleIndex = 0
            attachment.endOfVertexSampleIndex = 1
            attachment.startOfFragmentSampleIndex = 2
            attachment.endOfFragmentSampleIndex = 3
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(0, 1, 0, 1))
            encoder.endEncoding()
        }
        try gpu.finish([sampling])
        out.after = gpu.device.sampleTimestamps().gpu
        out.gpuMicroseconds = (sampling.gpuEndTime - sampling.gpuStartTime) * 1e6
        out.samples = words(try counters.resolveCounterRange(0..<samples))
        let resolved = try gpu.buffer(samples * 8)
        let resolving = try gpu.commit { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.resolveCounters(counters, range: 0..<samples, destinationBuffer: resolved, destinationOffset: 0)
            blit.endEncoding()
        }
        try gpu.finish([resolving])
        out.blitted = Array(
            UnsafeBufferPointer(
                start: resolved.contents().bindMemory(to: UInt64.self, capacity: samples), count: samples))
        out.paths = [RawFacade.path(of: sampling), RawFacade.path(of: resolving)]
        return out
    }

    static func words(_ data: Data?) -> [UInt64] {
        guard let data else { return [] }
        return data.withUnsafeBytes { Array($0.bindMemory(to: UInt64.self)) }
    }
}
