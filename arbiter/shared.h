// shared.h — Shared types, constants, and shared-memory layout for Chrono Rift.
//
// All three processes (arbiter, hip, asp) include this header. It is kept in
// arbiter/ so the Makefile's per-folder glob doesn't pick it up as a .cpp, and
// hip/asp reference it via ../arbiter/shared.h.
//
// Rules enforced here:
//   * IPC is strictly shared-memory based (no pipes).
//   * All synchronisation primitives are memory-based (unnamed semaphores,
//     pthread mutexes with the pshared attribute).
//   * Signals are used for stun (SIGUSR1), ultimate-ability pause (SIGSTOP/
//     SIGCONT orchestrated via SIGALRM in the arbiter) and quit (SIGTERM).

#pragma once

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <stdint.h>
#include <time.h>

// ---------- Configuration constants ----------
#define SHM_NAME         "/chrono_rift_shm"

#define MAX_PLAYERS      4
#define MAX_ENEMIES      9
#define INVENTORY_SIZE   20
#define LTS_SIZE         32
#define NUM_ARTIFACTS    3        // Solar Core, Lunar Blade, Eclipse Relic
#define LOG_CAPACITY     128
#define LOG_MSG_LEN      160
#define NAME_LEN         16

#define TICK_MS          1000     // 1 stamina tick per second
#define NPC_TURN_TIMEOUT_SEC 3
#define STUN_DURATION_SEC    3
#define ULTIMATE_PAUSE_SEC   10

#define KILL_GOAL        10       // win condition: kill this many enemies

// ---------- Enums ----------
enum GamePhase {
    PHASE_SETUP   = 0,
    PHASE_RUNNING = 1,
    PHASE_WIN     = 2,
    PHASE_LOSE    = 3,
    PHASE_QUIT    = 4
};

enum ActionType {
    ACT_NONE       = 0,
    ACT_STRIKE     = 1,
    ACT_EXHAUST    = 2,
    ACT_USE_WEAPON = 3,
    ACT_SWAP_IN    = 4,
    ACT_HEAL       = 5,
    ACT_SKIP       = 6,
    ACT_ULTIMATE   = 7,
    ACT_PICKUP     = 8,   // pick up dropped weapon / Eclipse Relic
    ACT_DECLINE    = 9
};

enum WeaponId {
    W_NONE           = 0,
    W_SOLAR_CORE     = 1,
    W_LUNAR_BLADE    = 2,
    W_IRON_HALBERD   = 3,
    W_VENOM_DAGGER   = 4,
    W_THUNDERSTAFF   = 5,
    W_OBSIDIAN_AXE   = 6,
    W_FROSTBOW       = 7,
    W_SPLINTER_STICK = 8,
    W_ECLIPSE_RELIC  = 9,   // dynamic artifact; 5 slots, 70 dmg
    W_COUNT
};

struct WeaponSpec {
    const char *name;
    int slot_size;
    int damage;
};

// Kept in a function to avoid multiple-definition issues across TUs while
// letting every process share one authoritative table.
inline const WeaponSpec &weapon_spec(int id) {
    static const WeaponSpec table[W_COUNT] = {
        {"-",              0,  0},
        {"Solar Core",    10, 95},
        {"Lunar Blade",   10, 90},
        {"Iron Halberd",   7, 55},
        {"Venom Dagger",   4, 30},
        {"Thunderstaff",   6, 50},
        {"Obsidian Axe",   5, 45},
        {"Frostbow",       6, 48},
        {"Splinter Stick", 2, 12},
        {"Eclipse Relic",  5, 70}
    };
    if (id < 0 || id >= W_COUNT) id = 0;
    return table[id];
}

// ---------- Entity ----------
struct Entity {
    int  active;              // 1 if this slot is in use
    int  is_player;           // 1 player, 0 enemy
    int  local_id;            // 0..MAX_PLAYERS-1 or 0..MAX_ENEMIES-1
    char name[NAME_LEN];

