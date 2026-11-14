// SPDX-License-Identifier: MIT
// raw's `edge miss pass.draws` lines (raw_gpu.cpp probeEdges) against what Apple's Metal is given and leaves on this Mac's
// GPU: IconServices generates an app icon in this process (ISGenerationRequest -generateImage, iconservicesagent's own
// call) through IconRendering and RenderBox, and every render pass is logged with its first colour attachment, its load
// and the box of its draws' scissors within their viewports, the box raw logs. A load pass whose box stops short of its
// target prints its draws' viewports and scissors.
//
// iOS's IconRendering pool hands RenderBox targets rounded up to 32 texels (boot 22: every iconservicesagent target is a
// multiple of 32, and its edge-miss box is the content), macOS's makes them the content's size. --round32 rounds the
// heap's and the device's 2D single-level render targets the same way here; --start zero|nan then fills each such
// target on the command buffer of its first loading pass (nan: NaN in the 16- and 32-bit float formats, zero elsewhere),
// as the Switch's fresh memory may hold it. The targets come from an MTLHeap whose memory RenderBox aliases, so a fill
// on another queue would land before reads of the memory it reuses. --dump writes the icon as RGBA8 (sRGB,
// premultiplied).
//
// Measured on an M4 Max, macOS 27.2.1, 64 pt at scale 2: with --round32 RenderBox's viewport is the whole target and its
// scissor the content (Settings: 128x128 f80, scissor 0,0 126x125, short right 2 bottom 3 as on the device), so Metal
// leaves the band as it was. The icon over zero-started and NaN-started targets is the exact-size icon to the pixel
// (App Store, Safari, Settings, Calendar, Messages; Photos within the 1 pixel it differs by from run to run): nothing
// reads the band.
//
// usage: iconpassoracle [--round32] [--start zero|nan] [--dump <prefix>] <bundle id> <points> <scale>
// build: xcrun clang++ -O1 -std=c++20 -fobjc-arc iconpassoracle.mm -F/System/Library/PrivateFrameworks
//        -framework IconServices -framework Foundation -framework Metal -framework CoreGraphics -o iconpassoracle
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Pass {
    unsigned tex = 0, w = 0, h = 0, fmt = 0, load = 0, draws = 0;
    double vp[4] = {};
    unsigned sc[4] = {};
    double box[4] = {1e9, 1e9, -1, -1};
    std::vector<std::string> draws_;
};

struct Options {
    bool round32 = false;
    const char *start = nullptr, *dump = nullptr;
};

Options gOptions;
std::mutex gLock;
std::map<std::pair<Class, SEL>, IMP> gOrig;
std::map<void *, Pass> gPasses;
std::map<void *, unsigned> gTextureIds;
std::map<void *, id<MTLTexture>> gUnstarted;
unsigned gCommits = 0, gRounded = 0, gStarted = 0, gShort = 0;

IMP orig(id self, SEL s)
{
    for (Class c = object_getClass(self); c; c = class_getSuperclass(c))
        if (auto it = gOrig.find({c, s}); it != gOrig.end())
            return it->second;
    return nullptr;
}

unsigned textureId(id t)
{
    auto [it, made] = gTextureIds.try_emplace((__bridge void *)t, unsigned(gTextureIds.size() + 1));
    return it->second;
}

void fillStart(id<MTLCommandBuffer> cb, id<MTLTexture> t);

id hRenderEncoder(id self, SEL s, MTLRenderPassDescriptor *d)
{
    for (unsigned j = 0; j < 8; j++) {
        id<MTLTexture> t = d.colorAttachments[j].texture;
        std::lock_guard<std::mutex> l(gLock);
        if (!t || !gUnstarted.erase((__bridge void *)t))
            continue;
        if (d.colorAttachments[j].loadAction == MTLLoadActionLoad)
            fillStart(self, t);
    }
    id e = ((id (*)(id, SEL, id))orig(self, s))(self, s, d);
    std::lock_guard<std::mutex> l(gLock);
    Pass p;
    if (id<MTLTexture> t = d.colorAttachments[0].texture) {
        p.tex = textureId(t);
        p.w = unsigned(t.width >> d.colorAttachments[0].level);
        p.h = unsigned(t.height >> d.colorAttachments[0].level);
        p.fmt = unsigned(t.pixelFormat);
        p.load = unsigned(d.colorAttachments[0].loadAction);
    }
    p.vp[2] = p.w, p.vp[3] = p.h, p.sc[2] = p.w, p.sc[3] = p.h;
    gPasses[(__bridge void *)e] = p;
    return e;
}

