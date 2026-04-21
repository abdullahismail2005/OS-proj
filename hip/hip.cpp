// hip.cpp — Human Interfacing Process.
//
// Responsibilities:
//   * Attach to the shared-memory segment created by the arbiter.
//   * Spawn one pthread per selected player character. Each waits on its
//     per-player turn semaphore in shared memory. Only the thread whose
//     entity is currently active reads input — others stay idle on the
//     semaphore, satisfying the "only the active player's thread processes
//     input" requirement.
//   * Spawn a dedicated rendering thread that uses ncurses to draw the game
//     state asynchronously (so rendering can never stall the scheduler).
//   * When the user chooses Quit, send SIGTERM to the arbiter (the spec's
//     mandatory signal-based quit path).
//   * Receive SIGUSR1 from the arbiter (stun). Dispatches to the target
//     player thread via pthread_kill(SIGUSR2); that thread's handler sleeps
//     3s, actually halting its execution — asynchronous and flag-free.

#include "../arbiter/shared.h"
#include "../arbiter/shm_util.h"
#include "../arbiter/inventory.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <pthread.h>
#include <ncurses.h>

namespace {

GameState        *G_gs = nullptr;
std::atomic<bool> G_shutdown{false};

pthread_t G_player_threads[MAX_PLAYERS];
int       G_player_ready[MAX_PLAYERS] = {0};

// Coarse mutex guarding ncurses calls (ncurses itself is not thread-safe).
pthread_mutex_t G_curses_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------- Signal handlers ----------
void on_sigusr2(int) {
    struct timespec ts { STUN_DURATION_SEC, 0 };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

void on_sigterm(int) { G_shutdown.store(true); }

void on_sigusr1(int, siginfo_t *info, void *) {
    int idx = info->si_value.sival_int;
    if (idx < 0 || idx >= MAX_PLAYERS) return;
    if (!G_player_ready[idx]) return;
    pthread_kill(G_player_threads[idx], SIGUSR2);
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
    sa2.sa_flags = 0;
    sigaction(SIGUSR2, &sa2, nullptr);

    struct sigaction sat {};
    sat.sa_handler = on_sigterm;
    sigemptyset(&sat.sa_mask);
    sat.sa_flags = 0;
    sigaction(SIGTERM, &sat, nullptr);

    signal(SIGPIPE, SIG_IGN);
}

// ---------- Rendering ----------
void draw_bar(int y, int x, int w, int cur, int max, const char *label) {
    mvprintw(y, x, "%-12s", label);
    int filled = (max > 0) ? (cur * w / max) : 0;
    if (filled < 0) filled = 0;
    if (filled > w) filled = w;
    mvaddch(y, x + 13, '[');
    for (int i = 0; i < w; ++i) mvaddch(y, x + 14 + i, i < filled ? '#' : '-');
    mvaddch(y, x + 14 + w, ']');
    mvprintw(y, x + 14 + w + 2, "%5d/%-5d", cur, max);
}

void draw_state(GameState *gs) {
    pthread_mutex_lock(&G_curses_lock);
    erase();

    mvprintw(0, 2, "CHRONO RIFT   seed=%d  players=%d  enemies=%d  killed=%d/%d",
             gs->seed, gs->num_players, gs->num_enemies,
             gs->enemies_killed, KILL_GOAL);

    int y = 2;
    mvprintw(y++, 2, "-- PLAYERS --");
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        Entity &p = gs->players[i];
        if (!p.active) continue;
        char name[32];
        snprintf(name, sizeof(name), "%s%s%s",
                 p.name,
                 p.stunned ? " STUN" : "",
                 (gs->active_global == entity_global_id(1, i)) ? " *" : "");
        if (!p.alive) snprintf(name, sizeof(name), "%s DEAD", p.name);
        mvprintw(y, 2, "%-12s", name);
        draw_bar(y,   16, 20, p.hp,      p.max_hp,       "HP");
        draw_bar(y+1, 16, 20, p.stamina, p.max_stamina,  "STAMINA");
        y += 3;
    }

    y++;
    mvprintw(y++, 2, "-- ENEMIES --");
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Entity &e = gs->enemies[i];
        if (!e.active) continue;
        char name[32];
        snprintf(name, sizeof(name), "%s%s%s",
                 e.name,
                 e.stunned ? " STUN" : "",
                 (gs->active_global == entity_global_id(0, i)) ? " *" : "");
        if (!e.alive) snprintf(name, sizeof(name), "%s DEAD", e.name);
        mvprintw(y, 2, "%-12s", name);
        draw_bar(y,   16, 20, e.hp,      e.max_hp,      "HP");
        draw_bar(y+1, 16, 20, e.stamina, e.max_stamina, "STAMINA");
        y += 3;
    }

