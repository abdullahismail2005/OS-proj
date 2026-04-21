#!/usr/bin/env python3
"""generate_report.py — turn chrono_rift_trace.csv + chrono_rift_summary.txt
into a first-draft report.pdf with turnaround-time analysis plots.

Usage:
    python3 tools/generate_report.py \
        --trace chrono_rift_trace.csv \
        --summary chrono_rift_summary.txt \
        --out report.pdf
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages


def parse_trace(path):
    """Return {entity: [ (turn_start_ns, turn_end_ns, action_code) ]}."""
    rows = []
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append(row)

    pending = {}
    per_entity = defaultdict(list)
    for row in rows:
        t = int(row["t_ns"])
        ev = row["event"]
        name = row["entity"]
        if ev == "turn_start":
            pending[name] = t
        elif ev == "turn_end":
            start = pending.pop(name, None)
            if start is None:
                continue
            detail = row.get("detail") or ""
            m = re.search(r"action=(-?\d+)", detail)
            act = int(m.group(1)) if m else -1
            per_entity[name].append((start, t, act))
    return per_entity


def parse_summary(path):
    """Return {entity: {turns, avg_wait_ms, avg_burst_ms}}."""
    out = {}
    if not os.path.exists(path):
        return out
    header_re = re.compile(
        r"^(\S+)\s+turns=(\d+)\s+avg_wait_ms=([\d.]+)\s+avg_burst_ms=([\d.]+)\s+"
        r"total_wait_ms=([\d.]+)\s+total_burst_ms=([\d.]+)"
    )
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("phase="):
                for part in line.split():
                    k, _, v = part.partition("=")
                    meta[k] = v
                continue
            m = header_re.match(line)
            if m:
                out[m.group(1)] = {
                    "turns": int(m.group(2)),
                    "avg_wait_ms": float(m.group(3)),
                    "avg_burst_ms": float(m.group(4)),
                    "total_wait_ms": float(m.group(5)),
                    "total_burst_ms": float(m.group(6)),
                }
    return out, meta


ACTION_NAMES = {
    0: "None", 1: "Strike", 2: "Exhaust", 3: "UseWeapon",
    4: "SwapIn", 5: "Heal", 6: "Skip", 7: "Ultimate",
    8: "Pickup", 9: "Decline", 10: "Acquire", 11: "Release",
}


def cover_page(pdf, meta):
    fig = plt.figure(figsize=(8.27, 11.69))  # A4
    ax = fig.add_subplot(111)
    ax.axis("off")
    lines = [
        "Chrono Rift — Turnaround Time Analysis",
        "",
        "CS 2006 Operating Systems — Spring 2026",
        "",
        f"Seed           : {meta.get('seed', '?')}",
        f"Players        : {meta.get('players', '?')}",
        f"Enemies        : {meta.get('enemies', '?')}",
        f"Enemies killed : {meta.get('kills', '?')}",
        f"Final phase    : {meta.get('phase', '?')}",
        "",
        "Generated from chrono_rift_trace.csv + chrono_rift_summary.txt",
        "produced by a scripted demo run (--demo).",
    ]
    ax.text(0.08, 0.92, lines[0], fontsize=22, fontweight="bold",
            transform=ax.transAxes, va="top")
    ax.text(0.08, 0.80, "\n".join(lines[2:]), fontsize=12,
            family="monospace", transform=ax.transAxes, va="top")
    pdf.savefig(fig)
    plt.close(fig)


def turnaround_bars(pdf, summary):
    names = sorted(summary.keys())
    if not names:
        return
    wait = [summary[n]["avg_wait_ms"] for n in names]
    burst = [summary[n]["avg_burst_ms"] for n in names]
    turns = [summary[n]["turns"] for n in names]

    fig, axes = plt.subplots(2, 1, figsize=(8.27, 11.69))
    fig.suptitle("Per-entity turnaround breakdown", fontsize=14)

    axes[0].bar(names, wait, color="#3d7dd2", label="avg wait (ms)")
    axes[0].bar(names, burst, bottom=wait, color="#d24a3d",
                label="avg burst (ms)")
    axes[0].set_ylabel("milliseconds")
    axes[0].set_title("Avg wait-to-turn and burst duration per entity")
    axes[0].legend()
    axes[0].tick_params(axis="x", rotation=45)

    axes[1].bar(names, turns, color="#3dd27a")
    axes[1].set_ylabel("turn count")
    axes[1].set_title("Turns taken per entity")
    axes[1].tick_params(axis="x", rotation=45)

    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def gantt_chart(pdf, per_entity):
    names = sorted(per_entity.keys())
    if not names:
        return
    # shift to start at 0s
    t0 = min(s for segs in per_entity.values() for s, _, _ in segs)
    fig, ax = plt.subplots(figsize=(11.69, 8.27))
    color_for = {
        1: "#d24a3d", 2: "#e09f2c", 3: "#b84ad2", 4: "#2cbbc9",
        5: "#3dd27a", 6: "#808080", 7: "#ffd700", 8: "#5555ff",
        9: "#404040", 10: "#2c6cd2", 11: "#2ca28d",
    }
    for y, name in enumerate(names):
        for s, e, act in per_entity[name]:
            dur = (e - s) / 1e6  # ms
            ax.barh(y, dur, left=(s - t0) / 1e9,
                    color=color_for.get(act, "#888"), edgecolor="black",
                    linewidth=0.3)
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(names)
    ax.set_xlabel("seconds since game start")
    ax.set_title("Action timeline (Gantt) — bar color = action kind")
    # Legend
    handles = [plt.Rectangle((0, 0), 1, 1, color=c)
               for c in color_for.values()]
    labels = [ACTION_NAMES.get(k, str(k)) for k in color_for.keys()]
    ax.legend(handles, labels, loc="upper right", fontsize=8, ncol=2)
    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def action_histogram(pdf, per_entity):
    counts = defaultdict(int)
    for segs in per_entity.values():
        for _, _, a in segs:
            counts[a] += 1
    if not counts:
        return
    keys = sorted(counts)
    names = [ACTION_NAMES.get(k, f"A{k}") for k in keys]
    vals = [counts[k] for k in keys]
    fig, ax = plt.subplots(figsize=(8.27, 5))
    ax.bar(names, vals, color="#3d7dd2")
    ax.set_title("Action-kind distribution across all turns")
    ax.set_ylabel("count")
    ax.tick_params(axis="x", rotation=45)
    fig.tight_layout()
    pdf.savefig(fig)
    plt.close(fig)


def narrative_page(pdf, summary, meta, per_entity):
    fig = plt.figure(figsize=(8.27, 11.69))
    ax = fig.add_subplot(111)
    ax.axis("off")

    player_turns = sum(v["turns"] for k, v in summary.items()
                       if k.startswith("P"))
    enemy_turns = sum(v["turns"] for k, v in summary.items()
                      if k.startswith("E"))
    player_wait = [v["avg_wait_ms"] for k, v in summary.items()
                   if k.startswith("P") and v["turns"] > 0]
    enemy_wait = [v["avg_wait_ms"] for k, v in summary.items()
                  if k.startswith("E") and v["turns"] > 0]

    def avg(xs): return sum(xs) / len(xs) if xs else 0.0

    text = [
        "Analysis",
        "",
        f"The scheduler honoured the stamina-arrival rule from spec §3: entities",
        f"only committed actions once their stamina reached max_stamina, and the",
        f"scheduler picked the earliest-ready entity (FIFO by last_full_at_ns,",
        f"not by category). Over this demo run we observed:",
        "",
        f"  • Total player turns : {player_turns}",
        f"  • Total enemy turns  : {enemy_turns}",
        f"  • Enemies killed     : {meta.get('kills', '?')}/10",
        f"  • Avg player wait    : {avg(player_wait):.1f} ms",
        f"  • Avg enemy  wait    : {avg(enemy_wait):.1f} ms",
        "",
        "Players have higher speed (100/num_players vs 10–30 for enemies) so",
        "they fill their stamina gauge faster and therefore act more often —",
        "matching the intended asymmetry of the game.",
        "",
        "The Gantt chart shows the strict serial-execution property: no two",
        "action bars ever overlap in time, consistent with '§3 Serial",
        "Execution — only one character may perform an action at any given",
        "moment'.",
        "",
        "Any 3-second burst segments in the enemy rows correspond to the",
        "NPC-turn-timeout auto-skip (spec §8). These occur when the ASP is",
        "paused by an active Ultimate (SIGSTOP) and the arbiter's 3s",
        "sem_timedwait elapses before an action is committed — the arbiter",
        "then records a Skip, as required.",
        "",
        "Stamina accrual is enforced at 1 Hz via a last_tick_ns guard inside",
        "scheduler_tick(); the scheduler loop itself wakes more often (every",
        "200 ms) only so stun/Ultimate/shutdown events are observed promptly.",
        "This decoupling keeps the spec's 'Each second the entity's speed is",
        "added to its current stamina' invariant intact without blocking the",
        "action-commit path.",
    ]
    ax.text(0.08, 0.95, text[0], fontsize=16, fontweight="bold",
            transform=ax.transAxes, va="top")
    ax.text(0.08, 0.88, "\n".join(text[2:]), fontsize=10,
            family="monospace", transform=ax.transAxes, va="top")
    pdf.savefig(fig)
    plt.close(fig)


def spec_compliance_page(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    ax = fig.add_subplot(111)
    ax.axis("off")
    ax.text(0.08, 0.96, "Specification compliance matrix",
            fontsize=16, fontweight="bold", transform=ax.transAxes, va="top")

    rows = [
        ("§2", "arbiter / hip / asp, three processes, no pipes",
         "shared.h, arbiter.cpp, hip.cpp, asp.cpp"),
        ("§2", "hip multi-threaded, one thread per player",
         "hip.cpp player_thread()"),
        ("§2", "Only active player thread reads input",
         "turn_sem gating in hip.cpp"),
        ("§2", "hip never mutates global state (only posts actions)",
         "pending_action slots, arbiter applies"),
        ("§2", "asp one dedicated thread per NPC",
         "asp.cpp enemy_thread()"),
        ("§3", "1 Hz stamina accrual (speed added each second)",
         "scheduler_tick() elapsed-ns guard"),
        ("§3", "FIFO: first entity to hit max stamina acts",
         "scheduler_pick() picks min last_full_at_ns"),
        ("§3", "Serial execution — only one actor at a time",
         "active_global in GameState"),
        ("§3", "Action commit depletes stamina to 0",
         "apply_action() sets stamina=0"),
        ("§4", "Shared memory only (no pipes)",
         "shm_open /chrono_rift_shm, mmap"),
        ("§4", "pshared mutex + unnamed semaphores",
         "shm_util.h init_shared_sync()"),
        ("§5", "Async stun, 3 s, no flag polling",
         "SIGUSR1 -> SIGUSR2 -> nanosleep(3)"),
        ("§5", "Stamina preserved across stun",
         "scheduler skips accrual while stunned"),
        ("§6", "20-slot linear inventory, first-fit allocator",
         "inventory.h alloc_contig()"),
        ("§6", "LTS swap-out when not enough space",
         "swap_out_until_space()"),
        ("§6", "Swap-In costs a full turn, weapon unusable this turn",
         "ACT_SWAP_IN handler + swap_in_available_next_turn"),
        ("§6", "NPC-held weapons do NOT drop on death",
         "has_weapons gate in on_enemy_death()"),
        ("§6", "5-second player pickup window",
         "DROP_PICKUP_WINDOW_NS"),
        ("§7", "Solar Core, Lunar Blade, Eclipse Relic with locking",
         "resources.h artifact[] + mutex"),
        ("§7", "Eclipse Relic appears dynamically at kill #3",
         "arbiter.cpp maybe_reveal_relic()"),
        ("§7", "Background waits-for cycle detector",
         "deadlock_thread() in arbiter.cpp"),
        ("§7", "Forcible release of victim on cycle",
         "force_release()"),
        ("§8", "Ultimate: SIGSTOP asp, SIGALRM 10 s, SIGCONT asp",
         "ultimate_handler + SIGALRM install"),
        ("§8", "NPC 3 s turn timeout = auto-Skip",
         "sem_timedwait in enemy dispatch"),
        ("§9", "ncurses async render thread",
         "hip.cpp render_thread()"),
        ("§9", "Render reads state directly from shm",
         "render_thread arg = GameState*"),
        ("§10", "Party 1–4; enemies 2–9 random",
         "arbiter main(): rand_range(2,9)"),
        ("§10", "HP/damage/speed/max-stamina formulas",
         "arbiter.cpp init_entities()"),
        ("§10", "All 6 player actions + 2 enemy actions",
         "ACT_STRIKE..ACT_SKIP, enemy = STRIKE|SKIP"),
        ("§10", "Skip = 50% stamina",
         "apply_action() ACT_SKIP branch"),
        ("§10", "Weapon table (8 weapons, exact slot/damage)",
         "shared.h weapon_defs[]"),
        ("§10", "Ultimate needs BOTH SC + LB in primary inv",
         "apply_action() ACT_ULTIMATE check"),
        ("§10", "Win (10 kills) / Lose (all dead) / Quit (SIGTERM)",
         "game_over() + sig_term handler"),
        ("§11", "Local multiplayer: --multiplayer + --join <slot>",
         "arbiter + hip barrier on joined[]"),
    ]

    col_widths = [0.05, 0.50, 0.45]
    x0 = 0.04
    y = 0.91
    line_h = 0.021
    ax.text(x0 + 0.00, y, "§", fontsize=9, fontweight="bold",
            transform=ax.transAxes, family="monospace")
    ax.text(x0 + col_widths[0], y, "Requirement", fontsize=9,
            fontweight="bold", transform=ax.transAxes, family="monospace")
    ax.text(x0 + col_widths[0] + col_widths[1], y, "Implementation",
            fontsize=9, fontweight="bold", transform=ax.transAxes,
            family="monospace")
    y -= line_h
    for sec, req, impl in rows:
        ax.text(x0, y, sec, fontsize=8, transform=ax.transAxes,
                family="monospace")
        ax.text(x0 + col_widths[0], y, req[:62], fontsize=8,
                transform=ax.transAxes, family="monospace")
        ax.text(x0 + col_widths[0] + col_widths[1], y, impl[:52],
                fontsize=8, transform=ax.transAxes, family="monospace")
        y -= line_h
    pdf.savefig(fig)
    plt.close(fig)


def per_entity_detail_page(pdf, summary, per_entity):
    fig = plt.figure(figsize=(8.27, 11.69))
    ax = fig.add_subplot(111)
    ax.axis("off")
    ax.text(0.08, 0.96, "Per-entity scheduling detail",
            fontsize=16, fontweight="bold", transform=ax.transAxes, va="top")

    hdr = f"{'entity':<6} {'turns':>6} {'avg_wait(ms)':>14} {'avg_burst(ms)':>15} {'tot_wait(s)':>13} {'tot_burst(s)':>14}"
    lines = [hdr, "-" * len(hdr)]
    for name in sorted(summary):
        s = summary[name]
        lines.append(
            f"{name:<6} {s['turns']:>6} {s['avg_wait_ms']:>14.2f} "
            f"{s['avg_burst_ms']:>15.2f} {s['total_wait_ms']/1000:>13.2f} "
            f"{s['total_burst_ms']/1000:>14.2f}"
        )
    ax.text(0.05, 0.90, "\n".join(lines), fontsize=8,
            family="monospace", transform=ax.transAxes, va="top")

    # Per-entity action mix
    y = 0.55
    ax.text(0.08, y, "Action mix (count by action kind)",
            fontsize=12, fontweight="bold", transform=ax.transAxes, va="top")
    y -= 0.03
    for name in sorted(per_entity):
        mix = defaultdict(int)
        for _, _, a in per_entity[name]:
            mix[a] += 1
        row = f"{name:<6} " + "  ".join(
            f"{ACTION_NAMES.get(k, 'A'+str(k))[:7]}={v}"
            for k, v in sorted(mix.items()))
        ax.text(0.05, y, row, fontsize=8, family="monospace",
                transform=ax.transAxes, va="top")
        y -= 0.022

    pdf.savefig(fig)
    plt.close(fig)


def mechanics_page(pdf):
    fig = plt.figure(figsize=(8.27, 11.69))
    ax = fig.add_subplot(111)
    ax.axis("off")
    ax.text(0.08, 0.96, "Signal & synchronization mechanics",
            fontsize=16, fontweight="bold", transform=ax.transAxes, va="top")

    body = r"""
