/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ack.h"

#include <stddef.h>

extern int32_t switch_a3_key_allocate(void *wlc, uint32_t first, uint32_t count, uint32_t consecutive);
extern uint32_t switch_a3_amt_clear(void *wlc, int32_t index);
extern int32_t switch_a3_receiver_set(void *wlc, int32_t index, const uint8_t address[6]);

static struct {
    void *wlc;
    void **keys;
    const uint8_t *key;
    uint32_t key_index;
    uint8_t address[6];
    uint8_t enabled;
} switch_a3_ack_state;

static void *switch_a3_ack_pointer(const void *base, size_t offset)
{
    return *(void *const *)((const uint8_t *)base + offset);
}

static uint32_t switch_a3_ack_word(const void *base, size_t offset)
{
    return *(const uint32_t *)((const uint8_t *)base + offset);
}

static void switch_a3_ack_forget_key(void)
{
    switch_a3_ack_state.keys = NULL;
    switch_a3_ack_state.key = NULL;
    switch_a3_ack_state.key_index = 0;
}

void switch_a3_ack_detached(void)
{
    switch_a3_ack_state.enabled = 0;
    switch_a3_ack_state.wlc = NULL;
    switch_a3_ack_forget_key();
}

static int32_t switch_a3_ack_lost(void)
{
    switch_a3_ack_state.enabled = 0;
    switch_a3_ack_forget_key();
    return -1;
}

static int switch_a3_ack_owns(void **keys, uint32_t key_count, uint32_t address_count)
{
    const uint32_t index = switch_a3_ack_state.key_index;
    const uint8_t *key = switch_a3_ack_state.key;
    if (!key || switch_a3_ack_state.keys != keys || index < 4 || index >= key_count || keys[index] != key)
        return 0;
    for (size_t offset = 0; offset < 224; ++offset) {
        const uint8_t expected = offset == 6 ? index : offset == 9 ? address_count : 0;
        if (key[offset] != expected)
            return 0;
    }
    return switch_a3_ack_pointer(key, 224) != NULL;
}

static int32_t switch_a3_ack_resource(void *wlc, int allocate)
{
    if (!wlc || wlc != switch_a3_ack_state.wlc)
        return -2;
    const void *pub = switch_a3_ack_pointer(wlc, 0);
    void **keys = switch_a3_ack_pointer(wlc, 0x2a0);
    if (!pub || !keys || !switch_a3_ack_pointer(pub, 28))
        return switch_a3_ack_lost();
    const uint32_t key_count = switch_a3_ack_word(pub, 0xbc);
    const uint32_t address_count = switch_a3_ack_word(pub, 0xc0);
    if (key_count <= 4 || key_count > 40 || address_count != 64)
        return switch_a3_ack_lost();
    if (switch_a3_ack_state.key && !switch_a3_ack_owns(keys, key_count, address_count))
        return switch_a3_ack_lost();
    if (allocate && !switch_a3_ack_state.key) {
        const int32_t index = switch_a3_key_allocate(wlc, 4, key_count - 4, 1);
        if (index < 0)
            return index;
        if (index < 4 || (uint32_t)index >= key_count)
            return switch_a3_ack_lost();
        switch_a3_ack_state.keys = keys;
        switch_a3_ack_state.key_index = index;
        switch_a3_ack_state.key = keys[index];
        if (!switch_a3_ack_owns(keys, key_count, address_count))
            return switch_a3_ack_lost();
    }
    return 0;
}

int32_t switch_a3_ack_prepare(void *wlc)
{
    if (!wlc)
        return -2;
    if (wlc != switch_a3_ack_state.wlc) {
        switch_a3_ack_detached();
        switch_a3_ack_state.wlc = wlc;
    }
    return switch_a3_ack_resource(wlc, 1);
}

int32_t switch_a3_ack_reconcile(void *wlc)
{
    const int32_t result = switch_a3_ack_resource(wlc, switch_a3_ack_state.enabled);
    if (result)
        return result;
    if (!switch_a3_ack_state.key)
        return 0;
    if (!*((const uint8_t *)wlc + 25))
        return -11;
    const int32_t address_index = switch_a3_ack_state.key_index - 4;
    switch_a3_amt_clear(wlc, address_index);
    if (!switch_a3_ack_state.enabled)
        return 0;
    return switch_a3_receiver_set(wlc, address_index, switch_a3_ack_state.address);
}

int32_t switch_a3_ack_configure(void *wlc, const uint8_t address[6], int enabled)
{
    if (!wlc)
        return -2;
    if (enabled) {
        if (!address || (address[0] & 1u))
            return -2;
        uint8_t nonzero = 0;
        for (size_t index = 0; index < 6; ++index)
            nonzero |= address[index];
        if (!nonzero)
            return -2;
    }
    if (wlc != switch_a3_ack_state.wlc) {
        switch_a3_ack_detached();
        switch_a3_ack_state.wlc = wlc;
    }
    switch_a3_ack_state.enabled = 0;
    if (enabled) {
        for (size_t index = 0; index < 6; ++index)
            switch_a3_ack_state.address[index] = address[index];
        switch_a3_ack_state.enabled = 1;
    }
    return switch_a3_ack_reconcile(wlc);
}
