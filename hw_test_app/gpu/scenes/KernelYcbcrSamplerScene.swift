import CoreVideo
import Foundation
import HWTestCore
import Metal

/// A kernel sampling a two-plane 4:2:0 YCbCr texture (540, RGB8_420_2P read as codes, over a '420v' surface) through a
/// constexpr clamp-to-edge sampler beside an RGBA8 texture through a bound repeating sampler, across a grid twice the
/// textures' width. The raw path refused every constexpr sampler beside a converted texture (compute.image: a kernel's
/// sampler is a word of its own, and the texture's address map could have changed it). The record now names the texture
/// each sampler samples (raw_pack.h kernelSamplerPairs), and the texture's map leaves clamp to edge, so the kernel runs
/// raw as compiled. Every sample is a texel's codes, exact: the YCbCr texture's edge past its right side, the RGBA one
/// repeated.
struct KernelYcbcrSamplerScene: GPUScene {
    let id = "gpu.raw.kernel_ycbcr_sampler"
    let name = "A kernel sampling a YCbCr and an RGBA texture through samplers of their own"

    static let width = 16
    static let height = 8
    static let codes: UInt = 540
    static let cb: UInt8 = 0x40
    static let cr: UInt8 = 0xC0

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (w, h) = (Self.width, Self.height)
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard
            CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &made)
                == kCVReturnSuccess,
            let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let format = MTLPixelFormat(rawValue: Self.codes)
        else { throw TestFailure("no '420v' surface") }
        Self.fill(buffer)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false)
        descriptor.usage = [.shaderRead]
        guard let ycbcr = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("makeTexture(540 over '420v') returned nil")
        }
        let rgba = try gpu.texture(.rgba8Unorm, w, h, usage: [.shaderRead])
        try await gpu.upload((0..<w * h).flatMap { Self.rgba($0 % w, $0 / w) }, to: rgba, bytesPerRow: w * 4)
        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.sAddressMode = .repeat
        samplerDescriptor.tAddressMode = .repeat
        guard let sampler = gpu.device.makeSamplerState(descriptor: samplerDescriptor) else {
            throw TestFailure("makeSamplerState returned nil")
        }
        let pipeline = try gpu.compute("ycbcr_sampler_pairs")
        let samples = 2 * w * h * 2
        let out = try gpu.buffer(samples * MemoryLayout<SIMD4<Float>>.stride)
        let refused = { RawFacade.counters(of: gpu.device)["pred.compute.image"] ?? 0 }
        let before = refused()
        let timing = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(ycbcr, index: 0)
            encoder.setTexture(rgba, index: 1)
            encoder.setSamplerState(sampler, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: 2 * w, height: h, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
            encoder.endEncoding()
        }
        let refusals = refused() - before
        let got = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: samples)
        var wrong = [0, 0]
        var maxError: Float = 0
        for y in 0..<h {
            for x in 0..<2 * w {
                for (k, want) in [Self.ycbcr(min(x, w - 1), y), Self.rgba(x % w, y)].enumerated() {
                    let sample = got[(y * 2 * w + x) * 2 + k] * 255
                    let error = (0..<4).map { abs(sample[$0] - Float(want[$0])) }.max() ?? 0
                    maxError = max(maxError, error.isNaN ? .infinity : error)
                    if !(error < 0.01) { wrong[k] += 1 }
                }
            }
        }
        result.metrics.set("path", timing.path)
        result.metrics.set("refusals", refusals)
        result.metrics.set("max_error", String(format: "%.3f", maxError))
        result.metrics.set("wrong_ycbcr", wrong[0])
        result.metrics.set("wrong_rgba", wrong[1])
        result.metrics.set("past_edge", String(format: "%.1f", got[(2 * w - 1) * 2][1] * 255))
        result.expect(
            wrong == [0, 0],
            "samples off their texel's codes: \(wrong[0]) YCbCr (clamped to its edge), \(wrong[1]) RGBA (repeated); "
                + "largest error \(maxError) codes")
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(timing.path == "raw", "the kernel took \(timing.path), not raw")
            result.expect(refusals == 0, "compute.image refused it \(refusals) times")
        }
        return result
    }

    /// Luma 32 + 11x + 3y, every chroma pair (Cb, Cr) the same
    private static func fill(_ buffer: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        let luma = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!.assumingMemoryBound(to: UInt8.self)
        let lumaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let chroma = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!.assumingMemoryBound(to: UInt8.self)
        let chromaRow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        for y in 0..<height {
            for x in 0..<width { luma[y * lumaRow + x] = UInt8(32 + 11 * x + 3 * y) }
        }
        for y in 0..<height / 2 {
            for x in 0..<width / 2 {
                chroma[y * chromaRow + 2 * x] = cb
                chroma[y * chromaRow + 2 * x + 1] = cr
            }
        }
    }

    /// The codes texture's texel: red Cr, green Y, blue Cb, alpha one
    private static func ycbcr(_ x: Int, _ y: Int) -> [UInt8] { [cr, UInt8(32 + 11 * x + 3 * y), cb, 255] }

    private static func rgba(_ x: Int, _ y: Int) -> [UInt8] { [UInt8(13 * x + 1), UInt8(29 * y + 2), 77, 255] }
}