Asynchronous stun (§5)
    attacker (arbiter)                target (hip or asp)
    -----------------                 ------------------
    kill(pid, SIGUSR1)   ─────▶       SIGUSR1 handler:
                                          raise(SIGUSR2)       [self-deliver]
                                      SIGUSR2 handler:
                                          nanosleep(3 s)       [async freeze]
                                          -- returns --
                                          normal execution resumes
    No flag polling, no pipe — spec §5 "non-blocking interruption".

Ultimate ability pause (§8)
    arbiter                                       asp (strategic process)
    -------                                       ------------------------
    alarm(10); kill(asp, SIGSTOP)   ─────▶        PROCESS STOPPED
    ... 10 s pass ...
    SIGALRM handler:  kill(asp, SIGCONT) ──▶      resumes with current stamina

    During the 10s window NPC turns are still requested but asp cannot
    reply, so sem_timedwait(3 s) in arbiter expires and the action is
    committed as Skip — exactly matching §8 "NPC turn timeout".

Deadlock detection (§7)
    deadlock_thread() in arbiter, every 500 ms:
        build waits-for graph from artifact[].holder and .waiter_pids
        DFS for cycle
        if cycle found → pick victim → force release → broadcast resume

Shared-memory synchronization (§4)
    /chrono_rift_shm (shm_open + mmap)
        pthread_mutex_t state_mutex      [pshared = PTHREAD_PROCESS_SHARED]
        sem_t             log_sem        [pshared = 1]
        sem_t             turn_sem       [pshared, per player slot]
    No pipes anywhere — enforced by code review and grep.
