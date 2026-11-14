// SPDX-License-Identifier: MIT
// See offline_metal.h: a state's reflection blob of RawOfflineDevice's functions.
#import "offline_metal.h"

#include <string>
#include <vector>
#include "bridge/NativeMetalDirect.h"

namespace offline {
namespace {

// A reflection blob's bytes into `out`, the blob released; false with `why`
bool blobBytes(NmbReflectionBlob *made, std::string &out, std::string &why)
{
    if (!made) {
        why = std::string("no reflection: ") + nmb_last_error();
        return false;
    }
    size_t length = 0;
    const uint8_t *bytes = nmb_reflection_blob_bytes(made, &length);
    out.assign(reinterpret_cast<const char *>(bytes), bytes ? length : 0);
    nmb_reflection_blob_release(made);
    return bytes != nullptr;
}

}  // namespace

bool renderReflection(MTLRenderPipelineDescriptor *descriptor, std::string &blob, std::string &why)
{
    NmbOfflineRenderPipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!descriptorOf(descriptor, p, linked)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    return blobBytes(nmb_offline_render_pipeline_reflection(&p), blob, why);
}

bool computeReflection(MTLComputePipelineDescriptor *descriptor, std::string &blob, std::string &why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!descriptorOf(descriptor, p, linked)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    return blobBytes(nmb_offline_compute_pipeline_reflection(&p), blob, why);
}

}  // namespace offline
