"""Every installed Mod in every state its keys reach, planned as the layer plans its draws (D-137).

Runs `--plan-folder-states` on the installed EFMI Mods folder (read-only), one generation for the
folder as the original loads it: the default state, then each key action's keys (as the original
parsed them) held for a presentation and released, up to eight presses or until the variables repeat a
state; after each, every Mod's sections are planned. The original's own dispatch decides what a key
changes (cycle, toggle, hold, conditions). A section the layer refuses in any state is a place where a
player sees the game's model or a broken one: the check fails and lists it with the first state it
appeared in. The offline stand-ins' own limits (no game pipeline) are not counted.

Plan digests (docs/architecture-review-2.1.md, P3-3): the run gives every Mod one number over all its
plans in all states. For the installed folder the number is compared with the one accepted before for
the same files (the Mod's own, and the Core's and the folder's shared INIs): a change of the layer that
changes any plan of an installed Mod fails here and names the Mod, until it is accepted with --accept.
A Mod whose files changed, or a new one, is accepted as it is. The accepted numbers are this machine's
(build/plan-digests/accepted.json, not in the repository: they describe the user's Mods).
No GPU. Writes build/mod-states/states.jsonl.
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODS = Path(os.environ.get("EFMI_VK_CHECK_MODS", r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Mods"))
CORE = Path(os.environ.get("EFMI_VK_CHECK_CORE", r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Core\EFMI"))


ACCEPTED = ROOT / "build" / "plan-digests" / "accepted.json"


def files_signature(folders: list[Path], whole: bool) -> str:
    """The files plans depend on: every INI by its bytes; with `whole` every other file by name and size
    (a buffer's size decides counts and bounds, its bytes do not reach a plan)."""
    h = hashlib.sha1()
    for folder in folders:
        if folder.is_file():  # an INI of its own in the Mods folder
            h.update(folder.read_bytes())
            continue
        for path in sorted(p for p in folder.rglob("*") if p.is_file()):
            name = path.relative_to(folder).as_posix().lower()
            if path.suffix.lower() == ".ini":
                h.update(name.encode("utf-8", "replace") + b"\0" + path.read_bytes() + b"\0")
            elif whole:
                h.update(("%s\0%d\0" % (name, path.stat().st_size)).encode("utf-8", "replace"))
    return h.hexdigest()


def compare_digests(mods: list[dict], accept: bool) -> bool:
    """True when a Mod's plans changed while its files did not."""
    accepted = json.loads(ACCEPTED.read_text(encoding="utf-8")) if ACCEPTED.is_file() else {}
    roots = sorted({Path(m["ini"]).parts[0] for m in mods})
    # What every Mod's plans share: the Core's lists and the folder's INIs outside the character Mods.
    shared = hashlib.sha1()
    shared.update(files_signature([CORE], False).encode())
    for path in sorted(p for p in MODS.rglob("*.ini") if p.relative_to(MODS).parts[0] not in roots):
        if any(part.lower().startswith("disabled") for part in path.relative_to(MODS).parts):
            continue
        shared.update(path.relative_to(MODS).as_posix().lower().encode("utf-8", "replace") + b"\0" + path.read_bytes())
    shared_signature = shared.hexdigest()
    same = new = changed = 0
    for mod in mods:
        if not mod.get("loaded") or "digest" not in mod:
            continue
        key = mod["ini"]
        inputs = files_signature([MODS / Path(key).parts[0]], True) + ":" + shared_signature
        before = accepted.get(key)
        if before and before["inputs"] == inputs and before["digest"] == mod["digest"]:
            same += 1
            continue
        if before and before["inputs"] == inputs and not accept:
            changed += 1
            print("  PLANS CHANGED: %s: its files are as accepted, its plans are not (%s, accepted %s)" % (key, mod["digest"], before["digest"]))
            continue
        new += 1
        accepted[key] = {"inputs": inputs, "digest": mod["digest"]}
    ACCEPTED.parent.mkdir(parents=True, exist_ok=True)
    ACCEPTED.write_text(json.dumps(accepted, indent=1, sort_keys=True), encoding="utf-8")
    print("plan digests: %d as accepted, %d accepted now (new, changed files, or --accept), %d changed%s" %
          (same, new, changed, "; review the change, then run with --accept" if changed else ""))
    return changed > 0


def main() -> int:
    dist = Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist"))
    exe = dist / "efmi_vk_character_test.exe"
    out = ROOT / "build" / "mod-states"
    out.mkdir(parents=True, exist_ok=True)
    if not MODS.is_dir() or not exe.is_file():
        print("mod states: the installed Mods folder or %s is missing" % exe)
        return 1
    result = subprocess.run([str(exe), "--plan-folder-states", str(MODS), str(CORE), str(out / "settings")], cwd=out,
                            capture_output=True, text=True, errors="replace", timeout=1800)
    (out / "states.jsonl").write_text(result.stdout, encoding="utf-8")
    if result.stderr.strip():
        (out / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    if result.returncode or not lines:
        print("mod states: the dry run failed: %s" % result.stderr[-1500:])
        return 1
    summary, mods = lines[0], lines[1:]
    print("mod states: %d key actions, %d states, %d Mods, %d of them drawing in the default state" %
          (summary["key_actions"], summary["states"], len(mods), sum(1 for m in mods if m.get("drawn"))))
    for item in summary.get("left_inputs", []):
        print("  INPUT LEFT OUT: %s: %s" % (item["ini"], item["reason"]))
    for error in summary.get("present_errors", []):
        print("  PRESENT REFUSED: %s" % error)
    failed = bool(summary.get("present_errors"))
    reasons = Counter()
    for mod in mods:
        refusals = mod.get("refusals", [])
        if not mod.get("loaded"):
            print("  NOT LOADED: %s: %s" % (mod["ini"], mod.get("reason", "")))
            failed = True
            continue
        # What the original does for a section and the layer does not: shown, as the layer shows it.
        for gap in mod.get("gaps", []):
            print("  GAP %s: %s" % (mod["ini"], gap))
        # With EFMI_VK_CHECK_DRAWS a Mod has to draw something of its own in the default state: a plan that
        # is made and draws nothing passes every refusal check (a Mod that never enables itself).
        if os.environ.get("EFMI_VK_CHECK_DRAWS") and not mod.get("drawn"):
            print("  NO DRAW: %s: no section draws the Mod in the default state" % mod["ini"])
            failed = True
        if not refusals:
            continue
        failed = True
        print("  %s: %d sections refused in some state (%d plans)" % (mod["ini"], len(refusals), mod.get("plans", 0)))
        for r in refusals[:12]:
            where = "%s c%s lod%s" % (r["hash"], r.get("component", "-"), r.get("lod", "-"))
            print("    %s in %s: %s" % (where, r["state"], r["reason"]))
            reasons[r["reason"]] += 1
        if len(refusals) > 12:
            print("    ... %d more" % (len(refusals) - 12))
            for r in refusals[12:]:
                reasons[r["reason"]] += 1
    if reasons:
        print("refusal reasons:")
        for reason, n in reasons.most_common():
            print("  %4d  %s" % (n, reason))
    # Only for the installed folder: the checks that plan a folder of their own have nothing accepted.
    if "EFMI_VK_CHECK_MODS" not in os.environ and compare_digests(mods, "--accept" in sys.argv):
        failed = True
    print("mod states: %s" % ("FAILED" if failed else "every section plans in every state"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
