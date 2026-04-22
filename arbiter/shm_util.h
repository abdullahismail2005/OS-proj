// shm_util.h — Tiny shared-memory helpers.
//
// Header-only (all functions `inline`) because the project's per-folder
// Makefile doesn't let us link a shared .cpp from outside the folder being
// built. Keeping these inline means arbiter/hip/asp all pick up the same
// implementation without duplicating code.

#pragma once

#include "shared.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstdarg>

// Open or create the shared-memory segment and mmap a GameState into it.
// `create` == true creates/truncates (arbiter only); false attaches read/write.
inline GameState *shm_open_gamestate(bool create) {
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(SHM_NAME, flags, 0666);
    if (fd < 0) {
        fprintf(stderr, "shm_open(%s) failed: %s\n", SHM_NAME, strerror(errno));
        return nullptr;
    }
    if (create) {
        if (ftruncate(fd, sizeof(GameState)) < 0) {
            fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
            close(fd);
            return nullptr;
        }
    }
    void *p = mmap(nullptr, sizeof(GameState), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return nullptr;
    }
    return static_cast<GameState *>(p);
}

inline void shm_destroy() { shm_unlink(SHM_NAME); }

// Append a line to the shared log ring-buffer. Caller should NOT hold
// state_mutex; we take log_mutex independently.
inline void shm_log(GameState *gs, const char *fmt, ...) {
    if (!gs) return;
    char buf[LOG_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&gs->log_mutex);
    int idx = (gs->log_head + gs->log_count) % LOG_CAPACITY;
    if (gs->log_count == LOG_CAPACITY) {
        idx = gs->log_head;
        gs->log_head = (gs->log_head + 1) % LOG_CAPACITY;
    } else {
        gs->log_count++;
    }
    strncpy(gs->log[idx].msg, buf, LOG_MSG_LEN - 1);
    gs->log[idx].msg[LOG_MSG_LEN - 1] = '\0';
    pthread_mutex_unlock(&gs->log_mutex);
}

// Helper to get an Entity* from a global id. Not thread-safe on its own —
// callers should hold state_mutex while dereferencing mutable fields.
inline Entity *entity_by_global(GameState *gs, int global_id) {
    if (global_id < 0) return nullptr;
    if (global_is_player(global_id)) {
        int i = global_local_id(global_id);
        if (i < 0 || i >= MAX_PLAYERS) return nullptr;
        return &gs->players[i];
    }
    int i = global_local_id(global_id);
    if (i < 0 || i >= MAX_ENEMIES) return nullptr;
    return &gs->enemies[i];
}
