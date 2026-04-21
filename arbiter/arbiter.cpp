// arbiter.cpp — Chrono Rift central authority.
//
// Responsibilities:
//   * Create the POSIX shared-memory segment and initialise all in-memory
//     synchronisation primitives (pshared mutexes and unnamed semaphores).
//   * Launch the hip and asp processes.
//   * Run the stamina scheduler (1 Hz).
//   * Enforce serial action execution (exactly one entity acts at a time).
//   * Dispatch stun signals (SIGUSR1 with sigqueue payload) asynchronously.
//   * Orchestrate the 10-second Ultimate Ability pause using SIGSTOP/SIGCONT
//     on the ASP process, driven by SIGALRM in the arbiter.
//   * Receive SIGTERM from hip (Quit condition).
//   * Run a background deadlock-detection thread that scans the artifact
//     waits-for graph and breaks cycles.

#include "shared.h"
#include "shm_util.h"
#include "inventory.h"
#include "resources.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <random>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace {

GameState *G_gs = nullptr;
std::atomic<bool> G_shutdown{false};
FILE *G_trace = nullptr;   // turnaround trace file

long long now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

// ---------- RNG ----------
std::mt19937 G_rng;

int rand_range(int lo, int hi) {
    std::uniform_int_distribution<int> d(lo, hi);
    return d(G_rng);
}

// ---------- Signal handlers ----------
void on_sigterm(int) {
    // Quit condition: hip sends SIGTERM when the user selects Quit.
    if (G_gs) {
        pthread_mutex_lock(&G_gs->state_mutex);
        if (G_gs->phase == PHASE_RUNNING) G_gs->phase = PHASE_QUIT;
        pthread_mutex_unlock(&G_gs->state_mutex);
    }
    G_shutdown.store(true);
}

void on_sigalrm(int) {
    // Fires 10 s after arbiter sent SIGSTOP to asp. Resume ASP.
    if (!G_gs) return;
    if (G_gs->asp_pid > 0) kill(G_gs->asp_pid, SIGCONT);
    pthread_mutex_lock(&G_gs->state_mutex);
    G_gs->ultimate_active = 0;
    pthread_mutex_unlock(&G_gs->state_mutex);
    shm_log(G_gs, "[ULT] Ultimate pause ended — ASP resumed");
}

void install_signals() {
    struct sigaction sa_term {};
    sa_term.sa_handler = on_sigterm;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);

    struct sigaction sa_alrm {};
    sa_alrm.sa_handler = on_sigalrm;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa_alrm, nullptr);

    // Ignore SIGPIPE — we don't use pipes but child exit churn can still fire
    // it via internal plumbing.
    signal(SIGPIPE, SIG_IGN);
    // Reap zombies
    signal(SIGCHLD, SIG_IGN);
}

