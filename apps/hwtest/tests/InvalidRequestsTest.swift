import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Requests Metal's validation layer stops complete on the raw path, never taking their command buffer to m2v: a blit
/// recording nothing (one raw:blit.invalid line a process) and a depth resolve with no resolve texture keeping its
/// store part (one raw:pass.resolve_store line). Each blit sits in a command buffer of its own between two buffer
/// copies that land: a copy from a 4x texture (samples), into an R8 level from an RGBA8 one (texel_size), from a region
/// past its level (past_level), into a texture from buffer rows shorter than its rows (rows_short_in), into a buffer
/// the same way (rows_short_out). Every byte each would touch, done as asked, lies inside what the test made: a level
/// past the texture's, a slice past it, a memoryless side and rows past a buffer are left out. The resolves decide the depth's
/// store in the encoder (MTLStoreActionUnknown, then setDepthStoreAction:): StoreAndMultisampleResolve keeps the store,
/// read by a later pass whose depth test shows the left half drawn at 0.25 (resolve_store); MultisampleResolve discards
/// it (resolve_discard). Per case: <case>_path, <case>_neighbours (the two copies' bytes off, or the pass's resolved
/// colour texels off), <case>_untouched (the request's destination as it was; resolve_store: the later pass saw the
/// kept depth), resolve_store_load_path (that later pass's).
struct InvalidRequestsTest: HWTest {
    let id = "gpu.raw.invalid_requests"
    let name = "Blits and a resolve store Metal's validation stops complete on raw, their neighbours intact"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64, level = 1, levelSide = 32, bytes = 4096

    /// What a request must leave as it was: level 1 of a texture, or a shared buffer
    enum Destination {
        case level(MTLTexture, bytesPerPixel: Int)
        case buffer(MTLBuffer)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let pattern = try gpu.buffer((0..<Self.bytes).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) })
        let junk = try gpu.buffer([UInt8](repeating: 0xee, count: Self.bytes))
        let source = try gpu.texture(.rgba8Unorm, Self.side, Self.side, mipmaps: 7)
        let chain = try gpu.texture(.rgba8Unorm, Self.side, Self.side, mipmaps: 7)
        let narrow = try gpu.texture(.r8Unorm, Self.side, Self.side, mipmaps: 7)
        let multisampled = try gpu.texture(.rgba8Unorm, 16, 16, samples: 4)
        let out = try gpu.buffer(Self.bytes)
        let o = MTLOrigin()
        let size = { (w: Int, h: Int) in MTLSize(width: w, height: h, depth: 1) }
        let level = Self.level
        let requests: [(String, Destination, (MTLBlitCommandEncoder) -> Void)] = [
            (
                "samples", .level(chain, bytesPerPixel: 4),
                {
                    $0.copy(
                        from: multisampled, sourceSlice: 0, sourceLevel: 0, sourceOrigin: o, sourceSize: size(8, 8),
                        to: chain, destinationSlice: 0, destinationLevel: level, destinationOrigin: o)
                }
            ),
            (
                "texel_size", .level(narrow, bytesPerPixel: 1),
                {
                    $0.copy(
                        from: source, sourceSlice: 0, sourceLevel: level, sourceOrigin: o, sourceSize: size(4, 4),
                        to: narrow, destinationSlice: 0, destinationLevel: level, destinationOrigin: o)
                }
            ),
            (
                "past_level", .level(chain, bytesPerPixel: 4),
                {
                    $0.copy(
                        from: source, sourceSlice: 0, sourceLevel: level, sourceOrigin: MTLOrigin(x: 28, y: 0, z: 0),
                        sourceSize: size(8, 8), to: chain, destinationSlice: 0, destinationLevel: level,
                        destinationOrigin: o)
                }
            ),
            (
                "rows_short_in", .level(chain, bytesPerPixel: 4),
                {
                    $0.copy(
                        from: junk, sourceOffset: 0, sourceBytesPerRow: 32, sourceBytesPerImage: 0,
                        sourceSize: size(16, 16), to: chain, destinationSlice: 0, destinationLevel: level,
                        destinationOrigin: o)
                }
            ),
            (
                "rows_short_out", .buffer(out),
                {
                    $0.copy(
                        from: source, sourceSlice: 0, sourceLevel: level, sourceOrigin: o, sourceSize: size(16, 16),
                        to: out, destinationOffset: 0, destinationBytesPerRow: 32, destinationBytesPerImage: 0)
                }
            ),
        ]
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (key, destination, request) in requests {
            let kept = try await Self.fill(destination, gpu)
            let before = try gpu.buffer(Self.bytes), after = try gpu.buffer(Self.bytes)
            let path = try await gpu.run(tolerateError: true) { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(from: pattern, sourceOffset: 0, to: before, destinationOffset: 0, size: Self.bytes)
                request(blit)
                blit.copy(from: pattern, sourceOffset: 0, to: after, destinationOffset: 0, size: Self.bytes)
                blit.endEncoding()
            }.path
            let neighbours = Self.off(before, pattern) + Self.off(after, pattern)
            let untouched = try await Self.read(destination, gpu) == kept
            Self.report(key, path: path, neighbours: neighbours, untouched: untouched, &metrics, &checks)
        }
        try await resolveStores(gpu, &metrics, &checks)
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func report(
        _ key: String, path: String, neighbours: Int, untouched: Bool?, _ metrics: inout Metrics,
        _ checks: inout [(String, Bool)]
    ) {
        metrics.set("\(key)_path", String(path.prefix(80)))
        metrics.set("\(key)_neighbours", neighbours)
        checks.append(("\(key): completed on the raw path", path == "raw"))
        checks.append(("\(key): its neighbours landed", neighbours == 0))
        if let untouched {
            metrics.set("\(key)_untouched", untouched)
            checks.append(("\(key): its destination as it was", untouched))
        }
    }

    /// The destination's contents before the request: a pattern of its own
    static func fill(_ destination: Destination, _ gpu: GPU) async throws -> [UInt8] {
        switch destination {
        case .level(let texture, let bytesPerPixel):
            let bytes = (0..<levelSide * levelSide * bytesPerPixel).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ 1) }
            try await gpu.upload(bytes, to: texture, bytesPerRow: levelSide * bytesPerPixel, level: level)
            return bytes
        case .buffer(let buffer):
            memset(buffer.contents(), 0x5c, buffer.length)
            return [UInt8](repeating: 0x5c, count: buffer.length)
        }
    }

    static func read(_ destination: Destination, _ gpu: GPU) async throws -> [UInt8] {
        switch destination {
        case .level(let texture, let bytesPerPixel):
            return try await gpu.read(texture, bytesPerPixel: bytesPerPixel, level: level)
        case .buffer(let buffer):
            return [UInt8](UnsafeRawBufferPointer(start: buffer.contents(), count: buffer.length))
        }
    }

    static func off(_ a: MTLBuffer, _ b: MTLBuffer) -> Int {
        let x = UnsafeRawBufferPointer(start: a.contents(), count: bytes)
        let y = UnsafeRawBufferPointer(start: b.contents(), count: bytes)
        return zip(x, y).filter { $0 != $1 }.count
    }
}

