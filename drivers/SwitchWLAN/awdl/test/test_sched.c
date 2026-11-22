// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft backend sharing one radio with the station: a simulated minute next to an Apple-like master. Our radio has
// no hops; it stays on the access point's channel (36) and leaves only for the excursions host.tx asks for (an
// actframe with a dwell): it gets there 1-3 ms after the request and comes back when the stay ends. The master runs
// its own schedule. Measured: the access point's share of our radio and the longest absence, the master's frames we
// catch in our AWDL slots, our EAW boundaries against the master's (AWDL accepts 3 ms, Stute et al. §6.2), whether the
// master sees us, and how often our core wakes and transmits.
#include "../awdl.h"
#include "../awdl_apple80211.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define SCHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

#define HOME_MHZ  5180u
#define OFFSET_US 2345678
#define PPM       20

static uint64_t s_now;  // global microseconds
static uint32_t s_rand = 12345;

static uint32_t rnd(uint32_t n)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (s_rand >> 8) % n;
}

static uint64_t us_local(uint64_t g)
{
    return g + OFFSET_US + g * PPM / 1000000;
}

static uint64_t us_global(uint64_t l)
{
    const int64_t x = (int64_t)l - OFFSET_US;
    return (uint64_t)(x - x * PPM / 1000000);
}

struct pending {
    uint64_t at;
    int from;  // 0 the master, 1 us
    uint16_t freq;
    size_t len;
    uint8_t data[1600];
};

static struct pending air[64];
static int nair;

// our radio: home unless inside [away_from, away_until) on away_freq
static uint64_t away_from, away_until;
static uint16_t away_freq;
static uint64_t away_time, longest_away, run_start;
static unsigned tx_calls, excursions;
static struct awdl_chan m_seq[AWDL_SEQ_SLOTS];
static struct awdl_core *m_core, *u_core;
static uint64_t m_anchor;

static uint16_t our_freq(uint64_t g)
{
    return g >= away_from && g < away_until ? away_freq : HOME_MHZ;
}

static uint16_t master_freq(uint64_t g)
{
    if (g < m_anchor)
        return 0;
    return awdl_chan_freq(m_seq[((g - m_anchor) / 65536) % AWDL_SEQ_SLOTS]);
}

static void queue(int from, const uint8_t *frame, size_t len, uint16_t freq, uint64_t at)
{
    if (nair == (int)(sizeof air / sizeof air[0]) || len > sizeof air[0].data)
        return;
    struct pending *p = &air[nair++];
    p->at = at;
    p->from = from;
    p->freq = freq;
    p->len = len;
    memcpy(p->data, frame, len);
}

static void close_excursion(void)
{
    if (away_until > away_from) {
        const uint64_t d = away_until - away_from;
        away_time += d;
        if (d > longest_away)
            longest_away = d;
    }
    away_from = away_until = 0;
}

static int u_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx;
    (void)no_ack;
    tx_calls++;
    const uint64_t send = s_now + 1000 + rnd(2000);
    if (freq != HOME_MHZ) {
        const uint64_t until = stay_until ? us_global(stay_until) : send + 500;
        if (away_freq == freq && s_now < away_until) {
            away_until = until > away_until ? until : away_until;  // one more frame on the same stay
        } else {
            if (s_now < away_until)
                away_until = s_now;
            if (s_now >= run_start)
                close_excursion();
            away_from = s_now + 300;
            away_until = until;
            away_freq = freq;
            excursions++;
        }
    }
    queue(1, frame, len, freq, send);
    return 0;
}

static int m_tx(void *ctx, const uint8_t *frame, size_t len, uint16_t freq, uint64_t stay_until, int no_ack)
{
    (void)ctx;
    (void)stay_until;
    (void)no_ack;
    queue(0, frame, len, freq, s_now);
    return 0;
}

static void m_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    (void)ctx;
    memcpy(m_seq, seq, sizeof m_seq);
    m_anchor = anchor;
}

