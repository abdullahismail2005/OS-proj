# Chrono Rift — CS 2006 Operating Systems (Spring 2026)

Multi-process tactical game implementing POSIX shared memory, pthreads, unnamed
semaphores, and POSIX real-time signals. Full spec: `PROJECT_BRIEF.txt`.

## Quick start

```bash
make                                    # builds build/arbiter, build/hip, build/asp
./build/arbiter --seed 240673 --players 2
```

Full run + key-table + demo + report instructions are in `BUILD.md`.

## Note on the provided Makefile / folder structure

The Docker setup guide ships a Makefile whose targets are named exactly
`arbiter`, `hip`, `asp` and emit binaries at the repository root
(`$(CXX) ... -o $@`). The same guide **also** requires source folders named
`arbiter/`, `hip/`, `asp/` at the repository root. Those two constraints are
mutually exclusive — the linker cannot write an output file named `arbiter`
when a directory named `arbiter` exists in the same location
(`ld: cannot open output file arbiter: Is a directory`).

Our Makefile keeps the required folder layout (`arbiter/arbiter.cpp`,
`hip/hip.cpp`, `asp/asp.cpp`), the required `LIBS` line (ncurses), and every
other part of the guide's Makefile **unchanged**. The only deviation is the
binary output directory:

| Guide template            | This repo                      |
| ------------------------- | ------------------------------ |
| `./arbiter`, `./hip`, `./asp` | `./build/arbiter`, `./build/hip`, `./build/asp` |

To invoke the three processes the daily-workflow commands in §8 of the setup
guide become:

```bash
./build/arbiter & ./build/hip & ./build/asp &
kill %1 %2 %3
```

This is the minimal change needed to make the guide's own Makefile compile
at all; please contact the TA if the grading harness requires the binaries at
`./` rather than `./build/`. A single-line tweak to the `TARGETS` line of the
Makefile is all it takes to relocate them.

## Submission artifacts

* `Dockerfile`        — verbatim from the Docker setup guide §5.
* `Makefile`          — LIBS line set to ncurses; TARGETS relocated to `build/`.
* `requirements.txt`  — no extra apt packages required (blank per §4 of the guide).
* `arbiter/`, `hip/`, `asp/` — mandatory per §3 of the guide.
* `report.pdf`        — turnaround analysis (generated from an 80-turn demo run
  via `tools/generate_report.py`).
* `BUILD.md`          — full run guide (native, Docker, demo, report, §11 MP).
* `PROJECT_BRIEF.txt` — original assignment brief.
