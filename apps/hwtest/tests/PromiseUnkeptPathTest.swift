import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal
import UIKit

/// Writer promises given up on since boot, from kern.switch_gpu_promises (debug kernels): each is reported once in a
/// "writer promise ... was not kept" line, and lastWaitedMs is how long the last one's reader had waited. direct are
/// the direct presents of promised frames: watched on their promise, handed to the job that kept it, given up.
struct PromiseTotals {
    let abandoned: Int, lastWaitedMs: Int
    let directPromised: Int, directKept: Int, directGivenUp: Int

    static func read() throws -> PromiseTotals {
        switch KernelSysctl.string("kern.switch_gpu_promises") {
        case .value(let line):
            var fields: [String: Int] = [:]
            for field in line.split(separator: " ") {
                let pair = field.split(separator: "=", maxSplits: 1).map(String.init)
                if pair.count == 2 { fields[pair[0]] = Int(pair[1]) }
            }
            return PromiseTotals(
                abandoned: fields["abandoned"] ?? -1, lastWaitedMs: fields["last_waited_ms"] ?? -1,
                directPromised: fields["direct_promised"] ?? -1, directKept: fields["direct_kept"] ?? -1,
                directGivenUp: fields["direct_given_up"] ?? -1)
        case .failed(let error):
            throw TestFailure(KernelSysctl.unreadable("kern.switch_gpu_promises", error))
        }
    }
}

/// The maker's cancel of its promise (TegraGM20BJobABI.h method 34), `afterSeconds` from its making or at once through
/// `cancel()`, whichever comes first, made once; `at` is when it was made (nil before).
final class PromiseMakerCancel: @unchecked Sendable {
    private let lock = NSLock()
    private let maker: GPUConnection, promise: UInt32
    private var made: DispatchTime?
    private var answer = KERN_SUCCESS

    init(_ maker: GPUConnection, promise: UInt32, afterSeconds: Double) {
        self.maker = maker
        self.promise = promise
        DispatchQueue.global().asyncAfter(deadline: .now() + afterSeconds) { [self] in cancel() }
    }

    func cancel() {
        lock.lock()
        defer { lock.unlock() }
        guard made == nil else { return }
        made = DispatchTime.now()
        let words = GPUConnection.words([0, 0, PromiseCancelPathTest.promiseCancel, 0, promise, 0])
        answer = maker.call(PromiseCancelPathTest.writerPromise, words, outputSize: 8).0
    }

    var at: DispatchTime? {
        lock.lock()
        defer { lock.unlock() }
        return made
    }

    var result: kern_return_t {
        lock.lock()
        defer { lock.unlock() }
        return answer
    }
}