// ---------- Shared-state init ----------
void init_pshared_mutex(pthread_mutex_t *m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

void init_pshared_sem(sem_t *s, unsigned int val) {
    sem_init(s, /*pshared*/ 1, val);
}

void init_entity_defaults(Entity *e) {
    memset(e, 0, sizeof(*e));
    init_pshared_sem(&e->turn_sem, 0);
    init_pshared_sem(&e->action_ready, 0);
}

void init_game_state(GameState *gs, int seed, int num_players) {
    memset(gs, 0, sizeof(*gs));
    init_pshared_mutex(&gs->state_mutex);
    init_pshared_mutex(&gs->resource_mutex);
    init_pshared_mutex(&gs->log_mutex);

    gs->seed = seed;
    gs->phase = PHASE_SETUP;
    gs->num_players = num_players;
    gs->active_global = -1;
    gs->arbiter_pid = getpid();

    for (int i = 0; i < MAX_PLAYERS; ++i) init_entity_defaults(&gs->players[i]);
    for (int i = 0; i < MAX_ENEMIES; ++i) init_entity_defaults(&gs->enemies[i]);

    // Derived stats per roll-number seed. The seed here is an int — callers
    // pass the digit-sequence of the roll number (e.g. 240673). Compute the
    // last-digit / last-two / full value from that.
    int roll = seed;
    int last_digit  = roll % 10;
    int second_last = (roll / 10) % 10;
    int last_two    = roll % 100;

    int player_speed = (num_players > 0) ? (100 / num_players) : 100;

    for (int i = 0; i < num_players; ++i) {
        Entity &p = gs->players[i];
        p.active = 1;
        p.is_player = 1;
        p.local_id = i;
        p.alive = 1;
        p.max_hp = roll + rand_range(100, 1000);
        p.hp = p.max_hp;
        p.damage = last_digit + 10;
        p.speed = player_speed;
        p.max_stamina = 100;
        p.stamina = 0;
        snprintf(p.name, NAME_LEN, "P%d", i + 1);
    }

    int ne = rand_range(2, MAX_ENEMIES);
    gs->num_enemies = ne;
    for (int i = 0; i < ne; ++i) {
        Entity &e = gs->enemies[i];
        e.active = 1;
        e.is_player = 0;
        e.local_id = i;
        e.alive = 1;
        e.max_hp = last_two + rand_range(50, 200);
        e.hp = e.max_hp;
        e.damage = second_last + 10;
        e.speed = rand_range(10, 30);
        e.max_stamina = 150;
        e.stamina = 0;
        snprintf(e.name, NAME_LEN, "E%d", i + 1);
    }

    // Artifact table
    gs->artifacts[0] = {W_SOLAR_CORE,    1, -1, 0, {}};
    gs->artifacts[1] = {W_LUNAR_BLADE,   1, -1, 0, {}};
    gs->artifacts[2] = {W_ECLIPSE_RELIC, 0, -1, 0, {}};  // appears dynamically
}

// ---------- Scheduler ----------

// Chooses next active entity: first alive, non-stunned, full-stamina entity.
// Returns global id or -1.
int scheduler_pick(GameState *gs) {
    int pick = -1;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        Entity &e = gs->players[i];
        if (!e.active || !e.alive) continue;
        if (e.stunned) continue;
        if (e.stamina >= e.max_stamina) {
            pick = entity_global_id(1, i);
            break;
        }
    }
    if (pick >= 0) return pick;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Entity &e = gs->enemies[i];
        if (!e.active || !e.alive) continue;
        if (e.stunned) continue;
        if (e.stamina >= e.max_stamina) {
            pick = entity_global_id(0, i);
            break;
        }
    }
    return pick;
}

// Apply an action committed by an entity. Returns true if the action ended
// the game.
void apply_action(GameState *gs, Entity *actor);

// Forward declarations for the resource/inventory reset-on-death path.
void handle_entity_death(GameState *gs, Entity *dead);

void scheduler_tick(GameState *gs) {
    pthread_mutex_lock(&gs->state_mutex);

    // Clear expired stuns
    time_t now = time(nullptr);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        Entity &e = gs->players[i];
        if (e.stunned && now >= e.stun_until) {
            e.stunned = 0;
            shm_log(gs, "[STUN] %s recovered", e.name);
        }
    }
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        Entity &e = gs->enemies[i];
        if (e.stunned && now >= e.stun_until) {
            e.stunned = 0;
            shm_log(gs, "[STUN] %s recovered", e.name);
        }
    }

    // Accrue stamina when no one is acting
    auto accrue = [&](Entity &e) {
        if (!e.active || !e.alive || e.stunned) return;
        bool was_full = (e.stamina >= e.max_stamina);
        if (e.stamina < e.max_stamina) {
            e.stamina += e.speed;
            if (e.stamina > e.max_stamina) e.stamina = e.max_stamina;
        }
        // Record the instant this entity first became turn-ready so we can
        // measure wait-to-act time.
        if (!was_full && e.stamina >= e.max_stamina) {
            e.last_full_at_ns = now_ns();
        }
    };
    if (gs->active_global < 0) {
        for (int i = 0; i < MAX_PLAYERS; ++i) accrue(gs->players[i]);
        for (int i = 0; i < MAX_ENEMIES; ++i) accrue(gs->enemies[i]);
    }

    // Expire pending weapon drops.
    if (gs->pending_drop_weapon != W_NONE && gs->pending_drop_open &&
        now >= gs->pending_drop_deadline) {
        int wid = gs->pending_drop_weapon;
        int cands[MAX_ENEMIES]; int nc = 0;
        for (int i = 0; i < MAX_ENEMIES; ++i)
            if (gs->enemies[i].active && gs->enemies[i].alive) cands[nc++] = i;
        if (nc > 0) {
            int ei = cands[G_rng() % nc];
            int inst;
            if (inv_add_weapon(&gs->enemies[ei], wid, &inst)) {
                shm_log(gs, "[DROP] %s expired — E%d grabbed %s",
                        weapon_spec(wid).name, ei + 1, weapon_spec(wid).name);
            }
        }
        gs->pending_drop_weapon = W_NONE;
        gs->pending_drop_open = 0;
    }

    // Pick next actor if none active.
    if (gs->active_global < 0) {
        int g = scheduler_pick(gs);
        if (g >= 0) {
            gs->active_global = g;
            Entity *e = entity_by_global(gs, g);
            e->pending_action = ACT_NONE;
            long long t = now_ns();
            if (e->last_full_at_ns > 0)
                e->total_wait_ns += (t - e->last_full_at_ns);
            e->last_turn_start_ns = t;
            // Post turn semaphore to wake the owning thread.
            sem_post(&e->turn_sem);
            shm_log(gs, "[TURN] %s begins turn (stamina=%d)",
                    e->name, e->stamina);
            if (G_trace) {
                fprintf(G_trace, "%lld,turn_start,%s\n", t, e->name);
            }
        }
    }

    pthread_mutex_unlock(&gs->state_mutex);
}

