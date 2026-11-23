// SPDX-License-Identifier: BSD-2-Clause-Patent
// The channel sequence both backends run (awdl.h awdl_schedule): the slots macOS keeps in every state, the free slots
// on the access point's channel, and the channel search a radio limited to legal channels runs until a peer is timed.
#include "awdl_internal.h"

static struct awdl_chan social5_chan(uint8_t ch)
{
    return (struct awdl_chan){ch == 44 ? 44 : 149, 0x80};
}

void awdl_schedule_from(struct awdl_chan out[AWDL_SEQ_SLOTS], uint16_t mask, struct awdl_chan free_chan, uint8_t first,
                        uint8_t second)
{
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        if (i == 8) {
            out[i] = (struct awdl_chan){6, 81};
        } else if (((unsigned)mask >> i) & 1u) {
            out[i] = social5_chan(i < 8 ? first : second);
        } else {
            out[i] = free_chan;  // the access point's channel, or empty while unassociated
        }
    }
}

static const uint8_t awdl_slot_channels[AWDL_SEQ_SLOTS] = {0, 0, 149, 0, 0, 0, 0, 0, 6, 0, 44, 0, 0, 0, 0, 0};

void awdl_schedule(struct awdl_chan out[AWDL_SEQ_SLOTS], uint16_t infra_mhz)
{
    const struct awdl_chan infra = awdl_chan_of_freq(infra_mhz);
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t ch = awdl_slot_channels[i];
        out[i] = ch == 6 ? (struct awdl_chan){6, 81} : ch ? social5_chan(ch) : infra;
    }
    if (!out[0].number)
        out[0] = (struct awdl_chan){6, 81};
}

unsigned awdl_slots_away(const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint16_t infra_mhz)
{
    const uint8_t ip = awdl_chan_of_freq(infra_mhz).number;
    unsigned n = 0;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t p = awdl_chan_primary(seq[i]);
        n += p && p != ip;
    }
    return n;
}

uint16_t awdl_social_map(const struct awdl_chan seq[AWDL_SEQ_SLOTS])
{
    uint16_t map = 0;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
        const uint8_t p = awdl_chan_primary(seq[i]);
        if (p == 6)
            map |= 0x1u;
        if (p == 44)
            map |= 0x2u;
        if (p == 149)
            map |= 0x4u;
    }
    return map;
}

static uint16_t social_bit(uint8_t channel)
{
    return channel == 6 ? 1u : channel == 44 ? 2u : channel == 149 ? 4u : 0u;
}

static uint8_t secondary_social(uint16_t legal)
{
    return (legal & 1u) ? 6 : (legal & 2u) ? 44 : 149;
}

// A 5 GHz social slot whose channel the radio may not use moves to the other 5 GHz one, else to 6.
static struct awdl_chan legal_stand_in(uint16_t legal, uint8_t channel)
{
    if (channel != 6 && (legal & 6u))
        return social5_chan((legal & 4u) ? 149 : 44);
    return channel != 6 && (legal & 1u) ? (struct awdl_chan){6, 81} : (struct awdl_chan){0, 0};
}

// A full advertised sequence takes 1.048576 s. Two cycles leave a full peer cycle after schedule installation.
#define SWITCH_AWDL_SEARCH_US (2 * AWDL_SEQ_US)

static void reset_search(struct awdl_core *c, uint64_t now)
{
    c->switch_search_pass = 0;
    c->switch_next_search = now + SWITCH_AWDL_SEARCH_US;
}

static int searching(const struct awdl_core *c)
{
#if DEVELOPMENT || DEBUG
    if (c->switch_home_trial_channel)
        return 0;
#endif
    return c->switch_channels_limited && c->switch_legal_channels && !c->seq_fixed && !c->switch_timed_peer;
}

static int search_moves(const struct awdl_core *c)
{
    const uint16_t mask = c->switch_legal_channels;
    return searching(c) && ((mask & (mask - 1u)) ||
                            (c->infra_mhz && mask != social_bit(awdl_chan_of_freq(c->infra_mhz).number)));
}

static void search_away(struct awdl_core *c, const uint8_t channels[3], unsigned count)
{
    const unsigned pass = c->switch_search_pass;
    const unsigned first = pass % 8u;
    const unsigned second = 8u + (first + 4u) % 8u;
    const unsigned channel = (first + pass / 8u) % count;
    const unsigned legal = c->switch_legal_channels;
    const uint8_t primary = (legal & 4u) ? 149 : (legal & 2u) ? 44 : 6;
    const uint8_t secondary = secondary_social((uint16_t)legal);
    unsigned anchor = primary == 44 ? 10u : 2u;
    if (first == anchor || second == anchor)
        anchor ^= 8u;

    // The captured Mac schedules use 2/10 for 5 GHz and 8/0 for 2.4 GHz. Stagger the searches to keep an anchor in each pair.
    memset(c->cfg.seq, 0, sizeof c->cfg.seq);
    c->cfg.seq[anchor].number = primary;
    c->cfg.seq[0].number = secondary;
    c->cfg.seq[8].number = secondary;
    c->cfg.seq[first].number = channels[channel];
    c->cfg.seq[second].number = channels[(channel + 1) % count];
}

