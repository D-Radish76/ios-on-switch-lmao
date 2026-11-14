import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The raw facade's reflections (drivers/TegraGM20BMetal/raw/runtime/RawMetalReflection.mm): asked with argument info, a
/// state's bindings come from the pack's blob or the system shader compiler's, never from m2v's device.
enum ReflectionCheck {
    static func describe(_ bindings: [MTLBinding]) -> String {
        bindings.isEmpty ? "-" : bindings.map { "\($0.name):\($0.index):\($0.type.rawValue)" }.joined(separator: ",")
    }

    static func needsAsked(_ device: MTLDevice, since before: [String: Any]) -> [String] {
        let after = RawFacade.m2vRecord(of: device)["needs"] as? [String: Any] ?? [:]
        return Set(after.keys).subtracting(before.keys).sorted()
    }
}

/// quad_vertex's corners at buffer 0 and solid_fragment's colour at buffer 0, from a state made with argument info
struct RenderReflectionTest: HWTest {
    let id = "gpu.raw.render_reflection"
    let name = "A render state's reflection, given without m2v"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = gpu.library.makeFunction(name: "solid_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        let (_, reflection) = try gpu.device.makeRenderPipelineState(
            descriptor: descriptor, options: [.argumentInfo, .bufferTypeInfo])
        let vertex = reflection?.vertexBindings ?? []
        let fragment = reflection?.fragmentBindings ?? []
        let asked = ReflectionCheck.needsAsked(gpu.device, since: before)
        var metrics = Metrics()
        metrics.set("reflection", reflection == nil ? "nil" : "given")
        metrics.set("vertex_bindings", ReflectionCheck.describe(vertex))
        metrics.set("fragment_bindings", ReflectionCheck.describe(fragment))
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        let corners = vertex.first { $0.name == "corners" }
        let color = fragment.first { $0.name == "color" }
        return .judged(
            [
                ("a reflection given", reflection != nil),
                ("corners: vertex buffer 0", corners?.index == 0 && corners?.type == .buffer),
                ("color: fragment buffer 0", color?.index == 0 && color?.type == .buffer),
                ("no m2v need", asked.isEmpty),
            ], metrics, proof: gpu.proof)
    }
}

/// multiply_add's a, b, out and counter at buffers 0 to 3, from a state made with argument info
struct ComputeReflectionTest: HWTest {
    let id = "gpu.raw.compute_reflection"
    let name = "A compute state's reflection, given without m2v"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        guard let function = gpu.library.makeFunction(name: "multiply_add") else { throw TestFailure("no multiply_add") }
        let (_, reflection) = try gpu.device.makeComputePipelineState(
            function: function, options: [.argumentInfo, .bufferTypeInfo])
        let bindings = reflection?.bindings ?? []
        let asked = ReflectionCheck.needsAsked(gpu.device, since: before)
        var metrics = Metrics()
        metrics.set("reflection", reflection == nil ? "nil" : "given")
        metrics.set("bindings", ReflectionCheck.describe(bindings))
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        let named = ["a", "b", "out", "counter"].enumerated().allSatisfy { index, name in
            bindings.contains { $0.name == name && $0.index == index && $0.type == .buffer }
        }
        return .judged(
            [
                ("a reflection given", reflection != nil), ("a, b, out, counter: buffers 0-3", named),
                ("no m2v need", asked.isEmpty),
            ], metrics, proof: gpu.proof)
    }
}