// Wait for the active entity to post its action_ready semaphore, with a
// timeout (NPC turns time out at 3s -> auto-skip).
bool wait_for_action(GameState * /*gs*/, Entity *actor, int timeout_sec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_sec;
    while (true) {
        int r = sem_timedwait(&actor->action_ready, &ts);
        if (r == 0) return true;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) return false;
        return false;
    }
}

void scheduler_loop(GameState *gs) {
    while (!G_shutdown.load()) {
        scheduler_tick(gs);

        // If someone is acting, wait for their action to be committed.
        pthread_mutex_lock(&gs->state_mutex);
        int active = gs->active_global;
        Entity *actor = active >= 0 ? entity_by_global(gs, active) : nullptr;
        pthread_mutex_unlock(&gs->state_mutex);

        if (actor) {
            int to = actor->is_player ? 120 : NPC_TURN_TIMEOUT_SEC;
            bool got = wait_for_action(gs, actor, to);
            if (!got) {
                // NPC timeout -> auto-skip per spec.
                actor->pending_action = ACT_SKIP;
                shm_log(gs, "[TIMEOUT] %s did not respond — auto Skip",
                        actor->name);
            }
            apply_action(gs, actor);
            // Clear active unless the action triggered ultimate (which also
            // consumes the turn).
            pthread_mutex_lock(&gs->state_mutex);
            long long t_end = now_ns();
            if (actor->last_turn_start_ns > 0)
                actor->total_burst_ns += (t_end - actor->last_turn_start_ns);
            actor->turns_taken++;
            actor->last_full_at_ns = 0;
            actor->last_turn_start_ns = 0;
            if (G_trace) {
                fprintf(G_trace, "%lld,turn_end,%s,action=%d\n",
                        t_end, actor->name, actor->pending_action);
            }
            actor->stamina = 0;
            gs->active_global = -1;
            // Check win/lose.
            int alive_players = 0;
            for (int i = 0; i < MAX_PLAYERS; ++i)
                if (gs->players[i].active && gs->players[i].alive) alive_players++;
            if (alive_players == 0) {
                gs->phase = PHASE_LOSE;
                G_shutdown.store(true);
            }
            if (gs->enemies_killed >= KILL_GOAL) {
                gs->phase = PHASE_WIN;
                G_shutdown.store(true);
            }
            pthread_mutex_unlock(&gs->state_mutex);
        }

        // Sleep until next tick. We use 200 ms slices so we can react to
        // shutdown without a noticeable delay.
        struct timespec slice { 0, 200'000'000 };
        nanosleep(&slice, nullptr);
    }
}

