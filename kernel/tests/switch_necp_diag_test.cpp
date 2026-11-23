// <switch/necp.h>: the first local-network drop of each of SWITCH_NECP_PROBE_PIDS processes is logged; iOS 27's NECP
// reports client drops whether or not their alert is suppressed and socket drops it alerts on, from inside its policy
// lock; iOS 17's has no hook, and release compiles the hook to nothing.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "bazel/testing/source_text.h"
#include "switch/necp.h"

using testing::HasSubstr;
using testing::Not;

namespace {

TEST(SwitchNecpDiag, EachPidIsClaimedOnce)
{
    int32_t slots[4] = {};
    EXPECT_TRUE(switch_necp_probe_claim(slots, 4, 94));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 4, 94));
    EXPECT_TRUE(switch_necp_probe_claim(slots, 4, 495));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 4, 94));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 4, 495));
}

TEST(SwitchNecpDiag, AFullTableAndTheKernelClaimNothing)
{
    int32_t slots[2] = {};
    EXPECT_TRUE(switch_necp_probe_claim(slots, 2, 1));
    EXPECT_TRUE(switch_necp_probe_claim(slots, 2, 2));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 2, 3));
    EXPECT_FALSE(switch_necp_probe_claim(slots, 2, 1));
    int32_t empty[2] = {};
    EXPECT_FALSE(switch_necp_probe_claim(empty, 2, 0));
    EXPECT_EQ(empty[0], 0);
}

TEST(SwitchNecpDiagSource, LocalNetworkDropsAreReportedUnderThePolicyLock)
{
    const std::string necp = switch_ios::test::SourceText("../xnu-12377.121.6/bsd/net/necp.c");
    EXPECT_THAT(necp, HasSubstr("#include <switch/necp.h>"));
    const std::string client = switch_ios::test::Body(necp, "\nnecp_application_find_policy_match_internal(");
    EXPECT_THAT(client, HasSubstr("NECP_KERNEL_POLICY_DROP_FLAG_LOCAL_NETWORK) {\n"
                                  "\t\tSWITCH_NECP_CLIENT_LOCAL_DROP(info, matched_policy, skip_policy_id, proc, "
                                  "effective_proc, responsible_proc, "
                                  "necp_kernel_application_policies_condition_mask);\n"
                                  "\t\tif (!(matched_policy->result_parameter.drop_flags & "
                                  "NECP_KERNEL_POLICY_DROP_FLAG_SUPPRESS_ALERTS)) {\n"));
    const std::string update = switch_ios::test::Body(necp, "\nnecp_socket_find_policy_match(");
    const std::string data = switch_ios::test::Body(necp, "\nnecp_socket_is_allowed_to_send_recv_internal(");
    for (const auto &[body, site] : {std::pair{update, "2"}, std::pair{data, "3"}}) {
        const std::string hook =
            std::string("\t\t\tsend_local_network_denied_event = true;\n\t\t\tSWITCH_NECP_SOCKET_LOCAL_DROP(") + site;
        const size_t at = body.find(hook);
        ASSERT_NE(at, std::string::npos) << site;
        EXPECT_LT(at, body.find("lck_rw_done(&necp_kernel_policy_lock);\n\n\tif (")) << site;
    }
    const std::string older = switch_ios::test::SourceText("../xnu-10063.141.1/bsd/net/necp.c");
    EXPECT_THAT(older, Not(HasSubstr("SWITCH_NECP_")));
}

TEST(SwitchNecpDiagSource, ReleaseKernelsCompileItToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/necp.h");
    EXPECT_THAT(header, HasSubstr("#else\n#define SWITCH_NECP_CLIENT_LOCAL_DROP("));
    const std::string source = switch_ios::test::SourceText("../switch/diag/necp.c");
    EXPECT_THAT(source, HasSubstr("\n#if DEVELOPMENT || DEBUG\n"));
    EXPECT_THAT(source, HasSubstr("\n#endif /* DEVELOPMENT || DEBUG */\n"));
}

}  // namespace
