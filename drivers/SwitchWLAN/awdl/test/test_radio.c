/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../awdl.h"
#include "../awdl_apple80211.h"
#include "../awdl_radio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define RCHECK(condition)                                                        \
    do {                                                                         \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++test_failures;                                                     \
        }                                                                        \
    } while (0)

static struct {
    int result, calls, accepted, starts, start_error;
    int async_schedule, schedules, pending, cancelled;
    uint16_t sequence[128];
    uint8_t marker[128], attempted[AWDL_MAX_FRAME];
    size_t attempted_bytes;
    uint64_t data_deadline;
    struct awdl_core *core;
} switch_radio_test;

static int switch_radio_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay, int no_ack)
{
    (void)ctx;
    (void)freq;
    (void)no_ack;
    if ((frame[0] & 0x0c) != 8)
        return 0;
    if (switch_radio_test.async_schedule && !switch_awdl_core_ready(switch_radio_test.core))
        return AWDL_TX_BUSY;
    ++switch_radio_test.calls;
    switch_radio_test.data_deadline = stay;
    memcpy(switch_radio_test.attempted, frame, len);
    switch_radio_test.attempted_bytes = len;
    if (!switch_radio_test.result) {
        switch_radio_test.pending++;
        uint8_t ether[AWDL_MAX_FRAME];
        uint16_t seq;
        const size_t bytes = awdl_data_decap(frame, len, ether, sizeof ether, &seq, NULL);
        RCHECK(bytes >= 15);
        const unsigned i = (unsigned)switch_radio_test.accepted++;
        if (i < 128) {
            switch_radio_test.sequence[i] = seq;
            switch_radio_test.marker[i] = ether[14];
        }
    }
    return switch_radio_test.result;
}

static void switch_radio_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    (void)ctx;
    (void)seq;
    (void)anchor;
    if (!switch_radio_test.async_schedule)
        return;
    switch_radio_test.schedules++;
    switch_radio_test.cancelled += switch_radio_test.pending;
    switch_radio_test.pending = 0;
    switch_awdl_core_radio_ready(switch_radio_test.core, 0);
}

static struct awdl_core *switch_radio_core(void)
{
    const uint8_t address[6] = {2, 1, 2, 3, 4, 5};
    struct awdl_config config;
    awdl_config_default(&config, address);
    config.backend = AWDL_BACKEND_SOFT;
    struct awdl_host host = {0};
    host.tx = switch_radio_tx;
    host.schedule = switch_radio_schedule;
    struct awdl_core *core = calloc(1, awdl_core_size());
    RCHECK(core && awdl_core_init(core, &host, &config) == 0);
    switch_radio_test.core = core;
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){6, 81};
    awdl_core_set_sequence(core, seq, 0);
    return core;
}

