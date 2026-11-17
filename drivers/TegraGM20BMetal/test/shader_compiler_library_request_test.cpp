// SPDX-License-Identifier: MIT
// A library the system's Metal front end makes (raw/runtime/raw_compile_request.h RequestKind::Library) as the client
// asks for it and the compiler takes it (compiler/compiler_request.h): keyed by its code generation request
// (raw_codegen_request.h), which the compiler takes only exactly as a client's builder writes it, its source options
// only what Metal passes, run with the compiler's own module cache.
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_codegen_request.h"
#include "MetallibSlice.h"
#include "made_metallib.h"

#include <gtest/gtest.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sstream>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

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
TEST(LibraryRequest, ALibraryIsKeyedByItsCodeGenerationRequest)
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
    staged.stages.resize(1);
    staged.stages[0].tag = staged.stages[0].function = staged.stages[0].name = "k";
    staged.libraries.resize(1);
    staged.libraries[0].bytes = "bytes";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(staged), back)) << "a library request names no stage or library";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(libraryRequest(7, r.codeGen)), back));
}

// Of a source request the compiler takes only the options a client's builder writes: nothing that acts in its process
TEST(LibraryRequest, ASourceRequestTakesOnlyAClientsOptions)
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
TEST(LibraryRequest, TheCompilerRunsASourceRequestWithItsOwnModuleCache)
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
TEST(LibraryRequest, AStitchingRequestIsTakenOnlyAsTheBuilderWritesIt)
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

TEST(LibraryRequest, SpecializationPreservesConstantsNamesAndAirBeforeStitching)
{
    const std::string air = made_metallib::wrappedBitcode(0x41);
    const raw::codegen::Bitcode function{reinterpret_cast<const uint8_t *>(air.data()), air.size()};
    raw::Digest hash;
    hash.fill(0xb0);
    std::string constants;
    raw::codegen::word(constants, 1);
    constants.append("\x03\x00\x21", 3);
    raw::codegen::word(constants, 1);
    const std::string request = raw::codegen::specializationRequest("add", "add_one", function, hash, constants);
    ASSERT_FALSE(request.empty());
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenSpecialize, request));
    std::string why;
    EXPECT_TRUE(prepared(libraryRequest(raw::kCodeGenSpecialize, request), why)) << why;
    EXPECT_EQ(memcmp(request.data() + raw::codegen::kThisKind.specializationHash, hash.data(), hash.size()), 0);
    constants[7] = 2;
    const std::string other = raw::codegen::specializationRequest("add", "add_one", function, hash, constants);
    EXPECT_NE(libraryRequest(raw::kCodeGenSpecialize, request).key, libraryRequest(raw::kCodeGenSpecialize, other).key);
    EXPECT_NE(other, raw::codegen::specializationRequest("add", "add_two", function, hash, constants));
    EXPECT_TRUE(raw::codegen::specializationRequest("", "", function, hash, constants).empty());
    EXPECT_TRUE(raw::codegen::specializationRequest("add", std::string("a\0b", 3), function, hash, constants).empty());
    const std::string_view forged("add", SIZE_MAX);
    EXPECT_TRUE(raw::codegen::specializationRequest(forged, "", function, hash, constants).empty());
    EXPECT_TRUE(raw::codegen::specializationRequest("add", forged, function, hash, constants).empty());
    EXPECT_TRUE(raw::codegen::specializationRequest("add", "", function, hash, forged).empty());
    EXPECT_TRUE(raw::codegen::specializationRequest("add", "", {function.bytes, SIZE_MAX}, hash, constants).empty());
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenSpecialize, request + std::string(8, '\0')));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenSpecialize, request.substr(0, request.size() - 1)));
    for (const size_t at : {size_t(12), size_t(0x5c), size_t(0x64), size_t(raw::codegen::kThisKind.specializedName + 4),
                            size_t(raw::codegen::kThisKind.functionHeader)}) {
        std::string malformed = request;
        malformed[at] ^= 0x7f;
        EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenSpecialize, malformed), why)) << at;
    }
    std::string malformed = request;
    malformed[raw::codegen::kThisKind.functionHeader + 6] = 0;
    EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenSpecialize, malformed), why));
    raw::codegen::word(constants, 2);
    EXPECT_FALSE(raw::codegen::constantDataOk(constants));
    constants.resize(11);
    constants.replace(0, 4, std::string("\x02\0\0\0", 4));
    constants += constants.substr(4);
    EXPECT_FALSE(raw::codegen::constantDataOk(constants));
}

TEST(LibraryRequest, PackedConstantsAcceptEveryDistinctUInt16Index)
{
    std::string constants;
    raw::codegen::word(constants, uint32_t(UINT16_MAX) + 1);
    for (uint32_t i = 0; i <= UINT16_MAX; i++) {
        const uint16_t index = uint16_t(i);
        constants.append(reinterpret_cast<const char *>(&index), sizeof index);
        constants.append("\x35\x01", 2);
    }
    EXPECT_TRUE(raw::codegen::constantDataOk(constants));
    const uint32_t repeated = uint32_t(UINT16_MAX) + 2;
    memcpy(constants.data(), &repeated, sizeof repeated);
    constants += constants.substr(constants.size() - 4);
    EXPECT_FALSE(raw::codegen::constantDataOk(constants));
}