static struct awdl_core *make(uint8_t last, uint32_t metric,
                              int (*tx)(void *, const uint8_t *, size_t, uint16_t, uint64_t, int),
                              void (*sched)(void *, const struct awdl_chan *, uint64_t))
{
    struct awdl_core *c = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t addr[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last};
    struct awdl_config cfg;
    awdl_config_default(&cfg, addr);
    cfg.self_metric = metric;
    const struct awdl_host h = {NULL, tx, sched, NULL, NULL, NULL, NULL, NULL};
    SCHECK(awdl_core_init(c, &h, &cfg) == 0);
    return c;
}

struct counts {
    unsigned m_frames_in_our_slots, m_frames_caught, wakeups;
    int64_t worst_sync;
};

// In one of our AWDL slots, past the first 4 ms (our 2 ms guard, then the trip there).
static int our_awdl_slot(uint64_t g)
{
    const struct awdl_chan *seq = awdl_core_sequence(u_core);
    const uint16_t aw = awdl_core_aw_seq(u_core, us_local(g));
    const uint16_t f = awdl_chan_freq(seq[(aw / 4) % 16]);
    const uint64_t start = us_global(awdl_core_aw_start(u_core, (uint16_t)(aw & ~3u), us_local(g)));
    return f && f != HOME_MHZ && g >= start + 4000;
}

// Runs both cores to global time `until`; counts from run_start on.
static void run(uint64_t until, uint64_t *m_deadline, uint64_t *u_deadline, struct counts *k)
{
    while (s_now < until) {
        uint64_t next = until;
        int what = -1;
        if (*m_deadline < next) {
            next = *m_deadline;
            what = 0;
        }
        if (us_global(*u_deadline) < next) {
            next = us_global(*u_deadline);
            what = 1;
        }
        for (int i = 0; i < nair; ++i) {
            if (air[i].at <= next) {
                next = air[i].at;
                what = 2 + i;
            }
        }
        if (next > s_now)
            s_now = next;
        if (what < 0)
            break;
        if (what == 0) {
            *m_deadline = awdl_core_timer(m_core, s_now);
        } else if (what == 1) {
            uint64_t l = us_local(s_now);
            if (l < *u_deadline)
                l = *u_deadline;
            *u_deadline = awdl_core_timer(u_core, l);
            if (s_now >= run_start)
                k->wakeups++;
        } else {
            const struct pending p = air[what - 2];
            air[what - 2] = air[--nair];
            if (p.from == 0) {
                const int counted = s_now >= run_start && our_awdl_slot(p.at);
                k->m_frames_in_our_slots += counted;
                if (our_freq(p.at) == p.freq) {
                    k->m_frames_caught += counted;
                    const struct awdl_rx_meta m = {us_local(p.at) + rnd(500), p.freq, -55};
                    awdl_core_rx(u_core, p.data, p.len, &m);
                    *u_deadline = awdl_core_timer(u_core, us_local(s_now));
                }
            } else if (master_freq(p.at) == p.freq) {
                const struct awdl_rx_meta m = {p.at, p.freq, -55};
                awdl_core_rx(m_core, p.data, p.len, &m);
                *m_deadline = awdl_core_timer(m_core, s_now);
            }
        }
    }
    s_now = until;
}

static void measure_sync(struct counts *k)
{
    for (uint64_t g = s_now; g < s_now + 1048576; g += 4099) {
        const uint16_t aw = awdl_core_aw_seq(m_core, g);
        const uint64_t m_eaw = awdl_core_aw_start(m_core, (uint16_t)(aw & ~3u), g);
        const uint64_t u_eaw = us_global(awdl_core_aw_start(u_core, (uint16_t)(aw & ~3u), us_local(g)));
        const int64_t d = (int64_t)u_eaw - (int64_t)m_eaw;
        if ((d < 0 ? -d : d) > k->worst_sync)
            k->worst_sync = d < 0 ? -d : d;
    }
}

