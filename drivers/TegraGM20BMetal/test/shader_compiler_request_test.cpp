// SPDX-License-Identifier: MIT
// A compile request (raw/runtime/raw_compile_request.h) as the client makes it and the compiler takes it
// (compiler/compiler_request.h): it round-trips, its fields key a descriptor as the descriptor itself keys, the compiler
// derives the runtime's own key from it and refuses one whose claimed key differs, and the store name binds what the
// key does not (a library's bytes, rasterization, a kernel's dispatch). A library the front end makes is keyed by its code
// generation request (raw_codegen_request.h), which the compiler takes only as a client's builder writes it.
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_codegen_request.h"
#include "../raw/runtime/raw_compute_key.h"
#include "MetallibSlice.h"
#include "made_metallib.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

raw::RequestStage stage(const char *tag, const char *function, uint32_t library = 0, const char *constants = "")
{
    raw::RequestStage s;
    s.tag = tag;
    s.function = function;
    s.name = function;
    s.constants = constants;
    s.specialized = constants[0] != 0;
    s.library = library;
    return s;
}

raw::RequestLibrary data(const std::string &bytes)
{
    raw::RequestLibrary l;
    l.bytes = bytes;
    return l;
}

raw::RequestLibrary systemLibrary(const char *path)
{
    raw::RequestLibrary l;
    l.path = path;
    l.system = true;
    return l;
}

MTLRenderPipelineDescriptor *descriptor()
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.rasterSampleCount = 4;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.rasterizationEnabled = NO;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA16Float;
    d.colorAttachments[2].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskAlpha;
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    v.attributes[0].format = MTLVertexFormatFloat2;
    v.attributes[0].bufferIndex = 1;
    v.attributes[3].format = MTLVertexFormatUChar4Normalized;
    v.attributes[3].offset = 8;
    v.attributes[3].bufferIndex = 3;
    v.layouts[1].stride = 8;
    v.layouts[3].stride = 20;
    v.layouts[3].stepFunction = MTLVertexStepFunctionPerInstance;
    v.layouts[3].stepRate = 2;
    d.vertexDescriptor = v;
    return d;
}

// A render request over a made-up library holding vertex_fn (HASH 0xA0...) and fragment_fn (0xA1...)
raw::CompileRequest renderRequest()
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.fields = raw::fieldsOf(descriptor());
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn", "fragment_fn"})));
    r.stages = {stage("v", "vertex_fn"), stage("f", "fragment_fn", 0, "#0:33:05000000")};
    return r;
}

// The CacheKey the runtime makes for `r`'s state (RawComplete.mm keyOf over RawRenderKey.mm)
raw::CacheKey runtimeKey(const raw::CompileRequest &r)
{
    bool own = false;
    const std::string key = raw::requestPackKey(r, own);
    raw::AirHash vertex, fragment;
    vertex.fill(0xA0);
    fragment.fill(0xA1);
    return raw::renderCacheKey(r.stack, key, vertex, fragment, {}, own);
}

std::vector<raw::LibraryIdentity> identities(const raw::CompileRequest &r)
{
    std::vector<raw::LibraryIdentity> out;
    for (const raw::RequestLibrary &l : r.libraries)
        out.push_back(raw::identityOf(l));
    return out;
}

TEST(CompileRequest, RoundTripsWhole)
{
    raw::CompileRequest r = renderRequest();
    r.stages.push_back(stage("fl", "fragment_fn", 0));
    r.cubic.samplers.push_back({2, 1, 1, 0, 3, true});
    r.cubic.textures.push_back({2, 80});
    r.fragmentTests = 3;
    r.key = "a key";
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));

    raw::CompileRequest k;
    k.stack = kStack;
    k.kind = raw::RequestKind::Kernel;
    k.key = "kernel | a key";
    k.libraries.push_back(data("bytes"));
    k.stages = {stage("k", "kernel_fn")};
    k.kernel.maxThreads = 256;
    k.kernel.threads[0] = 64;
    k.kernel.memory[2] = 1024;
    k.kernel.textures.push_back({1, 80, 2, 3, 1, 1, 1});
    NmbDirectBoundSampler sampler = {};
    sampler.slot = 4;
    sampler.descriptor.max_anisotropy = 8;
    k.kernel.samplers.push_back(sampler);
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(k), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(k));
    EXPECT_EQ(back.kernel.dispatch().memory[2], 1024u);
}