static void switch_test_backpressure(void)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_start(core, 1000);
    uint8_t ether[64] = {0x33, 0x33, 0, 0, 0, 1, 2, 1, 2, 3, 4, 5, 0x86, 0xdd, 0xa1};
    switch_radio_test.result = AWDL_TX_BUSY;
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1000));
    const size_t bytes = switch_radio_test.attempted_bytes;
    uint8_t first[AWDL_MAX_FRAME];
    memcpy(first, switch_radio_test.attempted, bytes);
    ether[14] = 0xb2;
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1000));
    RCHECK(switch_radio_test.calls == 1);
    switch_awdl_core_tx_ready(core, 1100);
    RCHECK(switch_radio_test.calls == 2 && switch_radio_test.attempted_bytes == bytes &&
           !memcmp(first, switch_radio_test.attempted, bytes));
    switch_radio_test.result = 0;
    switch_awdl_core_tx_ready(core, 1200);
    RCHECK(switch_radio_test.accepted == 2 && switch_radio_test.sequence[0] == 0 &&
           switch_radio_test.sequence[1] == 1 && switch_radio_test.marker[0] == 0xa1 &&
           switch_radio_test.marker[1] == 0xb2);
    switch_radio_test.result = -7;
    RCHECK(awdl_core_output(core, ether, sizeof ether, 1200) == -7);
    switch_radio_test.result = 0;
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1200));
    RCHECK(switch_radio_test.sequence[2] == 2);
    switch_radio_test.result = AWDL_TX_BUSY;
    for (unsigned i = 0; i < AWDL_TXQ_FRAMES; ++i) {
        ether[14] = (uint8_t)i;
        RCHECK(!awdl_core_output(core, ether, sizeof ether, 1300));
    }
    RCHECK(awdl_core_output(core, ether, sizeof ether, 1300) < 0);
    const int calls = switch_radio_test.calls;
    switch_awdl_core_radio_ready(core, 0);
    RCHECK(!switch_awdl_core_ready(core) && awdl_core_timer(core, 1300) == UINT64_MAX);
    switch_awdl_core_tx_ready(core, 1400);
    RCHECK(switch_radio_test.calls == calls);
    switch_awdl_core_radio_ready(core, 1);
    switch_radio_test.result = 0;
    switch_awdl_core_tx_ready(core, 1500);
    RCHECK(switch_radio_test.accepted == 3 + AWDL_TXQ_FRAMES);
    for (unsigned i = 0; i < AWDL_TXQ_FRAMES; ++i)
        RCHECK(switch_radio_test.sequence[i + 3] == i + 3 && switch_radio_test.marker[i + 3] == i);
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1500));
    RCHECK(switch_radio_test.sequence[3 + AWDL_TXQ_FRAMES] == 3 + AWDL_TXQ_FRAMES);
    switch_radio_test.result = AWDL_TX_BUSY;
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1600));
    switch_radio_test.result = -7;
    switch_awdl_core_tx_ready(core, 1700);
    switch_radio_test.result = 0;
    const int accepted = switch_radio_test.accepted;
    switch_awdl_core_tx_ready(core, 1800);
    RCHECK(switch_radio_test.accepted == accepted && awdl_core_stats(core)->data_drop == 3);
    switch_awdl_core_radio_ready(core, 0);
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1900));
    RCHECK(switch_radio_test.accepted == accepted);
    switch_awdl_core_radio_ready(core, 1);
    switch_awdl_core_tx_ready(core, 2000);
    RCHECK(switch_radio_test.accepted == accepted + 1);
    awdl_core_stop(core);
    free(core);
}

static void switch_radio_learn_peer(struct awdl_core *core, const uint8_t address[6], uint64_t now)
{
    struct awdl_config config;
    awdl_config_default(&config, address);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        config.seq[i] = (struct awdl_chan){6, 81};
    const struct awdl_host host = {.tx = switch_radio_tx};
    struct awdl_core *peer = calloc(1, awdl_core_size());
    RCHECK(peer && !awdl_core_init(peer, &host, &config));
    awdl_core_set_sequence(peer, config.seq, now);
    awdl_core_start(peer, now);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, now, frame, sizeof frame);
    const struct awdl_rx_meta meta = {now, 2437, -40};
    awdl_core_rx(core, frame, length, &meta);
    const struct awdl_peer *found = awdl_core_find(core, address);
    RCHECK(found && found->reachable);
    free(peer);
}

static void switch_test_home_discovery(void)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_set_infra(core, 2437, 0);
    awdl_core_start(core, 0);
    const uint64_t mid = AWDL_EAW_TU * AWDL_TU_US / 2;
    RCHECK(awdl_core_timer(core, 0) == mid);
    awdl_core_timer(core, mid);
    RCHECK(awdl_core_stats(core)->mif_tx == 1);

    struct awdl_chan seq[AWDL_SEQ_SLOTS] = {0};
    awdl_core_set_sequence(core, seq, mid + 1);
    RCHECK(awdl_core_timer(core, mid + 1) == UINT64_MAX);
    seq[8] = (struct awdl_chan){6, 81};
    awdl_core_set_sequence(core, seq, mid + 2);
    uint64_t t = awdl_core_timer(core, mid + 2);
    RCHECK(t > 16 * mid && t < 17 * mid);
    RCHECK(awdl_core_timer(core, t) == 17 * mid);
    awdl_core_timer(core, 19 * mid);
    RCHECK(awdl_core_stats(core)->mif_tx == 1);
    awdl_core_stop(core);
    free(core);
}

