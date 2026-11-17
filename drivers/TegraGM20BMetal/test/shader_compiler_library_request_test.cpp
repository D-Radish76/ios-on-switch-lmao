// SPDX-License-Identifier: MIT
// A library the system's Metal front end makes (raw/runtime/raw_compile_request.h RequestKind::Library) as the client
// asks for it and the compiler takes it (compiler/compiler_request.h): keyed by its code generation request
// (raw_codegen_request.h), which the compiler takes only framed as a client's builder writes it, its source options
// only what Metal passes, run with the compiler's own module cache.
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_codegen_request.h"
#include "MetallibSlice.h"
#include "made_metallib.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <utility>

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

const uint8_t *bytesOf(const std::string &s)
{
    return reinterpret_cast<const uint8_t *>(s.data());
}

// One value as MTLFunctionConstantValues serializes it for a function: the count, then its index, type and bytes
std::string serialized(uint16_t index, MTLDataType type, const void *value, size_t bytes)
{
    std::string constants;
    raw::codegen::word(constants, 1);
    constants.append(reinterpret_cast<const char *>(&index), sizeof index);
    constants.push_back(char(type));
    constants.append(static_cast<const char *>(value), bytes);
    return constants;
}

std::string gain(uint32_t value)
{
    return serialized(3, MTLDataTypeUInt, &value, sizeof value);
}

uint32_t wordOf(const std::string &r, size_t at)
{
    uint32_t v = 0;
    EXPECT_TRUE(raw::codegen::wordAt(r, at, v)) << at;
    return v;
}

uint16_t halfOf(const std::string &r, size_t at)
{
    uint16_t v = 0;
    memcpy(&v, r.data() + at, sizeof v);
    return v;
}

// A specialization request holds each field where this kind's table (MetalABI.h kSpecializeRequest) says its Metal writes
// it and its front end reads it: the constants right after the header, the source name with its byte count and an AIR
// bitcode kind, the bitcode, the specialized name NUL-padded to 8 bytes, the request type twice and the source's hash
TEST(LibraryRequest, ASpecializationRequestHoldsEachFieldWhereThisKindsTableSays)
{
    const SpecializeRequestLayout &l = kSpecializeRequest;
    const std::string air = made_metallib::wrappedBitcode(0x41, 52), constants = gain(7);
    raw::Digest hash;
    hash.fill(0xB0);
    const std::string r =
        raw::codegen::specializationRequest("add", "add_one", {bytesOf(air), air.size()}, hash, constants);
    ASSERT_GT(r.size(), l.headerBytes);
    EXPECT_EQ(wordOf(r, l.constants), l.headerBytes);
    EXPECT_EQ(wordOf(r, l.constants + 4), constants.size());
    EXPECT_EQ(r.substr(l.headerBytes, constants.size()), constants);
    EXPECT_STREQ(r.data() + wordOf(r, l.name), "add");
    EXPECT_EQ(halfOf(r, l.name + 4), 4u);
    EXPECT_EQ(halfOf(r, l.name + 6), 0u);
    EXPECT_EQ(r.substr(wordOf(r, l.air), wordOf(r, l.air + 4)), air);
    EXPECT_EQ(r.substr(wordOf(r, l.specializedName), wordOf(r, l.specializedName + 4)), std::string("add_one\0", 8));
    EXPECT_EQ(wordOf(r, l.requestType), raw::kCodeGenSpecialize);
    EXPECT_EQ(wordOf(r, l.requestType + 4), raw::kCodeGenSpecialize);
    EXPECT_EQ(memcmp(r.data() + l.hash, hash.data(), hash.size()), 0);
    for (const size_t field : {l.constants, l.name, l.air, l.specializedName})
        EXPECT_EQ(wordOf(r, field) % 8, 0u) << field;
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenSpecialize, r));
    std::string why;
    EXPECT_TRUE(prepared(libraryRequest(raw::kCodeGenSpecialize, r), why)) << why;

    const std::string own = raw::codegen::specializationRequest("add", "", {bytesOf(air), air.size()}, hash, constants);
    EXPECT_EQ(wordOf(own, l.specializedName), 0u);
    EXPECT_EQ(wordOf(own, l.specializedName + 4), 0u);
    EXPECT_TRUE(raw::codegen::requestOk(raw::kCodeGenSpecialize, own));
    const std::string other = raw::codegen::specializationRequest("add", "", {bytesOf(air), air.size()}, hash, gain(8));
    EXPECT_NE(libraryRequest(raw::kCodeGenSpecialize, own).key, libraryRequest(raw::kCodeGenSpecialize, other).key);

    EXPECT_TRUE(raw::codegen::specializationRequest("", "", {bytesOf(air), air.size()}, hash, constants).empty());
    EXPECT_TRUE(raw::codegen::specializationRequest(std::string_view("a\0d", 3), "", {bytesOf(air), air.size()}, hash,
                                                    constants)
                    .empty());
    EXPECT_TRUE(
        raw::codegen::specializationRequest("add", "", {bytesOf(air), air.size()}, hash, constants + "x").empty());
    const std::string bare(64, 'b');
    EXPECT_TRUE(raw::codegen::specializationRequest("add", "", {bytesOf(bare), bare.size()}, hash, constants).empty());
}