// The current macOS 32023/32024 services use a 0x128 header and HASH at 0xf0, unlike either pinned iOS reader.
// Reframe only this host probe; the facade and request-validation tests retain the device's exact bytes.
std::string hostSpecializationRequest(const std::string &request)
{
    if (!raw::codegen::specializationRequestOk(request))
        return {};
    const auto &device = raw::codegen::kThisKind;
    std::string host(0x128, '\0');
    memcpy(host.data(), request.data(), 0x68);
    memcpy(host.data() + 0xa8, request.data() + device.specializedName, 8);
    memcpy(host.data() + 0xd8, request.data() + device.functionType, 8);
    memcpy(host.data() + 0xf0, request.data() + device.specializationHash, 32);
    host.append(request, device.functionHeader, std::string::npos);
    for (const size_t at : {size_t(8), size_t(0x58), size_t(0x60), size_t(0xa8)}) {
        uint32_t offset = 0;
        raw::codegen::wordAt(host, at, offset);
        if (offset) {
            offset += 0x128 - device.functionHeader;
            memcpy(host.data() + at, &offset, 4);
        }
    }
    return host;
}

TEST(LibraryRequest, TheHostProbePreservesTheDeviceSpecializationPayload)
{
    const std::string air = made_metallib::wrappedBitcode(0x41);
    raw::Digest hash;
    hash.fill(0xb0);
    std::string constants;
    raw::codegen::word(constants, 1);
    constants.append("\x03\x00\x21", 3);
    raw::codegen::word(constants, 2);
    for (const std::string name : {"", "specialized_add"}) {
        const std::string request = raw::codegen::specializationRequest(
            "add", name, {reinterpret_cast<const uint8_t *>(air.data()), air.size()}, hash, constants);
        const std::string host = hostSpecializationRequest(request);
        ASSERT_FALSE(host.empty());
        EXPECT_EQ(host.substr(0x128), request.substr(raw::codegen::kThisKind.functionHeader));
        EXPECT_EQ(memcmp(host.data() + 0xf0, hash.data(), hash.size()), 0);
        for (const auto offsets : {std::pair{size_t(8), size_t(8)},
                                   {size_t(0x58), size_t(0x58)},
                                   {size_t(0x60), size_t(0x60)},
                                   {size_t(raw::codegen::kThisKind.specializedName), size_t(0xa8)}}) {
            uint32_t before = 0, after = 0;
            ASSERT_TRUE(raw::codegen::wordAt(request, offsets.first, before));
            ASSERT_TRUE(raw::codegen::wordAt(host, offsets.second, after));
            EXPECT_EQ(after, before ? before + 0x128 - raw::codegen::kThisKind.functionHeader : 0);
        }
        EXPECT_EQ(memcmp(host.data() + 0xd8, request.data() + raw::codegen::kThisKind.functionType, 8), 0);
        EXPECT_TRUE(hostSpecializationRequest(request.substr(0, request.size() - 1)).empty());
    }
}

std::string metallibSummary(const std::string &library)
{
    std::ostringstream out;
    out << "reply bytes=" << library.size() << " header=";
    for (size_t at = 0; at + 4 <= std::min<size_t>(library.size(), 88); at += 4) {
        uint32_t word;
        memcpy(&word, library.data() + at, 4);
        out << std::hex << word << ',';
    }
    const bool valid = switch_metal::forEachMetallibEntry(
        reinterpret_cast<const uint8_t *>(library.data()), library.size(), [&](const switch_metal::MetallibEntry &e) {
            out << " function=" << e.name << " type=" << unsigned(e.type.value_or(255)) << " placed=" << e.placed
                << " air=" << e.bitcodeOffset << '+' << e.bitcodeBytes;
            return true;
        });
    out << " valid=" << valid;
    return out.str();
}