void hScissor(id self, SEL s, MTLScissorRect r)
{
    {
        std::lock_guard<std::mutex> l(gLock);
        if (auto it = gPasses.find((__bridge void *)self); it != gPasses.end()) {
            unsigned *c = it->second.sc;
            c[0] = unsigned(r.x), c[1] = unsigned(r.y), c[2] = unsigned(r.width), c[3] = unsigned(r.height);
        }
    }
    ((void (*)(id, SEL, MTLScissorRect))orig(self, s))(self, s, r);
}

void hViewport(id self, SEL s, MTLViewport v)
{
    {
        std::lock_guard<std::mutex> l(gLock);
        if (auto it = gPasses.find((__bridge void *)self); it != gPasses.end()) {
            double *c = it->second.vp;
            c[0] = v.originX, c[1] = v.originY, c[2] = v.width, c[3] = v.height;
        }
    }
    ((void (*)(id, SEL, MTLViewport))orig(self, s))(self, s, v);
}

// raw_viewports.h viewportPixels: the scissor within the viewport rounded out
void noteDraw(id self)
{
    std::lock_guard<std::mutex> l(gLock);
    auto it = gPasses.find((__bridge void *)self);
    if (it == gPasses.end())
        return;
    Pass &p = it->second;
    p.draws++;
    const double x0 = std::max(std::floor(std::min(p.vp[0], p.vp[0] + p.vp[2])), double(p.sc[0])),
                 x1 = std::min(std::ceil(std::max(p.vp[0], p.vp[0] + p.vp[2])), double(p.sc[0]) + p.sc[2]),
                 y0 = std::max(std::floor(std::min(p.vp[1], p.vp[1] + p.vp[3])), double(p.sc[1])),
                 y1 = std::min(std::ceil(std::max(p.vp[1], p.vp[1] + p.vp[3])), double(p.sc[1]) + p.sc[3]);
    if (x1 <= x0 || y1 <= y0)
        return;
    p.box[0] = std::min(p.box[0], x0), p.box[1] = std::min(p.box[1], y0);
    p.box[2] = std::max(p.box[2], x1), p.box[3] = std::max(p.box[3], y1);
    char line[160];
    snprintf(line, sizeof line, "    viewport %g,%g %gx%g scissor %u,%u %ux%u", p.vp[0], p.vp[1], p.vp[2], p.vp[3],
             p.sc[0], p.sc[1], p.sc[2], p.sc[3]);
    if (p.draws_.empty() || p.draws_.back() != line)
        p.draws_.push_back(line);
}

void hDraw4(id self, SEL s, MTLPrimitiveType t, NSUInteger a, NSUInteger b, NSUInteger c)
{
    noteDraw(self);
    ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger, NSUInteger))orig(self, s))(self, s, t, a, b, c);
}

void hDraw3(id self, SEL s, MTLPrimitiveType t, NSUInteger a, NSUInteger b)
{
    noteDraw(self);
    ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger))orig(self, s))(self, s, t, a, b);
}

void hDrawIndexed(id self, SEL s, MTLPrimitiveType t, NSUInteger n, MTLIndexType it, id buf, NSUInteger off)
{
    noteDraw(self);
    ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger))orig(self, s))(self, s, t, n, it,
                                                                                                   buf, off);
}