// ---------- Death, drops, resource release ----------
void handle_entity_death(GameState *gs, Entity *dead) {
    shm_log(gs, "[DEATH] %s has fallen", dead->name);
    dead->alive = 0;
    dead->stamina = 0;
    // Release artifacts held by the dead entity.
    res_release_all(gs, entity_global_id(dead->is_player, dead->local_id));

    if (!dead->is_player) {
        gs->enemies_killed++;
        // Spec §6: "If an NPC holds a weapon, the weapon will not be
        // dropped when it dies." So drops only happen for empty-inventory
        // enemies; otherwise the carried weapons stay with the corpse.
        bool has_weapons = false;
        for (int i = 0; i < INVENTORY_SIZE; ++i) {
            if (dead->inv_slot[i] != W_NONE) { has_weapons = true; break; }
        }
        // 60% chance (unchanged) to drop a random non-artifact weapon.
        if (!has_weapons && rand_range(0, 99) < 60) {
            int pool[] = {W_IRON_HALBERD, W_VENOM_DAGGER, W_THUNDERSTAFF,
                          W_OBSIDIAN_AXE, W_FROSTBOW, W_SPLINTER_STICK};
            int wid = pool[rand_range(0, (int)(sizeof(pool)/sizeof(pool[0])) - 1)];
            // Offer to first alive player; if declined/timed out, an enemy
            // picks it up.
            int alive_players = 0;
            for (int i = 0; i < MAX_PLAYERS; ++i)
                if (gs->players[i].active && gs->players[i].alive) alive_players++;
            if (alive_players > 0) {
                gs->pending_drop_weapon = wid;
                gs->pending_drop_open = 1;
                gs->pending_drop_deadline = time(nullptr) + DROP_PICKUP_WINDOW_SEC;
                shm_log(gs, "[DROP] %s dropped %s — any player may pick it up within %ds",
                        dead->name, weapon_spec(wid).name, DROP_PICKUP_WINDOW_SEC);
            }
        }
        // Rarely reveal the Eclipse Relic on kill #3 (deterministic once).
        if (!gs->artifacts[2].present && gs->enemies_killed >= 3) {
            gs->artifacts[2].present = 1;
            shm_log(gs, "[ARTIFACT] Eclipse Relic has appeared!");
        }
    }
}

// ---------- Stun helper ----------
void deliver_stun(GameState *gs, Entity *target) {
    // Mark stun state + fire signal asynchronously at the host process.
    target->stunned = 1;
    target->stun_until = time(nullptr) + STUN_DURATION_SEC;
    union sigval sv;
    sv.sival_int = target->local_id;
    if (target->host_pid > 0) {
        // SIGUSR1 carries the local_id; host process dispatches to the
        // specific thread via pthread_kill internally.
        sigqueue(target->host_pid, SIGUSR1, sv);
    }
    shm_log(gs, "[STUN] %s was stunned for %ds",
            target->name, STUN_DURATION_SEC);
}