TEST(CompileRequest, ItsFieldsKeyADescriptorAsTheDescriptorKeysItself)
{
    MTLRenderPipelineDescriptor *d = descriptor(), *made = [MTLRenderPipelineDescriptor new];
    const raw::PipelineFields fields = raw::fieldsOf(d);
    raw::applyFields(fields, made);
    const raw::StageKey v{"vertex_fn", ""}, f{"fragment_fn", "#0:33:05000000"};
    EXPECT_EQ(raw::pipelineKey(made, &v, &f, "", ""), raw::pipelineKey(d, &v, &f, "", ""));
    EXPECT_EQ(raw::fieldsOf(made).rasterization, 0u);
    EXPECT_FALSE(made.rasterizationEnabled);
}

TEST(CompileRequest, ItsPackKeyIsTheFacadesForFrameworkAndOwnLibraries)
{
    raw::CompileRequest r = renderRequest();
    r.libraries = {systemLibrary("/System/Library/Frameworks/RenderBox.framework/default.metallib")};
    bool own = true;
    const raw::StageKey v{"vertex_fn", ""}, f{"fragment_fn", "#0:33:05000000"};
    EXPECT_EQ(raw::requestPackKey(r, own), raw::pipelineKey(descriptor(), &v, &f, "", "RenderBox"));
    EXPECT_FALSE(own);
    r.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    r.fragmentTests = 2;
    EXPECT_EQ(raw::requestPackKey(r, own),
              raw::pipelineKey(descriptor(), &v, &f, "", "RenderBox") + r.cubic.key() + " | tests=2");
    raw::CompileRequest data = renderRequest();
    EXPECT_EQ(raw::requestPackKey(data, own), raw::pipelineKey(descriptor(), &v, &f, "", ""));
    EXPECT_TRUE(own);
}

TEST(CompileRequest, TheCompilerKeysItAsTheRuntimeDoes)
{
    raw::CompileRequest r = renderRequest();
    const raw::CacheKey k = runtimeKey(r);
    r.key = k.key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_EQ(memcmp(p.key.vertexAir, k.vertexAir, 32), 0);
    EXPECT_EQ(memcmp(p.key.fragmentAir, k.fragmentAir, 32), 0);
    EXPECT_EQ(p.name, raw::storeName(k, r, identities(r)));
}

TEST(CompileRequest, OneWhoseClaimedKeyDiffersIsRefused)
{
    raw::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key + " ";
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why));
    EXPECT_NE(why.find("another key"), std::string::npos) << why;
    r.key = runtimeKey(r).key;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(r), kStack + 1, system, p, why));
}

TEST(CompileRequest, TheStoreNameBindsLibraryBytesAndRasterization)
{
    raw::CompileRequest r = renderRequest();
    const raw::CacheKey k = runtimeKey(r);
    const raw::Digest name = raw::storeName(k, r, identities(r));
    EXPECT_EQ(raw::storeName(k, r, identities(r)), name);
    raw::CompileRequest alike = r;
    alike.libraries[0] = data(made_metallib::universal({"vertex_fn", "fragment_fn"}, 0xA0, 0x11));
    EXPECT_EQ(runtimeKey(alike).key, k.key);
    EXPECT_NE(raw::storeName(k, alike, identities(alike)), name);
    raw::CompileRequest rasterizing = r;
    rasterizing.fields.rasterization = 1;
    EXPECT_NE(raw::storeName(k, rasterizing, identities(rasterizing)), name);
}