    int  alive;
    int  hp;
    int  max_hp;
    int  damage;
    int  speed;               // stamina gained per tick
    int  stamina;
    int  max_stamina;

    // Stun: when stunned is 1, scheduler must not schedule this entity and
    // stamina accrual is frozen until stun_until is past.
    int       stunned;
    time_t    stun_until;

    // Signal-targeting. pid is the process that hosts this entity's thread
    // (hip for players, asp for enemies).
    pid_t     host_pid;

    // Inventory. inv_slot[i] = weapon id in slot i, or W_NONE if free.
    // inv_inst[i] = instance id (shared across contiguous slots of the same
    // weapon instance). head_flag[i] = 1 if this is the first slot of the
    // instance (the "anchor"). Allows us to iterate distinct instances.
    int  inv_slot[INVENTORY_SIZE];
    int  inv_inst[INVENTORY_SIZE];
    int  inv_head[INVENTORY_SIZE];
    int  next_inst_id;

    // Long-term storage — simple stack of weapon ids.
    int  lts[LTS_SIZE];
    int  lts_count;

    // Per-entity turn gate. Posted by arbiter; waited on by the owning
    // player/NPC thread. Unnamed, pshared=1.
    sem_t turn_sem;

    // Action slot filled by the entity when responding to its turn.
    int  pending_action;
    int  action_target;       // target entity local_id (enemy index for
                              // player attack, player index for NPC attack)
    int  action_param;        // weapon instance id / LTS index / etc.
    sem_t action_ready;       // posted by entity; waited on by arbiter
};

// ---------- Artifacts ----------
struct ArtifactSlot {
    int  weapon_id;           // W_SOLAR_CORE / W_LUNAR_BLADE / W_ECLIPSE_RELIC
    int  present;             // Eclipse Relic starts absent (0); appears later
    int  held_by_global;      // -1 free, else global id (see entity_global_id)
    int  waiter_count;
    int  waiters[MAX_PLAYERS + MAX_ENEMIES];
};

// ---------- Log ring buffer ----------
struct LogEntry {
    char msg[LOG_MSG_LEN];
};

// ---------- Shared game state ----------
struct GameState {
    // Coarse-grained state mutex for most fields (players/enemies/phase/etc.).
    pthread_mutex_t state_mutex;
    // Separate lock for the artifact resource table (deadlock-detection path).
    pthread_mutex_t resource_mutex;
    // Log ring buffer lock.
    pthread_mutex_t log_mutex;

    // Config / identity
    int   seed;
    int   phase;
    int   num_players;
    int   num_enemies;
    int   enemies_killed;

    // Process ids (populated by each process on startup).
    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;

    // Scheduler state
    int   active_global;      // -1 when no one is acting; else entity_global_id

    // Entities
    Entity players[MAX_PLAYERS];
    Entity enemies[MAX_ENEMIES];

    // Artifact table
    ArtifactSlot artifacts[NUM_ARTIFACTS];

    // Weapon-drop state (after an enemy dies)
    int   pending_drop_weapon;       // 0 if none
    int   pending_drop_for_player;   // player local_id offered the pickup
    time_t pending_drop_deadline;

    // Ultimate-ability state (for renderer/log only; enforcement is via
    // SIGSTOP/SIGCONT in the arbiter).
    int   ultimate_active;
    time_t ultimate_until;

    // Log
    LogEntry log[LOG_CAPACITY];
    int   log_head;
    int   log_count;
};

// ---------- Helpers for global entity ids ----------
// Packs (is_player, local_id) into a single int so artifacts/waiters can
// reference either kind uniformly. Player ids occupy [0, MAX_PLAYERS),
// enemy ids occupy [MAX_PLAYERS, MAX_PLAYERS+MAX_ENEMIES).
inline int entity_global_id(int is_player, int local_id) {
    return is_player ? local_id : (MAX_PLAYERS + local_id);
}
inline int global_is_player(int g) { return g < MAX_PLAYERS; }
inline int global_local_id(int g)  { return global_is_player(g) ? g : (g - MAX_PLAYERS); }
