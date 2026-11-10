"""One generation for the Mods folder gives every character Mod what its own generation gives (D-131).

Runs the offline dry run twice on the installed EFMI Mods folder (read-only): `--plan-mods`, a
generation per character Mod as the layer has made them, and `--plan-folder`, the whole folder in one
generation as the original loads it. Per Mod the two must agree: whether it loads, its streams,
images, components and bones, its identification threshold, and the plan of every section (planned
or refused, skip, draws, the pixel slots and the files bound). A section whose hash another Mod also
declares runs both Mods' lists in one generation, as in the original; those are listed, not failed.
No GPU and no owned runtime. Writes build/folder-generation/compare.json.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODS = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Mods")
CORE = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Core\EFMI")
COMPARED = ("streams", "images", "components", "bones", "identify_components")
SECTION = ("planned", "reason", "skip", "draws", "required", "textures", "pixel_slots", "bound_files")


def run(exe: Path, mode: str, out: Path) -> dict[str, dict]:
    folder = out / mode.strip("-")
    folder.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(exe), mode, str(MODS), str(CORE), str(folder / "settings")], cwd=folder,
                            capture_output=True, text=True, errors="replace", timeout=900)
    (folder / "plans.jsonl").write_text(result.stdout, encoding="utf-8")
    if result.stderr.strip():
        (folder / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    mods = {}
    for line in result.stdout.splitlines():
        if line.startswith("{"):
            entry = json.loads(line)
            mods[entry["ini"]] = entry
    if not mods:
        raise RuntimeError("%s gave no Mod: %s" % (mode, result.stderr[-1500:]))
    return mods


def section_key(s: dict) -> tuple:
    return (s["hash"], s["count"], s.get("component"), s.get("lod"))


def main() -> int:
    dist = Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist"))
    exe = dist / "efmi_vk_character_test.exe"
    out = ROOT / "build" / "folder-generation"
    out.mkdir(parents=True, exist_ok=True)
    if not MODS.is_dir() or not exe.is_file():
        print("folder generation: the installed Mods folder or %s is missing" % exe)
        return 1
    own = run(exe, "--plan-mods", out)
    folder = run(exe, "--plan-folder", out)
    hashes = Counter()
    for entry in own.values():
        for h in {s["hash"] for s in entry.get("sections", [])}:
            hashes[h] += 1
    shared = {h for h, n in hashes.items() if n > 1}
    differences, shared_differences, not_compared = [], [], []
    for ini, a in own.items():
        b = folder.get(ini)
        if b is None:
            differences.append("%s: not in the folder generation" % ini)
            continue
        if a["loaded"] != b["loaded"]:
            differences.append("%s: loads %s alone, %s in the folder (%s)" % (ini, a["loaded"], b["loaded"], b.get("reason", "")))
            continue
        if not a["loaded"]:
            continue
        # A generation of one Mod does not load a library that acts on the game's resources by hash (it
        # would act once per generation, D-112); the folder generation loads it, as the original does. Such
        # a Mod's two runs are two different inputs: listed, not compared (the folder's is the one that plays).
        unloaded = [n for n in a.get("library_notes", []) if "acts on game resources by hash" in n]
        if unloaded:
            not_compared.append("%s: %s" % (ini, unloaded[0]))
            continue
        for k in COMPARED:
            if a.get(k) != b.get(k):
                differences.append("%s: %s %s alone, %s in the folder" % (ini, k, a.get(k), b.get(k)))
        if sorted(a.get("image_files", [])) != sorted(b.get("image_files", [])):
            differences.append("%s: image files differ" % ini)
        sections = {section_key(s): s for s in b.get("sections", [])}
        for s in a.get("sections", []):
            t = sections.get(section_key(s))
            if t is None:
                differences.append("%s %s: section missing in the folder" % (ini, s["hash"]))
                continue
            changed = [k for k in SECTION if s.get(k) != t.get(k)]
            if changed:
                text = "%s %s c%s lod%s: %s" % (ini, s["hash"], s.get("component"), s.get("lod"),
                                                 ", ".join("%s %s -> %s" % (k, s.get(k), t.get(k)) for k in changed))
                (shared_differences if s["hash"] in shared else differences).append(text)
    report = {"scope": "offline dry run, no GPU", "mods": len(own), "loaded_alone": sum(e["loaded"] for e in own.values()),
              "loaded_in_folder": sum(e["loaded"] for e in folder.values()), "shared_hashes": sorted(shared),
              "differences": differences, "shared_hash_differences": shared_differences}
    (out / "compare.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print("folder generation: %d character Mods, %d load alone, %d in the folder; %d sections on hashes two Mods declare"
          % (report["mods"], report["loaded_alone"], report["loaded_in_folder"], len(shared)))
    for text in not_compared:
        print("  not compared:", text.encode("ascii", "backslashreplace").decode())
    for text in shared_differences:
        print("  shared hash:", text.encode("ascii", "backslashreplace").decode())
    for text in differences:
        print("DIFFERENCE:", text.encode("ascii", "backslashreplace").decode())
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
