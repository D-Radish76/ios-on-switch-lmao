import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A heap's block-compressed textures, a mip chain and a 2D array (drivers/TegraGM20BMetal/raw/runtime/RawMetalHeap.mm
/// heapTextureAsk, raw::TextureAsk::Block): their size is the device's own answer and each is made in the heap by the
/// raw path, no m2v device asked and no twin made.
struct NativeHeapBlockTest: HWTest {
    let id = "metal.native.heap_block"
    let name = "A heap's block-compressed textures placed in it, with no m2v"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let needsBefore = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let heapDescriptor = MTLHeapDescriptor()
        heapDescriptor.storageMode = .private
        heapDescriptor.size = 4 << 20
        guard let heap = gpu.device.makeHeap(descriptor: heapDescriptor) else { throw TestFailure("no heap") }
        let mips = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .astc_4x4_ldr, width: 100, height: 60, mipmapped: true)
        let layers = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .etc2_rgb8, width: 64, height: 64, mipmapped: false)
        layers.textureType = .type2DArray
        layers.arrayLength = 3
        var metrics = Metrics()
        var wrong: [String] = []
        for (name, descriptor) in [("mips", mips), ("array", layers)] {
            descriptor.storageMode = .private
            let need = gpu.device.heapTextureSizeAndAlign(descriptor: descriptor)
            metrics.set("\(name)_size", need.size)
            metrics.set("\(name)_align", need.align)
            guard let texture = heap.makeTexture(descriptor: descriptor) else {
                wrong.append("\(name)_made")
                continue
            }
            let object = NativeObject.answer(texture)
            metrics.set("\(name)_class", object?.name ?? "-")
            metrics.set("\(name)_twin", object?.twin ?? true)
            if need.size == 0 { wrong.append("\(name)_size") }
            if texture.heap !== heap { wrong.append("\(name)_heap") }
            if object == nil || object!.twin { wrong.append("\(name)_twin") }
        }
        let needsAfter = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let asked = Set(needsAfter.keys).subtracting(needsBefore.keys).sorted()
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
