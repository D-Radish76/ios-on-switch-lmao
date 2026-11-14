import Foundation
import HWTestCore

/// The display held on the copy route while `body` runs (debug kernels' PORTRAIT, SwitchLandscapeRunABI.h): with V3's
/// column scan shipped, a landscape video goes to window A, and a test of windows B and C's own paths (VIC's composite,
/// the media completion flip) holds the copy route that puts it there. A kernel without the request runs `body` as is.
enum CopyRouteHold {
    static func during<R>(_ body: () async throws -> R) async rethrows -> R {
        guard case .success(let session) = VICKernelTest.Session.open() else { return try await body() }
        _ = session.call(
            LandscapeSample.request(op: LandscapeSample.portrait, value: LandscapeSample.portraitFrames), replyBytes: 0)
        defer {
            _ = session.call(LandscapeSample.request(op: LandscapeSample.portrait, value: 0), replyBytes: 0)
            session.close()
        }
        return try await body()
    }
}
