"""The original as the judge of a replaced draw's lists (docs/architecture-review-2.1.md, P3-1).

check_dx11_judge.py compares what [Present] lists leave; no draw runs there, so the path every character
draw takes had no comparison with the original. Here the judge's program is the "game"
(sandbox/dx11_judge_scene.h): each frame it draws two indexed quads of known index bytes, and the pinned
XXMI 3DMigoto runs the case's [TextureOverride] sections matched by the draws' index region hashes, through
a ShaderRegex that checks `ib` at every draw as the EFMI Core's does. What the lists did is read off the
picture (eight points) and the input assembler (the primitives each game draw put through).

The layer's side is the frontend alone (`runtime_test.exe --judge-scene`): the case in a CPU generation, as
the character renderer hosts a Mod's lists, the two game draws given by their hashes, and the plan of each
(bindings, draws, skip). From the plans this script works out the picture and the primitives the plan
means, by D3D11's rules for the scene's few files, and compares with what the original drew; the
generation's variables are compared with those the original saved.

A difference is a place where a list the original runs is planned otherwise, or refused. What this does
not cover: what the layer's renderer then makes of a plan (CharacterProgram::plan and the backend), and
lists that call the EFMI Core.

A line the layer cannot honour is degraded (docs/architecture-review-2.1.md Phase 1, D-161 to D-163): the
command does nothing or the game's binding stays, a notice names it, and the other lines and draws go on.
Here that reads: the plan is not refused, the layer said a notice, and the picture is what the record says
(the original's, where the scene's shaders do not read what the line changed).

With --sweep (a step of its own): "no single line makes a Mod vanish". One working replaced mesh (the
list of the `replace` case) and, one at a time, the lines of sandbox/judge_draw_sweep.json put in front of
its draw: bindings at every kind of slot, copies, queries in conditions, the draw forms, calls, layout
edits. Each is run under the original and planned by the layer, and the pictures compared. The file
records which lines are known to differ, each with why; the step fails when a line that agreed differs
(a regression) and when a line recorded as differing agrees (a fix: the record is to be changed). Its
summary is the count the review asks for: how many ways of writing a line the plan path still gets wrong.

Writes build/dx11-judge-draws/.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from efmi_vk.hashes import crc32c

import check_dx11_judge as judge

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "dx11-judge-draws"
CASES = ROOT / "sandbox" / "judge_draw_cases"

GAME_INDICES = ([0, 1, 2, 2, 1, 3], [0, 1, 2, 1, 3, 2])
HASHES = [crc32c(struct.pack("<6H", *indices)) for indices in GAME_INDICES]
GAME_TEXTURE = ((64, 64, 64, 255), (128, 128, 128, 255))
CLEAR = (0, 0, 0, 255)
NO_TEXTURE = (0, 0, 0, 0)
PLACES = "ABCD"
COLOURS = {"red.dds": (255, 0, 0, 255), "green.dds": (0, 255, 0, 255), "blue.dds": (0, 0, 255, 255)}
# Textures of other shapes for the property queries (the `properties` case): eight by four with its four mip
# levels, and one BC7 sRGB block behind a DX10 header.
SHAPED = {"wide.dds": (255, 255, 0, 255), "block.dds": (0, 0, 0, 255)}
INDEX_FILES = {"indices_first.buf": GAME_INDICES[0], "indices_second.buf": GAME_INDICES[1]}

# Cases where the plan is known to differ from what the original draws, each with the finding it shows
# (docs/architecture-review-2.1.md). They are run and must still differ: one that agrees is a fix, to be
# taken off this list. They do not fail the check; every other case does.
KNOWN: dict[str, str] = {}
# Cases with a line the layer degrades: the picture and the variables must be the original's (the scene's
# shaders do not read what the line changed) and the layer must have said a notice with this text.
DEGRADED = {
    "other_stage": "vs-cb1",
}

D3DX = judge.D3DX.replace("[Rendering]\n", "[Rendering]\ntrack_region_hashes = 1\n") + (
    "[ShaderRegexJudgeTextureOverrides]\nshader_model = vs_4_0 vs_4_1 vs_5_0 vs_5_1\n"
    "if DRAW_TYPE % 2 == 0\n    CheckTextureOverride = ib\nendif\n")


def quad(place: int) -> bytes:
    """The four vertices of a place as the scene makes them: left top, right top, left bottom, right bottom."""
    left, top = (-1.0 if place % 2 == 0 else 0.0), (1.0 if place < 2 else 0.0)
    return struct.pack("<8f", left, top, left + 1, top, left, top - 1, left + 1, top - 1)


def dds(colour: tuple[int, int, int, int], width: int = 4, height: int = 4, mips: int = 1) -> bytes:
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8 | (0x20000 if mips > 1 else 0)
    header = struct.pack("<4s7I44x", b"DDS ", 124, flags, height, width, width * 4, 0, mips)
    pixel_format = struct.pack("<2I4s5I", 32, 0x1 | 0x40, bytes(4), 32, 0xff, 0xff00, 0xff0000, 0xff000000)
    texels = sum(max(1, width >> level) * max(1, height >> level) for level in range(mips))
    return header + pixel_format + struct.pack("<5I", 0x1000 | (0x8 | 0x400000 if mips > 1 else 0), 0, 0, 0, 0) + bytes(colour) * texels


def dds_block() -> bytes:
    """A 4 x 4 BC7 sRGB texture: one block, the DX10 header naming DXGI_FORMAT_BC7_UNORM_SRGB (99)."""
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, 4, 4, 16, 0, 1)
    pixel_format = struct.pack("<2I4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    return header + pixel_format + struct.pack("<5I", 0x1000, 0, 0, 0, 0) + struct.pack("<5I", 99, 3, 0, 1, 0) + bytes(16)


def files() -> dict[str, bytes]:
    out = {"place_%s.buf" % PLACES[i].lower(): quad(i) for i in range(4)}
    out.update({name: struct.pack("<6H", *indices) for name, indices in INDEX_FILES.items()})
    out.update({name: dds(colour) for name, colour in COLOURS.items()})
    out["wide.dds"] = dds(SHAPED["wide.dds"], 8, 4, 4)
    out["block.dds"] = dds_block()
    return out


def stage(case: Path, mods: Path) -> Path:
    shutil.rmtree(mods, ignore_errors=True)
    mods.mkdir(parents=True)
    text = case.read_text(encoding="ascii").replace("HASH_A", "%08x" % HASHES[0]).replace("HASH_B", "%08x" % HASHES[1])
    (mods / case.name).write_text(text, encoding="ascii")
    for name, data in files().items():
        (mods / name).write_bytes(data)
    return mods / case.name


def run_original(exe: Path, folder: Path, case: Path, frames: int) -> tuple[dict, dict[str, float]]:
    stage(case, folder / "Mods")
    (folder / "d3dx.ini").write_text(D3DX, encoding="utf-8")
    user = folder / "d3dx_user.ini"
    user.unlink(missing_ok=True)
    run = subprocess.run([str(exe), str(folder), str(frames), "scene"], cwd=folder, capture_output=True, text=True, timeout=300)
    scene = next((json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")), None)
    if run.returncode or scene is None:
        raise RuntimeError("the original's program failed: " + (run.stdout + run.stderr)[-800:])
    return scene, judge.persisted(user.read_text(encoding="utf-8", errors="replace")) if user.is_file() else {}


def run_layer(case: Path, frames: int) -> dict:
    # EFMI_VK_JUDGE_RUNTIME: another build's runtime_test.exe, to see a record fail on the build before a change.
    exe = Path(os.environ.get("EFMI_VK_JUDGE_RUNTIME", ROOT / "build" / "xxmi-runtime" / "runtime_test.exe"))
    if not exe.is_file():
        raise RuntimeError("build the INI runtime test first (scripts/build_xxmi_runtime.py)")
    work = OUT / "layer" / case.stem
    ini = stage(case, work / "Mods")
    # The installation's d3dx.ini beside the Mods folder, as the original reads it and the layer takes its own
    # sections (BUG-090: its patternless ShaderRegex over vertex shaders is what `vs` reads).
    (work / "d3dx.ini").write_text(D3DX, encoding="utf-8")
    run = subprocess.run([str(exe), "--judge-scene", str(ini), str(frames), "%08x" % HASHES[0], "%08x" % HASHES[1]], cwd=work,
                         capture_output=True, text=True, errors="replace", timeout=300)
    result = next((json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")), None)
    if run.returncode or result is None:
        raise RuntimeError("the layer's frontend failed: " + (run.stdout + run.stderr)[-800:])
    return result


def predict(draws: list[dict]) -> tuple[dict[str, tuple], list[int]]:
    """The picture and the primitives the plans mean, by D3D11's rules: an index past its buffer or without
    one reads 0, a vertex of a null buffer reads zeros (a triangle without area), a slot without a texture
    reads zero, later draws paint over earlier ones (no depth test)."""
    pixels = {place + point: CLEAR for place in PLACES for point in "12"}
    primitives = []
    for i, draw in enumerate(draws):
        state = {"place": PLACES[i], "indices": GAME_INDICES[i], "texture": GAME_TEXTURE[i]}
        count = 0

        def issue(indexed: bool, n: int, first: int, base: int, instances: int) -> None:
            nonlocal count
            count += (n // 3) * instances
            for k in range(n // 3):
                at = [first + 3 * k + j for j in range(3)]
                if indexed:
                    indices = state["indices"]
                    at = [(indices[a] if indices is not None and a < len(indices) else 0) + base for a in at]
                if state["place"] is None:
                    continue
                if set(at) == {0, 1, 2}:
                    pixels[state["place"] + "1"] = state["texture"]
                elif set(at) == {1, 2, 3}:
                    pixels[state["place"] + "2"] = state["texture"]

        def apply(item: dict) -> None:
            op, name = item["op"], item.get("file")
            # `application` on a buffer binding: the game's own buffer back in its place.
            if op == "bind_vertex":
                if item["slot"] == 0:
                    state["place"] = PLACES[i] if item.get("application", -1) >= 0 else None if name is None else name[len("place_")].upper()
            elif op == "bind_index":
                state["indices"] = GAME_INDICES[i] if item.get("application", -1) >= 0 else None if name is None else INDEX_FILES[name]
            elif op == "bind_pixel_resource":
                if item["slot"] == 0:
                    state["texture"] = GAME_TEXTURE[i] if item.get("application", -1) == 0 else NO_TEXTURE if name is None else COLOURS.get(name) or SHAPED[name]
            elif op == "draw_indexed":
                n = item["count"]
                if item.get("automatic"):
                    n = (len(state["indices"]) if state["indices"] is not None else 0) if item["indexed"] else (4 if state["place"] else 0)
                issue(item["indexed"], n, 0 if item.get("automatic") else item["first"], 0 if item.get("automatic") else item["base"], item["instances"])
            else:
                raise RuntimeError("a plan item the scene has no rule for: %s" % op)

        if draw["refused"]:
            # The layer keeps the game's draw as the game made it.
            issue(True, 6, 0, 0, 1)
        else:
            items = draw["items"]
            before = len(items) if draw["pre_items"] < 0 else min(draw["pre_items"], len(items))
            for item in items[:before]:
                apply(item)
            if not draw["skip"]:
                issue(True, 6, 0, 0, 1)
            for item in items[before:]:
                apply(item)
        primitives.append(count)
    return pixels, primitives


SWEEP = ROOT / "sandbox" / "judge_draw_sweep.json"
SWEEP_INI = """namespace = judge
[Constants]
global persist $mark = 0
global $x = 0
[ResourcePlaceC]
type = Buffer
stride = 8
filename = place_c.buf
[ResourcePlaceD]
type = Buffer
stride = 8
filename = place_d.buf
[ResourceIndexFirst]
type = Buffer
format = DXGI_FORMAT_R16_UINT
filename = indices_first.buf
[ResourceIndexSecond]
type = Buffer
format = DXGI_FORMAT_R16_UINT
filename = indices_second.buf
[ResourceRed]
filename = red.dds
[ResourceGreen]
filename = green.dds
[ResourceBlue]
filename = blue.dds
[ResourceConstants]
type = Buffer
format = R32_FLOAT
data = 1 2 3 4
[ResourceWritable]
type = RWBuffer
format = R32_FLOAT
data = 5 6 7 8
[ResourceSaved]
[CommandListCounted]
$x = $x + 1
[TextureOverrideFirst]
hash = HASH_A
match_index_count = 6
handling = skip
vb0 = ResourcePlaceC
ib = ResourceIndexFirst
ps-t0 = ResourceRed
%s
drawindexed = 6, 0, 0
[Present]
$mark = 1
"""


def sweep(exe: Path, folder: Path, frames: int, only: str | None, report_only: bool) -> int:
    entries = json.loads(SWEEP.read_text(encoding="ascii"))["lines"]
    work = OUT / "sweep"
    work.mkdir(parents=True, exist_ok=True)
    failed = same = degraded = differs = unjudged = 0
    results = []
    for entry in entries:
        name = entry["name"]
        if only and name != only:
            continue
        case = work / (name + ".ini")
        case.write_text(SWEEP_INI % "\n".join(entry["lines"]), encoding="ascii")
        problems = []
        layer: dict = {}
        try:
            scene, _ = run_original(exe, folder, case, frames)
            layer = run_layer(case, 2)
            pixels, primitives = predict(layer["draws"])
            refused = [d["refused"] for d in layer["draws"] if d["refused"]]
            drawn = {k: tuple(v) for k, v in scene["pixels"].items()}
            wrong = {k: (drawn[k], pixels[k]) for k in sorted(drawn) if drawn[k] != pixels[k]}
            if refused:
                problems.append("refused: " + refused[0])
            if wrong:
                problems.append("points (original, plan): %s" % wrong)
            if scene["primitives"] != primitives:
                problems.append("primitives: original %s, plan %s" % (scene["primitives"], primitives))
        except (RuntimeError, KeyError) as e:
            problems.append("not run: %s" % e)
        # Not refused, the picture as the original's, and the layer said what it did not do: degraded.
        gaps = layer.get("gaps", [])
        actual = "differs" if problems else "degraded" if gaps else "same"
        results.append({"name": name, "lines": entry["lines"], "actual": actual, "problems": problems, "gaps": gaps})
        scene_limit = actual == "differs" and entry.get("class") == "scene"
        same += actual == "same"; degraded += actual == "degraded"; differs += actual == "differs" and not scene_limit; unjudged += scene_limit
        expected = entry.get("expect", "same")
        # A recorded difference of the layer states what may differ (`only`) and never a refusal: the line
        # is degraded for its command or its draw alone, with a notice.
        if actual == "differs" and expected == "differs" and entry.get("class") == "layer" and not report_only:
            allowed = entry.get("only", "")
            beyond = [p for p in problems if not any(p.startswith(kind) for kind in allowed.split(" and "))]
            if beyond or not gaps:
                failed += 1
                print("FAIL %s: recorded to differ in its %s only, with a notice: %s%s" %
                      (name, allowed or "(nothing stated)", "; ".join(beyond)[:300], "" if gaps else "; the layer said no notice"))
                continue
        if report_only:
            print("%-8s %s%s" % (actual, name, ": " + problems[0][:200] if problems else ": " + gaps[0][:200] if gaps else ""))
        elif actual != expected:
            failed += 1
            print("FAIL %s: recorded as %s, now %s%s" % (name, expected, actual, ": " + "; ".join(problems)[:400] if problems else
                                                         " (a fix: change the record in sandbox/judge_draw_sweep.json)"))
        elif problems:
            print("known %s: %s" % (name, entry.get("finding", problems[0])[:200]))
        elif gaps:
            print("degraded %s: %s" % (name, entry.get("finding", gaps[0])[:200]))
    (OUT / "sweep.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
    (folder / "d3dx.ini").write_text(judge.D3DX, encoding="utf-8")
    print("dx11 judge, one line at a time: %d lines, %d as the original's, %d degraded with a notice (the picture as the original's), "
          "%d differ in the layer, %d not judged by this scene; %s" %
          (same + degraded + differs + unjudged, same, degraded, differs, unjudged, "FAILED" if failed else "as recorded"))
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--only", help="a case name")
    ap.add_argument("--sweep", action="store_true", help="the lines of sandbox/judge_draw_sweep.json, one at a time")
    ap.add_argument("--report", action="store_true", help="with --sweep: print what each line does now, without comparing with the record")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    exe = judge.build_host()
    folder = judge.prepare_original()
    shutil.copy(exe, folder / exe.name)
    if args.sweep:
        return sweep(folder / exe.name, folder, 8 if args.frames == 120 else args.frames, args.only, args.report)
    failed = 0
    for case in sorted(CASES.glob("*.ini")):
        if args.only and case.stem != args.only:
            continue
        try:
            scene, saved = run_original(folder / exe.name, folder, case, args.frames)
            layer = run_layer(case, args.frames)
            pixels, primitives = predict(layer["draws"])
        except (RuntimeError, KeyError) as e:
            print("FAIL %s: %s" % (case.stem, e)); failed += 1; continue
        problems = []
        refused = [d["refused"] for d in layer["draws"] if d["refused"]]
        if refused:
            problems.append("the layer refuses a list the original runs: %s" % refused[0])
        drawn = {name: tuple(value) for name, value in scene["pixels"].items()}
        wrong = {name: (drawn[name], pixels[name]) for name in sorted(drawn) if drawn[name] != pixels[name]}
        if wrong:
            problems.append("points (original, plan): %s" % wrong)
        if scene["primitives"] != primitives:
            problems.append("primitives per game draw: original %s, plan %s" % (scene["primitives"], primitives))
        values = {k.lower(): v for k, v in layer["variables"].items()}
        differ = {k: (v, values.get(k)) for k, v in saved.items() if values.get(k) is None or abs(values[k] - v) > 1e-4 * max(1, abs(v))}
        if differ:
            problems.append("variables (original, layer): %s" % differ)
        if not saved:
            problems.append("the original saved no variable")
        (OUT / (case.stem + ".json")).write_text(json.dumps({"original": scene, "saved": saved, "layer": layer}, indent=1), encoding="utf-8")
        if case.stem in DEGRADED and not any(DEGRADED[case.stem] in gap for gap in layer.get("gaps", [])):
            problems.append("the layer degrades a line of this case and said no notice naming %s (%s)" % (DEGRADED[case.stem], layer.get("gaps", [])))
        if case.stem in KNOWN:
            if problems:
                print("known %s: %s (%s)" % (case.stem, KNOWN[case.stem], problems[0][:160]))
            else:
                failed += 1
                print("FAIL %s: a known difference no longer differs; take it off KNOWN" % case.stem)
        elif problems:
            failed += 1
            print("FAIL %s: %s" % (case.stem, "; ".join(problems)))
        else:
            print("ok   %s: 8 points, %s primitives, %d values as the original's" % (case.stem, primitives, len(saved)))
    # The Present cases' d3dx.ini back in the shared folder.
    (folder / "d3dx.ini").write_text(judge.D3DX, encoding="utf-8")
    print("dx11 judge of replaced draws: %s" % ("FAILED" if failed else "passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