TEST(CompileRequest, AKernelIsKeyedByItsFunctionAndNamedByItsDispatch)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Kernel;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.maxThreads = 1024;
    r.kernel.threads[0] = 64;
    r.kernel.threads[1] = r.kernel.threads[2] = 1;
    r.kernel.textures.push_back({0, 80, 2, 0x17, 1, 1, 1});
    raw::AirHash air;
    air.fill(0x30);
    const std::string compute = raw::computeKey(air, "", 1024, false, r.kernel.threads, "", "0:80:2:1:1:00010203", "");
    r.key = "kernel | " + compute;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, compute);
    EXPECT_EQ(p.key.key, raw::kernelCacheKey(kStack, compute).key);

    raw::CompileRequest used = r;
    used.kernel.textures[0].usage = 0x13;
    EXPECT_NE(raw::storeName(p.key, used, identities(used)), p.name);

    raw::CompileRequest region = r;
    region.kernel.regionMode = true;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(region), kStack, system, p, why));
    raw::CompileRequest other = r;
    other.libraries[0] = data(made_metallib::universal({"kernel_fn"}, 0x31));
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(other), kStack, system, p, why));
}

// A compute state's facts: keyed by its function's AIR, constants and limits as the pack keys PackComputeFacts, named apart
// from a kernel request of the same function and limits, and refused when its claimed key names another state
TEST(CompileRequest, ComputeFactsAreKeyedAsThePacksStateKey)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn", 0, "#0:33:05000000")};
    r.kernel.maxThreads = 256;
    r.kernel.multipleOfWidth = true;
    raw::AirHash air;
    air.fill(0x30);
    const std::string state = raw::computeStateKey(air, "#0:33:05000000", 256, true);
    r.key = raw::factsCacheKey(kStack, state).key;
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(back.kind, raw::RequestKind::Facts);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    EXPECT_EQ(p.key.key, "facts | " + state);

    raw::CompileRequest kernel = r;
    kernel.kind = raw::RequestKind::Kernel;
    EXPECT_NE(raw::storeName(p.key, kernel, identities(kernel)), p.name);
    raw::CompileRequest wider = r;
    wider.kernel.maxThreads = 512;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(wider), kStack, system, p, why));
    raw::CompileRequest two = r;
    two.stages.push_back(stage("k", "kernel_fn"));
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(two), back)) << "a facts request names one function";
}

raw::CompileRequest libraryRequest(uint32_t type, const std::string &codeGen)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Library;
    r.codeGenType = type;
    r.codeGen = codeGen;
    r.key = raw::libraryCacheKey(kStack, type, codeGen).key;
    return r;
}

const std::string kOptions = "--driver-mode=metal -x metal -std=metal3.1 -ffast-math";

bool prepared(const raw::CompileRequest &r, std::string &why)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    return switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why);
}

// A library's request carries the code generation request alone and is keyed by its type and bytes; it names no stage
// or library, and only the front end's request types
TEST(CompileRequest, ALibraryIsKeyedByItsCodeGenerationRequest)
{
    const raw::CompileRequest r =
        libraryRequest(raw::kCodeGenSource, raw::codegen::sourceRequest("kernel void k() {}", kOptions));
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));
    EXPECT_EQ(back.codeGen, r.codeGen);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key.rfind("library | 13 | ", 0), 0u) << p.key.key;
    EXPECT_EQ(p.name, raw::storeName(p.key, r, {}));

    const raw::CompileRequest other =
        libraryRequest(raw::kCodeGenSource, raw::codegen::sourceRequest("kernel void j() {}", kOptions));
    EXPECT_NE(other.key, r.key);
    raw::CompileRequest claimed = r;
    claimed.key = other.key;
    EXPECT_FALSE(prepared(claimed, why));
    EXPECT_NE(why.find("another key"), std::string::npos) << why;
    raw::CompileRequest staged = r;
    staged.stages = {stage("k", "k")};
    staged.libraries.push_back(data("bytes"));
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(staged), back)) << "a library request names no stage or library";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(libraryRequest(7, r.codeGen)), back));
}

