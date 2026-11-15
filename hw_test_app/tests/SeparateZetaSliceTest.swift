import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A Depth32Float array and a Stencil8 array of three slices as one pass's attachments, each from a slice of its own (a pair
/// from a slice past 0 was pass.ds, m2v's). Three passes clear depth slice s to 0.1 x (s + 1) and stencil slice s to s + 1;
/// a fourth loads depth slice 1 beside stencil slice 2, draws z 0.5 with stencil 0x5A replaced over the left half and stores
/// both. Read back: depth slice 1 is 0.5 on the left and 0.2 on the right, stencil slice 2 0x5A on the left and 3 on the
/// right, every other slice its clear; every pass on the raw path.
struct SeparateZetaSliceTest: HWTest {
    let id = "gpu.raw.separate_zeta_slice"
    let name = "Depth32Float and Stencil8 array slices past 0 as one pass's attachments, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = 16, slices = 3
        func array(_ format: MTLPixelFormat) throws -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: n, height: n, mipmapped: false)
            d.textureType = .type2DArray
            d.arrayLength = slices
            d.storageMode = .private
            d.usage = [.renderTarget]
            guard let t = gpu.device.makeTexture(descriptor: d) else {
                throw TestFailure("no \(format.rawValue) array")
            }
            return t
        }
        let depth = try array(.depth32Float), stencil = try array(.stencil8)
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float, stencil: .stencil8)
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = .always
        face.depthStencilPassOperation = .replace
        d.frontFaceStencil = face
        d.backFaceStencil = face
        guard let write = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        func pass(depthSlice: Int, stencilSlice: Int, clear: Bool) -> MTLRenderPassDescriptor {
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .store
            p.depthAttachment.texture = depth
            p.depthAttachment.slice = depthSlice
            p.depthAttachment.loadAction = clear ? .clear : .load
            p.depthAttachment.clearDepth = 0.1 * Double(depthSlice + 1)
            p.depthAttachment.storeAction = .store
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.slice = stencilSlice
            p.stencilAttachment.loadAction = clear ? .clear : .load
            p.stencilAttachment.clearStencil = UInt32(stencilSlice + 1)
            p.stencilAttachment.storeAction = .store
            return p
        }
        var paths: [String] = []
        for s in 0..<slices {
            paths.append(
                try await gpu.run { commands in
                    commands.makeRenderCommandEncoder(descriptor: pass(depthSlice: s, stencilSlice: s, clear: true))!
                        .endEncoding()
                }.path)
        }
        let depths = try gpu.buffer(slices * n * n * 4), stencils = try gpu.buffer(slices * n * n)
        paths.append(
            try await gpu.run { commands in
                let e = commands.makeRenderCommandEncoder(
                    descriptor: pass(depthSlice: 1, stencilSlice: 2, clear: false))!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(write)
                e.setStencilReferenceValue(0x5A)
                var corners: [SIMD4<Float>] = [[-1, -1, 0.5, 1], [0, -1, 0.5, 1], [-1, 1, 0.5, 1], [0, 1, 0.5, 1]]
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                var red = SIMD4<Float>(1, 0, 0, 1)
                e.setFragmentBytes(&red, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
                let b = commands.makeBlitCommandEncoder()!
                for s in 0..<slices {
                    b.copy(
                        from: depth, sourceSlice: s, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                        sourceSize: MTLSize(width: n, height: n, depth: 1), to: depths,
                        destinationOffset: s * n * n * 4, destinationBytesPerRow: n * 4,
                        destinationBytesPerImage: n * n * 4)
                    b.copy(
                        from: stencil, sourceSlice: s, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                        sourceSize: MTLSize(width: n, height: n, depth: 1), to: stencils,
                        destinationOffset: s * n * n, destinationBytesPerRow: n, destinationBytesPerImage: n * n)
                }
                b.endEncoding()
            }.path)
        let z = depths.contents().bindMemory(to: Float.self, capacity: slices * n * n)
        let st = stencils.contents().bindMemory(to: UInt8.self, capacity: slices * n * n)
        var depthWrong = [0, 0, 0], stencilWrong = [0, 0, 0]
        for s in 0..<slices {
            for y in 0..<n {
                for x in 0..<n {
                    let i = s * n * n + y * n + x, left = x < n / 2
                    let wantDepth = s == 1 && left ? Float(0.5) : Float(0.1 * Double(s + 1))
                    let wantStencil = s == 2 && left ? UInt8(0x5A) : UInt8(s + 1)
                    depthWrong[s] += z[i] == wantDepth ? 0 : 1
                    stencilWrong[s] += st[i] == wantStencil ? 0 : 1
                }
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("depth_wrong", depthWrong.map(String.init).joined(separator: "/"))
        metrics.set("stencil_wrong", stencilWrong.map(String.init).joined(separator: "/"))
        metrics.set("slice1_depth", String(format: "%.3f/%.3f", z[n * n + 2], z[n * n + n - 2]))
        metrics.set("slice2_stencil", String(format: "%02x/%02x", st[2 * n * n + 2], st[2 * n * n + n - 2]))
        var failures: [String] = []
        if paths.contains(where: { $0 != "raw" }) { failures.append("a pass went off the raw path") }
        if depthWrong.reduce(0, +) != 0 { failures.append("depth texels wrong by slice \(depthWrong)") }
        if stencilWrong.reduce(0, +) != 0 { failures.append("stencil texels wrong by slice \(stencilWrong)") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
