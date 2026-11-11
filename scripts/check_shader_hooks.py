"""Check EFMITTL's hook rules on Vulkan (D-113, D-171) against the paired catalogue (ground truth, D-075).

The library recognises the game's shaders by DX11 patterns (its ShaderRegex groups). The layer reads the
same facts off SPIR-V (layer/src/shader_hooks.cpp). In the paired catalogue
(captures/catalogue-20261019: charNN DX11, vkNN Vulkan of the same scenes), a character draw of one
API pairs with the other's by index region hash, colour target count and order.

- Every Vulkan vertex shader that pairs with the library's DX11 lit vertex shader must be recognised as
  lit, and none that pairs only with other DX11 vertex shaders; the copy rule must name the game's copy
  shader and no shader of a character draw (D-113).
- With the installed library (D-171): its groups' own patterns are run on the DX11 listings of the
  catalogue's shaders, as XXMI runs them. Every Vulkan vertex shader must be given the vertex group its
  DX11 partners match (lit, lit skin, outline, or none), and every Vulkan fragment shader the pixel groups
  its DX11 partner matches (ClipFirst, the DiffuseAt of a slot).

Read-only on the catalogue, the installed library and the sibling project's disassembler.

    python scripts/check_shader_hooks.py [--catalogue DIR] [--dist DIR] [--library INI]
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
LIT_DX11 = "1479b2b594b9c91a"  # the library's lit vertex shader on game 1.6.3 (translucency.ini)
SIBLING = r"F:\EFMI MCP\server"  # the DX11 disassembler (read-only)
LIBRARY = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Mods\EFMITTL\translucency.ini")
VERTEX_GROUPS = {"EFMITTLLitVS": 1, "EFMITTLLitVSSkin": 2, "EFMITTLOutlineVS": 3}
# Fragment pairs where the layer's pixel facts and the DX11 partner's groups differ, (Vulkan module, DX11 shader):
# why. Both are draws whose vertex shader is of no library group, where the pixel groups' own conditions
# ($vs_kind 1 or 4) do nothing. The check fails when one of them no longer differs.
KNOWN_PIXEL = {
    ("7691207e", "524a724a453fd672"): "an effect pass of one colour target that the catalogue pairs by index region with four "
                                      "different DX11 pixel shaders; this one samples another texture at another input",
    ("df360e42", "857960e2e308ca7f"): "a G-buffer pixel shader: FXC lists the normal map's two-channel sample ahead of the "
                                      "diffuse's, where the SPIR-V build keeps the source order and nothing between them "
                                      "(no discard) tells why",
}


def colours(pipe: dict) -> int:
    recipe = pipe.get("recipe") or {}
    subpasses = (recipe.get("render_pass") or {}).get("subpasses") or []
    sub = subpasses[recipe.get("subpass", 0)] if subpasses else {}
    return len(sub.get("colors", []))


def library_groups(ini: Path) -> dict[str, tuple[str, "re.Pattern"]]:
    """The library's groups that have a pattern: name -> (shader model, pattern as Python reads it)."""
    models, patterns, section = {}, {}, None
    for line in ini.read_text(encoding="utf-8").splitlines():
        header = re.match(r"\[(.+)\]\s*$", line)
        if header:
            section = header.group(1)
            continue
        if not section or not section.startswith("ShaderRegex") or not line.strip() or line.lstrip().startswith(";"):
            continue
        name = section[len("ShaderRegex"):]
        if name.endswith(".Pattern"):
            patterns[name[:-len(".Pattern")]] = patterns.get(name[:-len(".Pattern")], "") + line
        elif "." not in name and line.startswith("shader_model"):
            models[name] = line.split("=", 1)[1].strip()
    # PCRE2 as XXMI compiles it (multiline): \h is a horizontal blank, (?<name> a named group.
    return {name: (models.get(name, ""), re.compile(text.replace(r"\h", r"[ \t]").replace("(?<", "(?P<"), re.M))
            for name, text in patterns.items()}