static void switch_test_legacy_channels(void)
{
    const uint8_t frame[59] = {
        [0] = 0xd0, [16] = 0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
        [24] = 0x7f, 0x00, 0x17, 0xf2, 0x08, 0x10, 0x03,
        [40] = AWDL_TLV_CHANNEL_SEQUENCE, 16, 0, 4, AWDL_CHAN_ENC_LEGACY, 0, 3, 0xff, 0xff,
        0x1d, 46, 0x1e, 46, 0x2b, 6, 0x1d, 1, 0x1e, 255,
    };
    struct awdl_af a;
    RCHECK(!awdl_af_parse(frame, sizeof frame, &a) && a.chanseq.count == 5);
    const uint8_t want[5] = {44, 48, 6, 0, 0};
    for (unsigned i = 0; i < 5; ++i)
        RCHECK(awdl_chan_primary(a.chanseq.chan[i]) == want[i]);
}

static struct awdl_core *switch_timed_peer(const uint8_t address[6], unsigned metric, uint64_t start)
{
    struct awdl_config cfg;
    awdl_config_default(&cfg, address);
    cfg.self_metric = metric;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        cfg.seq[i] = (struct awdl_chan){44, 115};
    cfg.seq[1] = (struct awdl_chan){6, 81};
    const struct awdl_host host = {.tx = switch_radio_tx};
    struct awdl_core *peer = calloc(1, awdl_core_size());
    RCHECK(peer && awdl_core_init(peer, &host, &cfg) == 0);
    awdl_core_set_sequence(peer, cfg.seq, start);
    awdl_core_start(peer, start);
    return peer;
}

static void switch_receive_mif(struct awdl_core *core, struct awdl_core *peer, uint64_t now)
{
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, now, frame, sizeof frame);
    const struct awdl_rx_meta meta = {now, 5220, -39};
    RCHECK(length > 0);
    awdl_core_rx(core, frame, length, &meta);
}

static void switch_test_peer_window(uint64_t start, int rephase)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_set_infra(core, 2437, start);
    awdl_core_start(core, start);
    uint8_t eth[64] = {2, 1, 2, 3, 4, 7, 2, 1, 2, 3, 4, 5, 0x86, 0xdd, 0x60};
    const uint8_t mdns[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
    struct awdl_core *peer = switch_timed_peer(eth, 10, start + 81920);
    const uint64_t now = start + 83000;
    switch_receive_mif(core, peer, now);
    RCHECK(awdl_core_role(core) == AWDL_ROLE_ROOT);
    RCHECK(!awdl_core_output(core, eth, sizeof eth, now));
    const uint8_t peer_address[6] = {2, 1, 2, 3, 4, 7};
    memcpy(eth, mdns, sizeof mdns);
    RCHECK(!awdl_core_output(core, eth, sizeof eth, now));
    uint64_t t = awdl_core_timer(core, now);
    const uint64_t shift = rephase ? 16384 : 0;
    if (rephase) {
        free(peer);
        peer = switch_timed_peer(peer_address, 10, start + 81920 + shift);
        switch_receive_mif(core, peer, start + 100000);
        t = awdl_core_timer(core, start + 100000);
    }
    const uint64_t open = start + 147456 + shift + 2048;
    for (unsigned i = 0; i < 8 && t < open; ++i)
        t = awdl_core_timer(core, t);
    RCHECK(t == open && switch_radio_test.accepted == 0);
    awdl_core_timer(core, open);
    RCHECK(switch_radio_test.accepted == 2 && switch_radio_test.sequence[0] == 0 && switch_radio_test.sequence[1] == 1);
    RCHECK(!awdl_core_output(core, eth, sizeof eth, start + 205000));
    RCHECK(switch_radio_test.accepted == 3 && switch_radio_test.data_deadline == start + 212992 + shift - 2048);
    awdl_core_stop(core);
    free(core);
    free(peer);
}