// Of a source request the compiler takes only the options a client's builder writes: nothing that acts in its process
TEST(CompileRequest, ASourceRequestTakesOnlyAClientsOptions)
{
    std::string why;
    const auto takes = [&](const std::string &options, const std::string &source = "kernel void k() {}") {
        return prepared(libraryRequest(raw::kCodeGenSource, raw::codegen::sourceRequest(source, options)), why);
    };
    EXPECT_TRUE(takes(kOptions)) << why;
    EXPECT_TRUE(
        takes(kOptions + " -fpreserve-invariance -Os -DTILE=16 " + raw::codegen::quotedArgument("-DNAME=\"a b\\'c\"")))
        << why;
    EXPECT_TRUE(takes("--driver-mode=metal -x metal -std=ios-metal2.4 -fno-fast-math")) << why;
    for (const char *extra :
         {" -fmodules-cache-path=/private/var/mobile", " -fplugin=/tmp/x.dylib", " -Xclang -load", " -o /tmp/out",
          "  -Os", " -Os ", " \"-Os\"x", " -DA=1\n#include", " -D1X=2", " '-Os'", " \"-DA=\\n\""})
        EXPECT_FALSE(takes(kOptions + extra)) << extra;
    EXPECT_FALSE(takes("--driver-mode=metal -x c -std=metal3.1 -ffast-math"));
    EXPECT_FALSE(takes("--driver-mode=metal -x metal -std=metal3 -ffast-math"));
    std::string padded = raw::codegen::sourceRequest("kernel void k() {}", kOptions);
    padded[16 + 19] = 'x';
    EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenSource, padded), why)) << "a byte past the source's NUL";
}

// The compiler runs a source request with a module cache of its own, the only place its sandbox lets the front end write
TEST(CompileRequest, TheCompilerRunsASourceRequestWithItsOwnModuleCache)
{
    const std::string request = raw::codegen::sourceRequest("kernel void k() {}", kOptions);
    const std::string run = raw::codegen::withModuleCache(request, "/private/var/Switch/shaders/modules");
    std::string_view source, options;
    ASSERT_TRUE(raw::codegen::readSource(run, source, options));
    EXPECT_EQ(source, "kernel void k() {}");
    EXPECT_EQ(options, kOptions + " -fmodules-cache-path=/private/var/Switch/shaders/modules");
    const std::string spaced = raw::codegen::withModuleCache(request, "/a b");
    ASSERT_TRUE(raw::codegen::readSource(spaced, source, options));
    std::vector<std::string> arguments;
    ASSERT_TRUE(raw::codegen::splitArguments(options, arguments));
    EXPECT_EQ(arguments.back(), "-fmodules-cache-path=/a b");
}

// A stitching request is the DAG, each function's bitcode as its metallib places it and this kind's tail, taken only
// exactly as the builder writes them
TEST(CompileRequest, AStitchingRequestIsTakenOnlyAsTheBuilderWritesIt)
{
    const std::string add = made_metallib::wrappedBitcode(0x41), mul = made_metallib::wrappedBitcode(0x42, 96);
    const std::string library = made_metallib::thin({"add", "mul"}, {4, 4}, {add, mul});
    const auto *bytes = reinterpret_cast<const uint8_t *>(library.data());
    raw::codegen::Bitcode a, m;
    ASSERT_TRUE(raw::codegen::functionBitcode(bytes, library.size(), "add", a));
    ASSERT_TRUE(raw::codegen::functionBitcode(bytes, library.size(), "mul", m));
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(a.bytes), a.size), add);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(m.bytes), m.size), mul);
    raw::codegen::Bitcode none;
    EXPECT_FALSE(raw::codegen::functionBitcode(bytes, library.size(), "sub", none));
    const std::string placeless = made_metallib::universal({"add"}, 0xA0, 0xEE, {4});
    size_t at = 0, size = 0;
    ASSERT_TRUE(
        switch_metal::airSlice(reinterpret_cast<const uint8_t *>(placeless.data()), placeless.size(), at, size));
    EXPECT_FALSE(
        raw::codegen::functionBitcode(reinterpret_cast<const uint8_t *>(placeless.data()) + at, size, "add", none));

    const std::string dag = "DAGS {\nstitched {\nOUTPUT(node_output) : FUNCTION(mul), INPUT(0), INPUT(1);\n}\n}\n";
    const std::string request = raw::codegen::stitchRequest(dag, {a, m});
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenStitch, request));
    uint32_t tag = 0;
    memcpy(&tag, request.data(), 4);
    EXPECT_EQ(tag, raw::codegen::fourcc("dag "));
    EXPECT_EQ(request.size(),
              4 + dag.size() + 1 + 8 + 8 + add.size() + 8 + mul.size() + (raw::codegen::kThisKind.stitchTail ? 24 : 0));
    std::string why;
    EXPECT_TRUE(prepared(libraryRequest(raw::kCodeGenStitch, request), why)) << why;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, request + std::string(4, '\0')));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, request.substr(0, request.size() - 4)));
    std::string notBitcode = request;
    notBitcode[4 + dag.size() + 1 + 8 + 8] ^= 0x7f;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, notBitcode));
    std::string more = request;
    more[4 + dag.size() + 1 + 4] = 3;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, more));
    EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenStitch, more), why));
}

