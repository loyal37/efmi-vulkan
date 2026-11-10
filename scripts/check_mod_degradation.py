"""A Mod whose files are broken degrades as the original does, not as a whole (BUG-077, D-137).

Copies the smallest installed character Mod (read-only source) into folders of its own and breaks one
thing in each, then runs the every-key-state check (check_mod_states.py) on that folder with the
installed Core:

- an index buffer file deleted: the original's resource is null and that draw shows nothing;
- a vertex buffer file one byte short of whole elements: the original's buffer holds it, the last
  partial element is never read;
- a vertex buffer file declared a second time with another stride: the original makes two buffers;
- more distinct draw lists than the layer's cache holds;
- a section matched by a texture's description (match_type, match_width ...): the original runs it at
  a CheckTextureOverride of a pixel slot (D-138);
- a section matched by a buffer's description and a section that changes the game's resources when they
  are created: the layer does not do these, the rest of the Mod loads and the layer says so;
- a texture the Mod binds given as a PNG: the original loads every file not named .dds with WIC (D-139);
- a texture file the loader makes nothing of: the original's resource is null and the draw goes on;
- a texture declared with a colour space and a size: the loaders take the first and ignore the second;
  one whose declared format its file is not: the original's view fails and the slot binds nothing (D-140);
- a section's first draw as two instances, as a non-indexed draw, as `draw = from_caller` and as
  `drawindexed = auto`: the original issues each (D-142);
- a slot range bound to none and to one texture before a section's first draw: the original binds
  each slot of the range (D-143);
- a texture check of the draw's index buffer and of a vertex buffer: the original looks up the resource's
  sections (D-144);
- a dispatch before a section's first draw: the layer does not issue it there; the draw goes on (D-146);
- a draw past its index file and a position file cut in half: the original reads index 0 and zeros past
  them; the layer keeps the draw's triangles inside the file and leaves out a draw reading past a
  vertex file, the list's others going on (D-148).

Each must load, and every section must plan in every state. Writes build/mod-degradation/.
"""
from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODS = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Mods")
CORE = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Core\EFMI")


def resources(ini: str) -> list[tuple[str, dict]]:
    out, name, keys = [], None, {}
    for line in ini.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("["):
            if name: out.append((name, keys))
            name, keys = line.strip("[]"), {}
        elif "=" in line and name:
            k, v = line.split("=", 1)
            keys[k.strip().lower()] = v.strip()
    if name: out.append((name, keys))
    return [(n, k) for n, k in out if n.lower().startswith("resource") and "filename" in k]