// ---------- Action execution ----------
void apply_action(GameState *gs, Entity *actor) {
    pthread_mutex_lock(&gs->state_mutex);
    int action = actor->pending_action;
    int tgt    = actor->action_target;
    int param  = actor->action_param;

    auto apply_damage_to_player = [&](int pid, int dmg) {
        Entity &p = gs->players[pid];
        if (!p.active || !p.alive) return;
        p.hp -= dmg;
        shm_log(gs, "[HIT] %s -> P%d for %d dmg (hp %d)",
                actor->name, pid + 1, dmg, p.hp);
        if (p.hp <= 0) handle_entity_death(gs, &p);
    };
    auto apply_damage_to_enemy = [&](int eid, int dmg) {
        Entity &e = gs->enemies[eid];
        if (!e.active || !e.alive) return;
        e.hp -= dmg;
        shm_log(gs, "[HIT] %s -> E%d for %d dmg (hp %d)",
                actor->name, eid + 1, dmg, e.hp);
        if (e.hp <= 0) handle_entity_death(gs, &e);
    };

    switch (action) {
    case ACT_STRIKE: {
        if (actor->is_player) apply_damage_to_enemy(tgt, actor->damage);
        else                  apply_damage_to_player(tgt, actor->damage);
        break;
    }
    case ACT_EXHAUST: {
        if (actor->is_player) {
            Entity &e = gs->enemies[tgt];
            e.stamina = std::max(0, e.stamina - actor->damage);
            shm_log(gs, "[EXH] %s drained %d stamina from E%d",
                    actor->name, actor->damage, tgt + 1);
        }
        break;
    }
    case ACT_USE_WEAPON: {
        // param = weapon instance id in actor's inventory.
        int wid = W_NONE;
        for (int i = 0; i < INVENTORY_SIZE; ++i)
            if (actor->inv_inst[i] == param) { wid = actor->inv_slot[i]; break; }
        if (wid == W_NONE) {
            shm_log(gs, "[USE] %s tried to use an unknown weapon -> Skip",
                    actor->name);
            break;
        }
        int dmg = weapon_spec(wid).damage;
        if (actor->is_player) apply_damage_to_enemy(tgt, dmg);
        else                  apply_damage_to_player(tgt, dmg);
        shm_log(gs, "[USE] %s wields %s (%d dmg)",
                actor->name, weapon_spec(wid).name, dmg);
        // High-tier attacks stun their target per spec ("Certain high-tier
        // attacks can stun"). The three artifact weapons qualify.
        if (wid == W_SOLAR_CORE || wid == W_LUNAR_BLADE || wid == W_ECLIPSE_RELIC) {
            Entity *victim = actor->is_player
                             ? &gs->enemies[tgt] : &gs->players[tgt];
            if (victim && victim->active && victim->alive) deliver_stun(gs, victim);
        }
        break;
    }
    case ACT_SWAP_IN: {
        int inst;
        if (inv_swap_in_from_lts(actor, param, &inst)) {
            shm_log(gs, "[SWAP] %s swapped in from LTS slot %d",
                    actor->name, param);
        } else {
            shm_log(gs, "[SWAP] %s swap-in failed", actor->name);
        }
        break;
    }
    case ACT_HEAL: {
        int amt = actor->max_hp / 10;
        actor->hp = std::min(actor->max_hp, actor->hp + amt);
        shm_log(gs, "[HEAL] %s healed %d hp (hp %d)",
                actor->name, amt, actor->hp);
        break;
    }
    case ACT_SKIP: {
        actor->stamina = actor->max_stamina / 2;
        shm_log(gs, "[SKIP] %s skipped", actor->name);
        pthread_mutex_unlock(&gs->state_mutex);
        return;   // preserve half-stamina (skip behaviour)
    }
    case ACT_ULTIMATE: {
        // Spec §10: "Ultimate Ability Eligibility: a player character may
        // only trigger the Ultimate Ability if both the Solar Core and the
        // Lunar Blade are present in their active primary inventory
        // simultaneously." So we check inventory directly (not just the
        // resource-table lock) — this is the authoritative check.
        bool has_solar = false, has_lunar = false;
        for (int i = 0; i < INVENTORY_SIZE; ++i) {
            if (!actor->inv_head[i]) continue;
            if (actor->inv_slot[i] == W_SOLAR_CORE)  has_solar = true;
            if (actor->inv_slot[i] == W_LUNAR_BLADE) has_lunar = true;
        }
        if (!has_solar || !has_lunar) {
            shm_log(gs,
                "[ULT] %s attempted Ultimate without both artifacts (SC=%d LB=%d)",
                actor->name, has_solar, has_lunar);
            break;
        }
        shm_log(gs, "[ULT] %s triggers Ultimate — ASP paused 10s",
                actor->name);
        gs->ultimate_active = 1;
        gs->ultimate_until = time(nullptr) + ULTIMATE_PAUSE_SEC;
        // Signal-only: SIGSTOP asp, schedule SIGALRM via alarm() which will
        // SIGCONT it in the handler.
        if (gs->asp_pid > 0) kill(gs->asp_pid, SIGSTOP);
        alarm(ULTIMATE_PAUSE_SEC);
        // Damage all alive enemies for big burst.
        for (int i = 0; i < MAX_ENEMIES; ++i) {
            Entity &e = gs->enemies[i];
            if (e.active && e.alive) {
                e.hp -= 200;
                shm_log(gs, "[ULT] E%d takes 200 dmg", i + 1);
                if (e.hp <= 0) handle_entity_death(gs, &e);
            }
        }
        break;
    }
    case ACT_PICKUP: {
        if (gs->pending_drop_weapon != W_NONE && gs->pending_drop_open &&
            actor->is_player) {
            int inst;
            if (inv_add_weapon(actor, gs->pending_drop_weapon, &inst)) {
                shm_log(gs, "[PICK] P%d picked up %s",
                        actor->local_id + 1,
                        weapon_spec(gs->pending_drop_weapon).name);
            } else {
                shm_log(gs, "[PICK] P%d inventory too full",
                        actor->local_id + 1);
            }
            gs->pending_drop_weapon = W_NONE;
            gs->pending_drop_open = 0;
        }
        break;
    }
    case ACT_DECLINE: {
        // Declining doesn't immediately hand it to enemies — other players
        // still get a chance. Only expire it when the deadline passes (the
        // scheduler_loop enforces that).
        break;
    }
    case ACT_ACQUIRE: {
        int ai = tgt;
        if (ai < 0 || ai >= NUM_ARTIFACTS) {
            shm_log(gs, "[ACQ] %s invalid artifact index %d", actor->name, ai);
            break;
        }
        int wid = gs->artifacts[ai].weapon_id;
        int gid = entity_global_id(actor->is_player, actor->local_id);
        // Must actually carry the artifact in inventory too — but for the
        // purposes of the resource table we treat "acquire" as locking the
        // shared artifact. A successful acquire also places an instance
        // into inventory if there's room.
        if (!gs->artifacts[ai].present) {
            shm_log(gs, "[ACQ] %s tried to lock %s (not yet present)",
                    actor->name, weapon_spec(wid).name);
            break;
        }
        bool ok = res_try_acquire(gs, gid, wid);
        if (ok) {
            int inst;
            if (inv_add_weapon(actor, wid, &inst)) {
                shm_log(gs, "[ACQ] %s LOCKED %s",
                        actor->name, weapon_spec(wid).name);
            } else {
                // No inventory room; release the lock rather than fake-own
                // the artifact with no inventory footprint.
                res_release(gs, gid, wid);
                shm_log(gs, "[ACQ] %s couldn't fit %s in inventory — lock released",
                        actor->name, weapon_spec(wid).name);
            }
        } else {
            shm_log(gs, "[ACQ] %s WAITING for %s",
                    actor->name, weapon_spec(wid).name);
        }
        break;
    }
    case ACT_RELEASE: {
        int ai = tgt;
        if (ai < 0 || ai >= NUM_ARTIFACTS) break;
        int wid = gs->artifacts[ai].weapon_id;
        int gid = entity_global_id(actor->is_player, actor->local_id);
        if (gs->artifacts[ai].held_by_global == gid) {
            // Evict the artifact's instance from inventory too.
            for (int i = 0; i < INVENTORY_SIZE; ++i) {
                if (actor->inv_head[i] && actor->inv_slot[i] == wid) {
                    inv_clear_instance(actor, actor->inv_inst[i]);
                    break;
                }
            }
            res_release(gs, gid, wid);
            shm_log(gs, "[REL] %s released %s",
                    actor->name, weapon_spec(wid).name);
        }
        break;
    }
    default:
        actor->stamina = actor->max_stamina / 2;
        shm_log(gs, "[?] %s submitted unknown action %d", actor->name, action);
        pthread_mutex_unlock(&gs->state_mutex);
        return;
    }

    pthread_mutex_unlock(&gs->state_mutex);
}