static void test_shared_radio(void)
{
    // The master: AirDrop open (Stute et al.'s "Data": 149 everywhere but channel 6 in slot 8), unassociated.
    m_core = make(0x0a, 520, m_tx, m_schedule);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = i == 8 ? (struct awdl_chan){6, 0x51} : (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(m_core, seq, 0);
    u_core = make(0x0b, 60, u_tx, NULL);
    awdl_core_set_infra(u_core, HOME_MHZ, us_local(0));
    awdl_core_start(m_core, 0);
    awdl_core_start(u_core, us_local(37000));
    uint64_t m_deadline = awdl_core_timer(m_core, 0);
    uint64_t u_deadline = awdl_core_timer(u_core, us_local(37000));
    s_now = 37000;
    struct counts k = {0};
    run_start = 10000000;
    run(run_start, &m_deadline, &u_deadline, &k);
    SCHECK(awdl_core_role(u_core) == AWDL_ROLE_FOLLOWER && awdl_core_election(u_core)->root[5] == 0x0a);
    SCHECK(awdl_slots_away(awdl_core_sequence(u_core), HOME_MHZ) == 4);  // idle: 0, 8, 9, 10
    away_time = longest_away = 0;
    tx_calls = excursions = 0;
    const uint64_t window = 57 * 1048576ull;  // whole sequences
    run(run_start + window, &m_deadline, &u_deadline, &k);
    close_excursion();
    const double home = 1.0 - (double)away_time / (double)window;
    double caught = k.m_frames_in_our_slots ? (double)k.m_frames_caught / k.m_frames_in_our_slots : 0;
    measure_sync(&k);
    const struct awdl_peer *seen = awdl_core_find(m_core, (const uint8_t[6]){0x02, 0x11, 0x22, 0x33, 0x44, 0x0b});
    printf("  shared radio, idle: AP keeps %.1f %%, longest absence %.1f ms, %.1f excursions/s, caught %.1f %% of "
           "the master's frames in our AWDL slots, EAW within %lld us, %.1f wakeups/s, %.1f sends/s\n",
           home * 100, (double)longest_away / 1000, excursions / (window / 1e6), caught * 100, (long long)k.worst_sync,
           k.wakeups / (window / 1e6), tx_calls / (window / 1e6));
    SCHECK(home > 0.74 && home < 0.77);
    SCHECK(longest_away <= 3 * 65536 + 3000);  // slots 8-10, under any access point's power-save buffering
    SCHECK(caught > 0.9);
    SCHECK(k.worst_sync < 3000);
    SCHECK(seen && seen->reachable);
    SCHECK(k.wakeups < 12 * 60 && tx_calls < 11 * 60);

    // wifip2pd registers AirDrop traffic: "Data+Infra", the access point keeps 4 of 16 slots
    awdl_core_traffic_registered(u_core, "com.apple.airdrop", us_local(s_now));
    u_deadline = awdl_core_timer(u_core, us_local(s_now));  // the driver re-arms after every request
    SCHECK(awdl_slots_away(awdl_core_sequence(u_core), HOME_MHZ) == 12);
    away_time = 0;
    excursions = 0;
    run_start = s_now;
    memset(&k, 0, sizeof k);
    run(s_now + 2 * 1048576, &m_deadline, &u_deadline, &k);
    close_excursion();
    const double home_t = 1.0 - (double)away_time / (2 * 1048576.0);
    caught = k.m_frames_in_our_slots ? (double)k.m_frames_caught / k.m_frames_in_our_slots : 0;
    printf("  shared radio, transfer: AP keeps %.1f %%, caught %.1f %%, %u excursions\n", home_t * 100, caught * 100,
           excursions);
    SCHECK(home_t > 0.24 && home_t < 0.3);
    SCHECK(caught > 0.9);
    free(m_core);
    free(u_core);
}

static unsigned switch_installs;
static struct awdl_chan switch_installed[AWDL_SEQ_SLOTS];

static void switch_schedule(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor)
{
    (void)ctx;
    (void)anchor;
    ++switch_installs;
    memcpy(switch_installed, seq, sizeof switch_installed);
}

static void switch_test_legal_channels(void)
{
    struct awdl_core *core = make(0x0c, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, 5260, 0);
    switch_awdl_core_set_channels(core, 3, 0);
    switch_installs = 0;
    awdl_core_start(core, 0);
    SCHECK(switch_installs == 1);
    SCHECK(awdl_slots_away(switch_installed, 5260) == 4);
    SCHECK(awdl_social_map(switch_installed) == 3);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(switch_installed[i].number == 0 || switch_installed[i].opclass ==
               awdl_opclass_20mhz(switch_installed[i].number));
    SCHECK(!switch_installed[1].number && switch_installed[0].number == 44);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t length = awdl_core_build_af(core, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    struct awdl_af parsed;
    SCHECK(length && !awdl_af_parse(frame, length, &parsed));
    SCHECK(!memcmp(parsed.chanseq.chan, switch_installed, sizeof switch_installed));
    switch_awdl_core_set_channels(core, 3, 0);
    SCHECK(switch_installs == 1);
    awdl_core_set_infra(core, 5180, 0);
    SCHECK(switch_installs == 2 && !memcmp(parsed.chanseq.chan, switch_installed, sizeof switch_installed));
    awdl_core_set_infra(core, 5745, 0);
    SCHECK(switch_installed[0].number == 44 && !switch_installed[1].number);
    struct awdl_core *peer = make(0x0d, 520, m_tx, NULL);
    struct awdl_chan peer_seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        peer_seq[i] = (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(peer, peer_seq, 0);
    awdl_core_start(peer, 0);
    const size_t peer_length = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, 0, frame, sizeof frame);
    const struct awdl_rx_meta meta = {0, 5745, -55};
    awdl_core_rx(core, frame, peer_length, &meta);
    SCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER && awdl_social_map(switch_installed) == 2);
    awdl_core_stop(peer);
    free(peer);
    awdl_core_traffic_registered(core, "com.apple.airdrop", 0);
    SCHECK(awdl_slots_away(switch_installed, 5745) == 4);
    switch_awdl_core_set_channels(core, 0, 0);
    SCHECK(!awdl_social_map(switch_installed));
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(!switch_installed[i].number);
    switch_awdl_core_set_channels(core, 7, 0);
    SCHECK(switch_installed[0].number == 149 && switch_installed[1].number == 149);
    struct awdl_chan requested[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        requested[i] = (struct awdl_chan){149, 0x80};
    awdl_core_set_sequence(core, requested, 0);
    switch_awdl_core_set_channels(core, 3, 0);
    SCHECK(awdl_social_map(switch_installed) == 1 && switch_installed[0].number == 6);
    switch_awdl_core_set_channels(core, 7, 0);
    SCHECK(switch_installed[0].number == 149 && switch_installed[0].opclass == 124);
    awdl_core_stop(core);
    free(core);
}

static const uint8_t switch_mac[][AWDL_SEQ_SLOTS] = {
    {6, 0, 149, 0, 0, 44, 0, 0, 6, 0, 44, 0, 0, 44, 0, 0},
    {0, 0, 149, 0, 0, 0, 0, 0, 0, 0, 149, 0, 0, 0, 0, 0},
    {44, 44, 149, 0, 0, 0, 0, 0, 6, 149, 44, 0, 0, 0, 0, 0},
};

static unsigned switch_mac_overlap(const uint8_t mac[AWDL_SEQ_SLOTS], unsigned offset)
{
    unsigned overlap = 0;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        if (mac[i] && mac[i] == switch_installed[(i + offset) % AWDL_SEQ_SLOTS].number)
            ++overlap;
    return overlap;
}

static void switch_check_home_six(unsigned mask, unsigned away, unsigned pass)
{
    SCHECK(away <= 4);
    if (mask == 1)
        SCHECK(away == 0);
    if (mask & 1u)
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
            SCHECK(switch_installed[i].number != 0);
    if ((mask & 6u) == 6u) {
        const uint8_t slot2 = switch_installed[2].number;
        const uint8_t slot10 = switch_installed[10].number;
        SCHECK(slot2 == 149 || (slot2 == 44 && slot10 == 149));
        if (pass % 3u == 0u)
            SCHECK(slot2 == 149 && slot10 == 44);
    } else {
        if (mask & 4u)
            SCHECK(switch_installed[2].number == 149);
        if (mask & 2u)
            SCHECK(switch_installed[10].number == 44);
    }
}

static void switch_test_search_windows(uint16_t infra_mhz)
{
    const uint64_t period = 2 * AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    const uint8_t channels[] = {6, 44, 149};
    const int home_six = infra_mhz == 2437;
    for (unsigned mask = 1; mask <= 7; ++mask) {
        struct awdl_core *core = make(0x0d, 60, m_tx, switch_schedule);
        awdl_core_set_infra(core, infra_mhz, 0);
        switch_awdl_core_set_channels(core, (uint16_t)mask, 0);
        awdl_core_start(core, 0);
        const unsigned bands = (mask & 1u) + ((mask >> 1) & 1u) + (mask >> 2);
        const unsigned cycle = home_six ? ((mask & 6u) == 6u ? 24u : 16u) : 8u * bands;
        const uint8_t primary = (mask & 4u) ? 149 : (mask & 2u) ? 44 : 6;
        const uint8_t secondary = (mask & 1u) ? 6 : (mask & 2u) ? 44 : primary;
        struct awdl_chan first[AWDL_SEQ_SLOTS];
        memcpy(first, switch_installed, sizeof first);
        unsigned visits[3] = {0};
        unsigned away_total = 0;
        unsigned gaps[3][AWDL_SEQ_SLOTS] = {{0}};
        unsigned max_gap[3] = {0};
        for (unsigned pass = 0; pass <= 2 * cycle; ++pass) {
            const uint64_t now = period * pass;
            awdl_core_timer(core, now);
            const unsigned away = awdl_slots_away(switch_installed, infra_mhz);
            if (home_six) {
                switch_check_home_six(mask, away, pass);
            } else {
                SCHECK(away == (pass % 4u ? 5u : 4u));
                SCHECK(switch_installed[2].number == primary || switch_installed[10].number == primary);
                SCHECK(switch_installed[8].number == secondary || switch_installed[0].number == secondary);
            }
            SCHECK(!(awdl_social_map(switch_installed) & ~mask));
            if (!(pass % cycle)) {
                SCHECK(!memcmp(first, switch_installed, sizeof first));
                if (pass) {
                    for (unsigned n = 0; n < 3; ++n) {
                        if (home_six && n == 0 && (mask & 1u))
                            SCHECK(visits[n] != 0);
                        else
                            SCHECK(visits[n] == ((mask & (1u << n)) ? 0xffffu : 0u));
                    }
                    if (home_six)
                        SCHECK(away_total * bands <= 4u * (bands - (mask & 1u)) * cycle);
                }
                memset(visits, 0, sizeof visits);
                away_total = 0;
            }
            for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
                for (unsigned n = 0; n < 3; ++n)
                    if (switch_installed[i].number == channels[n])
                        visits[n] |= 1u << i;
            away_total += away;
            if (mask == 7 || (home_six && mask == 6)) {
                for (unsigned n = 0; n < sizeof switch_mac / sizeof switch_mac[0]; ++n) {
                    SCHECK(switch_mac_overlap(switch_mac[n], 0));
                    if (home_six && mask == 7) {
                        for (unsigned offset = 0; offset < AWDL_SEQ_SLOTS; ++offset) {
                            unsigned *gap = &gaps[n][offset];
                            *gap = switch_mac_overlap(switch_mac[n], offset) ? 0 : *gap + 1;
                            if (*gap > max_gap[n])
                                max_gap[n] = *gap;
                        }
                    }
                }
            }
            for (unsigned n = 0; n < 2; ++n) {
                uint8_t frame[AWDL_MAX_FRAME];
                struct awdl_af action;
                const size_t size = awdl_core_build_af(core, n ? AWDL_SUBTYPE_MIF : AWDL_SUBTYPE_PSF,
                                                       now, frame, sizeof frame);
                SCHECK(size && !awdl_af_parse(frame, size, &action));
                SCHECK(action.sync.has_seq && !memcmp(action.sync.seq.chan, switch_installed, sizeof switch_installed));
                SCHECK(action.chanseq.count == AWDL_SEQ_SLOTS &&
                       !memcmp(action.chanseq.chan, switch_installed, sizeof switch_installed));
            }
        }
        if (home_six && mask == 7) {
            SCHECK(max_gap[0] <= 2 && max_gap[1] <= 11 && max_gap[2] <= 2);
            struct awdl_chan requested[AWDL_SEQ_SLOTS] = {{6, 81}, {44, 115}};
            const uint64_t end = 2 * cycle * period;
            awdl_core_set_sequence(core, requested, end);
            const unsigned installed = switch_installs;
            awdl_core_timer(core, end + period);
            SCHECK(installed == switch_installs && !memcmp(switch_installed, requested, sizeof requested));
        }
        if (!(mask & (mask - 1u))) {
            const uint64_t end = 2 * cycle * period;
            const uint16_t freq = awdl_chan_freq((struct awdl_chan){primary, awdl_opclass_20mhz(primary)});
            awdl_core_set_infra(core, freq, end);
            const unsigned fixed = switch_installs;
            awdl_core_timer(core, end + period);
            SCHECK(switch_installs == fixed);
        }
        awdl_core_stop(core);
        free(core);
    }
}

static unsigned switch_first_slot_illegal(const struct awdl_core *core, unsigned mask)
{
    const uint8_t first = awdl_core_sequence(core)[0].number;
    const unsigned bit = first == 6 ? 1u : first == 44 ? 2u : first == 149 ? 4u : 0u;
    return !(bit & mask);
}

static struct awdl_core *switch_first_slot_core(uint16_t infra, unsigned mask)
{
    struct awdl_core *core = make(0x0f, 60, m_tx, switch_schedule);
    awdl_core_set_infra(core, infra, 0);
    switch_awdl_core_set_channels(core, (uint16_t)mask, 0);
    awdl_core_start(core, 0);
    return core;
}

static void switch_hear_peer_on(struct awdl_core *core, uint8_t channel, uint64_t now)
{
    struct awdl_core *peer = make(0x10, 520, m_tx, NULL);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){channel, awdl_opclass_20mhz(channel)};
    awdl_core_set_sequence(peer, seq, 0);
    awdl_core_start(peer, 0);
    awdl_core_timer(peer, now);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t bytes = awdl_core_build_af(peer, AWDL_SUBTYPE_PSF, now, frame, sizeof frame);
    const struct awdl_rx_meta meta = {now, awdl_chan_freq(seq[0]), -45};
    awdl_core_rx(core, frame, bytes, &meta);
    awdl_core_stop(peer);
    free(peer);
}

// Every search pass idle and in transfer, then a timed peer's sequence idle and in transfer.
static void switch_test_first_slot(void)
{
    const uint64_t period = 2 * AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    for (unsigned channel = 0; channel <= 177; ++channel) {
        if (channel && !awdl_opclass_20mhz((uint8_t)channel))
            continue;
        const uint16_t infra = awdl_chan_freq((struct awdl_chan){(uint8_t)channel, 0});
        for (unsigned mask = 1; mask <= 7; ++mask) {
            unsigned illegal = 0;
            for (unsigned transfer = 0; transfer < 2; ++transfer) {
                struct awdl_core *core = switch_first_slot_core(infra, mask);
                for (unsigned pass = 0; pass < 48; ++pass) {
                    if (transfer)
                        awdl_core_traffic_registered(core, "com.apple.airdrop", period * pass);
                    awdl_core_timer(core, period * pass);
                    illegal += switch_first_slot_illegal(core, mask);
                }
                awdl_core_stop(core);
                free(core);
            }
            struct awdl_core *core = switch_first_slot_core(infra, mask);
            switch_hear_peer_on(core, mask & 1u ? 6 : mask & 2u ? 44 : 149, period);
            SCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER);
            illegal += switch_first_slot_illegal(core, mask);
            awdl_core_traffic_registered(core, "com.apple.airdrop", period);
            SCHECK(awdl_core_mode(core) == AWDL_MODE_TRANSFER);
            illegal += switch_first_slot_illegal(core, mask);
            awdl_core_stop(core);
            free(core);
            if (illegal)
                fprintf(stderr, "  slot 0 outside the legal channels %u times: AP channel %u, legal mask %u\n", illegal,
                        channel, mask);
            SCHECK(!illegal);
        }
    }
}

