// SPDX-License-Identifier: MIT
// A state whose fragment function names raster order groups (HW Test gpu.raw.raster_order): m2v runs its body in the
// pixel interlock with no colour in the storage form. The direct compile keeps that program (it was refused as "storage
// form: compiled shader does not match the ordered attachment contract" and made again without the interlock, its
// groups unordered) and the record carries PipelineRasterOrdered, which the runtime, the compiler's client and the pack
// accept only with the record's own ordered interlock control.
#include "../raw/runtime/raw_storage.h"
#include <gtest/gtest.h>

namespace {

constexpr uint32_t kInterlockControl = 0x1224, kPixel = 2, kSample = 1;

TEST(RasterOrdered, AnInterlockAroundNoStorageColourIsTheRasterOrderGroupsOrdering)
{
    EXPECT_TRUE(raw::interlockMatchesStorage(kPixel, 0));
    EXPECT_TRUE(raw::interlockMatchesStorage(kSample, 0));
    EXPECT_TRUE(raw::interlockMatchesStorage(0, 0));
    EXPECT_TRUE(raw::interlockMatchesStorage(kPixel, 1));
    EXPECT_FALSE(raw::interlockMatchesStorage(0, 1));
    EXPECT_FALSE(raw::interlockMatchesStorage(3, 0));
}

TEST(RasterOrdered, ARecordCarriesItsOrderedInterlockControl)
{
    raw::PackPipeline p = {};
    p.flags = raw::PipelineRasterOrdered;
    const uint32_t ordered[2] = {kInterlockControl, kPixel}, none[2] = {kInterlockControl, 0};
    EXPECT_TRUE(raw::usableStorageMetadata(p, ordered, 2));
    EXPECT_FALSE(raw::usableStorageMetadata(p, none, 2)) << "raster ordered with no interlock";
    EXPECT_FALSE(raw::usableStorageMetadata(p, nullptr, 0)) << "raster ordered with no interlock control";
    p.flags = 0;
    EXPECT_FALSE(raw::usableStorageMetadata(p, ordered, 2)) << "an interlock the record does not say orders anything";
}

TEST(RasterOrdered, AStorageFormIsNotRasterOrdered)
{
    raw::PackPipeline p = {};
    p.flags = raw::PipelineStorageMetadata | raw::PipelineRasterOrdered;
    p.storageMask = p.storageWriteMask = 1;
    p.storageSamples = 1;
    p.storageTic[0] = 1;
    p.entries = 1;
    p.entry[0].kind = raw::EntryFetch;
    p.entry[0].stage = 2;
    const uint32_t blob[4] = {kInterlockControl, kPixel, 0x1a00, 0};
    EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 4));
    p.flags = raw::PipelineStorageMetadata;
    EXPECT_TRUE(raw::usableStorageMetadata(p, blob, 4));
}

}  // namespace