def png_bytes(width: int, height: int) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    # Filter byte 0, then RGBA8 samples.
    rows = b"".join(b"\x00" + b"".join(bytes((x * 40 & 255, y * 40 & 255, 128, 255)) for x in range(width)) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def set_key(lines: list[str], section: str, name: str, value: str) -> None:
    """`name = value` in [section], in place of the line that sets it."""
    inside = False
    for i, line in enumerate(lines):
        stripped = line.split(";", 1)[0].strip()
        if stripped.startswith("["):
            inside = stripped.strip("[]").lower() == section.lower()
        elif inside and "=" in stripped and stripped.split("=", 1)[0].strip().lower() == name:
            lines[i] = "%s = %s" % (name, value)
            return
    raise RuntimeError("[%s] sets no %s" % (section, name))


def binds_dds(folder: Path) -> bool:
    """Whether one of the folder's INIs binds a declared .dds file at a pixel slot itself (`ps-tN = Resource...`),
    which the texture variants below change; a Mod that binds its textures through a library does not."""
    for ini in folder.glob("*.ini"):
        text = ini.read_text(encoding="utf-8", errors="replace")
        files = {n.lower(): k["filename"] for n, k in resources(text)}
        if any(files.get(b.lower(), "").lower().endswith(".dds")
               for b in re.findall(r"(?im)^\s*ps-t\d+\s*=\s*(?:ref\s+|reference\s+)?(Resource[^\s;]+)", text)):
            return True
    return False


def smallest_character_mod(suits=lambda folder: True) -> Path:
    candidates = []
    for folder in MODS.iterdir():
        if not folder.is_dir() or folder.name.upper().startswith("DISABLED"):
            continue
        inis = [p for p in folder.glob("*.ini")]
        if not any(re.search(r"(?im)^\s*\[TextureOverride", p.read_text(encoding="utf-8", errors="replace")) for p in inis):
            continue
        if not list(folder.rglob("*.buf")) or not suits(folder):
            continue
        candidates.append((sum(f.stat().st_size for f in folder.rglob("*") if f.is_file()), folder))
    return min(candidates)[1]


def main() -> int:
    # The smallest installed character Mod whose own lists bind a .dds texture (the texture variants need one).
    source = smallest_character_mod(binds_dds)
    out = ROOT / "build" / "mod-degradation"
    if out.exists():
        shutil.rmtree(out)
    failed = False
    ini_name = next(p.name for p in source.glob("*.ini") if re.search(r"(?im)^\s*\[TextureOverride", p.read_text(encoding="utf-8", errors="replace")))
    text = (source / ini_name).read_text(encoding="utf-8", errors="replace")
    declared = resources(text)
    fmt = lambda k: k.get("format", "").upper().replace("DXGI_FORMAT_", "")
    index = next((k["filename"] for n, k in declared if fmt(k) in ("R32_UINT", "R16_UINT") and k["filename"].lower().endswith(".buf")), None)
    vertex = next(((n, k) for n, k in declared if "stride" in k and k["filename"].lower().endswith(".buf") and fmt(k) not in ("R32_UINT", "R16_UINT")), None)
    if not index or not vertex:
        print("mod degradation: %s has no index or vertex buffer declaration to break" % source.name)
        return 1

    def variant(label: str, change, expect: tuple[str, ...] = ()) -> None:
        nonlocal failed
        mods = out / label / "Mods"
        mod = mods / source.name
        shutil.copytree(source, mod)
        change(mod)
        env = dict(os.environ, EFMI_VK_CHECK_MODS=str(mods), EFMI_VK_CHECK_CORE=str(CORE))
        result = subprocess.run([sys.executable, str(ROOT / "scripts" / "check_mod_states.py")], env=env, capture_output=True,
                                text=True, errors="replace", timeout=1800)
        (out / label / "check.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
        ok = result.returncode == 0 and all(e in result.stdout for e in expect)
        failed |= not ok
        print("%s %s: %s" % ("ok  " if ok else "FAIL", label, (result.stdout.strip().splitlines() or ["(no output)"])[-1]))
        if not ok:
            print("\n".join("     " + line for line in result.stdout.strip().splitlines()[-8:]))

    variant("index buffer file deleted", lambda mod: (mod / index.replace("\\", "/")).unlink())

    def short(mod: Path) -> None:
        path = mod / vertex[1]["filename"].replace("\\", "/")
        path.write_bytes(path.read_bytes()[:-1])
    variant("vertex buffer one byte short", short)

    def twice(mod: Path) -> None:
        name, keys = vertex
        stride = int(keys["stride"])
        extra = "\n[ResourceEfvkSecondMeaning]\ntype = Buffer\nstride = %d\nfilename = %s\n" % (stride * 2, keys["filename"])
        # Bound after a list's last draw, so the generation declares it in use without a draw reading it.
        lines = text.splitlines()
        first = next(i for i, line in enumerate(lines) if line.strip().lower().startswith("drawindexed"))
        end = next((i for i in range(first + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
        last = max(i for i in range(first, end) if lines[i].strip().lower().startswith("drawindexed"))
        lines.insert(last + 1, "vb1 = ResourceEfvkSecondMeaning")
        (mod / ini_name).write_text("\n".join(lines) + "\n" + extra, encoding="utf-8")
    variant("vertex buffer file declared with a second stride", twice)

    def many_lists(mod: Path) -> None:
        # A draw whose first index follows a counter the Mod's Present advances every frame: 1100 binding
        # states, more than the layer's cache of distinct draw lists holds (1024).
        lines = text.splitlines()
        at = next(i for i, line in enumerate(lines) if line.strip().lower().startswith("drawindexed"))
        lines[at] = "drawindexed = 3, ($efvk_frame % 1100) * 3, 0"
        def insert_after(header: str, line: str) -> None:
            where = next((i for i, l in enumerate(lines) if l.strip().lower() == header.lower()), None)
            if where is None:
                lines.extend(["", header, line])
            else:
                lines.insert(where + 1, line)
        insert_after("[Constants]", "global $efvk_frame = 0")
        insert_after("[Present]", "$efvk_frame = $efvk_frame + 1")
        (mod / ini_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    variant("more distinct draw lists than the cache holds", many_lists)

    texture = next((n for n, k in declared if k["filename"].lower().endswith(".dds")), None)

    def by_description(mod: Path) -> None:
        bind = "ps-t0 = %s" % texture if texture else "ps-t0 = null"
        extra = "\n[TextureOverrideEfvkBySize]\nmatch_type = Texture2D\nmatch_width = 1024\nmatch_height = width\n%s\n" % bind
        (mod / ini_name).write_text(text + extra, encoding="utf-8")
    variant("texture section matched by its description", by_description)

    def not_for_vulkan(mod: Path) -> None:
        extra = "\n[TextureOverrideEfvkByteWidth]\nmatch_type = Buffer\nmatch_byte_width = 1024\nhandling = skip\n"
        lines = text.splitlines()
        first = next(i for i, line in enumerate(lines) if line.strip().lower().startswith("[textureoverride"))
        lines.insert(first + 1, "deny_cpu_read = 1")
        (mod / ini_name).write_text("\n".join(lines) + "\n" + extra, encoding="utf-8")
    variant("buffer section by description, creation-time key", not_for_vulkan,
            ("matches buffers by their description", "changes how the game creates"))

    # A texture a draw of the Mod binds (`ps-tN = Resource...`) from a .dds file.
    files = {n.lower(): k["filename"] for n, k in declared}
    bound = [b for b in re.findall(r"(?im)^\s*ps-t\d+\s*=\s*(?:ref\s+|reference\s+)?(Resource[^\s;]+)", text)
             if files.get(b.lower(), "").lower().endswith(".dds")]
    if not bound:
        print("mod degradation: %s binds no DDS texture" % source.name)
        return 1

    def as_png(mod: Path) -> None:
        (mod / "efvk_texture.png").write_bytes(png_bytes(8, 4))
        lines = text.splitlines()
        set_key(lines, bound[0], "filename", "efvk_texture.png")
        (mod / ini_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    variant("texture given as a PNG", as_png)

    def unloadable(mod: Path) -> None:
        (mod / files[bound[0].lower()].replace("\\", "/")).write_bytes(b"DDS " + bytes(range(60)))
    variant("texture file the loader makes nothing of", unloadable, ("does not load",))

    def insert_key(mod: Path, lines_to_add: list[str]) -> None:
        lines = text.splitlines()
        header = next(i for i, line in enumerate(lines) if line.split(";", 1)[0].strip().strip("[]").lower() == bound[0].lower()
                      and line.strip().startswith("["))
        lines[header + 1:header + 1] = lines_to_add
        (mod / ini_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    variant("texture declared with a colour space and a size", lambda mod: insert_key(mod, ["color_space = sRGB", "width = 64"]))
    variant("texture declared with a format its file is not", lambda mod: insert_key(mod, ["format = R32_FLOAT"]))

    def first_draw_as(mod: Path, make) -> None:
        lines = text.splitlines()
        at = next(i for i, line in enumerate(lines) if line.strip().lower().startswith("drawindexed"))
        args = [a.strip() for a in lines[at].split("=", 1)[1].split(",")]
        indent = lines[at][:len(lines[at]) - len(lines[at].lstrip())]
        lines[at] = indent + make(args)
        (mod / ini_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    variant("a draw as two instances", lambda mod: first_draw_as(mod, lambda a: "drawindexedinstanced = %s, 2, %s, %s, 0" % (a[0], a[1], a[2])))
    variant("a non-indexed draw", lambda mod: first_draw_as(mod, lambda a: "draw = 3, 0"))
    variant("a draw from the caller", lambda mod: first_draw_as(mod, lambda a: "draw = from_caller"))
    variant("an indexed draw of the bound buffer's count", lambda mod: first_draw_as(mod, lambda a: "drawindexed = auto"))

    def before_first_draw(mod: Path, line: str) -> None:
        lines = text.splitlines()
        at = next(i for i, l in enumerate(lines) if l.strip().lower().startswith("drawindexed"))
        lines.insert(at, lines[at][:len(lines[at]) - len(lines[at].lstrip())] + line)
        (mod / ini_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    variant("a slot range bound to none", lambda mod: before_first_draw(mod, "ps-t[20:21] = null"))
    variant("a slot range bound to one texture", lambda mod: before_first_draw(mod, "ps-t[20:21] = %s" % bound[0]))

    def pool_range(mod: Path) -> None:
        before_first_draw(mod, "ps-t[20:21] = ref PoolEfvkRange[0:1]")
        with open(mod / ini_name, "a", encoding="utf-8") as ini:
            ini.write("\n[PoolEfvkRange]\npool_size = 2\n")
    variant("a slot range bound from a pool range", pool_range)
    variant("a texture check of the draw's index buffer", lambda mod: before_first_draw(mod, "checktextureoverride = ib"))
    variant("a texture check of a vertex buffer", lambda mod: before_first_draw(mod, "checktextureoverride = vb0"))
    variant("a dispatch in a replaced draw", lambda mod: before_first_draw(mod, "dispatch = 1, 1, 1"))

    # D-152: the application's vs-t0 saved, nulled and put back (Snowshine's LOD 1 correction) leaves the
    # replaced draw as the application bound it.
    def saved_and_restored(mod: Path) -> None:
        lines = text.splitlines()
        at = next(i for i, l in enumerate(lines) if l.strip().lower().startswith("drawindexed"))
        indent = lines[at][:len(lines[at]) - len(lines[at].lstrip())]
        lines[at:at] = [indent + "ResourceEfvkSaved = ref vs-t0", indent + "vs-t0 = null", indent + "vs-t0 = ref ResourceEfvkSaved"]
        (mod / ini_name).write_text("\n".join(lines) + "\n\n[ResourceEfvkSaved]\n", encoding="utf-8")
    variant("the application's vs-t0 saved, nulled and put back", saved_and_restored)

    # Past the files (D-148): the original reads index 0 past the index file, zeros past a vertex file.
    variant("a draw past its index file", lambda mod: first_draw_as(mod, lambda a: "drawindexed = %s + 10000000, %s, %s" % (a[0], a[1], a[2])))
    vb0 = next((m.group(1) for m in re.finditer(r"(?im)^\s*vb0\s*=\s*(?:ref\s+)?(Resource[^\s;]+)", text)
                if files.get(m.group(1).lower(), "").lower().endswith(".buf")), None)

    def half_positions(mod: Path) -> None:
        keys = dict(declared)[next(n for n, _ in declared if n.lower() == vb0.lower())]
        path = mod / keys["filename"].replace("\\", "/")
        stride = int(keys.get("stride", "1"))
        data = path.read_bytes()
        path.write_bytes(data[:len(data) // 2 // stride * stride])
    if vb0:
        variant("a draw reading past its vertex file", half_positions)
    print("mod degradation: %s (%s)" % ("FAILED" if failed else "passed", source.name))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
