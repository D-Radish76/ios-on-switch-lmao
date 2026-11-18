// SPDX-License-Identifier: BSD-2-Clause-Patent
// The soft backend's data path: awdl0's Ethernet frames out as AWDL data frames when the peer listens on our channel
// (now, or queued for its next slot), and AWDL data frames in, A-MSDUs included, as Ethernet frames.
#include "awdl_internal.h"

enum { SWITCH_AWDL_UNICAST_GUARD_US = 3u * AWDL_TU_US, SWITCH_AWDL_MULTICAST_GUARD_US = 16u * AWDL_TU_US };

struct channel_window {
    uint64_t open, close, boundary;
    uint8_t channel;
};

static struct channel_window channel_window(const struct awdl_chan *seq, unsigned count, uint64_t phase, uint64_t now,
                                            unsigned guard, int bonjour)
{
    const unsigned slot = (unsigned)((phase / AWDL_EAW_US) % AWDL_SEQ_SLOTS);
    const uint64_t elapsed = phase % AWDL_EAW_US;
    const uint8_t current = awdl_chan_primary(seq[slot % count]);
    const uint64_t boundary = now + AWDL_EAW_US - elapsed;
    struct channel_window w = {now, boundary, boundary, current};
    if (bonjour && slot != 0 && slot != 10)
        w.channel = 0;
    if (elapsed < guard &&
        (bonjour || current != awdl_chan_primary(seq[((slot + AWDL_SEQ_SLOTS - 1) % AWDL_SEQ_SLOTS) % count])))
        w.open += guard - elapsed;
    if (bonjour || current != awdl_chan_primary(seq[((slot + 1) % AWDL_SEQ_SLOTS) % count]))
        w.close = boundary - now > guard ? boundary - guard : now;
    return w;
}

