# Chrono Rift — Build & Run

## Inside the provided Docker container

```bash
docker build -t chrono-rift-env .
docker run -it --rm -v "$(pwd):/app" chrono-rift-env
# …now inside container…
make
./build/arbiter                     # prompts for party size (1-4)
# or non-interactively:
./build/arbiter --seed 240673 --players 2
```

The arbiter will fork-and-exec the `asp` and `hip` sibling binaries located in
`./build/`. It also tries `./build/<name>` and `./<name>` as fallbacks, so it
works regardless of the cwd you invoke it from.

## Controls (in the TUI)

While your active player's turn starts (indicated by `*` next to the name),
press one of:

| Key | Action |
|-----|--------|
| s | Strike — prompts for a target enemy index |
| x | Exhaust — drains target enemy stamina |
| w | Use weapon — pick a weapon instance and a target. Using an artifact weapon (Solar Core / Lunar Blade / Eclipse Relic) stuns the target for 3 s. |
| i | Swap-in from long-term storage |
| h | Heal 10% of max HP |
| u | Ultimate ability (requires holding BOTH Solar Core AND Lunar Blade). Triggers `SIGSTOP` on the ASP process for 10 s; `SIGALRM` in the arbiter resumes it. |
| a | Acquire an artifact (1=Solar Core, 2=Lunar Blade, 3=Eclipse Relic). Registers you as a waiter if already held — this is how circular waits arise for the deadlock detector. |
| r | Release an artifact you hold (frees the resource + evicts it from inventory). |
| k | Skip (keeps 50% stamina) |
| q | Quit (sends SIGTERM to the arbiter) |

When an enemy dies and drops a weapon, any alive player is offered the pickup
with `y/n` — declining lets an enemy grab it.

## Turnaround analysis

At shutdown the arbiter writes two files next to the binary (cwd):

* `chrono_rift_trace.csv` — per-turn event log (`t_ns,event,entity,action=…`) suitable for plotting in the `report.pdf`.
* `chrono_rift_summary.txt` — per-entity summary: turns taken, average wait-to-act ms, average burst ms, totals.

The same summary is also echoed to `stderr` right before the arbiter unlinks the shared-memory segment, so you can copy-paste it into the report directly.

## Seed

Pass `--seed <NNNNNN>` to override the roll-number seed used for all stat
randomisation. The default in-source is `240673` (inferred from the
repository owner's roll number); swap it per your own roll when grading.

## Folder layout

```
.
├── Dockerfile
├── Makefile
├── requirements.txt            (apt extras; blank by design)
├── arbiter/                    central authority process
│   ├── arbiter.cpp
│   ├── shared.h                shared types & shm layout
│   ├── shm_util.h              shm attach + log ring buffer
│   ├── inventory.h             20-slot first-fit allocator + LTS swap
│   └── resources.h             artifact table + deadlock detector
├── hip/hip.cpp                 human input + ncurses renderer
├── asp/asp.cpp                 NPC threads
├── PROJECT_BRIEF.txt           original project statement (renamed from
│                               requirements.txt to free that filename for
│                               the Dockerfile's apt requirements list)
└── Chrono_Rift_Docker_Guide.docx
```

## OS-concepts checklist (per project statement)

| Spec | Where |
|------|-------|
| POSIX shared memory | `shm_open_gamestate` in `shm_util.h` |
| Unnamed semaphores in shm | `Entity::turn_sem`, `Entity::action_ready` |
| pshared pthread mutexes | `state_mutex`, `resource_mutex`, `log_mutex` |
| One thread per player (hip) | `player_thread` in `hip/hip.cpp` |
| One thread per NPC (asp)   | `npc_thread` in `asp/asp.cpp` |
| Only active player reads input | semaphore gate on `turn_sem` |
| Stamina-based scheduling | `scheduler_tick` in `arbiter.cpp` |
| Serial action execution | `gs->active_global` + `action_ready` wait |
| Asynchronous stun via signal | `sigqueue(SIGUSR1)` → `pthread_kill(SIGUSR2)` → `nanosleep(3s)` |
| Ultimate ability (signal-only) | `SIGSTOP`/`SIGCONT` on asp + `SIGALRM` timer |
| NPC turn timeout → auto-skip | `sem_timedwait` in `wait_for_action` |
| Quit via SIGTERM from hip | `'q'` key in `player_turn` |
| Resource table + deadlock detect | `resources.h` + `deadlock_thread` |
| Inventory 20 slots, first-fit, LTS | `inventory.h` |
| Async rendering thread | `render_thread` in `hip` |
| Roll-number seed | `--seed` CLI (default 240673) |

## Known deviations from the Docker guide template

* **Makefile outputs to `build/`** rather than repo root. The template as
  provided tries to emit `./arbiter`, `./hip`, `./asp` alongside the
  mandatory `arbiter/`, `hip/`, `asp/` **directories** — on POSIX filesystems
  these collide ("ld: cannot open output file arbiter: Is a directory"). The
  smallest correction is to redirect binaries into a sibling `build/`
  folder; the Makefile otherwise matches the template verbatim.
* **`requirements.txt`** is the **apt-packages** file the Dockerfile
  `COPY`s. The project statement text that originally shipped at that
  filename has been moved to `PROJECT_BRIEF.txt` so both files can coexist.