void hDrawIndexedInstanced(id self, SEL s, MTLPrimitiveType t, NSUInteger n, MTLIndexType it, id buf, NSUInteger off,
                           NSUInteger inst)
{
    noteDraw(self);
    ((void (*)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger, NSUInteger))orig(self, s))(
        self, s, t, n, it, buf, off, inst);
}

void hEnd(id self, SEL s)
{
    {
        std::lock_guard<std::mutex> l(gLock);
        if (auto it = gPasses.find((__bridge void *)self); it != gPasses.end()) {
            const Pass &p = it->second;
            const bool known = p.draws && p.box[2] >= 0 && p.load != MTLLoadActionClear;
            const unsigned right = known ? unsigned(std::max(0.0, p.w - p.box[2])) : 0,
                           bottom = known ? unsigned(std::max(0.0, p.h - p.box[3])) : 0;
            if (right || bottom) {
                gShort++;
                printf("  edge: tex %u %ux%u f%u load %u rect %g,%g-%g,%g short bottom %u right %u\n", p.tex, p.w, p.h,
                       p.fmt, p.load, p.box[0], p.box[1], p.box[2], p.box[3], bottom, right);
                for (const auto &d : p.draws_)
                    printf("%s\n", d.c_str());
            }
            gPasses.erase(it);
        }
    }
    ((void (*)(id, SEL))orig(self, s))(self, s);
}

void hCommit(id self, SEL s)
{
    {
        std::lock_guard<std::mutex> l(gLock);
        gCommits++;
    }
    ((void (*)(id, SEL))orig(self, s))(self, s);
}

unsigned texelBytes(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatR8Unorm:
        return 1;
    case MTLPixelFormatR16Float:
        return 2;
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatR32Float:
        return 4;
    case MTLPixelFormatRGBA16Float:
        return 8;
    default:
        return 0;
    }
}

uint16_t startHalf(MTLPixelFormat f)
{
    const bool isFloat = f == MTLPixelFormatR16Float || f == MTLPixelFormatRG16Float ||
                         f == MTLPixelFormatRGBA16Float || f == MTLPixelFormatR32Float;
    return gOptions.start && !strcmp(gOptions.start, "nan") && isFloat ? 0x7e00 : 0;
}