static void switch_test_acquisition(void)
{
    struct awdl_core *core = make(0x0d, 60, m_tx, switch_schedule);
    const uint64_t period = 2 * AWDL_SEQ_SLOTS * AWDL_EAW_TU * AWDL_TU_US;
    awdl_core_set_infra(core, 2437, 0);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_start(core, 0);
    SCHECK(switch_installed[2].number == 149 && switch_installed[10].number == 44);
    for (unsigned slot = 0; slot < AWDL_SEQ_SLOTS; ++slot)
        awdl_core_timer(core, slot * (period / 32) + period / 64);
    SCHECK(awdl_core_stats(core)->mif_tx == AWDL_SEQ_SLOTS);
    awdl_core_set_infra(core, 5180, period);
    struct awdl_core *peer = make(0x0e, 520, m_tx, NULL);
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){44, 115};
    const uint64_t now = 12 * period;
    awdl_core_set_sequence(peer, seq, 0);
    awdl_core_start(peer, 0);
    awdl_core_timer(peer, now);
    uint8_t frame[AWDL_MAX_FRAME];
    const size_t bytes = awdl_core_build_af(peer, AWDL_SUBTYPE_PSF, now, frame, sizeof frame);
    const struct awdl_rx_meta meta = {now, 5220, -45};
    const unsigned before = switch_installs;
    awdl_core_rx(core, frame, bytes, &meta);
    SCHECK(switch_installs == before + 1);
    SCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER);
    awdl_core_set_infra(core, 2437, now);
    SCHECK(switch_installed[0].number == 44 && switch_installed[2].number == 6);
    awdl_core_set_infra(core, 5745, now);
    SCHECK(switch_installed[0].number == 44 && switch_installed[1].number == 149);
    unsigned installed = switch_installs;
    awdl_core_timer(core, now + period / 2);
    SCHECK(switch_installs == installed);
    switch_awdl_core_set_channels(core, 1, now + period / 2);
    SCHECK(awdl_slots_away(switch_installed, 5745) == 4 && !switch_installed[10].number);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        seq[i] = (struct awdl_chan){6, 81};
    awdl_core_set_sequence(peer, seq, now + period);
    const size_t six = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, now + period, frame, sizeof frame);
    const struct awdl_rx_meta ch6 = {now + period, 2437, -45};
    awdl_core_rx(core, frame, six, &ch6);
    SCHECK(switch_installed[0].number == 6 && switch_installed[10].number == 6 && !switch_installed[1].number);

    const uint8_t src[] = {2, 0x11, 0x22, 0x33, 0x44, 0x0e};
    uint8_t eth[54] = {2, 0x11, 0x22, 0x33, 0x44, 0x0d};
    memcpy(eth + 6, src, sizeof src);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    const size_t data = awdl_data_encap(eth, sizeof eth, src, 1, frame, sizeof frame);
    const struct awdl_rx_meta late = {ch6.tsf + AWDL_PEER_TIMEOUT_US + period / 2, 2437, -45};
    SCHECK(data != 0);
    awdl_core_rx(core, frame, data, &late);
    awdl_core_timer(core, late.tsf);
    const struct awdl_peer *known = awdl_core_find(core, src);
    SCHECK(known && known->last_seen == late.tsf);
    SCHECK(awdl_slots_away(switch_installed, 5745) == 4 && !switch_installed[10].number);
    const size_t fresh = awdl_core_build_af(peer, AWDL_SUBTYPE_MIF, late.tsf, frame, sizeof frame);
    awdl_core_rx(core, frame, fresh, &late);
    SCHECK(switch_installed[10].number == 6);

    awdl_core_set_addr(core, (const uint8_t[6]){2, 1, 2, 3, 4, 5});
    awdl_core_set_infra(core, 0, late.tsf);
    switch_awdl_core_set_channels(core, 3, late.tsf);
    for (unsigned pass = 0; pass < 2; ++pass) {
        awdl_core_timer(core, late.tsf + period * pass);
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
            SCHECK(switch_installed[i].number == (pass ? 6 : 44));
    }
    awdl_core_stop(peer);
    awdl_core_stop(core);
    free(peer);
    free(core);
}

