// resources.h — Global artifact (Solar Core / Lunar Blade / Eclipse Relic)
// resource table with simple deadlock detection (cycle detection in the
// waits-for graph). Header-only so arbiter.cpp can use it without fighting
// the per-folder Makefile.

#pragma once

#include "shared.h"
#include "shm_util.h"

#include <string.h>

inline int artifact_index(GameState *gs, int weapon_id) {
    for (int i = 0; i < NUM_ARTIFACTS; ++i)
        if (gs->artifacts[i].weapon_id == weapon_id) return i;
    return -1;
}

// Try to acquire `weapon_id` for `global_id`. Returns true on success.
// On failure, `global_id` is registered as a waiter (used by the cycle
// detector below).
inline bool res_try_acquire(GameState *gs, int global_id, int weapon_id) {
    pthread_mutex_lock(&gs->resource_mutex);
    int ai = artifact_index(gs, weapon_id);
    if (ai < 0 || !gs->artifacts[ai].present) {
        pthread_mutex_unlock(&gs->resource_mutex);
        return false;
    }
    ArtifactSlot &a = gs->artifacts[ai];
    if (a.held_by_global == -1) {
        a.held_by_global = global_id;
        // clear any waiter entry for this entity on this artifact
        int n = 0;
        for (int i = 0; i < a.waiter_count; ++i)
            if (a.waiters[i] != global_id) a.waiters[n++] = a.waiters[i];
        a.waiter_count = n;
        pthread_mutex_unlock(&gs->resource_mutex);
        return true;
    }
    // Record waiter (once).
    bool dup = false;
    for (int i = 0; i < a.waiter_count; ++i)
        if (a.waiters[i] == global_id) { dup = true; break; }
    if (!dup && a.waiter_count < MAX_PLAYERS + MAX_ENEMIES) {
        a.waiters[a.waiter_count++] = global_id;
    }
    pthread_mutex_unlock(&gs->resource_mutex);
    return false;
}

inline void res_release(GameState *gs, int global_id, int weapon_id) {
    pthread_mutex_lock(&gs->resource_mutex);
    int ai = artifact_index(gs, weapon_id);
    if (ai >= 0 && gs->artifacts[ai].held_by_global == global_id) {
        gs->artifacts[ai].held_by_global = -1;
    }
    pthread_mutex_unlock(&gs->resource_mutex);
}

inline void res_release_all(GameState *gs, int global_id) {
    pthread_mutex_lock(&gs->resource_mutex);
    for (int i = 0; i < NUM_ARTIFACTS; ++i) {
        if (gs->artifacts[i].held_by_global == global_id)
            gs->artifacts[i].held_by_global = -1;
        // drop from waiter lists
        int n = 0;
        for (int j = 0; j < gs->artifacts[i].waiter_count; ++j)
            if (gs->artifacts[i].waiters[j] != global_id)
                gs->artifacts[i].waiters[n++] = gs->artifacts[i].waiters[j];
        gs->artifacts[i].waiter_count = n;
    }
    pthread_mutex_unlock(&gs->resource_mutex);
}

// Detect a circular wait in the artifact waits-for graph.
// Each entity either holds some artifacts and waits for another, or waits for
// nothing. We DFS from each waiting entity through the holder of the
// artifact(s) it's waiting for. If we return to the starting entity -> cycle.
//
// On cycle detected, the arbiter forces `victim` (the first entity found in
// the cycle) to release every artifact it holds.
//
// Returns the victim global id if a deadlock was broken, -1 otherwise.
inline int res_break_deadlock(GameState *gs) {
    pthread_mutex_lock(&gs->resource_mutex);

    const int N = MAX_PLAYERS + MAX_ENEMIES;
    // For each entity, the "waits-for" set is the set of HOLDERS of artifacts
    // this entity is currently waiting on.
    // Build adjacency (waiter -> holder).
    int holder_of[NUM_ARTIFACTS];
    for (int i = 0; i < NUM_ARTIFACTS; ++i)
        holder_of[i] = gs->artifacts[i].held_by_global;

    // DFS from each node that is waiting on something.
    int victim = -1;
    for (int start = 0; start < N && victim < 0; ++start) {
        int visited[N] = {0};
        int stack[N];
        int top = 0;
        stack[top++] = start;
        visited[start] = 1;
        while (top > 0 && victim < 0) {
            int u = stack[--top];
            // Iterate waiter lists to find which artifact(s) u is waiting on.
            for (int ai = 0; ai < NUM_ARTIFACTS; ++ai) {
                ArtifactSlot &a = gs->artifacts[ai];
                bool u_waits = false;
                for (int k = 0; k < a.waiter_count; ++k)
                    if (a.waiters[k] == u) { u_waits = true; break; }
                if (!u_waits) continue;
                int h = holder_of[ai];
                if (h < 0) continue;
                if (h == start) { victim = start; break; }
                if (!visited[h]) {
                    visited[h] = 1;
                    stack[top++] = h;
                }
            }
        }
    }

    if (victim >= 0) {
        // Force the victim to release every artifact it holds.
        for (int ai = 0; ai < NUM_ARTIFACTS; ++ai)
            if (gs->artifacts[ai].held_by_global == victim)
                gs->artifacts[ai].held_by_global = -1;
    }
    pthread_mutex_unlock(&gs->resource_mutex);
    return victim;
}