static void search_from_channel_six(struct awdl_core *c, struct awdl_chan home)
{
    const unsigned away = c->switch_legal_channels & 6u;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        c->cfg.seq[i] = home;
    if (away & 4u)
        c->cfg.seq[2].number = 149;
    if (away & 2u)
        c->cfg.seq[10].number = 44;

    unsigned phase = c->switch_search_pass;
    if (away == 6u) {
        // Rotate through both bands on two passes out of three. Keep 149 reachable during the sweep.
        if (phase % 3u == 0u)
            return;
        c->cfg.seq[10] = home;
        phase -= phase / 3u + 1u;
    }
    phase %= AWDL_SEQ_SLOTS;
    if (away & 4u)
        c->cfg.seq[phase].number = 149;
    if (away & 2u)
        c->cfg.seq[phase ^ 8u].number = 44;
    if (away == 6u && phase == 10u && !(c->switch_legal_channels & 1u)) {
        // Both anchors are exchanged. These 44 slots reach the captured idle and infrastructure-priority profiles.
        c->cfg.seq[0].number = 44;
        c->cfg.seq[5].number = 44;
    }
}

static void search_sequence(struct awdl_core *c, struct awdl_chan home)
{
    if (c->infra_mhz && home.number == 6) {
        search_from_channel_six(c, home);
        return;
    }
    const uint8_t candidates[] = {home.number, 44, 149, 6};
    uint8_t channels[3];
    unsigned count = 0;
    uint16_t remaining = c->switch_legal_channels;
    for (unsigned i = 0; i < sizeof candidates; ++i) {
        const uint16_t bit = social_bit(candidates[i]);
        if (remaining & bit) {
            channels[count++] = candidates[i];
            remaining &= (uint16_t)~bit;
        }
    }
    if (c->infra_mhz && !social_bit(home.number)) {
        search_away(c, channels, count);
        return;
    }
    const uint8_t channel = channels[c->switch_search_pass % count];
    const struct awdl_chan listen = {channel, awdl_opclass_20mhz(channel)};
    // With a station, a visiting channel gets one EAW in four. Moving that EAW on each sweep covers every peer phase.
    const unsigned offset = (c->switch_search_pass / count) % 4u;
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
        c->cfg.seq[i] = !c->infra_mhz || i % 4 == offset ? listen : home;
}

static int usable_timing(const struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    if (!p || !p->used || !(p->has_v2 || p->reachable) || !p->has_seq || !p->next_aw_tsf ||
        (now > p->next_aw_tsf && now - p->next_aw_tsf > AWDL_PEER_TIMEOUT_US))
        return 0;
    for (unsigned i = 0; i < p->seq.count; ++i)
        if (c->switch_legal_channels & social_bit(awdl_chan_primary(p->seq.chan[i])))
            return 1;
    return 0;
}

static int any_timed_peer(const struct awdl_core *c, uint64_t now)
{
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i)
        if (usable_timing(c, &c->peers[i], now))
            return 1;
    return 0;
}

static void build(struct awdl_core *c)
{
#if DEVELOPMENT || DEBUG
    if (c->switch_home_trial_channel) {
        const uint8_t home = awdl_chan_of_freq(c->infra_mhz).number;
        if (home == c->switch_home_trial_channel && (social_bit(home) & c->switch_legal_channels)) {
            for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i)
                c->cfg.seq[i] = (struct awdl_chan){home, awdl_opclass_20mhz(home)};
            return;
        }
        c->switch_home_trial_channel = 0;
    }
#endif
    if (c->seq_fixed) {
        memcpy(c->cfg.seq, c->switch_requested_seq, sizeof c->cfg.seq);
    } else if (awdl_native(c)) {
        awdl_native_build_seq(c);
    } else if (c->switch_channels_limited && searching(c)) {
        search_sequence(c, awdl_chan_of_freq(c->infra_mhz));
    } else {
        awdl_schedule(c->cfg.seq, c->infra_mhz);
        for (unsigned i = 0; c->switch_channels_limited && i < AWDL_SEQ_SLOTS; ++i)
            if (awdl_slot_channels[i] && !(social_bit(awdl_slot_channels[i]) & c->switch_legal_channels))
                c->cfg.seq[i] = legal_stand_in(c->switch_legal_channels, awdl_slot_channels[i]);
    }
    if (!awdl_native(c) && c->switch_channels_limited) {
        for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) {
            const uint8_t channel = awdl_chan_primary(c->cfg.seq[i]);
            const uint16_t bit = social_bit(channel);
            c->cfg.seq[i] = (bit & c->switch_legal_channels)
                               ? (struct awdl_chan){channel, awdl_opclass_20mhz(channel)}
                               : (struct awdl_chan){0, 0};
        }
        // macOS drops a peer's whole frame when slot 0 of its sequence is empty; other empty slots are accepted.
        if (!c->cfg.seq[0].number && c->switch_legal_channels) {
            const uint8_t channel = secondary_social(c->switch_legal_channels);
            c->cfg.seq[0] = (struct awdl_chan){channel, awdl_opclass_20mhz(channel)};
        }
    }
}

