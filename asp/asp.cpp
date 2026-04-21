// asp.cpp — Automated Strategic Process.
//
// Spawns one pthread per active enemy (NPC). Each thread:
//   * Waits on its own per-entity turn semaphore in shared memory.
//   * When posted by the arbiter, runs a small heuristic to decide an action.
//   * Writes the action back into shared memory and posts action_ready.
//
// Stun handling: the arbiter delivers SIGUSR1 to this process with
// sival_int = target's local enemy id. The process-level SIGUSR1 handler
// dispatches to that thread via pthread_kill(..., SIGUSR2). SIGUSR2's
// handler does a nanosleep for STUN_DURATION_SEC, actually halting that
// thread's execution — no polling, no flags.
//
// Ultimate-ability suspension: driven entirely from the arbiter via
// SIGSTOP / SIGCONT on this whole process. No coordination needed here.

#include "../arbiter/shared.h"
#include "../arbiter/shm_util.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <unistd.h>
#include <pthread.h>

namespace {

GameState       *G_gs = nullptr;
std::atomic<bool> G_shutdown{false};

// Per-enemy pthread table so SIGUSR1 can dispatch to the right thread.
pthread_t G_npc_threads[MAX_ENEMIES];
int       G_npc_ready[MAX_ENEMIES] = {0};

// Per-thread RNG (seeded from shared seed XOR thread index).
thread_local std::mt19937 T_rng;

int rand_range(int lo, int hi) {
    std::uniform_int_distribution<int> d(lo, hi);
    return d(T_rng);
}

// SIGUSR2 handler: halt this thread for STUN_DURATION_SEC.
void on_sigusr2(int) {
    struct timespec ts { STUN_DURATION_SEC, 0 };
    // EINTR-safe nanosleep
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void on_sigterm(int) { G_shutdown.store(true); }

// SIGUSR1 dispatcher: arbiter sends target enemy local_id via sival_int.
void on_sigusr1(int sig, siginfo_t *info, void *) {
    (void)sig;
    int idx = info->si_value.sival_int;
    if (idx < 0 || idx >= MAX_ENEMIES) return;
    if (!G_npc_ready[idx]) return;
    pthread_kill(G_npc_threads[idx], SIGUSR2);
}

void install_signals() {
    struct sigaction sa1 {};
    sa1.sa_sigaction = on_sigusr1;
    sigemptyset(&sa1.sa_mask);
    sa1.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &sa1, nullptr);

    struct sigaction sa2 {};
    sa2.sa_handler = on_sigusr2;
    sigemptyset(&sa2.sa_mask);
    sa2.sa_flags = 0;   // allow nanosleep to be interrupted/returned
    sigaction(SIGUSR2, &sa2, nullptr);

    struct sigaction sat {};
    sat.sa_handler = on_sigterm;
    sigemptyset(&sat.sa_mask);
    sat.sa_flags = 0;
    sigaction(SIGTERM, &sat, nullptr);

    signal(SIGPIPE, SIG_IGN);
}

// Simple heuristic NPC decision.
void npc_decide(GameState *gs, Entity *e) {
    pthread_mutex_lock(&gs->state_mutex);
    // Pick a random alive player as target.
    int alive[MAX_PLAYERS]; int n = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (gs->players[i].active && gs->players[i].alive) alive[n++] = i;
    if (n == 0) {
        e->pending_action = ACT_SKIP;
        pthread_mutex_unlock(&gs->state_mutex);
        return;
    }
    int target = alive[rand_range(0, n - 1)];
    // 85% strike, 15% skip
    if (rand_range(0, 99) < 85) {
        e->pending_action = ACT_STRIKE;
        e->action_target  = target;
    } else {
        e->pending_action = ACT_SKIP;
    }
    pthread_mutex_unlock(&gs->state_mutex);
}

void *npc_thread(void *arg) {
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    T_rng.seed(G_gs->seed ^ (idx * 2654435761u) ^ getpid());
    Entity *e = &G_gs->enemies[idx];
    e->host_pid = getpid();

    // Unblock SIGUSR2 so this thread can be stunned.
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    pthread_sigmask(SIG_UNBLOCK, &s, nullptr);

    G_npc_threads[idx] = pthread_self();
    G_npc_ready[idx]   = 1;

    while (!G_shutdown.load()) {
        // Wait for our turn.
        while (sem_wait(&e->turn_sem) != 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (G_shutdown.load() || G_gs->phase != PHASE_RUNNING) break;
        if (!e->alive) break;

        npc_decide(G_gs, e);
        // Small thinking delay to feel realistic (and exercise timeout path
        // occasionally on tiny machines).
        struct timespec pause { 0, 200'000'000 };
        nanosleep(&pause, nullptr);
        sem_post(&e->action_ready);
    }
    G_npc_ready[idx] = 0;
    return nullptr;
}

} // namespace

int main() {
    // Block SIGUSR1 and SIGUSR2 on the main thread so only the dispatch
    // thread / target threads receive them correctly.
    sigset_t block_all;
    sigemptyset(&block_all);
    sigaddset(&block_all, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &block_all, nullptr);

    install_signals();

    GameState *gs = shm_open_gamestate(/*create=*/false);
    if (!gs) {
        fprintf(stderr, "[asp] shared memory not available\n");
        return 1;
    }
    G_gs = gs;
    gs->asp_pid = getpid();

    // Wait for arbiter to finish entity init.
    while (gs->phase == PHASE_SETUP && !G_shutdown.load()) {
        struct timespec t { 0, 100'000'000 };
        nanosleep(&t, nullptr);
    }

    // Record host pid on each enemy; spawn one thread per active enemy.
    pthread_t tids[MAX_ENEMIES];
    int spawned = 0;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Entity &e = gs->enemies[i];
        if (!e.active) continue;
        e.host_pid = getpid();
        pthread_create(&tids[spawned++], nullptr, npc_thread,
                       reinterpret_cast<void *>((intptr_t)i));
    }

    shm_log(gs, "[asp] %d NPC threads running", spawned);

    // Wait for game end.
    while (!G_shutdown.load() && gs->phase == PHASE_RUNNING) {
        struct timespec t { 0, 250'000'000 };
        nanosleep(&t, nullptr);
    }

    // Wake any threads stuck on turn_sem and join.
    for (int i = 0; i < MAX_ENEMIES; ++i) sem_post(&gs->enemies[i].turn_sem);
    for (int i = 0; i < spawned; ++i) pthread_join(tids[i], nullptr);

    return 0;
}
