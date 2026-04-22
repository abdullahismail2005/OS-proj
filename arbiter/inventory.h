// inventory.h — First-fit contiguous inventory allocator with long-term
// storage fallback. Header-only so the allocator can be reused by arbiter
// helper TUs without fighting the per-folder Makefile.

#pragma once

#include "shared.h"

inline bool inv_slot_free(const Entity *e, int i) {
    return e->inv_slot[i] == W_NONE;
}

// Find the first window of `len` contiguous free slots. Returns -1 if none.
inline int inv_find_first_fit(const Entity *e, int len) {
    if (len <= 0 || len > INVENTORY_SIZE) return -1;
    int run = 0;
    for (int i = 0; i < INVENTORY_SIZE; ++i) {
        if (inv_slot_free(e, i)) {
            if (++run >= len) return i - len + 1;
        } else {
            run = 0;
        }
    }
    return -1;
}

// Place a weapon at [start, start+len). Returns the newly assigned instance id.
inline int inv_place(Entity *e, int weapon_id, int start) {
    int len = weapon_spec(weapon_id).slot_size;
    int inst = ++e->next_inst_id;
    for (int i = 0; i < len; ++i) {
        e->inv_slot[start + i] = weapon_id;
        e->inv_inst[start + i] = inst;
        e->inv_head[start + i] = (i == 0) ? 1 : 0;
    }
    return inst;
}

// Clear a weapon instance from inventory. Returns its weapon id.
inline int inv_clear_instance(Entity *e, int instance_id) {
    int wid = W_NONE;
    for (int i = 0; i < INVENTORY_SIZE; ++i) {
        if (e->inv_inst[i] == instance_id && e->inv_slot[i] != W_NONE) {
            wid = e->inv_slot[i];
            e->inv_slot[i] = W_NONE;
            e->inv_inst[i] = 0;
            e->inv_head[i] = 0;
        }
    }
    return wid;
}

// Move an instance into long-term storage.
inline void inv_evict_to_lts(Entity *e, int instance_id) {
    int wid = inv_clear_instance(e, instance_id);
    if (wid != W_NONE && e->lts_count < LTS_SIZE) {
        e->lts[e->lts_count++] = wid;
    }
}

// Try to make room for `needed` contiguous slots by evicting as few
// instances as possible. Greedy: repeatedly pick the smallest instance whose
// removal makes the largest contiguous gap grow, until `needed` fits. Returns
// true on success with the first-fit start slot stored in *out_start.
inline bool inv_make_room(Entity *e, int needed, int *out_start) {
    int s = inv_find_first_fit(e, needed);
    if (s >= 0) { *out_start = s; return true; }

    // Collect unique instance ids in inventory order, with their slot sizes.
    int guard = 0;
    while (guard++ < INVENTORY_SIZE) {
        // Find the smallest-slot-size instance to evict.
        int best_inst = -1;
        int best_size = 1 << 30;
        int seen[INVENTORY_SIZE] = {0};
        for (int i = 0; i < INVENTORY_SIZE; ++i) {
            int inst = e->inv_inst[i];
            if (inst == 0) continue;
            bool already = false;
            for (int j = 0; j < i; ++j) if (seen[j] == inst) { already = true; break; }
            if (already) continue;
            seen[i] = inst;
            int sz = weapon_spec(e->inv_slot[i]).slot_size;
            if (sz < best_size) { best_size = sz; best_inst = inst; }
        }
        if (best_inst < 0) break;
        inv_evict_to_lts(e, best_inst);
        s = inv_find_first_fit(e, needed);
        if (s >= 0) { *out_start = s; return true; }
    }
    return false;
}

// Public entry points used by the arbiter on pickup / swap-in.
inline bool inv_add_weapon(Entity *e, int weapon_id, int *out_instance) {
    int need = weapon_spec(weapon_id).slot_size;
    int s = -1;
    if (!inv_make_room(e, need, &s)) return false;
    int inst = inv_place(e, weapon_id, s);
    if (out_instance) *out_instance = inst;
    return true;
}

inline bool inv_swap_in_from_lts(Entity *e, int lts_index, int *out_instance) {
    if (lts_index < 0 || lts_index >= e->lts_count) return false;
    int wid = e->lts[lts_index];
    // remove from lts
    for (int i = lts_index; i < e->lts_count - 1; ++i) e->lts[i] = e->lts[i + 1];
    e->lts_count--;
    int inst;
    if (!inv_add_weapon(e, wid, &inst)) {
        // re-insert into lts on failure
        e->lts[e->lts_count++] = wid;
        return false;
    }
    if (out_instance) *out_instance = inst;
    return true;
}

// Enumerate distinct weapon instances currently in inventory.
// Fills out_ids[] and out_weapon[] up to `max` and returns the count.
inline int inv_enumerate(const Entity *e, int *out_inst, int *out_weapon,
                         int max) {
    int n = 0;
    for (int i = 0; i < INVENTORY_SIZE && n < max; ++i) {
        if (e->inv_head[i]) {
            out_inst[n]   = e->inv_inst[i];
            out_weapon[n] = e->inv_slot[i];
            n++;
        }
    }
    return n;
}