static void switch_test_queued_expiry(void)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_start(core, 1000);
    uint8_t ether[64] = {0x33, 0x33, 0, 0, 0, 1, 2, 1, 2, 3, 4, 5, 0x86, 0xdd};
    switch_radio_test.result = AWDL_TX_BUSY;
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1000) && switch_radio_test.calls == 1);
    switch_radio_test.result = 0;
    const uint64_t expiry = 1000 + AWDL_TRANSFER_HOLD_US + AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    awdl_core_timer(core, expiry);
    switch_awdl_core_tx_ready(core, expiry + 1);
    RCHECK(switch_radio_test.accepted == 0 && awdl_core_stats(core)->data_drop == 1);
    RCHECK(!awdl_core_output(core, ether, sizeof ether, expiry + 5) && switch_radio_test.accepted == 1);
    const uint64_t resumed = (uint64_t)1 << 33;
    struct awdl_chan seq[AWDL_SEQ_SLOTS] = {0};
    awdl_core_set_sequence(core, seq, expiry + 10);
    RCHECK(awdl_core_timer(core, expiry + 10) == UINT64_MAX);
    seq[8] = (struct awdl_chan){6, 81};
    awdl_core_set_sequence(core, seq, resumed);
    const uint64_t due = awdl_core_timer(core, resumed);
    RCHECK(due > resumed && due < resumed + AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US);
    awdl_core_stop(core);
    free(core);
}

static void switch_test_delayed_action(void)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_start(core, 0);
    const uint8_t addr[6] = {2, 7, 0, 0, 0, 1};
    struct awdl_core *peer = switch_timed_peer(addr, 180, 81920);
    switch_awdl_core_radio_ready(core, 0);
    switch_radio_test.async_schedule = 1;
    switch_receive_mif(core, peer, 84000);
    const struct awdl_peer *p = awdl_core_find(core, addr);
    RCHECK(p && p->reachable && p->last_seen == 84000);
    const uint64_t anchor = p ? p->next_aw_tsf : 0;
    switch_receive_mif(core, peer, 83000);
    RCHECK(p && p->frames == 1 && p->next_aw_tsf == anchor && p->last_seen == 84000);
    RCHECK(switch_radio_test.schedules == 0 && awdl_core_timer(core, 85000) == UINT64_MAX);
    switch_awdl_core_radio_ready(core, 1);
    RCHECK(awdl_core_timer(core, 85000) == UINT64_MAX && switch_radio_test.schedules == 1);
    switch_awdl_core_radio_ready(core, 1);
    awdl_core_timer(core, 85001);
    RCHECK(switch_radio_test.schedules == 1);
    awdl_core_set_addr(core, (const uint8_t[6]){2, 1, 0, 0, 0, 7});
    RCHECK(!awdl_core_find(core, addr));
    awdl_core_stop(core);
    free(core);
    free(peer);
}

static void switch_test_transfer_backpressure(int queued)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_start(core, 1000);
    uint8_t ether[64] = {2, 1, 2, 3, 4, 6, 2, 1, 2, 3, 4, 5, 8, 0, 0x45};
    ether[23] = 6;
    ether[63] = 0xa1;
    switch_radio_learn_peer(core, ether, 1000);
    uint8_t expected[AWDL_MAX_FRAME];
    const size_t expected_bytes = awdl_data_encap(ether, sizeof ether, ether + 6, 0, expected, sizeof expected);
    switch_radio_test.async_schedule = 1;
    if (queued)
        switch_awdl_core_radio_ready(core, 0);
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1000));
    RCHECK(awdl_core_mode(core) == AWDL_MODE_TRANSFER && !switch_awdl_core_ready(core));
    RCHECK(switch_radio_test.schedules == 1 && !switch_radio_test.accepted && !switch_radio_test.cancelled);
    memset(ether + 14, 0, sizeof ether - 14);
    uint64_t now = 1100;
    if (queued) {
        now = 1001 + AWDL_TRANSFER_HOLD_US;
        switch_radio_test.async_schedule = 0;
        switch_radio_learn_peer(core, ether, now);
        switch_radio_test.async_schedule = 1;
        switch_awdl_core_radio_ready(core, 1);
        awdl_core_timer(core, now);
        RCHECK(awdl_core_mode(core) == AWDL_MODE_IDLE && !switch_awdl_core_ready(core));
        switch_awdl_core_radio_ready(core, 1);
        switch_awdl_core_tx_ready(core, ++now);
        RCHECK(awdl_core_mode(core) == AWDL_MODE_TRANSFER && !switch_awdl_core_ready(core));
        RCHECK(switch_radio_test.schedules == 3 && !switch_radio_test.accepted && !switch_radio_test.cancelled);
    }
    switch_awdl_core_radio_ready(core, 1);
    switch_awdl_core_tx_ready(core, ++now);
    RCHECK(switch_radio_test.accepted == 1 && switch_radio_test.pending == 1 && !switch_radio_test.cancelled);
    RCHECK(switch_radio_test.attempted_bytes == expected_bytes &&
           !memcmp(switch_radio_test.attempted, expected, expected_bytes) && switch_radio_test.sequence[0] == 0);
    switch_awdl_core_tx_ready(core, ++now);
    RCHECK(switch_radio_test.accepted == 1 && awdl_core_stats(core)->data_drop == 0);
    awdl_core_stop(core);
    free(core);
}