// The compiler takes a specialization request only with every field in place and inside it, and every other header byte
// zero: one set would ask the front end to insert into an archive, run its loader or read a path
TEST(LibraryRequest, ASpecializationRequestIsTakenOnlyWithEachFieldInPlace)
{
    const SpecializeRequestLayout &l = kSpecializeRequest;
    const std::string air = made_metallib::wrappedBitcode(0x41, 52);
    raw::Digest hash;
    hash.fill(0xB0);
    const std::string r =
        raw::codegen::specializationRequest("add", "add_one", {bytesOf(air), air.size()}, hash, gain(7));
    ASSERT_TRUE(raw::codegen::requestOk(raw::kCodeGenSpecialize, r));
    const auto taken = [](const std::string &request) {
        return raw::codegen::requestOk(raw::kCodeGenSpecialize, request);
    };
    std::vector<bool> written(l.headerBytes);
    for (const auto &[field, bytes] : {std::pair<size_t, size_t>{l.constants, 8},
                                       {l.name, 6},
                                       {l.air, 8},
                                       {l.specializedName, 8},
                                       {l.requestType, 8},
                                       {l.hash, sizeof(raw::Digest)}})
        std::fill_n(written.begin() + ptrdiff_t(field), bytes, true);
    for (size_t at = 0; at < l.headerBytes; at++)
        if (!written[at]) {
            std::string flagged = r;
            flagged[at] = 1;
            EXPECT_FALSE(taken(flagged)) << at;
        }
    const auto with = [&](size_t at, uint32_t v) {
        std::string changed = r;
        memcpy(changed.data() + at, &v, sizeof v);
        return changed;
    };
    EXPECT_FALSE(taken(r.substr(0, l.headerBytes)));
    EXPECT_FALSE(taken(r.substr(0, r.size() - 8)));
    EXPECT_FALSE(taken(with(l.constants, l.headerBytes + 8)));
    EXPECT_FALSE(taken(with(l.constants + 4, wordOf(r, l.constants + 4) + 1)));
    EXPECT_FALSE(taken(with(l.air + 4, uint32_t(r.size()))));
    EXPECT_FALSE(taken(with(l.requestType, raw::kCodeGenStitch)));
    EXPECT_FALSE(taken(with(l.requestType + 4, 0)));
    EXPECT_FALSE(taken(with(l.specializedName + 4, 0)));
    std::string unterminated = r;
    unterminated[wordOf(r, l.name) + 3] = 'x';
    EXPECT_FALSE(taken(unterminated));
    std::string longer = r;
    const uint16_t nameBytes = 5;
    memcpy(longer.data() + l.name + 4, &nameBytes, sizeof nameBytes);
    EXPECT_FALSE(taken(longer));
    std::string unwrapped = r;
    unwrapped[wordOf(r, l.air)] ^= 0x7f;
    EXPECT_FALSE(taken(unwrapped));
    std::string unnamed = r;
    std::fill_n(unnamed.begin() + ptrdiff_t(wordOf(r, l.specializedName)), 8, 'x');
    EXPECT_FALSE(taken(unnamed));
    std::string counted = r;
    counted[l.headerBytes] = 2;
    EXPECT_FALSE(taken(counted));
    std::string untyped = r;
    untyped[l.headerBytes + 6] = char(MTLDataTypeNone);
    EXPECT_FALSE(taken(untyped));
    std::string why;
    EXPECT_FALSE(prepared(libraryRequest(raw::kCodeGenSpecialize, untyped), why));
    EXPECT_FALSE(raw::codegen::requestOk(raw::kCodeGenStitch, r));
}

// Constant values are taken as Metal serializes them, each value as many bytes as Metal stores its type in
TEST(LibraryRequest, ConstantValuesAreTakenAsMetalSerializesThem)
{
    EXPECT_TRUE(raw::codegen::constantDataOk(std::string(4, '\0')));
    const float tint[4] = {1, 0, 0, 1};
    const bool on = true;
    std::string three = gain(2);
    three[0] = 3;
    three += serialized(0, MTLDataTypeFloat4, tint, sizeof tint).substr(4);
    three += serialized(9, MTLDataTypeBool, &on, sizeof on).substr(4);
    EXPECT_TRUE(raw::codegen::constantDataOk(three));
    EXPECT_FALSE(raw::codegen::constantDataOk(three + "x"));
    EXPECT_FALSE(raw::codegen::constantDataOk(three.substr(0, three.size() - 1)));
    std::string four = three;
    four[0] = 4;
    EXPECT_FALSE(raw::codegen::constantDataOk(four));
    EXPECT_FALSE(raw::codegen::constantDataOk(serialized(1, MTLDataTypeNone, tint, 4)));
    EXPECT_FALSE(raw::codegen::constantDataOk(std::string(3, '\0')));
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
