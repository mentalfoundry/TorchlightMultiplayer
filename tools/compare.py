"""Compares the state dumps two machines wrote at the same checkpoint of a test run.

    python tools/compare.py <run dir> <tag> [<tag> ...]

For every tag: the floor owner's monsters against the other machine's copies (same id -> same
type, same name (champions!), similar health and position, same dead/alive), and each player's
stand-in on the other machine against that player's own state (health, max health).
Prints PASS/FAIL lines; exit code 1 if anything failed.
"""
import json
import math
import os
import sys

POS_TOLERANCE = 4.0      # copies are steered toward the owner's positions, a little behind
HEALTH_TOLERANCE = 0.15  # fraction of max health
STATE_RADIUS = 80.0      # copies farther than this from their player get no updates (monsters.cpp)


def load(run, profile, tag):
    path = os.path.join(run, profile, f"dump_{tag}.json")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.loads(f.readline())


def main():
    run = sys.argv[1]
    tags = sys.argv[2:]
    profiles = sorted(p for p in os.listdir(run) if os.path.isdir(os.path.join(run, p)))
    failures = 0

    def report(ok, text):
        nonlocal failures
        print(("PASS " if ok else "FAIL ") + text)
        failures += 0 if ok else 1

    for tag in tags:
        dumps = {p: load(run, p, tag) for p in profiles}
        dumps = {p: d for p, d in dumps.items() if d}
        if len(dumps) < 2:
            report(False, f"[{tag}] dumps from {len(dumps)} machine(s) only")
            continue
        by_slot = {d["slot"]: (p, d) for p, d in dumps.items()}

        # Monsters: owner vs copies, per level.
        for p, d in dumps.items():
            if d["owner"] != d["slot"]:
                continue
            owner_mon = {m["id"]: m for m in d["monsters"] if not m["copy"]}
            for q, e in dumps.items():
                if q == p or e["level"] != d["level"]:
                    continue
                copies = {m["id"]: m for m in e["monsters"] if m["copy"]}
                alive = [m for m in owner_mon.values() if not m["dead"]]
                missing = [m for m in alive if m["id"] not in copies]
                report(not missing, f"[{tag}] {q} shows copies of {len(alive) - len(missing)}/{len(alive)} of {p}'s living monsters"
                       + (f" (missing e.g. {missing[0]['name']} id {missing[0]['id']})" if missing else ""))
                bad_type, bad_name, bad_health, far, dead_diff = [], [], [], [], []
                for mid, c in copies.items():
                    o = owner_mon.get(mid)
                    if not o:
                        continue
                    if c["guid"] != o["guid"]:
                        bad_type.append((o, c))
                    if c["name"] != o["name"]:
                        bad_name.append((o, c))
                    if o["dead"] != c["dead"]:
                        dead_diff.append((o, c))
                    near = "me" in e and math.hypot(o["x"] - e["me"]["x"], o["z"] - e["me"]["z"]) <= STATE_RADIUS
                    if not o["dead"]:
                        if c["max"] != o["max"] or (near and abs(c["health"] - o["health"]) > HEALTH_TOLERANCE * max(o["max"], 1)):
                            bad_health.append((o, c))
                        if math.hypot(c["x"] - o["x"], c["z"] - o["z"]) > POS_TOLERANCE:
                            far.append((o, c))
                report(not bad_type, f"[{tag}] {q}: copies have the owner's monster types ({len(bad_type)} differ)")
                report(not bad_name, f"[{tag}] {q}: copies have the owner's names (champions) ({len(bad_name)} differ)"
                       + (f" e.g. '{bad_name[0][0]['name']}' shown as '{bad_name[0][1]['name']}'" if bad_name else ""))
                report(not dead_diff, f"[{tag}] {q}: dead/alive matches ({len(dead_diff)} differ)")
                report(not bad_health, f"[{tag}] {q}: health matches ({len(bad_health)} differ)"
                       + (f" e.g. {bad_health[0][0]['name']} {bad_health[0][0]['health']:.0f} vs {bad_health[0][1]['health']:.0f}" if bad_health else ""))
                report(len(far) <= max(1, len(copies) // 10),
                       f"[{tag}] {q}: positions match ({len(far)} of {len(copies)} more than {POS_TOLERANCE} apart)")

        # Stand-ins vs the real players.
        for p, d in dumps.items():
            for a in d["avatars"]:
                real = by_slot.get(a["slot"])
                if not real or "health" not in a or "me" not in real[1]:
                    continue
                me = real[1]["me"]
                if real[1]["level"] != d["level"]:
                    continue
                ok = abs(a["health"] - me["health"]) <= HEALTH_TOLERANCE * max(me["max"], 1) and a["max"] == me["max"]
                report(ok, f"[{tag}] {p}'s stand-in for slot {a['slot']}: health {a['health']:.0f}/{a['max']} vs real {me['health']:.0f}/{me['max']}")
                if "stats" in a and "stats" in me:
                    report(a["stats"] == me["stats"],
                           f"[{tag}] {p}'s stand-in for slot {a['slot']}: level and stats {a['stats']} vs real {me['stats']}")

    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