/// A writer promise nobody keeps (drivers/TegraGM20B/SwitchGpuWriterPromise.h): the test's connection promises an
/// IOSurface's writes and never submits the job. The kernel gives up an unkept promise only at a bound its holder armed
/// (SwitchGpuPromiseGiveUp.h: a held command buffer's submit worker arms Metal's 5 s bound, gpu.path.holder_timeout);
/// this promise is the test connection's own, with no command buffer and no bound, so its readers wait for the maker's
/// own event: here its cancel, cancelSeconds after the promise, past the one-second give-up there was before. Each
/// reader then goes on at once with the frame the surface held before. - composite: a layer showing the surface,
/// rendered by the render server, whose job acquires the promise and parks at its submit, so the display's present
/// waits behind that job: composite_ms about cancelSeconds. A composite that ended before the cancel with nothing given
/// up never parked: SKIP with that reason, its promise cancelled then. - copy: the Metal device's blit of the surface
/// (this process's own job, run only when its command buffers take the raw path), parked at its submit, and beside it a
/// CPU lock of the surface that sleeps behind the same promise. Judged: no promise given up (d_abandoned 0: no "was not
/// kept" line), each reader went on no sooner than the cancel and within PromiseCancelPathTest.goesOnMs of it
/// (`<reader>_after_cancel_ms`: the cancel's CPU write reached the parked channel, and the CPU lock was woken, with no
/// close), the frame before shown or read, the copy completed.
struct PromiseUnkeptPathTest: GPUPathTest {
    let id = "gpu.path.promise_unkept"
    let name = "A writer promise nobody keeps is never given up: its readers go on at its maker's cancel"
    let counters = ["writer_promises", "promise_waits"]
    static let size = 64
    static let fill: [UInt8] = [0, 0, 255, 255]  // BGRA red: the frame before the promised one
    static let cancelSeconds = 1.5

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let rowBytes = Self.size * 4
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: Self.size, .height: Self.size, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * Self.size, .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties), let maker = GPUConnection() else {
            throw TestFailure("no IOSurface or TegraGM20B connection")
        }
        surface.lock(options: [], seed: nil)
        for at in stride(from: 0, to: rowBytes * Self.size, by: 4) {
            (surface.baseAddress + at).copyMemory(from: Self.fill, byteCount: 4)
        }
        surface.unlock(options: [], seed: nil)
        var failures: [String] = []

        let start = try PromiseTotals.read()
        let (handle, compositePromise) = try PromiseCancelPathTest.promise(surface, maker)
        defer {
            _ = maker.call(
                PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(handle)), outputSize: 0)
        }
        let shown = await context.onStage { stage -> UIView in
            let view = UIView(frame: CGRect(x: 0, y: 0, width: 96, height: 96))
            view.layer.contents = surface
            view.layer.contentsGravity = .resize
            stage.addSubview(view)
            return view
        }
        let compositeCancel = PromiseMakerCancel(maker, promise: compositePromise, afterSeconds: Self.cancelSeconds)
        let rendering = DispatchTime.now()
        let render = await MainActor.run { Snapshots.composited(shown) }
        let compositeEnded = DispatchTime.now()
        let compositeCancelledFirst = compositeCancel.at
        compositeCancel.cancel()
        await context.clearStage()
        let afterComposite = try PromiseTotals.read()
        let centre = render.pixel(render.width / 2, render.height / 2)
        let compositeGivenUp = afterComposite.abandoned - start.abandoned
        metrics.set("composite_ms", String(format: "%.1f", Self.ms(rendering, compositeEnded)))
        metrics.set("composite_d_abandoned", compositeGivenUp)
        metrics.set(
            "centre", centre.prefix(3).map { String(Int(($0 * render.levels).rounded())) }.joined(separator: ","))
        if compositeGivenUp != 0 {
            failures.append("the composite's unkept promise was given up \(compositeGivenUp) times")
        }
        if compositeCancel.result != KERN_SUCCESS {
            failures.append("the composite's cancel was refused: \(GPUConnection.hex(compositeCancel.result))")
        }
        if let cancelled = compositeCancelledFirst {
            let afterMs = Self.ms(cancelled, compositeEnded)
            metrics.set("composite_after_cancel_ms", String(format: "%.1f", afterMs))
            if afterMs > PromiseCancelPathTest.goesOnMs {
                failures.append(String(format: "the composite went on %.0f ms after the cancel", afterMs))
            }
            if centre[0] < 0.8 || centre[1] > 0.2 {
                failures.append("the composite shows \(centre.prefix(3)), not the frame before (red)")
            }
        } else if compositeGivenUp == 0 {
            PathParts.current?.skip(
                String(
                    format: "composite: the render server never parked on the promise (%.0f ms)",
                    Self.ms(rendering, compositeEnded)),
                counters: [])
        }

        guard PathParts.ownRouteRaw(gpu, part: "copy and CPU lock", counters: ["promise_waits"], &metrics) else {
            return failures
        }
        let (copyHandle, copyPromise) = try PromiseCancelPathTest.promise(surface, maker)
        defer {
            _ = maker.call(
                PromiseCancelPathTest.boFree, GPUConnection.words(PromiseCancelPathTest.halves(copyHandle)),
                outputSize: 0)
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.size, height: Self.size, mipmapped: false)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let source = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0),
            let copied = gpu.device.makeBuffer(length: rowBytes * Self.size, options: .storageModeShared),
            let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder()
        else { throw TestFailure("no surface texture, buffer or command buffer") }
        blit.copy(
            from: source, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
            sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1), to: copied, destinationOffset: 0,
            destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes * Self.size)
        blit.endEncoding()
        let copyCancel = PromiseMakerCancel(maker, promise: copyPromise, afterSeconds: Self.cancelSeconds)
        let copying = DispatchTime.now()
        let cpuReader = CpuReader(surface)
        commands.commit()
        commands.waitUntilCompleted()
        let copyEnded = DispatchTime.now()
        let cpuRead = cpuReader.wait()
        let copyCancelledFirst = copyCancel.at
        copyCancel.cancel()
        let afterCopy = try PromiseTotals.read()
        let copyGivenUp = afterCopy.abandoned - afterComposite.abandoned
        metrics.set("copy_ms", String(format: "%.1f", Self.ms(copying, copyEnded)))
        metrics.set("cpu_reader_woken_ms", String(format: "%.1f", Self.ms(copying, cpuRead.woken)))
        metrics.set("copy_d_abandoned", copyGivenUp)
        if copyGivenUp != 0 { failures.append("the copy's unkept promise was given up \(copyGivenUp) times") }
        if copyCancel.result != KERN_SUCCESS {
            failures.append("the copy's cancel was refused: \(GPUConnection.hex(copyCancel.result))")
        }
        if let cancelled = copyCancelledFirst {
            let copyAfterMs = Self.ms(cancelled, copyEnded), cpuAfterMs = Self.ms(cancelled, cpuRead.woken)
            metrics.set("copy_after_cancel_ms", String(format: "%.1f", copyAfterMs))
            metrics.set("cpu_after_cancel_ms", String(format: "%.1f", cpuAfterMs))
            if copyAfterMs > PromiseCancelPathTest.goesOnMs {
                failures.append(String(format: "the copy went on %.0f ms after the cancel", copyAfterMs))
            }
            if cpuAfterMs < 0 { failures.append("the CPU lock went on before the cancel") }
            if cpuAfterMs > PromiseCancelPathTest.goesOnMs {
                failures.append(String(format: "the CPU lock went on %.0f ms after the cancel", cpuAfterMs))
            }
        } else {
            failures.append("the copy ended before its maker's cancel: it never waited for the promise")
        }
        if cpuRead.pixel != Self.fill {
            failures.append("the CPU lock read \(cpuRead.pixel), not the frame before \(Self.fill)")
        }
        if commands.status != .completed { failures.append("the copy ended \(commands.status.rawValue)") }
        let got = Array(UnsafeRawBufferPointer(start: copied.contents(), count: 4))
        if got != Self.fill { failures.append("the copy read \(got), not the frame before \(Self.fill)") }
        return failures
    }

    static func ms(_ from: DispatchTime, _ to: DispatchTime) -> Double {
        Double(Int64(to.uptimeNanoseconds) - Int64(from.uptimeNanoseconds)) / 1e6
    }

    static func ms(since start: DispatchTime) -> Double {
        Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
    }
}

/// A read-only CPU lock of the surface on its own thread: when it woke and the first pixel it read.
final class CpuReader: @unchecked Sendable {
    private let done = DispatchSemaphore(value: 0)
    private var result: (woken: DispatchTime, pixel: [UInt8]) = (DispatchTime.now(), [])

    init(_ surface: IOSurface) {
        DispatchQueue.global().async { [self] in
            surface.lock(options: .readOnly, seed: nil)
            let woken = DispatchTime.now()
            let pixel = Array(UnsafeRawBufferPointer(start: surface.baseAddress, count: 4))
            surface.unlock(options: .readOnly, seed: nil)
            result = (woken, pixel)
            done.signal()
        }
    }

    func wait() -> (woken: DispatchTime, pixel: [UInt8]) {
        done.wait()
        return result
    }
}