static void switch_test_clock(void)
{
    struct switch_awdl_radio_clock clock = {0};
    RCHECK(switch_awdl_radio_clock_update(&clock, 10000, 1000000000, 100000, 1, 1000010000) == 1);
    RCHECK(switch_awdl_radio_clock_valid(&clock, 1000010000));
    RCHECK(switch_awdl_radio_clock_expires(&clock) == 2800000000ULL);
    RCHECK(switch_awdl_radio_clock_valid(&clock, 2799999999ULL));
    RCHECK(!switch_awdl_radio_clock_valid(&clock, 2800000000ULL));
    RCHECK(switch_awdl_radio_clock_update(&clock, 510010, 1500000000, 50000, 1, 1500010000) == 0);
    RCHECK(awdl_tsf_clock_now(&clock.affine, 1600000000) >= 610010);
    RCHECK(switch_awdl_radio_clock_update(&clock, 510010, 1500000000, 50000, 1, 1500020000) == 0);
    RCHECK(switch_awdl_radio_clock_update(&clock, 600000, 1600000000, 0, 1, 1500020000) < 0);
    RCHECK(switch_awdl_radio_clock_update(&clock, 610010, 1600000000, 1000000, 1, 1600000000) < 0);
    RCHECK(switch_awdl_radio_clock_update(&clock, 9999999, 1600000000, 0, 1, 1600000000) < 0);
    RCHECK(switch_awdl_radio_clock_valid(&clock, 1600000000));
    RCHECK(clock.affine.host0_ns == 1500000000 && !clock.invalid_epoch);
    RCHECK(switch_awdl_radio_clock_update(&clock, 10000000, 1700000000, 0, 1, 1700000000) < 0);
    RCHECK(switch_awdl_radio_clock_update(&clock, 710010, 1700000000, 50000, 1, 1700000000) == 0);
    RCHECK(switch_awdl_radio_clock_update(&clock, 5, 1700000000, 0, 2, 1700000000) == 1);
    RCHECK(clock.affine.ppm_x16 == 0 && clock.epoch == 2);
    RCHECK(switch_awdl_radio_clock_update(&clock, 8, 1800000000, 1000000, 3, 1800000000) < 0);
    RCHECK(!switch_awdl_radio_clock_valid(&clock, 1800000000));
    RCHECK(switch_awdl_radio_clock_update(&clock, 8, 1800000000, 0, 3, 3800000000) < 0);
    switch_awdl_radio_clock_clear(&clock);
    RCHECK(switch_awdl_radio_clock_update(&clock, 10000, 1000000000, 100000, 1, 1000010000) == 1);
    RCHECK(switch_awdl_radio_clock_tolerance_us(100000, 100000, 10000000000ULL) == 5200);
    RCHECK(switch_awdl_radio_clock_tolerance_us(0, 0, 1) == 1);
    RCHECK(switch_awdl_radio_clock_update(&clock, 10014000, 11000000000ULL, 100000, 1, 11000010000ULL) == 0);
    RCHECK(switch_awdl_radio_clock_valid(&clock, 11000010000ULL));
}