// ---------- Deadlock-detection thread ----------
void *deadlock_thread(void *arg) {
    GameState *gs = static_cast<GameState *>(arg);
    while (!G_shutdown.load()) {
        int victim = res_break_deadlock(gs);
        if (victim >= 0) {
            Entity *e = entity_by_global(gs, victim);
            shm_log(gs, "[DEADLOCK] Circular wait broken — forced %s to release",
                    e ? e->name : "?");
        }
        struct timespec ts { 0, 500'000'000 };
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

// ---------- Child launch ----------
// Try a few likely paths for the child binary. The Makefile emits binaries
// into build/, but users may also run arbiter from inside build/.
std::string G_exe_dir;

pid_t launch_child(const char *name) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        std::string candidates[] = {
            G_exe_dir + "/" + name,
            std::string("./build/") + name,
            std::string("./") + name,
        };
        for (const auto &path : candidates) {
            execl(path.c_str(), name, (char *)nullptr);
            // ENOENT → try next
        }
        fprintf(stderr, "[arbiter] could not exec %s from any candidate path\n", name);
        _exit(127);
    }
    return pid;
}

} // namespace

int main(int argc, char **argv) {
    int seed = 240673;            // default roll-number digits
    int num_players = -1;

    // Resolve the directory this binary lives in so we can locate sibling
    // executables (hip, asp) regardless of the caller's cwd.
    {
        std::string argv0 = argv[0] ? argv[0] : "./arbiter";
        auto slash = argv0.find_last_of('/');
        G_exe_dir = (slash == std::string::npos) ? std::string(".")
                                                 : argv0.substr(0, slash);
    }

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc)        seed = std::atoi(argv[++i]);
        else if (a == "--players" && i + 1 < argc) num_players = std::atoi(argv[++i]);
    }

    if (num_players < 1 || num_players > MAX_PLAYERS) {
        fprintf(stderr, "Select party size [1-%d]: ", MAX_PLAYERS);
        fflush(stderr);
        char buf[16];
        if (fgets(buf, sizeof(buf), stdin)) num_players = std::atoi(buf);
        if (num_players < 1 || num_players > MAX_PLAYERS) num_players = 2;
    }

    G_rng.seed(seed);

    G_trace = fopen("chrono_rift_trace.csv", "w");
    if (G_trace) {
        fprintf(G_trace, "t_ns,event,entity,detail\n");
    }

    // Pre-clean any stale shm from a previous run.
    shm_destroy();

    GameState *gs = shm_open_gamestate(/*create=*/true);
    if (!gs) return 1;
    G_gs = gs;

    init_game_state(gs, seed, num_players);
    install_signals();

    fprintf(stderr, "[arbiter] pid=%d seed=%d players=%d enemies=%d — launching hip/asp\n",
            getpid(), seed, num_players, gs->num_enemies);

    // Launch asp first so it's ready to receive stun signals.
    pid_t asp_pid = launch_child("asp");
    pid_t hip_pid = launch_child("hip");
    gs->asp_pid = asp_pid;
    gs->hip_pid = hip_pid;

    gs->phase = PHASE_RUNNING;
    shm_log(gs, "Game started. seed=%d players=%d enemies=%d",
            seed, num_players, gs->num_enemies);

    // Deadlock-detection background thread
    pthread_t dl_tid;
    pthread_create(&dl_tid, nullptr, deadlock_thread, gs);

    // Main scheduler loop runs in this thread.
    scheduler_loop(gs);

    // Shutdown: mark phase (if not already), tear down children.
    pthread_mutex_lock(&gs->state_mutex);
    if (gs->phase == PHASE_RUNNING) gs->phase = PHASE_QUIT;
    int final_phase = gs->phase;
    pthread_mutex_unlock(&gs->state_mutex);

    // Wake all waiting threads so they can exit.
    for (int i = 0; i < MAX_PLAYERS; ++i) sem_post(&gs->players[i].turn_sem);
    for (int i = 0; i < MAX_ENEMIES; ++i) sem_post(&gs->enemies[i].turn_sem);

    if (asp_pid > 0) { kill(asp_pid, SIGCONT); kill(asp_pid, SIGTERM); }
    if (hip_pid > 0) kill(hip_pid, SIGTERM);

    pthread_join(dl_tid, nullptr);

    waitpid(asp_pid, nullptr, 0);
    waitpid(hip_pid, nullptr, 0);

    fprintf(stderr, "[arbiter] game ended with phase=%d\n", final_phase);

    // Turnaround-time summary for the report.
    FILE *sum = fopen("chrono_rift_summary.txt", "w");
    auto report_entity = [&](FILE *f, const Entity &e) {
        if (!e.active) return;
        long long avg_wait = e.turns_taken ? e.total_wait_ns  / e.turns_taken : 0;
        long long avg_burst = e.turns_taken ? e.total_burst_ns / e.turns_taken : 0;
        fprintf(f,
            "%-6s  turns=%-3d  avg_wait_ms=%-6.2f  avg_burst_ms=%-6.2f  "
            "total_wait_ms=%-8.2f  total_burst_ms=%-8.2f\n",
            e.name, e.turns_taken,
            avg_wait / 1e6, avg_burst / 1e6,
            e.total_wait_ns / 1e6, e.total_burst_ns / 1e6);
    };
    FILE *dst[] = { stderr, sum };
    for (FILE *f : dst) {
        if (!f) continue;
        fprintf(f, "\n=== Chrono Rift turnaround summary ===\n");
        fprintf(f, "phase=%d seed=%d players=%d enemies=%d kills=%d\n",
                final_phase, gs->seed, gs->num_players, gs->num_enemies,
                gs->enemies_killed);
        for (int i = 0; i < MAX_PLAYERS; ++i) report_entity(f, gs->players[i]);
        for (int i = 0; i < MAX_ENEMIES; ++i) report_entity(f, gs->enemies[i]);
    }
    if (sum) fclose(sum);
    if (G_trace) { fclose(G_trace); G_trace = nullptr; }

    shm_destroy();
    return 0;
}