#if DEVELOPMENT || DEBUG
uint8_t switch_awdl_core_home_trial(struct awdl_core *c, int enable, uint64_t now)
{
    const uint8_t home = enable ? awdl_chan_of_freq(c->infra_mhz).number : 0;
    if (enable && (!c->running || awdl_native(c) || !c->switch_channels_limited ||
                   !(social_bit(home) & c->switch_legal_channels)))
        return 0;
    if (home != c->switch_home_trial_channel) {
        c->switch_home_trial_channel = home;
        build(c);
        awdl_soft_install(c, now ? now : c->last_observation);
    }
    return home;
}
#endif

static void apply(struct awdl_core *c, uint16_t old_map, uint64_t now)
{
    if (awdl_native(c)) {
        awdl_native_resend(c, old_map);
    } else {
        awdl_soft_install(c, now);
    }
}

void awdl_core_set_sequence(struct awdl_core *c, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t now)
{
    memcpy(c->switch_requested_seq, seq, sizeof c->switch_requested_seq);
    c->seq_fixed = 1;
    build(c);
    awdl_soft_install(c, now);
}

void switch_awdl_core_set_channels(struct awdl_core *c, uint16_t mask, uint64_t now)
{
    mask &= 7u;
    if (awdl_native(c) || (c->switch_channels_limited && c->switch_legal_channels == mask))
        return;
    struct awdl_chan was[AWDL_SEQ_SLOTS];
    memcpy(was, c->cfg.seq, sizeof was);
    c->switch_channels_limited = 1;
    c->switch_legal_channels = mask;
    now = now ? now : c->last_observation;
    c->switch_timed_peer = (uint8_t)any_timed_peer(c, now);
    reset_search(c, now);
    build(c);
    if (c->running && memcmp(was, c->cfg.seq, sizeof was))
        awdl_soft_install(c, now);
}

// One line per schedule: why, and how many slots leave the access point's channel.
void awdl_sched_log(struct awdl_core *c, const char *fmt, ...)
{
    char why[96];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    const unsigned n = awdl_slots_away(c->cfg.seq, c->infra_mhz);
    if (c->infra_mhz) {
        awdl_log(c, "AWDL: schedule (%s): %u of 16 slots away from infra (%u MHz)", why, n, c->infra_mhz);
    } else {
        awdl_log(c, "AWDL: schedule (%s): AWDL in %u of 16 slots, not associated", why, n);
    }
}

void awdl_sched_start(struct awdl_core *c)
{
    c->switch_timed_peer = 0;
    reset_search(c, c->last_observation);
    build(c);
}

void switch_awdl_sched_peer_timing(struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    if (!c->running || !c->switch_channels_limited)
        return;
    const int present = usable_timing(c, p, now) || (c->switch_timed_peer && any_timed_peer(c, now));
    if (present == c->switch_timed_peer)
        return;
    c->switch_timed_peer = (uint8_t)present;
    reset_search(c, now);
    struct awdl_chan previous[AWDL_SEQ_SLOTS];
    memcpy(previous, c->cfg.seq, sizeof previous);
    build(c);
    if (memcmp(previous, c->cfg.seq, sizeof previous))
        awdl_soft_install(c, now);
}

void awdl_sched_deadline(struct awdl_core *c, uint64_t now)
{
    if (search_moves(c) && now >= c->switch_next_search) {
        const uint64_t passes = 1 + (now - c->switch_next_search) / SWITCH_AWDL_SEARCH_US;
        c->switch_next_search += passes * SWITCH_AWDL_SEARCH_US;
        // Both split-band searches and the channel-6 sweep repeat within 48 passes.
        c->switch_search_pass = (uint8_t)((c->switch_search_pass + passes) % 48u);
        struct awdl_chan previous[AWDL_SEQ_SLOTS];
        memcpy(previous, c->cfg.seq, sizeof previous);
        build(c);
        if (memcmp(previous, c->cfg.seq, sizeof previous))
            awdl_soft_install(c, now);
    }
}

uint64_t awdl_sched_next(const struct awdl_core *c)
{
    return search_moves(c) ? c->switch_next_search : UINT64_MAX;
}

void awdl_core_set_infra(struct awdl_core *c, uint16_t infra_mhz, uint64_t now)
{
    if (infra_mhz == c->infra_mhz)
        return;
    c->infra_mhz = infra_mhz;
    reset_search(c, now ? now : c->last_observation);
    if (!c->running)
        return;
    const uint16_t map = awdl_social_map(c->cfg.seq);
    struct awdl_chan was[AWDL_SEQ_SLOTS];
    memcpy(was, c->cfg.seq, sizeof was);
    build(c);
    // Raw empty slots still depend on the current home channel.
    if (!memcmp(was, c->cfg.seq, sizeof was) && !c->switch_channels_limited)
        return;  // (v3: only association on/off changes its slots)
    awdl_sched_log(c, "%s", infra_mhz ? "infra channel changed" : "infra lost");
    apply(c, map, now);
}