#if DEVELOPMENT || DEBUG
static void switch_test_home_trial(void)
{
    struct awdl_core *core = make(0x0d, 60, m_tx, switch_schedule);
    switch_awdl_core_set_channels(core, 7, 0);
    awdl_core_set_infra(core, 2437, 0);
    SCHECK(!switch_awdl_core_home_trial(core, 1, 0));
    awdl_core_start(core, 0);
    SCHECK(switch_installed[2].number == 149 && switch_installed[10].number == 44);
    SCHECK(switch_awdl_core_home_trial(core, 1, 1000) == 6);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        SCHECK(switch_installed[i].number == 6 && switch_installed[i].opclass == 81);
        SCHECK(switch_mac_overlap(switch_mac[2], i) == 1);
    }
    uint8_t frame[AWDL_MAX_FRAME];
    const uint8_t subtypes[] = {AWDL_SUBTYPE_PSF, AWDL_SUBTYPE_MIF};
    for (unsigned i = 0; i < sizeof subtypes; ++i) {
        const size_t bytes = awdl_core_build_af(core, subtypes[i], 1000, frame, sizeof frame);
        struct awdl_af action;
        SCHECK(bytes && !awdl_af_parse(frame, bytes, &action));
        SCHECK(action.sync.has_seq && !memcmp(action.sync.seq.chan, switch_installed, sizeof switch_installed));
        SCHECK(!memcmp(action.chanseq.chan, switch_installed, sizeof switch_installed));
    }

    struct awdl_chan desired[AWDL_SEQ_SLOTS] = {{44, 115}};
    awdl_core_set_sequence(core, desired, 2000);
    awdl_core_traffic_registered(core, "com.apple.airdrop", 2100);
    SCHECK(switch_installed[0].number == 6 && switch_installed[15].number == 6);
    switch_awdl_core_home_trial(core, 0, 3000);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));

    SCHECK(switch_awdl_core_home_trial(core, 1, 4000) == 6);
    switch_awdl_core_set_channels(core, 2, 4100);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));
    SCHECK(!switch_awdl_core_home_trial(core, 1, 4200));
    awdl_core_set_infra(core, 5220, 4300);
    SCHECK(switch_awdl_core_home_trial(core, 1, 4400) == 44);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        SCHECK(switch_installed[i].number == 44 && switch_installed[i].opclass == 115);
    awdl_core_set_infra(core, 5180, 4500);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));
    SCHECK(!switch_awdl_core_home_trial(core, 1, 4600));
    awdl_core_set_infra(core, 5220, 4700);
    SCHECK(switch_awdl_core_home_trial(core, 1, 4800) == 44);
    awdl_core_stop(core);
    awdl_core_start(core, 5000);
    SCHECK(!memcmp(switch_installed, desired, sizeof desired));
    awdl_core_stop(core);
    free(core);
}
#endif

void test_sched_main(void)
{
    const int before = test_failures;
    test_shared_radio();
    switch_test_legal_channels();
    switch_test_search_windows(2462);
    switch_test_search_windows(2437);
    switch_test_first_slot();
    switch_test_acquisition();
#if DEVELOPMENT || DEBUG
    switch_test_home_trial();
#endif
    printf("shared radio: idle and transfer schedules beside the access point: %s\n",
           test_failures == before ? "ok" : "FAILED");
}
