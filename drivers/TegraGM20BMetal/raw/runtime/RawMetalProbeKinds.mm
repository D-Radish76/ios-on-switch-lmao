// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's probes of routes no device run has covered yet, and of the routes a test compares one with
// (raw::Device::probeMipCandidates, probeZetaKinds, probeStencilBits and probeZetaDirect): @YES turns one on for this
// process, @NO off; each answers what it was. Work recorded while one is on keeps its route.
@implementation RMDevice (ProbeKinds)

- (NSNumber *)switchRawProbeMipCandidates:(NSNumber *)on
{
    return @(gRM->raw->probeMipCandidates.exchange(on.boolValue));
}

- (NSNumber *)switchRawProbeZetaKinds:(NSNumber *)on
{
    return @(gRM->raw->probeZetaKinds.exchange(on.boolValue));
}

- (NSNumber *)switchRawProbeStencilBits:(NSNumber *)on
{
    return @(gRM->raw->probeStencilBits.exchange(on.boolValue));
}

- (NSNumber *)switchRawProbeZetaDirect:(NSNumber *)on
{
    return @(gRM->raw->probeZetaDirect.exchange(on.boolValue));
}

@end
#endif