static void switch_test_clock_renewal(void)
{
    struct switch_awdl_radio_clock clock = {0};
    RCHECK(!switch_awdl_radio_clock_deadline(&clock) && !switch_awdl_radio_clock_try_renew(&clock, 1));
    RCHECK(switch_awdl_radio_clock_update(&clock, 10000, 1000000000, 100000, 1, 1000010000) == 1);
    const uint64_t expires = switch_awdl_radio_clock_expires(&clock);
    const uint64_t renewal = switch_awdl_radio_clock_deadline(&clock);
    RCHECK(renewal > clock.affine.host0_ns && renewal < expires);
    RCHECK(!switch_awdl_radio_clock_try_renew(&clock, renewal - 1));
    RCHECK(switch_awdl_radio_clock_try_renew(&clock, renewal));
    RCHECK(switch_awdl_radio_clock_deadline(&clock) == expires);
    RCHECK(switch_awdl_radio_clock_update(&clock, 10000, 1000000000, 100000, 1, renewal + 1) == 0);
    RCHECK(switch_awdl_radio_clock_update(&clock, 1710000, renewal, 1000000, 1, renewal + 1) < 0);
    RCHECK(!switch_awdl_radio_clock_try_renew(&clock, renewal + 1));
    RCHECK(switch_awdl_radio_clock_valid(&clock, expires - 1));
    RCHECK(!switch_awdl_radio_clock_valid(&clock, expires));
    RCHECK(!switch_awdl_radio_clock_try_renew(&clock, expires));

    RCHECK(switch_awdl_radio_clock_update(&clock, 2010000, 3000000000ULL, 100000, 1, 3000010000ULL) == 0);
    const uint64_t next = switch_awdl_radio_clock_deadline(&clock);
    RCHECK(next > expires && next < switch_awdl_radio_clock_expires(&clock));
    RCHECK(switch_awdl_radio_clock_try_renew(&clock, next));
    RCHECK(!switch_awdl_radio_clock_try_renew(&clock, next + 1));
}

static void switch_test_clock_request_events(void)
{
    struct switch_awdl_radio_clock_request request = {0};
    RCHECK(switch_awdl_radio_clock_request_begin(&request, 0));
    RCHECK(!switch_awdl_radio_clock_request_begin(&request, 0));
    RCHECK(!switch_awdl_radio_clock_request_begin(&request, 1));
    switch_awdl_radio_clock_request_complete(&request, EAGAIN);
    RCHECK(!request.pending && request.blocked);
    for (unsigned i = 0; i < 10; ++i)
        RCHECK(!switch_awdl_radio_clock_request_begin(&request, 0));
    RCHECK(switch_awdl_radio_clock_request_begin(&request, 1));
    RCHECK(!switch_awdl_radio_clock_request_begin(&request, 0));
    switch_awdl_radio_clock_request_complete(&request, 0);
    RCHECK(!request.pending && !request.blocked);
    RCHECK(switch_awdl_radio_clock_request_begin(&request, 0));
    switch_awdl_radio_clock_request_complete(&request, EIO);
    request = (struct switch_awdl_radio_clock_request){0};
    RCHECK(switch_awdl_radio_clock_request_begin(&request, 0));
}

static void switch_test_address_change(void)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    awdl_core_start(core, 1000);
    uint8_t ether[64] = {0x33, 0x33, 0, 0, 0, 1, 2, 1, 2, 3, 4, 5, 0x86, 0xdd, 0xa1};
    switch_radio_test.result = AWDL_TX_BUSY;
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1000));
    const uint32_t generation = switch_awdl_core_generation(core);
    awdl_core_set_addr(core, ether + 6);
    RCHECK(switch_awdl_core_generation(core) == generation);
    const uint8_t address[6] = {2, 6, 7, 8, 9, 10};
    awdl_core_set_addr(core, address);
    RCHECK(switch_awdl_core_generation(core) != generation);
    switch_radio_test.result = 0;
    switch_awdl_core_tx_ready(core, 1200);
    RCHECK(!switch_radio_test.accepted && awdl_core_stats(core)->data_drop == 1);
    memcpy(ether + 6, address, sizeof address);
    RCHECK(!awdl_core_output(core, ether, sizeof ether, 1200));
    RCHECK(switch_radio_test.accepted == 1 && !memcmp(switch_radio_test.attempted + 10, address, sizeof address));
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(core, AWDL_SUBTYPE_MIF, 1200, frame, sizeof frame);
    struct awdl_af parsed;
    RCHECK(length && !awdl_af_parse(frame, length, &parsed));
    RCHECK(!memcmp(parsed.src, address, sizeof address));
    RCHECK(!memcmp(awdl_core_election(core)->root, address, sizeof address));
    awdl_core_stop(core);
    free(core);
}