// A filter link request is taken only as ImageFilterLink.h writes it: its functions, their infos and each module once
TEST(CompileRequest, AFilterLinkRequestIsTakenOnlyAsTheBuilderWritesIt)
{
    const std::string bitcode = made_metallib::wrappedBitcode(0x51, 40);
    std::vector<switch_metal::ImageFilterFunction> functions(2);
    for (switch_metal::ImageFilterFunction &f : functions) {
        f.bitcode = reinterpret_cast<const uint8_t *>(bitcode.data());
        f.bitcodeBytes = bitcode.size();
    }
    functions[0].name = "tint";
    functions[0].earlyOut = true;
    functions[0].arguments = {{3, {1, 2}}};
    functions[1].name = "swap_rb";
    const std::vector<uint8_t> made = switch_metal::imageFilterLinkRequest(functions);
    const std::string request(made.begin(), made.end());
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenImageFilters, request));
    std::vector<switch_metal::ImageFilterFunction> back;
    ASSERT_TRUE(raw::codegen::readImageFilters(request, back));
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[1].name, "swap_rb");
    EXPECT_TRUE(back[0].earlyOut);
    ASSERT_EQ(back[0].arguments.size(), 1u);
    EXPECT_EQ(back[0].arguments[0].operands[1], 2u);
    std::string why;
    EXPECT_TRUE(prepared(libraryRequest(raw::kCodeGenImageFilters, request), why)) << why;
    std::string flagged = request;
    flagged[0x10] = 1;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenImageFilters, flagged));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenImageFilters, request + std::string(8, '\0')));
    std::string counted = request;
    counted[0] = 3;
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenImageFilters, counted));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenSource, request));
}

TEST(CompileRequest, TheCompilerRefusesWhatItCannotTrust)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    raw::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key;

    raw::CompileRequest claimed = r;
    claimed.libraries[0].system = true;
    claimed.libraries[0].path = "/System/Library/Frameworks/RenderBox.framework/default.metallib";
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(claimed), kStack, system, p, why));

    raw::CompileRequest leaving = r;
    leaving.libraries[0] = systemLibrary("/System/Library/../../private/var/mobile/x.metallib");
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(leaving), kStack, system, p, why));

    raw::CompileRequest empty = r;
    empty.libraries[0].bytes.clear();
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(empty), kStack, system, p, why));

    raw::CompileRequest nowhere = r;
    nowhere.stages[1].library = 3;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(nowhere), kStack, system, p, why));

    std::string cut = raw::encodeRequest(r);
    cut.pop_back();
    EXPECT_FALSE(switch_compiler::prepare(cut, kStack, system, p, why));

    std::string real;
    EXPECT_FALSE(switch_compiler::SystemLibraries::resolve("", "/System/Library/../../etc/x.metallib", real, why));
}

TEST(CompileRequest, ConstantsReadBackAsTheirText)
{
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    const uint32_t word = 0x01020304;
    const float half4[4] = {1, 2, 3, 4};
    [values setConstantValue:&word type:MTLDataTypeUInt atIndex:3];
    [values setConstantValue:half4 type:MTLDataTypeFloat4 withName:@"tint"];
    const std::string text = raw::constantsText(values);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(raw::constantsText(raw::constantValuesOf(text)), text);
    EXPECT_EQ(raw::constantValuesOf("#0:9999:00"), nil);
}

}  // namespace