void *nativeFrontendSpecialization(void *)
{
    @autoreleasepool {
        const auto run = [] {
            const char *path =
                raw::codegen::kThisKind.kind == "21H461"
                    ? "/System/Library/PrivateFrameworks/MTLCompiler.framework/Versions/32023/MTLCompiler"
                    : "/System/Library/PrivateFrameworks/MTLCompiler.framework/Versions/32024/MTLCompiler";
            void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            ASSERT_NE(library, nullptr) << dlerror();
            using Reply = void (^)(int32_t, const void *, size_t, const char *);
            using Build = void (*)(void *, void *, uint32_t, const void *, size_t, Reply);
            const auto create = reinterpret_cast<void *(*)(const char *)>(dlsym(library, "MTLCodeGenServiceCreate"));
            const auto build = reinterpret_cast<Build>(dlsym(library, "MTLCodeGenServiceBuildRequest"));
            ASSERT_NE(create, nullptr);
            ASSERT_NE(build, nullptr);
            Dl_info image{};
            ASSERT_NE(dladdr(reinterpret_cast<const void *>(build), &image), 0);
            ASSERT_STREQ(image.dli_fname, path);
            void *service = create("SwitchCompilerProtocolTest");
            ASSERT_NE(service, nullptr);
            NSString *modules = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
            ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:modules
                                                  withIntermediateDirectories:YES
                                                                   attributes:nil
                                                                        error:nil]);
            const auto compile = [&](uint32_t type, const std::string &request) {
                std::string result, log;
                bool called = false;
                auto *out = &result, *failure = &log;
                bool *answered = &called;
                const std::string sent = type == raw::kCodeGenSource
                                             ? raw::codegen::withModuleCache(request, modules.UTF8String)
                                         : type == raw::kCodeGenSpecialize ? hostSpecializationRequest(request)
                                                                           : request;
                build(service, nullptr, type, sent.data(), sent.size(),
                      ^(int32_t status, const void *reply, size_t bytes, const char *message) {
                          *answered = true;
                          if (message)
                              *failure = message;
                          uint32_t header = 0, warnings = 0;
                          if (reply && bytes >= 16) {
                              memcpy(&header, static_cast<const uint8_t *>(reply) + 8, 4);
                              memcpy(&warnings, static_cast<const uint8_t *>(reply) + 12, 4);
                          }
                          EXPECT_EQ(status, 0) << *failure;
                          if (!status && header >= 16 && uint64_t(header) + warnings < bytes)
                              out->assign(static_cast<const char *>(reply) + header + warnings,
                                          bytes - header - warnings);
                      });
                EXPECT_TRUE(called);
                EXPECT_FALSE(result.empty()) << "request type " << type << ": " << log;
                return result;
            };
            const std::string source = "#include <metal_stdlib>\nusing namespace metal;\n"
                                       "constant uint gain [[function_constant(3)]];\n"
                                       "[[stitchable]] float add(float a, float b) { return a * float(gain) + b; }\n";
            const std::string original = compile(raw::kCodeGenSource, raw::codegen::sourceRequest(source, kOptions));
            ASSERT_FALSE(original.empty());
            raw::codegen::Bitcode air;
            ASSERT_TRUE(raw::codegen::functionBitcode(reinterpret_cast<const uint8_t *>(original.data()),
                                                      original.size(), "add", air));
            std::map<std::string, raw::AirHash> hashes;
            ASSERT_TRUE(
                raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(original.data()), original.size(), hashes));
            ASSERT_EQ(hashes.count("add"), 1u);
            for (const std::string name : {"add", "specialized_add"}) {
                std::vector<raw::AirHash> results;
                const std::string dag =
                    "DAGS {\nstitched {\nOUTPUT(value) : FUNCTION(" + name + "), INPUT(0), INPUT(1);\n}\n}\n";
                for (const uint32_t value : {1u, 2u, 1u}) {
                    SCOPED_TRACE(::testing::Message() << "function=" << name << " gain=" << value);
                    std::string constants;
                    raw::codegen::word(constants, 1);
                    constants.append("\x03\x00\x21", 3);
                    raw::codegen::word(constants, value);
                    const std::string specialized = compile(
                        raw::kCodeGenSpecialize, raw::codegen::specializationRequest("add", name == "add" ? "" : name,
                                                                                     air, hashes["add"], constants));
                    ASSERT_FALSE(specialized.empty());
                    raw::codegen::Bitcode specializedAir;
                    ASSERT_TRUE(raw::codegen::functionBitcode(reinterpret_cast<const uint8_t *>(specialized.data()),
                                                              specialized.size(), name, specializedAir))
                        << metallibSummary(specialized);
                    const std::string stitched =
                        compile(raw::kCodeGenStitch, raw::codegen::stitchRequest(dag, {specializedAir}));
                    ASSERT_FALSE(stitched.empty());
                    std::map<std::string, raw::AirHash> output;
                    ASSERT_TRUE(raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(stitched.data()),
                                                       stitched.size(), output));
                    ASSERT_EQ(output.count("stitched"), 1u);
                    results.push_back(output["stitched"]);
                }
                EXPECT_NE(results[0], results[1]);
                EXPECT_EQ(results[0], results[2]);
            }
            [[NSFileManager defaultManager] removeItemAtPath:modules error:nil];
        };
        run();
    }
    return nullptr;
}

TEST(LibraryRequest, TheNativeAppleFrontendSpecializesRealAirBeforeStitching)
{
    pthread_attr_t attributes;
    ASSERT_EQ(pthread_attr_init(&attributes), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attributes, 16u << 20), 0);
    pthread_t thread;
    const int made = pthread_create(&thread, &attributes, nativeFrontendSpecialization, nullptr);
    pthread_attr_destroy(&attributes);
    ASSERT_EQ(made, 0);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
}

// A filter link request is taken only as ImageFilterLink.h writes it: its functions, their infos and each module once
TEST(LibraryRequest, AFilterLinkRequestIsTakenOnlyAsTheBuilderWritesIt)
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

}  // namespace