static int switch_radio_start(void *ctx)
{
    (void)ctx;
    ++switch_radio_test.starts;
    switch_awdl_core_radio_ready(switch_radio_test.core, 0);
    return switch_radio_test.start_error;
}

static void switch_test_recovery(void)
{
    memset(&switch_radio_test, 0, sizeof switch_radio_test);
    struct awdl_core *core = switch_radio_core();
    switch_radio_test.core = core;
    struct a80211_awdl facade;
    const uint8_t caps[12] = {0};
    const struct a80211_awdl_ops ops = {0, switch_radio_start, 0, 0, 0, 0};
    a80211_awdl_init(&facade, core, &ops, "awdl0", "en0", caps);
#if defined(SWITCH_IPSW_KIND_24B119)
    struct switch_a80211_virtual_if_state request = {1, A80211_VIF_ROLE_AWDL, {1, 0, 0}};
    const int32_t activation = A80211_IOC_VIRTUAL_IF_STATE;
#else
    struct a80211_awdl_sync_enabled request = {1, 0, 1};
    const int32_t activation = A80211_IOC_AWDL_SYNC_ENABLED;
#endif
    uint32_t size = sizeof request;
    int32_t val = 0;
    RCHECK(!a80211_awdl_request(&facade, 1, activation, &val, &request, &size, sizeof request, 1000));
    RCHECK(facade.enabled && awdl_core_running(core) && !switch_awdl_core_ready(core));
    struct a80211_awdl_sync_enabled response;
    uint32_t response_size = 0;
    RCHECK(!a80211_awdl_request(&facade, 0, A80211_IOC_AWDL_SYNC_ENABLED, &val, &response, &response_size,
                               sizeof response, 1000) &&
           !response.enabled);
    switch_awdl_core_radio_ready(core, 1);
    switch_a80211_awdl_refresh(&facade);
    RCHECK(facade.published_ready);
    switch_a80211_awdl_suspend(&facade);
    RCHECK(facade.enabled && !facade.published_ready && !awdl_core_running(core));
    switch_radio_test.start_error = EIO;
    RCHECK(switch_a80211_awdl_resume(&facade, 2000) == EIO && facade.enabled && !awdl_core_running(core));
    switch_radio_test.start_error = 0;
    RCHECK(!switch_a80211_awdl_resume(&facade, 3000) && awdl_core_running(core) && !switch_awdl_core_ready(core));
#if defined(SWITCH_IPSW_KIND_24B119)
    request.active = 0;
#else
    request.enabled = 0;
#endif
    RCHECK(!a80211_awdl_request(&facade, 1, activation, &val, &request, &size, sizeof request, 5000));
    RCHECK(!switch_a80211_awdl_resume(&facade, 6000) && !facade.enabled && !awdl_core_running(core));
    switch_radio_test.start_error = EINPROGRESS;
#if defined(SWITCH_IPSW_KIND_24B119)
    request.active = 1;
#else
    request.enabled = 1;
#endif
    RCHECK(!a80211_awdl_request(&facade, 1, activation, &val, &request, &size, sizeof request, 7000));
    RCHECK(facade.enabled && !awdl_core_running(core));
    switch_radio_test.start_error = 0;
    RCHECK(!switch_a80211_awdl_resume(&facade, 8000) && awdl_core_running(core));
    awdl_core_stop(core);
    free(core);
}

void test_radio_main(void)
{
    switch_test_legacy_channels();
    switch_test_home_discovery();
    switch_test_peer_window(0, 0);
    switch_test_peer_window(0xffff0000u, 1);
    switch_test_queued_expiry();
    switch_test_delayed_action();
    switch_test_backpressure();
    switch_test_transfer_backpressure(0);
    switch_test_transfer_backpressure(1);
    switch_test_clock();
    switch_test_clock_renewal();
    switch_test_clock_request_events();
    switch_test_address_change();
    switch_test_recovery();
}