// Both phases are expressed in local radio TSF. Their slot boundaries may fall at different times.
static uint64_t transmit_window(struct awdl_core *c, const uint8_t *dst, const struct awdl_peer *p, uint64_t now)
{
    const uint8_t mdns6[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
    const uint8_t mdns4[6] = {1, 0, 0x5e, 0, 0, 0xfb};
    const int bonjour = !p && (awdl_addr_eq(dst, mdns6) || awdl_addr_eq(dst, mdns4));
    const unsigned guard =
        bonjour ? SWITCH_AWDL_MULTICAST_GUARD_US : (p ? SWITCH_AWDL_UNICAST_GUARD_US : AWDL_SLOT_GUARD_US);
    for (uint64_t at = now; at - now < AWDL_SEQ_US;) {
        const uint64_t phase = at - c->base_tsf + (uint64_t)c->base_seq * AWDL_AW_US;
        struct channel_window w = channel_window(c->cfg.seq, AWDL_SEQ_SLOTS, phase, at, guard, bonjour);
        if (p) {
            const uint64_t peer_phase = at - p->next_aw_tsf + (uint64_t)p->next_aw_seq * AWDL_AW_US;
            const struct channel_window other = channel_window(p->seq.chan, p->seq.count, peer_phase, at, guard, 0);
            if (other.channel != w.channel)
                w.channel = 0;
            if (other.open > w.open)
                w.open = other.open;
            if (other.close < w.close)
                w.close = other.close;
            if (other.boundary < w.boundary)
                w.boundary = other.boundary;
        }
        if (w.channel && w.open < w.close) {
            if (w.open == now)
                return w.close;
            if (w.open < c->next_data)
                c->next_data = w.open;
            break;
        }
        at = w.boundary;
    }
    return 0;
}

static int send(struct awdl_core *c, const uint8_t *frame, size_t len, int group, uint64_t now, uint64_t stay)
{
    const uint16_t freq = awdl_chan_freq(awdl_core_channel_at(c, now));
    const int result = c->host.tx(c->host.ctx, frame, len, freq, stay, group);
    if (result) {
        c->stats.tx_refused++;
        return result;
    }
    c->stats.data_tx++;
    return 0;
}

void awdl_data_forget(struct awdl_core *c, const uint8_t peer[6])
{
    unsigned kept = 0;
    for (unsigned i = 0; i < c->txq_count; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (awdl_addr_eq(e->dst, peer)) {
            ++c->stats.data_drop;
            continue;
        }
        if (kept != i)
            c->txq[(c->txq_head + kept) % AWDL_TXQ_FRAMES] = *e;
        ++kept;
    }
    c->txq_count = kept;
}

void awdl_data_flush(struct awdl_core *c, uint64_t now)
{
    if (!switch_awdl_core_ready(c))
        return;
    c->waiting_credit = 0;
    c->next_data = UINT64_MAX;
    unsigned kept = 0;
    int blocked = 0;
    const unsigned n = c->txq_count;
    for (unsigned i = 0; i < n; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        const struct awdl_peer *p = e->group ? NULL : awdl_core_find(c, e->dst);
        if (now >= e->expires || (!e->group && (!p || !p->reachable || !p->seq.count))) {
            c->stats.data_drop++;
            continue;
        }
        uint64_t stay = blocked ? 0 : transmit_window(c, e->dst, p, now);
        if (stay) {
            const int change = e->transfer && c->mode != AWDL_MODE_TRANSFER;
            if (e->transfer)
                awdl_sched_transfer(c, e->dst, 1, now);
            if (!switch_awdl_core_ready(c)) {
                blocked = 1;
            } else if (!change || (stay = transmit_window(c, e->dst, p, now)) != 0) {
                const int result = send(c, e->frame, e->len, e->group, now, stay);
                if (!result)
                    continue;
                if (result != AWDL_TX_BUSY) {
                    c->stats.data_drop++;
                    continue;
                }
                c->waiting_credit = 1;
                blocked = 1;
            }
        }
        struct awdl_txq_entry *k = &c->txq[(c->txq_head + kept) % AWDL_TXQ_FRAMES];
        if (k != e)
            *k = *e;
        if (k->expires < c->next_data)
            c->next_data = k->expires;
        kept++;
    }
    c->txq_count = kept;
    if (!kept)
        c->next_data = UINT64_MAX;
}

int awdl_core_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now)
{
    if (!c->running || len < 14 || len > AWDL_MAX_FRAME - 26) {
        c->stats.data_drop++;
        return -1;
    }
    const int group = awdl_is_group(eth);
    const struct awdl_peer *p = group ? NULL : awdl_core_find(c, eth);
    if (!group && (!p || !p->reachable)) {
        c->stats.data_drop++;  // no such peer: no route (the kernel only knows peers we announced)
        return -2;
    }
    if (awdl_native(c))
        return awdl_native_output(c, eth, len, now);
    if (p && !p->seq.count)
        return -2;
    if (c->txq_count == AWDL_TXQ_FRAMES) {
        c->stats.data_drop++;
        return -4;
    }
    struct awdl_txq_entry *e = &c->txq[(c->txq_head + c->txq_count) % AWDL_TXQ_FRAMES];
    const size_t n = awdl_data_encap(eth, len, c->cfg.addr, c->data_seq, e->frame, sizeof e->frame);
    if (!n)
        return -5;
    e->len = (uint16_t)n;
    e->expires = (now > c->last_observation ? now : c->last_observation) + AWDL_TXQ_LIFETIME_US;
    e->group = (uint8_t)group;
    e->transfer = (uint8_t)awdl_transfer_frame(eth, len);
    memcpy(e->dst, eth, 6);
    if (now || c->radio_ready)
        awdl_sched_traffic(c, eth, len, 1, now);
    const uint64_t stay = switch_awdl_core_ready(c) ? transmit_window(c, eth, p, now) : 0;
    if (!c->txq_count && stay) {
        const int result = send(c, e->frame, e->len, group, now, stay);
        if (!result) {
            c->data_seq++;
            return 0;
        }
        if (result != AWDL_TX_BUSY) {
            c->stats.data_drop++;
            return result;
        }
        c->waiting_credit = 1;
    } else if (stay && !c->waiting_credit) {
        c->next_data = now;
    }
    c->data_seq++;
    c->txq_count++;
    if (e->expires < c->next_data)
        c->next_data = e->expires;
    return 0;
}

void awdl_data_rx(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m)
{
    struct awdl_msdu msdus[AWDL_MAX_MSDUS];
    uint16_t seq = 0;
    int retry = 0;
    const size_t n = awdl_data_msdus(f, len, msdus, AWDL_MAX_MSDUS, &seq, &retry);
    if (!n) {
        c->stats.data_bad++;
        return;
    }
    if (!awdl_is_group(f + 4) && !awdl_addr_eq(f + 4, c->cfg.addr))
        return;  // someone else's
    struct awdl_peer *p = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS && !p; ++i) {
        if (c->peers[i].used && awdl_addr_eq(c->peers[i].addr, f + 10))
            p = &c->peers[i];
    }
    if (p) {
        if (m->tsf > p->last_seen)
            p->last_seen = m->tsf;
        if (retry && p->has_data_seq && p->last_data_seq == seq)
            return;  // a retransmission we already took
        p->last_data_seq = seq;
        p->has_data_seq = 1;
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t l = awdl_msdu_ether(&msdus[i], c->scratch, sizeof c->scratch);
        if (!l) {
            c->stats.data_bad++;
            continue;
        }
        c->stats.data_rx++;
        if (c->host.deliver)
            c->host.deliver(c->host.ctx, c->scratch, l);
        if (c->last_observation - m->tsf < AWDL_TRANSFER_HOLD_US)
            awdl_sched_traffic(c, c->scratch, l, 0, m->tsf);
    }
}