    // Artifact table
    y++;
    mvprintw(y++, 2, "-- ARTIFACTS --");
    for (int i = 0; i < NUM_ARTIFACTS; ++i) {
        ArtifactSlot &a = gs->artifacts[i];
        const char *name = weapon_spec(a.weapon_id).name;
        if (!a.present) {
            mvprintw(y++, 4, "%-16s : hidden", name);
        } else if (a.held_by_global < 0) {
            mvprintw(y++, 4, "%-16s : FREE  (waiters=%d)", name, a.waiter_count);
        } else {
            Entity *h = entity_by_global(gs, a.held_by_global);
            mvprintw(y++, 4, "%-16s : held by %s (waiters=%d)",
                     name, h ? h->name : "?", a.waiter_count);
        }
    }

    // Action log (right column)
    int lh, lw;
    getmaxyx(stdscr, lh, lw);
    int col = lw - 62;
    if (col < 40) col = 40;
    mvprintw(1, col, "-- LOG --");
    pthread_mutex_lock(&gs->log_mutex);
    int start = gs->log_count > (lh - 4) ? gs->log_count - (lh - 4) : 0;
    for (int i = start, row = 2; i < gs->log_count && row < lh - 1; ++i, ++row) {
        int idx = (gs->log_head + i) % LOG_CAPACITY;
        mvprintw(row, col, "%.*s", lw - col - 1, gs->log[idx].msg);
    }
    pthread_mutex_unlock(&gs->log_mutex);

    refresh();
    pthread_mutex_unlock(&G_curses_lock);
}

void *render_thread(void *arg) {
    GameState *gs = static_cast<GameState *>(arg);
    while (!G_shutdown.load() && gs->phase != PHASE_WIN
           && gs->phase != PHASE_LOSE && gs->phase != PHASE_QUIT) {
        draw_state(gs);
        struct timespec ts { 0, 100'000'000 };  // 10 fps
        nanosleep(&ts, nullptr);
    }
    draw_state(gs);
    return nullptr;
}

// ---------- Player input ----------

// Prompt user for a bounded integer; uses the curses-lock so we don't
// collide with the renderer.
int prompt_int(const char *label, int lo, int hi) {
    int val = lo;
    pthread_mutex_lock(&G_curses_lock);
    int h, w; getmaxyx(stdscr, h, w); (void)w;
    echo();
    curs_set(1);
    mvprintw(h - 2, 2, "                                                       ");
    mvprintw(h - 2, 2, "%s", label);
    clrtoeol();
    refresh();
    char buf[16] = {0};
    getnstr(buf, sizeof(buf) - 1);
    noecho();
    curs_set(0);
    pthread_mutex_unlock(&G_curses_lock);
    val = std::atoi(buf);
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    return val;
}

int prompt_choice(const char *label, const char *allowed) {
    pthread_mutex_lock(&G_curses_lock);
    int h, w; getmaxyx(stdscr, h, w); (void)w;
    mvprintw(h - 2, 2, "                                                       ");
    mvprintw(h - 2, 2, "%s [%s]: ", label, allowed);
    clrtoeol();
    refresh();
    nodelay(stdscr, FALSE);
    int c = getch();
    pthread_mutex_unlock(&G_curses_lock);
    return c;
}