extension InvalidRequestsTest {
    /// A 4x Depth32Float pass whose depth store the encoder decides as a resolve with no resolve texture, its colour
    /// resolved beside it (red left, where a quad at 0.25 is drawn, black right). The store kept is read by a later
    /// pass that loads the depth and draws green at 0.5 under Less: black left, green right.
    func resolveStores(_ gpu: GPU, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let n = 16, samples = 4
        let colour = try gpu.texture(.rgba8Unorm, n, n, samples: samples)
        let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget], samples: samples)
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, samples: samples, depth: .depth32Float)
        func depthState(_ compare: MTLCompareFunction, write: Bool) throws -> MTLDepthStencilState {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = write
            guard let s = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
            return s
        }
        let mark = try depthState(.always, write: true), behind = try depthState(.less, write: false)
        func pass(_ commands: MTLCommandBuffer, into seen: MTLTexture, load: Bool, decided: MTLStoreAction?) {
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].resolveTexture = seen
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            p.colorAttachments[0].storeAction = .multisampleResolve
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = load ? .load : .clear
            p.depthAttachment.clearDepth = 1
            p.depthAttachment.storeAction = decided == nil ? .dontCare : .unknown
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(load ? behind : mark)
            let (x1, z, rgba): (Float, Float, SIMD4<Float>) = load ? (1, 0.5, [0, 1, 0, 1]) : (0, 0.25, [1, 0, 0, 1])
            GPU.draw(e, corners: [[-1, -1, z, 1], [x1, -1, z, 1], [-1, 1, z, 1], [x1, 1, z, 1]], color: rgba)
            if let decided { e.setDepthStoreAction(decided) }
            e.endEncoding()
        }
        func off(_ seen: MTLTexture, left: [UInt8], right: [UInt8]) async throws -> Int {
            let pixels = try await gpu.read(seen, bytesPerPixel: 4)
            return (0..<n * n).filter { Array(pixels[4 * $0..<4 * $0 + 4]) != ($0 % n < n / 2 ? left : right) }.count
        }
        let red: [UInt8] = [255, 0, 0, 255], green: [UInt8] = [0, 255, 0, 255], black: [UInt8] = [0, 0, 0, 255]
        let decisions: [(String, MTLStoreAction)] = [
            ("resolve_store", .storeAndMultisampleResolve), ("resolve_discard", .multisampleResolve),
        ]
        for (key, decided) in decisions {
            let seen = try gpu.texture(.rgba8Unorm, n, n)
            let path = try await gpu.run(tolerateError: true) {
                pass($0, into: seen, load: false, decided: decided)
            }.path
            let neighbours = try await off(seen, left: red, right: black)
            var untouched: Bool?
            if decided == .storeAndMultisampleResolve {
                let later = try gpu.texture(.rgba8Unorm, n, n)
                let loadPath = try await gpu.run(tolerateError: true) {
                    pass($0, into: later, load: true, decided: nil)
                }.path
                metrics.set("\(key)_load_path", String(loadPath.prefix(80)))
                checks.append(("\(key): the pass loading the kept depth on the raw path", loadPath == "raw"))
                untouched = try await off(later, left: black, right: green) == 0
            }
            Self.report(key, path: path, neighbours: neighbours, untouched: untouched, &metrics, &checks)
        }
    }
}