def listing(lister, path: Path) -> str:
    """The shader's listing as the patterns meet it: from the shader model line on, no comment header."""
    text = lister.disassemble(path.read_bytes())
    return "\n".join(line for line in text.splitlines() if line.strip() and not line.startswith("//")) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalogue", type=Path, default=Path("F:/efmi vulkan/captures/catalogue-20261019"))
    ap.add_argument("--dist", type=Path, default=Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist")))
    ap.add_argument("--library", type=Path, default=LIBRARY)
    args = ap.parse_args()
    paired = collections.defaultdict(set)   # Vulkan VS hash -> DX11 VS hashes it pairs with
    paired_ps = collections.defaultdict(set)  # Vulkan FS hash -> DX11 PS hashes of the same draws
    files = {}                              # Vulkan shader hash -> SPIR-V file
    dxbc = {}                               # DX11 shader hash -> DXBC file
    character_ps, fullscreen_ps = set(), set()
    for n in range(1, 17):
        dx_file, vk_dir = args.catalogue / "dx11" / ("char%02d.json" % n), args.catalogue / "vulkan" / ("vk%02d" % n)
        if not dx_file.is_file() or not vk_dir.is_dir():
            continue
        for f in (args.catalogue / "dx11" / ("char%02d.dxbc" % n)).glob("*.dxbc"):
            dxbc.setdefault(f.stem, f)
        dx = json.loads(dx_file.read_text())
        shaders = {s["id"]: s["hash"] for s in dx["shaders"]}
        regions = dx.get("regions", [])
        dx_seq = collections.defaultdict(list)
        for d in dx["draws"]:
            ib = d.get("ib")
            if not ib or len(ib) < 4 or ib[3] is None or not 0 <= ib[3] < len(regions) or d["sh"][0] < 0:
                continue
            rts = len([o for o in d.get("out", []) if o[0] == 10])
            dx_seq[(regions[ib[3]][3], rts)].append((shaders.get(d["sh"][0], ""), shaders.get(d["sh"][4], "") if d["sh"][4] >= 0 else ""))
        frame = json.loads((vk_dir / "frame.json").read_text())
        pipes = {p["id"]: p for p in json.loads((vk_dir / "pipelines.json").read_text())}
        for s in json.loads((vk_dir / "shaders.json").read_text()):
            if s.get("file"):
                files.setdefault(s["hash"], vk_dir / s["file"])
        vk_seq = collections.defaultdict(list)
        for d in frame["draws"]:
            p = pipes.get(d["pipeline"], {})
            stages = {s["stage"]: s["hash"] for s in p.get("shaders", [])}
            if d["kind"] == "draw" and 16 in stages:
                fullscreen_ps.add(stages[16])
            if d["kind"] != "indexed" or not (d.get("ib") or {}).get("hash") or 1 not in stages:
                continue
            vk_seq[(d["ib"]["hash"], colours(p))].append((stages[1], stages.get(16, "")))
            if 16 in stages:
                character_ps.add(stages[16])
        for key, vk in vk_seq.items():
            dxs = dx_seq.get(key, [])
            if len(dxs) == len(vk):
                for a, b in zip(vk, dxs):
                    paired[a[0]].add(b[0])
                    if a[1] and b[1]:
                        paired_ps[a[1]].add(b[1])
    names = sorted(set(paired) | set(paired_ps) | character_ps | fullscreen_ps)
    names = [h for h in names if h in files]
    tool = args.dist / "efmi_vk_character_test.exe"
    facts = {}
    for chunk in range(0, len(names), 64):
        part = names[chunk:chunk + 64]
        out = subprocess.run([str(tool), "--shader-hooks"] + [str(files[h]) for h in part], capture_output=True, text=True, check=True).stdout
        for h, line in zip(part, out.splitlines()):
            _, lit, copy, vertex, known, slot, narrow = line.rsplit(" ", 6)
            facts[h] = (lit == "1", copy == "1", int(vertex), known == "1", int(slot), narrow == "1")
    problems = []
    lit_ok = [h for h in paired if LIT_DX11 in paired[h]]
    for h in sorted(paired):
        if h not in facts:
            continue
        expect = LIT_DX11 in paired[h]
        if facts[h][0] != expect:
            problems.append("vertex %s pairs with %s: lit %s, expected %s" % (h, sorted(paired[h]), facts[h][0], expect))
    copies = sorted(h for h in names if facts.get(h, (0, 0))[1])
    for h in copies:
        if h in character_ps:
            problems.append("fragment %s of a character draw is taken for the copy shader" % h)
    if not copies:
        problems.append("no copy shader recognised among the full-screen passes")
    print("shader hooks: %d paired vertex shaders (%d with the lit DX11 one), recognised lit %s; copy shaders %s" % (
        len(paired), len(lit_ok), sorted(h for h in paired if facts.get(h, (0, 0))[0]), copies))

    # D-171: the installed library's groups, by their own patterns on the DX11 listings.
    if not args.library.is_file():
        print("shader hooks: no installed library at %s; its other groups are not checked" % args.library)
    else:
        sys.path.insert(0, SIBLING)
        from efmi_mcp import shader as lister  # noqa: E402
        groups = library_groups(args.library)
        listings = {}

        def matched(h: str, model: str) -> set[str] | None:
            if h not in dxbc:
                return None
            if h not in listings:
                listings[h] = listing(lister, dxbc[h])
            return {name for name, (shader_model, pattern) in groups.items() if shader_model.startswith(model) and pattern.search(listings[h])}

        counts = collections.Counter()
        for h in sorted(paired):
            if h not in facts:
                continue
            expected = set()
            for partner in paired[h]:
                found = matched(partner, "vs")
                if found is None:
                    expected = None
                    break
                expected.add(max((VERTEX_GROUPS.get(name, 0) for name in found), default=0))
            if expected is None:
                counts["vertex shaders without a DX11 listing"] += 1
            elif expected != {facts[h][2]}:
                problems.append("vertex %s: the layer gives group %d, its DX11 partners %s match %s" % (h, facts[h][2], sorted(paired[h]), sorted(expected)))
            else:
                counts["vertex group %d" % facts[h][2]] += 1
        known_seen = set()
        for h in sorted(paired_ps):
            if h not in facts:
                continue
            known, slot, narrow = facts[h][3:]
            layer = ({"EFMITTLDiffuseAt%d" % slot} if 8 <= slot <= 23 else set()) | ({"EFMITTLClipFirst"} if narrow else set())
            for partner in sorted(paired_ps[h]):
                found = matched(partner, "ps")
                if found is None:
                    counts["fragment shader pairs without a DX11 listing"] += 1
                    continue
                found = {name for name in found if name.startswith("EFMITTLDiffuseAt") or name == "EFMITTLClipFirst"}
                if not known:
                    problems.append("fragment %s is not read by the pixel rule; its DX11 partner %s matches %s" % (h, partner, sorted(found)))
                elif found != layer and (h, partner) in KNOWN_PIXEL:
                    known_seen.add((h, partner))
                    counts["fragment pairs known to differ (KNOWN_PIXEL)"] += 1
                elif found != layer:
                    problems.append("fragment %s: the layer gives %s, its DX11 partner %s matches %s" % (h, sorted(layer), partner, sorted(found)))
                else:
                    counts["fragment pairs: " + (", ".join(sorted(n[len("EFMITTL"):] for n in found)) or "no group")] += 1
        for key in sorted(set(KNOWN_PIXEL) - known_seen):
            problems.append("known difference %s %s was not met: take it off KNOWN_PIXEL" % key)
        print("shader hooks, the installed library's groups (%d with a pattern):" % len(groups))
        for name, n in sorted(counts.items()):
            print("  %4d %s" % (n, name))
    for line in problems:
        print("CHECK FAILED: " + line)
    if not problems:
        print("shader hook checks passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
