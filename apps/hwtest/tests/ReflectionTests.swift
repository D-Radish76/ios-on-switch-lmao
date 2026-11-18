import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The raw facade's reflections (drivers/TegraGM20BMetal/raw/runtime/RawMetalReflection.mm): asked with argument info,
/// a state's bindings come from the pack's blob or the system shader compiler's.
enum ReflectionCheck {
    static func describe(_ bindings: [MTLBinding]) -> String {
        bindings.isEmpty ? "-" : bindings.map { "\($0.name):\($0.index):\($0.type.rawValue)" }.joined(separator: ",")
    }

    // Synchronous on purpose: inside an async run Swift resolves the tuple-returning calls to Metal's completion-handler
    // forms
    static func render(
        _ device: MTLDevice, _ descriptor: MTLRenderPipelineDescriptor
    ) throws
        -> MTLRenderPipelineReflection?
    {
        try device.makeRenderPipelineState(descriptor: descriptor, options: [.bindingInfo, .bufferTypeInfo]).1
    }

    static func compute(_ device: MTLDevice, _ function: MTLFunction) throws -> MTLComputePipelineReflection? {
        var reflection: MTLComputePipelineReflection?
        _ = try device.makeComputePipelineState(
            function: function, options: [.bindingInfo, .bufferTypeInfo], reflection: &reflection)
        return reflection
    }
}

/// quad_vertex's corners at buffer 0 and solid_fragment's colour at buffer 0, from a state made with argument info
struct RenderReflectionTest: HWTest {
    let id = "gpu.raw.render_reflection"
    let name = "A render state's reflection, given by the facade"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = gpu.library.makeFunction(name: "solid_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        let reflection = try ReflectionCheck.render(gpu.device, descriptor)
        let vertex = reflection?.vertexBindings ?? []
        let fragment = reflection?.fragmentBindings ?? []
        var metrics = Metrics()
        metrics.set("reflection", reflection == nil ? "nil" : "given")
        metrics.set("vertex_bindings", ReflectionCheck.describe(vertex))
        metrics.set("fragment_bindings", ReflectionCheck.describe(fragment))
        let corners = vertex.first { $0.name == "corners" }
        let color = fragment.first { $0.name == "color" }
        return .judged(
            [
                ("a reflection given", reflection != nil),
                ("corners: vertex buffer 0", corners?.index == 0 && corners?.type == .buffer),
                ("color: fragment buffer 0", color?.index == 0 && color?.type == .buffer),
            ], metrics, proof: gpu.proof)
    }
}

/// multiply_add's a, b, out and counter at buffers 0 to 3, from a state made with argument info
struct ComputeReflectionTest: HWTest {
    let id = "gpu.raw.compute_reflection"
    let name = "A compute state's reflection, given by the facade"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let function = gpu.library.makeFunction(name: "multiply_add") else {
            throw TestFailure("no multiply_add")
        }
        let reflection = try ReflectionCheck.compute(gpu.device, function)
        let bindings = reflection?.bindings ?? []
        var metrics = Metrics()
        metrics.set("reflection", reflection == nil ? "nil" : "given")
        metrics.set("bindings", ReflectionCheck.describe(bindings))
        let named = ["a", "b", "out", "counter"].enumerated().allSatisfy { index, name in
            bindings.contains { $0.name == name && $0.index == index && $0.type == .buffer }
        }
        return .judged(
            [
                ("a reflection given", reflection != nil), ("a, b, out, counter: buffers 0-3", named),
            ], metrics, proof: gpu.proof)
    }
}