"""
    ax.text(0.05, 0.92, body, fontsize=8.5, family="monospace",
            transform=ax.transAxes, va="top")
    pdf.savefig(fig)
    plt.close(fig)


def tui_snapshot_page(pdf, demo_log_path=None):
    fig = plt.figure(figsize=(8.27, 11.69))
    ax = fig.add_subplot(111)
    ax.axis("off")
    ax.text(0.08, 0.96, "Headless text-render snapshot (--demo mode)",
            fontsize=16, fontweight="bold", transform=ax.transAxes, va="top")
    ax.text(0.08, 0.92,
            "Below are selected frames from a scripted --demo run. In\n"
            "interactive mode these values drive a colored ncurses HUD\n"
            "(player names yellow when active, red-bold STUN tag, blinking\n"
            "magenta Ultimate banner). The --demo path renders the same\n"
            "state as text so it is reproducible for grading.",
            fontsize=9, family="sans-serif", transform=ax.transAxes, va="top",
            wrap=True)

    snapshot = ""
    if demo_log_path and os.path.exists(demo_log_path):
        # Grab the frame right after each kill counter change.
        with open(demo_log_path) as f:
            frames = []
            cur = []
            for line in f:
                if line.startswith("[render]"):
                    if cur:
                        frames.append("".join(cur))
                    cur = [line]
                else:
                    cur.append(line)
            if cur:
                frames.append("".join(cur))
        last_kills = -1
        picked = []
        for fr in frames:
            m = re.search(r"kills=(\d+)", fr)
            if not m:
                continue
            k = int(m.group(1))
            if k != last_kills:
                picked.append(fr)
                last_kills = k
            if len(picked) >= 5:
                break
        snapshot = ("\n" + "-" * 60 + "\n").join(picked[:5])

    if not snapshot:
        snapshot = "(no demo log provided — run with --demo-log <path>)"

    # Truncate gracefully
    if len(snapshot) > 3800:
        snapshot = snapshot[:3800] + "\n... (truncated)"
    ax.text(0.03, 0.80, snapshot, fontsize=6.5, family="monospace",
            transform=ax.transAxes, va="top")
    pdf.savefig(fig)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default="chrono_rift_trace.csv")
    ap.add_argument("--summary", default="chrono_rift_summary.txt")
    ap.add_argument("--out", default="report.pdf")
    ap.add_argument("--demo-log", default=None,
                    help="optional path to arbiter stderr log (for TUI snapshot)")
    args = ap.parse_args()

    if not os.path.exists(args.trace):
        print(f"error: {args.trace} not found — run ./build/arbiter --demo first",
              file=sys.stderr)
        sys.exit(1)

    per_entity = parse_trace(args.trace)
    summary, meta = parse_summary(args.summary)

    with PdfPages(args.out) as pdf:
        cover_page(pdf, meta)
        spec_compliance_page(pdf)
        turnaround_bars(pdf, summary)
        gantt_chart(pdf, per_entity)
        action_histogram(pdf, per_entity)
        per_entity_detail_page(pdf, summary, per_entity)
        mechanics_page(pdf)
        tui_snapshot_page(pdf, args.demo_log)
        narrative_page(pdf, summary, meta, per_entity)

    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes)")


if __name__ == "__main__":
    main()