void player_turn(GameState *gs, Entity *p) {
    // Handle pending drop pickup prompt first if offered to this player.
    pthread_mutex_lock(&gs->state_mutex);
    bool pending_drop = (gs->pending_drop_weapon != W_NONE &&
                         gs->pending_drop_for_player == p->local_id);
    int drop_w = gs->pending_drop_weapon;
    pthread_mutex_unlock(&gs->state_mutex);
    if (pending_drop) {
        char lbl[96];
        snprintf(lbl, sizeof(lbl), "P%d — pick up %s? (y/n)",
                 p->local_id + 1, weapon_spec(drop_w).name);
        int c = prompt_choice(lbl, "yn");
        p->pending_action = (c == 'y' || c == 'Y') ? ACT_PICKUP : ACT_DECLINE;
        return;
    }

    char lbl[96];
    snprintf(lbl, sizeof(lbl), "P%d turn: (s)trike (x)exhaust (w)pn (i)swap (h)eal (u)lt (k)skip (q)uit",
             p->local_id + 1);
    int c = prompt_choice(lbl, "sxwihukq");
    switch (c) {
    case 's': {
        p->pending_action = ACT_STRIKE;
        // pick a target enemy
        int alive[MAX_ENEMIES]; int n = 0;
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (gs->enemies[i].active && gs->enemies[i].alive) alive[n++] = i;
        if (n == 0) { p->pending_action = ACT_SKIP; return; }
        int choice = prompt_int("Target enemy index (1..9): ", 1, MAX_ENEMIES);
        int idx = choice - 1;
        if (!gs->enemies[idx].active || !gs->enemies[idx].alive) idx = alive[0];
        p->action_target = idx;
        break;
    }
    case 'x':
        p->pending_action = ACT_EXHAUST;
        p->action_target = prompt_int("Target enemy (1..9): ", 1, MAX_ENEMIES) - 1;
        break;
    case 'w': {
        // Use weapon: list instances
        int insts[INVENTORY_SIZE], wids[INVENTORY_SIZE];
        int n = inv_enumerate(p, insts, wids, INVENTORY_SIZE);
        if (n == 0) { p->pending_action = ACT_SKIP; return; }
        pthread_mutex_lock(&G_curses_lock);
        int h, w; getmaxyx(stdscr, h, w); (void)w;
        for (int i = 0; i < n; ++i)
            mvprintw(h - 4 - (n - i), 2, "  %d) %s (dmg %d)",
                     i + 1, weapon_spec(wids[i]).name,
                     weapon_spec(wids[i]).damage);
        refresh();
        pthread_mutex_unlock(&G_curses_lock);
        int pick = prompt_int("Pick weapon #: ", 1, n) - 1;
        p->pending_action = ACT_USE_WEAPON;
        p->action_param   = insts[pick];
        p->action_target  = prompt_int("Target enemy (1..9): ", 1, MAX_ENEMIES) - 1;
        break;
    }
    case 'i': {
        if (p->lts_count == 0) { p->pending_action = ACT_SKIP; return; }
        p->pending_action = ACT_SWAP_IN;
        p->action_param = prompt_int("LTS index (1..): ", 1, p->lts_count) - 1;
        break;
    }
    case 'h':
        p->pending_action = ACT_HEAL;
        break;
    case 'u':
        p->pending_action = ACT_ULTIMATE;
        break;
    case 'q':
        // Quit condition: send SIGTERM to arbiter and submit a skip.
        if (gs->arbiter_pid > 0) kill(gs->arbiter_pid, SIGTERM);
        p->pending_action = ACT_SKIP;
        G_shutdown.store(true);
        break;
    case 'k':
    default:
        p->pending_action = ACT_SKIP;
        break;
    }
}

void *player_thread(void *arg) {
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Entity *p = &G_gs->players[idx];
    p->host_pid = getpid();

    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    pthread_sigmask(SIG_UNBLOCK, &s, nullptr);

    G_player_threads[idx] = pthread_self();
    G_player_ready[idx]   = 1;

    while (!G_shutdown.load() && G_gs->phase == PHASE_RUNNING) {
        while (sem_wait(&p->turn_sem) != 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (G_shutdown.load() || G_gs->phase != PHASE_RUNNING) break;
        if (!p->alive) break;

        player_turn(G_gs, p);
        sem_post(&p->action_ready);
    }
    G_player_ready[idx] = 0;
    return nullptr;
}

} // namespace

int main() {
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &block, nullptr);

    install_signals();

    GameState *gs = shm_open_gamestate(false);
    if (!gs) { fprintf(stderr, "[hip] shared memory not available\n"); return 1; }
    G_gs = gs;
    gs->hip_pid = getpid();

    // Wait for arbiter to move to RUNNING.
    while (gs->phase == PHASE_SETUP && !G_shutdown.load()) {
        struct timespec t { 0, 100'000'000 };
        nanosleep(&t, nullptr);
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    pthread_t r_tid;
    pthread_create(&r_tid, nullptr, render_thread, gs);

    pthread_t p_tids[MAX_PLAYERS];
    int spawned = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (!gs->players[i].active) continue;
        gs->players[i].host_pid = getpid();
        pthread_create(&p_tids[spawned++], nullptr, player_thread,
                       reinterpret_cast<void *>((intptr_t)i));
    }
    shm_log(gs, "[hip] %d player threads running", spawned);

    while (!G_shutdown.load() && gs->phase == PHASE_RUNNING) {
        struct timespec t { 0, 250'000'000 };
        nanosleep(&t, nullptr);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i) sem_post(&gs->players[i].turn_sem);
    for (int i = 0; i < spawned; ++i) pthread_join(p_tids[i], nullptr);
    pthread_join(r_tid, nullptr);

    pthread_mutex_lock(&G_curses_lock);
    endwin();
    pthread_mutex_unlock(&G_curses_lock);

    const char *phase_txt =
        gs->phase == PHASE_WIN  ? "VICTORY"   :
        gs->phase == PHASE_LOSE ? "DEFEAT"    :
        gs->phase == PHASE_QUIT ? "QUIT"      : "ENDED";
    fprintf(stderr, "[hip] game phase: %s\n", phase_txt);
    return 0;
}