// On the command buffer of the target's first pass, so that it lands after every earlier use of memory the heap aliases
void fillStart(id<MTLCommandBuffer> cb, id<MTLTexture> t)
{
    const unsigned bytes = texelBytes(t.pixelFormat);
    if (!bytes)
        return;
    const size_t row = t.width * bytes, size = row * t.height;
    std::vector<uint16_t> words((size + 1) / 2, startHalf(t.pixelFormat));
    id<MTLBuffer> b = [t.device newBufferWithBytes:words.data() length:size options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:b
               sourceOffset:0
          sourceBytesPerRow:row
        sourceBytesPerImage:size
                 sourceSize:MTLSizeMake(t.width, t.height, 1)
                  toTexture:t
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    gStarted++;
}

id hNewTexture(id self, SEL s, MTLTextureDescriptor *d)
{
    const bool target = d.textureType == MTLTextureType2D && (d.usage & MTLTextureUsageRenderTarget) &&
                        d.mipmapLevelCount == 1 && d.sampleCount == 1;
    MTLTextureDescriptor *use = d;
    if (target && gOptions.round32) {
        use = [d copy];
        use.width = (d.width + 31) / 32 * 32;
        use.height = (d.height + 31) / 32 * 32;
        gRounded += use.width != d.width || use.height != d.height;
    }
    id<MTLTexture> t = ((id (*)(id, SEL, id))orig(self, s))(self, s, use);
    if (t && target && gOptions.start) {
        std::lock_guard<std::mutex> l(gLock);
        gUnstarted[(__bridge void *)t] = t;
    }
    return t;
}

void hook(Class c, const char *name, IMP imp)
{
    SEL s = sel_registerName(name);
    if (Method m = class_getInstanceMethod(c, s))
        gOrig[{c, s}] = method_setImplementation(m, imp);
    else
        printf("no %s on %s\n", name, class_getName(c));
}

void installHooks()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:4
                                                                                 height:4
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = [device newTextureWithDescriptor:td];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    const Class cbClass = object_getClass(cb), encoderClass = object_getClass(e);
    [e endEncoding];
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = 1 << 20;
    const Class heapClass = object_getClass([device newHeapWithDescriptor:hd]);
    hook(cbClass, "renderCommandEncoderWithDescriptor:", (IMP)hRenderEncoder);
    hook(cbClass, "commit", (IMP)hCommit);
    hook(encoderClass, "setScissorRect:", (IMP)hScissor);
    hook(encoderClass, "setViewport:", (IMP)hViewport);
    hook(encoderClass, "drawPrimitives:vertexStart:vertexCount:instanceCount:", (IMP)hDraw4);
    hook(encoderClass, "drawPrimitives:vertexStart:vertexCount:", (IMP)hDraw3);
    hook(encoderClass, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", (IMP)hDrawIndexed);
    hook(encoderClass, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
         (IMP)hDrawIndexedInstanced);
    hook(encoderClass, "endEncoding", (IMP)hEnd);
    hook(object_getClass(device), "newTextureWithDescriptor:", (IMP)hNewTexture);
    hook(heapClass, "newTextureWithDescriptor:", (IMP)hNewTexture);
}

void dump(CGImageRef image, const char *path)
{
    const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    std::vector<uint8_t> rgba(w * h * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, space, uint32_t(kCGImageAlphaPremultipliedLast));
    CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
    if (FILE *f = fopen(path, "wb")) {
        fwrite(rgba.data(), 1, rgba.size(), f);
        fclose(f);
    }
    CGContextRelease(context);
    CGColorSpaceRelease(space);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        int k = 1;
        for (; k < argc && argv[k][0] == '-'; k++) {
            if (!strcmp(argv[k], "--round32"))
                gOptions.round32 = true;
            else if (!strcmp(argv[k], "--start") && k + 1 < argc)
                gOptions.start = argv[++k];
            else if (!strcmp(argv[k], "--dump") && k + 1 < argc)
                gOptions.dump = argv[++k];
        }
        if (argc - k != 3) {
            fprintf(stderr, "usage: iconpassoracle [--round32] [--start zero|nan] [--dump <prefix>] <bundle id> "
                            "<points> <scale>\n");
            return 2;
        }
        installHooks();
        const double points = atof(argv[k + 1]), scale = atof(argv[k + 2]);
        id icon = ((id (*)(id, SEL, id))objc_msgSend)([NSClassFromString(@"ISIcon") alloc],
                                                      sel_registerName("initWithBundleIdentifier:"),
                                                      [NSString stringWithUTF8String:argv[k]]);
        id descriptor = ((id (*)(id, SEL, CGSize, CGFloat))objc_msgSend)(
            [NSClassFromString(@"ISImageDescriptor") alloc], sel_registerName("initWithSize:scale:"),
            CGSizeMake(points, points), scale);
        id request = [NSClassFromString(@"ISGenerationRequest") new];
        ((void (*)(id, SEL, id))objc_msgSend)(request, sel_registerName("setIcon:"), icon);
        ((void (*)(id, SEL, id))objc_msgSend)(request, sel_registerName("setImageDescriptor:"), descriptor);
        id image = ((id (*)(id, SEL))objc_msgSend)(request, sel_registerName("generateImage"));
        CGImageRef cg =
            image ? (__bridge CGImageRef)((id (*)(id, SEL))objc_msgSend)(image, sel_registerName("CGImage")) : nullptr;
        if (cg && gOptions.dump)
            dump(cg, gOptions.dump);
        printf("%s %g pt scale %g: image %zux%zu, %u command buffers, %u targets rounded, %u started, %u short load "
               "passes\n",
               argv[k], points, scale, cg ? CGImageGetWidth(cg) : 0, cg ? CGImageGetHeight(cg) : 0, gCommits, gRounded,
               gStarted, gShort);
        return cg ? 0 : 1;
    }
}
