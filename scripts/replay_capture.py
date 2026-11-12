"""Replay a captured character's game draws through the layer, with the installed Mod (sandbox).

A catalogue bundle of the Vulkan session (captures/catalogue-20261019/vulkan/vkNN) holds every
pipeline recipe of the frame, its SPIR-V, the descriptor contents of every draw and the uniform
buffers they read. This rebuilds the draws an installed Mod's sections match, in their recorded
order, and runs them for a few frames in a child process that loads the layer with the Mods folder
(directory generation, D-078): the layer must match them, plan them with the Mod's live INI,
import the bones and draw the Mod's mesh with the game's own shaders.

What the capture does not hold is made up, never guessed into game data: the index bytes (only the
region's hash is kept) are forged to the same CRC-32C with small indices, vertex buffers and
storage buffers are zero, the bone buffer holds upright stand-in bones where the instance entries
point, and every texture is a zero or grey image of the recorded format, size, mip chain and view type. The Mod's
mesh therefore stands in its bind pose at the place and with the projection the game used; the
original draws collapse to nothing. Passes of one depth format share their depth. Read-only on the
capture and the Mods; output under build/. docs/capture-replay.md has the details.

    py -B scripts/replay_capture.py --bundle "F:/efmi vulkan/captures/catalogue-20261019/vulkan/vk03"
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "sandbox"))
from efmi_vk.hashes import crc32c  # noqa: E402
from efmi_vk.source_slots import core_facts, declarations  # noqa: E402

MODS = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Mods")
ANY_COUNT = 0xFFFFFFFF            # a section matched by its hash alone (any index count)
CORE = Path(r"D:\XXMI-Launcher-Portable-v2.1.6\EFMI\Core\EFMI")
SETTINGS = None  # --settings: a folder of persistent INI variable files copied into each run
RESERVE_MB = None  # --reserve-mb: m2_reserve_mb for every run
IDENTITY_BONES = 1024            # bones written as identity at each instance entry's offsets
# The stand-in bone: the character meshes are stored Z-up and their skeleton turns them upright
# (-90 degrees about X), so a bone that only does that shows the Mod's mesh standing in bind pose.
# Three rows of a 3x4 matrix, as the vertex shaders read them (position' = row . (v, 1)).
UPRIGHT = (1, 0, 0, 0,  0, 0, 1, 0,  0, -1, 0, 0)
BONE_BYTES = 48                  # three float4 rows per bone (MergedSkeleton::kBytesPerBone)
STORAGE_CAP = 64 << 20           # a storage range above this is not replayable here
UNIFORM, STORAGE, UNIFORM_DYNAMIC, STORAGE_DYNAMIC, SAMPLER, SAMPLED_IMAGE = 6, 7, 8, 9, 0, 2


# CRC-32C forging -------------------------------------------------------------------------------
POLY = 0x82F63B78
TABLE = []
for n in range(256):
    c = n
    for _ in range(8):
        c = (c >> 1) ^ POLY if c & 1 else c >> 1
    TABLE.append(c)


def zero_step_matrix():
    """Columns of the linear map reg -> (reg >> 8) ^ TABLE[reg & 0xff] (one zero byte)."""
    cols = []
    for bit in range(32):
        reg = 1 << bit
        cols.append((reg >> 8) ^ TABLE[reg & 0xFF])
    return cols


def apply(cols, vector):
    out = 0
    bit = 0
    while vector:
        if vector & 1:
            out ^= cols[bit]
        vector >>= 1
        bit += 1
    return out


def compose(a, b):
    """a after b, as column lists."""
    return [apply(a, column) for column in b]


def power(cols, n):
    result = [1 << i for i in range(32)]
    base = cols
    while n:
        if n & 1:
            result = compose(base, result)
        base = compose(base, base)
        n >>= 1
    return result


def forge_region(length: int, index_size: int, target: int) -> bytes:
    """`length` bytes of small indices whose CRC-32C is `target`: zero indices except the low byte
    of the first few, solved over GF(2)."""
    base = crc32c(bytes(length))
    want = base ^ target
    z = zero_step_matrix()
    candidates = []                              # (byte position, bit, effect on the final register)
    for i in range(min(8, length // index_size)):
        pos = i * index_size
        shift = power(z, length - 1 - pos)
        for b in range(8):
            candidates.append((pos, b, apply(shift, TABLE[1 << b])))
    basis = {}                                   # pivot bit -> (vector, set of candidate indices)
    for k, (_pos, _b, v) in enumerate(candidates):
        combo = {k}
        while v:
            pivot = v.bit_length() - 1
            if pivot not in basis:
                basis[pivot] = (v, combo)
                break
            bv, bc = basis[pivot]
            v ^= bv
            combo = combo ^ bc
    chosen = set()
    v = want
    while v:
        pivot = v.bit_length() - 1
        if pivot not in basis:
            raise ValueError("CRC forging has no solution with these positions")
        bv, bc = basis[pivot]
        v ^= bv
        chosen ^= bc
    data = bytearray(length)
    for k in chosen:
        pos, b, _ = candidates[k]
        data[pos] ^= 1 << b
    if crc32c(bytes(data)) != target:
        raise ValueError("CRC forging failed")
    return bytes(data)


def only_mod(name: str) -> Path:
    """A Mods folder of the project holding a copy of the installed Mod `name`, whether the user has it
    enabled or renamed to DISABLED_ (the installed folder is only read). A suite step that tests one
    Mod's mechanism then does not depend on which Mods the user has on. Copied once; when the user has
    removed the Mod since, the copy made before stays the step's data."""
    source = MODS / name if (MODS / name).is_dir() else MODS / ("DISABLED_" + name)
    # The project's folder takes a plain name (it names the replay's output and build folders); the copy
    # inside keeps the Mod's own, whatever its characters.
    target = ROOT / "build" / "replay-mods" / "".join(c if c.isascii() and (c.isalnum() or c in "._-") else "_" for c in name)
    copy = target / name
    if not source.is_dir():
        if (target / "source.txt").is_file() and copy.is_dir():
            print("only-mod: %s is no longer installed; the project's copy of it is used" % name)
            return target
        raise ValueError("no installed Mod folder named %s (enabled or DISABLED_) and no copy of it" % name)
    files = sorted(f for f in source.rglob("*") if f.is_file())
    stamp = "\n".join("%s %d %d" % (f.relative_to(source), f.stat().st_size, f.stat().st_mtime_ns) for f in files)
    if not (target / "source.txt").is_file() or (target / "source.txt").read_text(encoding="utf-8") != stamp:
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(source, copy)
        (target / "source.txt").write_text(stamp, encoding="utf-8")
    return target


# Capture ---------------------------------------------------------------------------------------
def mod_sections(dist: Path, out: Path) -> tuple[dict, dict]:
    """The installed Mods' sections by (index hash, count): the INI of each, and its component (None for a hide section)."""
    exe = dist / "efmi_vk_character_test.exe"
    run = subprocess.run([str(exe), "--plan-mods", str(MODS), str(CORE), str(out / "plan-settings")], cwd=out,
                         capture_output=True, text=True, errors="replace", timeout=600)
    if run.returncode:
        raise ValueError("dry run failed: " + run.stderr[-500:])
    sections, components = {}, {}
    for line in run.stdout.splitlines():
        m = json.loads(line)
        for s in m.get("sections", []):
            sections[(s["hash"], s["count"])] = m["ini"]
            components[(s["hash"], s["count"])] = s.get("component")
    return sections, components


class Replay:
    def __init__(self, bundle: Path, out: Path):
        self.bundle, self.out = bundle, out
        self.frame = json.loads((bundle / "frame.json").read_text())
        self.pipes = {p["id"]: p for p in json.loads((bundle / "pipelines.json").read_text())}
        self.shader_files = {s["id"]: bundle / s["file"] for s in json.loads((bundle / "shaders.json").read_text()) if s.get("file")}
        snaps = json.loads((bundle / "snapshots.json").read_text())
        self.blobs = {b["id"]: b for b in snaps["blobs"]}
        self.snapshot_bytes = (bundle / "snapshots.bin").read_bytes()
        self.data = bytearray()
        self.buffers, self.images, self.cases, self.case_of = [], [], [], {}
        self.bone_regions = []  # (buffer, offset, bytes) of the stand-in bones, for --animate-bones
        self.image_of, self.shared = {}, {}
        self.omitted = Counter()
        self.probe_vertices = False  # diagnosis: the original draws get a small known shape
        self.small_images = False    # diagnosis: 4x4 single-level textures instead of the recorded size

    def put(self, payload: bytes) -> int:
        while len(self.data) % 16:
            self.data.append(0)
        at = len(self.data)
        self.data += payload
        return at

    def buffer(self, size: int, fills=()) -> int:
        self.buffers.append({"size": max(16, size), "fills": list(fills)})
        return len(self.buffers) - 1

    def image(self, e: dict) -> int:
        layers = e["subresource"]["layers"] if e.get("subresource") else e.get("array_layers", 1)
        extent = (e.get("extent") or [4, 4, 1])[:2]
        # The recorded size and mip chain (capped), so a texture keeps the description the layer reads
        # (RabbitFX's roles classify by it, D-082); the content stays zero or grey.
        width, height = min(max(1, extent[0]), 4096), min(max(1, extent[1]), 4096)
        mips = min(max(1, e.get("mip_levels") or 1), max(width, height).bit_length())
        if self.small_images:  # diagnosis: the earlier 4x4 single-level images
            width, height, mips = 4, 4, 1
        key = (e["image_format"], e["format"], e["image_type"], e["view_type"], max(1, layers), width, height, mips)
        if key not in self.image_of:
            self.image_of[key] = len(self.images)
            self.images.append(dict(zip(("format", "view_format", "type", "view_type", "layers", "width", "height", "mips"), key)))
        return self.image_of[key]

    def case(self, pid: int) -> int:
        if pid in self.case_of:
            return self.case_of[pid]
        p = self.pipes[pid]
        if not p.get("recipe") or not p["recipe"].get("complete"):
            raise ValueError("pipeline %d has no complete recipe" % pid)
        files = [self.shader_files[s["shader"]] for s in p["recipe"]["stages"]]
        self.cases.append({"pipeline": p, "shader_files": files, "vertex": {"bindings": p["bindings"], "attributes": p["attributes"]},
                           "patch": {"images": [], "nulls": []}, "draws": 0})
        self.case_of[pid] = len(self.cases) - 1
        return self.case_of[pid]

    def bones_site(self, p: dict):
        vs = [s for s in p["recipe"]["stages"] if s["stage"] == 1]
        if not vs:
            return None, None
        facts = core_facts(declarations(self.shader_files[vs[0]["shader"]].read_bytes()))
        return facts, (tuple(facts["bones"]) if facts["rolling"] else None)

    def draw(self, d: dict) -> dict | None:
        p = self.pipes[d["pipeline"]]
        sets = p["layout_recipe"]["sets"]
        if len(d.get("sets", [])) < len(sets):
            self.omitted["fewer bound sets than the layout"] += 1
            return None
        snaps = {(s["set"], s["binding"], s["element"]): s for s in d.get("snapshot", [])}
        facts, bones = self.bones_site(p)
        entry_rows = []
        window = snaps.get((2, 0, 0))
        if window is not None and window["id"] in self.blobs:
            at = self.blobs[window["id"]]["file_offset"]
            entry_rows = list(struct.unpack_from("<2I", self.snapshot_bytes, at + 80))
        descriptors = []
        for si, layout in enumerate(sets):
            contents = d["sets"][si].get("contents") or {}
            bound = {b["binding"]: b for b in contents.get("bindings", [])}
            for lb in layout["bindings"]:
                b = bound.get(lb["binding"])
                if b is None or not b.get("elements") or not b["elements"][0].get("known"):
                    self.omitted["a binding of the layout is not in the capture"] += 1
                    return None
                e, t = b["elements"][0], lb["type"]
                item = {"set": si, "binding": lb["binding"], "type": t, "buffer": 0, "offset": 0, "range": 0, "image": 0}
                if t == SAMPLER:
                    pass
                elif t == SAMPLED_IMAGE:
                    if not e.get("image_known"):
                        self.omitted["an image of unknown description"] += 1
                        return None
                    item["image"] = self.image(e)
                elif t in (UNIFORM, UNIFORM_DYNAMIC):
                    snap = snaps.get((si, lb["binding"], 0))
                    size = e["range"]
                    if snap and snap["id"] in self.blobs:
                        blob = self.blobs[snap["id"]]
                        payload = self.snapshot_bytes[blob["file_offset"]:blob["file_offset"] + blob["size"]]
                        item["buffer"] = self.buffer(len(payload), [(0, self.put(payload), len(payload))])
                        item["range"] = len(payload)
                    else:
                        item["buffer"] = self.buffer(size)
                        item["range"] = size
                elif t in (STORAGE, STORAGE_DYNAMIC):
                    size = e["offset"] + e["range"]
                    if size > STORAGE_CAP:
                        self.omitted["a storage range above the replay's cap"] += 1
                        return None
                    key = (e["buf"], e["offset"], e["range"])
                    is_bones = bones == (si, lb["binding"])
                    if key not in self.shared:
                        self.shared[key] = self.buffer(size)
                    item["buffer"], item["offset"], item["range"] = self.shared[key], e["offset"], e["range"]
                    if is_bones:
                        identity = struct.pack("<12f", *UPRIGHT) * IDENTITY_BONES
                        src = self.put(identity)
                        for row in entry_rows:
                            dst = e["offset"] + row * 16
                            room = max(0, min(len(identity), size - dst))
                            if room:
                                self.buffers[item["buffer"]]["fills"].append((dst, src, room))
                                self.bone_regions.append((item["buffer"], dst, room))
                else:
                    self.omitted["descriptor type %d" % t] += 1
                    return None
                descriptors.append(item)
        ib = d.get("ib")
        index_size, index = 2, 0xFFFFFFFF  # a draw without indices (a full-screen pass of the game)
        if ib:
            index_size = ib["index_size"]
            region = forge_region(d["count"] * index_size, index_size, int(ib["hash"], 16))
            index = self.buffer(d["first"] * index_size + len(region), [(d["first"] * index_size, self.put(region), len(region))])
        vertex = []
        strides = {b["binding"]: b["stride"] for b in p["bindings"]}
        # Every binding the pipeline declares gets zero data (the capture names four vertex slots only).
        for slot in sorted(strides):
            count = 256 + max(0, d["vertex_offset"]) + d["first_instance"] + d["instances"]
            fills = []
            if self.probe_vertices and strides[slot]:
                shape = probe_shape(p, slot, strides[slot], count)
                if shape:
                    fills = [(0, self.put(shape), len(shape))]
            vertex.append({"binding": slot, "buffer": self.buffer(count * max(strides[slot], 16), fills)})
        case = self.case(d["pipeline"])
        self.cases[case]["draws"] += 1
        return {"pipeline": case, "group": (d["cb"], d["rp"]), "count": d["count"], "instances": d["instances"],
                "first": d["first"], "first_instance": d["first_instance"], "vertex_offset": d["vertex_offset"],
                "index_buffer": index, "index_type": 1 if index_size == 4 else 0, "descriptors": descriptors, "vertex": vertex,
                "n": d["n"], "facts": facts, "key": (ib["hash"], d["count"]) if ib else None}


def probe_shape(p: dict, binding: int, stride: int, count: int) -> bytes | None:
    """Vertex data of a known shape for the original draws (diagnosis): positions spread over a
    1.6 m tall box at the object's origin, full weight on the first bone, everything else zero."""
    attrs = [a for a in p["attributes"] if a["binding"] == binding]
    if not any(a["format"] in (106, 91) for a in attrs):
        return None
    out = bytearray(count * stride)
    for v in range(count):
        for a in attrs:
            at = v * stride + a["offset"]
            if a["format"] == 106 and a["location"] == 0:
                struct.pack_into("<3f", out, at, ((v * 37) % 101) / 100 - 0.5, ((v * 53) % 97) / 97 * 1.6, ((v * 71) % 89) / 89 - 0.5)
            elif a["format"] == 91:
                struct.pack_into("<4H", out, at, 65535, 0, 0, 0)
    return bytes(out)


def emit(replay: Replay, draws: list[dict]) -> str:
    from check_historical_variants import generate_case
    lines = []
    for i, c in enumerate(replay.cases):
        lines.append(generate_case(c, i))
    lines.append("static HistoricalCase replay_case(unsigned n){switch(n){")
    lines += ["case %d:return case_%d();" % (i, i) for i in range(len(replay.cases))]
    lines.append('default:throw std::runtime_error("Unknown replay case");}}')
    lines.append("constexpr unsigned replay_case_count=%du;" % len(replay.cases))
    lines.append("static std::vector<ReplayBuffer> replay_buffers(){std::vector<ReplayBuffer> b;")
    for b in replay.buffers:
        fills = ",".join("{%du,%du,%du}" % f for f in b["fills"])
        lines.append("b.push_back({%du,{%s}});" % (b["size"], fills))
    lines.append("return b;}")
    lines.append("static std::vector<ReplayBones> replay_bones(){return {")
    lines += ["{%du,%du,%du}," % r for r in sorted(set(replay.bone_regions))]
    lines.append("};}")
    lines.append("static std::vector<ReplayImage> replay_images(){return {")
    lines += ["{%du,%du,%du,%du,%du,%du,%du,%du}," % (i["format"], i["view_format"], i["type"], i["view_type"], i["layers"],
                                                     i["width"], i["height"], i["mips"]) for i in replay.images]
    lines.append("};}")
    groups = {}
    for d in draws:
        groups.setdefault(d["group"], len(groups))
    lines.append("static std::vector<ReplayDraw> replay_draws(){std::vector<ReplayDraw> d;")
    for d in draws:
        desc = ",".join("{%du,%du,%du,%du,%du,%du,%du}" % (x["set"], x["binding"], x["type"], x["buffer"], x["offset"], x["range"], x["image"])
                        for x in d["descriptors"])
        vert = ",".join("{%du,%du}" % (v["binding"], v["buffer"]) for v in d["vertex"])
        lines.append("d.push_back({%du,%du,%du,%du,%du,%du,%d,%du,%du,{%s},{%s},%du});" % (
            d["pipeline"], groups[d["group"]], d["count"], d["instances"], d["first"], d["first_instance"], d["vertex_offset"],
            d["index_buffer"], d["index_type"], desc, vert, d["n"]))
    lines.append("return d;}")
    return "\n".join(lines) + "\n"


def build(out: Path) -> Path:
    from build import COMMON, BuildError, find_vcvars, find_vulkan_include, run_cl
    sources = [ROOT / "sandbox/replay_gpu.cpp"]
    sources += [ROOT / "frontend/assets" / n for n in ("dds.cpp", "png.cpp", "image_upload.cpp", "resource_cache.cpp", "buffer_upload.cpp",
                                                         "file_plan.cpp", "file_draws.cpp")]
    sources += [ROOT / "frontend/gpu" / n for n in ("fragment_images.cpp", "dither_kills.cpp", "material_plan.cpp", "draw_transaction.cpp")]
    sources += [ROOT / "layer/src" / n for n in ("pipeline_recipe.cpp", "pipeline_variant.cpp", "spirv_repoint.cpp", "variant_cache.cpp",
                                                 "draw_snapshots.cpp")]
    exe = out / "replay_gpu.exe"
    args = COMMON + ["/DNOMINMAX", "/I" + find_vulkan_include(), "/I" + str(out), "/I" + str(ROOT / "frontend/assets"),
                     "/Fo" + str(out) + "/", "/Fd" + str(out / "replay.pdb")]
    args += [str(p) for p in sources] + ["/Fe:" + str(exe), "/link", "/NOLOGO", "user32.lib", "dbghelp.lib"]
    log = io.StringIO()
    try:
        with contextlib.redirect_stdout(log):
            run_cl(find_vcvars(), str(out), args)
    except BuildError as error:
        (out / "build.txt").write_text((log.getvalue() + str(error)).encode("ascii", "backslashreplace").decode(), encoding="ascii")
        raise
    return exe


def dimension_policy() -> str:
    """vendor:device of the measured dx11-invalid-2d-to-3d-zero evidence, checked as packaging checks it."""
    report = Path(r"F:\efmi vulkan\build\dx11-dimensions\report.json")
    if not report.is_file():
        return ""
    evidence = json.loads(report.read_text())
    hardware = [r for r in evidence.get("runs", []) if r.get("device") == "hardware-normal"]
    if not evidence.get("passed") or len(hardware) != 1 or hardware[0]["returncode"] or not hardware[0]["controls_passed"] or any(
            hardware[0]["pixels"][name] != [0, 0, 0, 0] for name in ("3d_shader_2d_view_z0", "3d_shader_2d_view_z05", "3d_shader_2d_view_z2")):
        return ""
    return "%d:%d" % tuple(hardware[0]["adapter"][:2])


def run(exe: Path, out: Path, dist: Path, frames: int, enabled: bool, validation: Path, open_tests: bool = False,
        two_queues: bool = False, gpu_bones: bool | None = None, name: str | None = None, split_passes: bool = False,
        animate_bones: bool = False, rolling_bones: bool = False) -> dict:
    from harness import layer_env
    folder = out / (name or ("enabled" if enabled else "disabled"))
    folder.mkdir(parents=True, exist_ok=True)
    if SETTINGS is not None and SETTINGS.is_dir():
        # Persistent INI variables to start from (a variant key's state), as the game's settings folder.
        (folder / "settings").mkdir(exist_ok=True)
        for item in SETTINGS.glob("*.ini"):
            shutil.copy2(item, folder / "settings" / item.name)
    extra = dict(EFMI_VK_M2_MODS=str(MODS), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(folder / "settings"),
                 EFMI_VK_M2_ENABLED="1" if enabled else "0", EFMI_VK_M2_DIMENSION_ZERO=dimension_policy() or "off",
                 VK_LAYER_PATH=os.pathsep.join([str(dist), str(validation)]), XDG_CACHE_HOME=str(folder),
                 TEMP=str(folder), TMP=str(folder), TMPDIR=str(folder))
    if gpu_bones is not None:
        extra["EFMI_VK_M2_GPU_BONES"] = "1" if gpu_bones else "0"
    if RESERVE_MB is not None:
        extra["EFMI_VK_M2_RESERVE_MB"] = str(RESERVE_MB)
    env = layer_env(str(folder / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    flags = (["open"] if open_tests else []) + (["split-passes"] if split_passes else ["two-queues"] if two_queues else [])
    flags += ["rolling-bones"] if rolling_bones else ["animate-bones"] if animate_bones else []
    result = subprocess.run([str(exe), str(out / "replay.bin"), str(frames)] + flags, cwd=folder, env=env,
                            capture_output=True, text=True, errors="replace", timeout=600)
    (folder / "stdout.txt").write_text((result.stdout + result.stderr).encode("ascii", "backslashreplace").decode(), encoding="ascii")
    status = json.loads((folder / "status.json").read_text()) if (folder / "status.json").is_file() else {}
    return {"returncode": result.returncode, "status": status, "output": result.stdout + result.stderr, "folder": folder}


# Targets to pictures -----------------------------------------------------------------------------
def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    import zlib
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def unpack_float(bits: int, mantissa: int) -> float:
    exponent, frac = bits >> mantissa, bits & ((1 << mantissa) - 1)
    if exponent == 0:
        return frac / (1 << mantissa) * 2.0 ** -14
    if exponent == 31:
        return 65504.0
    return (1 + frac / (1 << mantissa)) * 2.0 ** (exponent - 15)


def picture(raw: bytes, fmt: int, count: int) -> tuple[bytes, int]:
    """8-bit RGB of a target and the number of pixels that are not the clear value."""
    out = bytearray(count * 3)
    covered = 0
    if fmt in (37, 43, 44, 50):
        for i in range(count):
            r, g, b, a = raw[i * 4:i * 4 + 4]
            if fmt in (44, 50):
                r, b = b, r
            out[i * 3:i * 3 + 3] = bytes((r, g, b))
            covered += (r | g | b | a) != 0
    elif fmt == 64:
        for i, (v,) in enumerate(struct.iter_unpack("<I", raw[:count * 4])):
            out[i * 3:i * 3 + 3] = bytes((((v >> s) & 1023) >> 2 for s in (0, 10, 20)))
            covered += v != 0
    elif fmt == 122:
        for i, (v,) in enumerate(struct.iter_unpack("<I", raw[:count * 4])):
            channels = (unpack_float(v & 0x7FF, 6), unpack_float((v >> 11) & 0x7FF, 6), unpack_float(v >> 22, 5))
            out[i * 3:i * 3 + 3] = bytes(min(255, int(255 * (c / (1 + c)) ** (1 / 2.2))) for c in channels)
            covered += v != 0
    elif fmt in (124, 126, 130):
        values = [x for (x,) in struct.iter_unpack("<H" if fmt == 124 else "<f", raw[:count * (2 if fmt == 124 else 4)])]
        clear = max(set(values[:: max(1, count // 1000)]), key=values[:: max(1, count // 1000)].count)
        drawn = [x for x in values if x != clear]
        low, high = (min(drawn), max(drawn)) if drawn else (0, 1)
        for i, x in enumerate(values):
            level = 0 if x == clear else 64 + int(191 * ((x - low) / (high - low) if high > low else 1))
            out[i * 3:i * 3 + 3] = bytes((level, level, level))
        covered = len(drawn)
    return bytes(out), covered


def pictures(folder: Path) -> dict:
    index = json.loads((folder / "targets.json").read_text())
    w, h = index["width"], index["height"]
    result = {}
    for target in index["targets"]:
        raw = (folder / target["file"]).read_bytes()
        rgb, covered = picture(raw, target["format"], w * h)
        name = "group%d-attachment%d.png" % (target["group"], target["attachment"])
        # The game's vertex shaders negate y: its targets hold the picture upside down until the
        # final blit. The pictures are turned the right way up; the raw files stay as read back.
        rows = [rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)]
        write_png(folder / name, w, h, b"".join(reversed(rows)))
        result[name] = {"format": target["format"], "covered": covered, "raw": hashlib_digest(raw)}
    return result


def hashlib_digest(data: bytes) -> str:
    import hashlib
    return hashlib.sha256(data).hexdigest()[:16]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bundle", type=Path, required=True)
    ap.add_argument("--frames", type=int, default=90)
    ap.add_argument("--rp", type=int, action="append", help="replay only the draws of these recorded render pass instances")
    ap.add_argument("--open-tests", action="store_true", help="diagnosis: no depth, stencil or culling in the replayed pipelines")
    ap.add_argument("--probe-vertices", action="store_true", help="diagnosis: the original draws get a small known shape")
    ap.add_argument("--small-images", action="store_true", help="diagnosis: 4x4 single-level textures")
    ap.add_argument("--mods", type=Path, help="another Mods folder than the installed one (a project-owned copy)")
    ap.add_argument("--only-mod", help="a project-owned Mods folder with a copy of this installed Mod only, enabled or "
                    "DISABLED_ (a suite step about one Mod's mechanism)")
    ap.add_argument("--settings", type=Path, help="a folder of persistent INI variable files the Mods start from (a variant key's state)")
    ap.add_argument("--check", action="store_true", help="fail unless both runs are clean, the originals draw nothing and every "
                    "render pass with a component of the Mod shows the Mod's draws (regression suite)")
    ap.add_argument("--copy-passes", action="store_true", help="also replay the game's full-screen draws of a pass with one colour "
                    "target and no depth right after a pass the Mod draws in (the copy after the forward pass, D-113)")
    ap.add_argument("--expect-translucency", action="store_true", help="with --check: fail unless the layer captured translucent "
                    "calls and composited them without an error (EFMITTL, D-113)")
    ap.add_argument("--expect-shadows", action="store_true", help="with --check: fail unless translucent calls were drawn in "
                    "the shadow passes by their opacity (EFMITTL's shadow shaders, D-185)")
    ap.add_argument("--no-refusal", action="store_true", help="with --check: fail when a matched draw's plan was refused, "
                    "as when an INI calls a library or asks a question the plan cannot answer (D-112)")
    ap.add_argument("--two-queues", action="store_true", help="the game's use of its two graphics queues (D-119): a frame's passes "
                    "split over two command buffers on the two queues, the queues swapping every frame, three frames in flight")
    ap.add_argument("--mod-first", action="store_true", help="with --check: fail when a matched draw drew the game's mesh "
                    "(kept by the Mod's lists, or before the object was the Mod's): the Mod is the first thing shown")
    ap.add_argument("--reserve-mb", type=int, help="device memory the layer keeps free for the game (m2_reserve_mb); beyond the device's "
                    "size it leaves no free memory, as a full card in the game (D-122: activation does not wait for it)")
    ap.add_argument("--animate-bones", action="store_true", help="the stand-in bones move every frame, written with both queues "
                    "idle (BUG-054): a draw reading another recording's older import then differs from the CPU import")
    ap.add_argument("--rolling-bones", action="store_true", help="the stand-in bones move every frame and live in two copies, "
                    "frame f binding copy f%%2; the copy of the frame before is zeroed before a frame is submitted, as the game "
                    "reuses a completed frame's bone region (BUG-060): a read of another frame's bones then finds none")
    ap.add_argument("--split-passes", action="store_true", help="with two queues, every pass's draws split over the two command "
                    "buffers (BUG-054): one recording draws components whose bones another recording imported")
    ap.add_argument("--bones-identical", action="store_true", help="also replay with the merged skeleton imported on the CPU "
                    "(m2_gpu_bones = 0) and, with --check, fail unless its pictures equal the GPU import's (D-117)")
    args = ap.parse_args()
    global MODS, SETTINGS, RESERVE_MB
    MODS = args.mods or MODS
    RESERVE_MB = args.reserve_mb
    if args.only_mod:
        MODS = args.mods = only_mod(args.only_mod)
    SETTINGS = args.settings
    dist = Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist"))
    validation = Path(os.environ.get("EFMI_VK_VALIDATION_DIR", "F:/vulkan/validation-1.4.321.0/bin"))
    out = ROOT / "build" / "replay" / (args.bundle.name + ("-rp" + "-".join(map(str, args.rp)) if args.rp else "") + ("-open" if args.open_tests else "") + ("-probe" if args.probe_vertices else "") + ("-" + args.mods.name if args.mods else "") + ("-small" if args.small_images else "") + ("-split" if args.split_passes else "-2q" if args.two_queues else "") + ("-roll" if args.rolling_bones else "-anim" if args.animate_bones else ""))
    out.mkdir(parents=True, exist_ok=True)
    sections, components = mod_sections(dist, out)
    replay = Replay(args.bundle, out)
    replay.probe_vertices = args.probe_vertices
    replay.small_images = args.small_images
    def section_of(d: dict):
        """The (hash, count) a Mod section matches this draw by: its own count, or any count for a
        section matched by its hash alone."""
        ib = (d.get("ib") or {}).get("hash")
        for key in ((ib, d.get("count")), (ib, ANY_COUNT)):
            if key in sections:
                return key
        return None

    draws = []
    mod_passes = {(d["cb"], d["rp"]) for d in replay.frame["draws"] if section_of(d) and d.get("kind") == "indexed"}

    def copy_pass(d: dict) -> bool:
        if not args.copy_passes or d.get("kind") != "draw" or (d["cb"], d["rp"] - 1) not in mod_passes:
            return False
        recipe = replay.pipes[d["pipeline"]].get("recipe") or {}
        subpasses = (recipe.get("render_pass") or {}).get("subpasses") or []
        sub = subpasses[recipe.get("subpass", 0)] if subpasses else {}
        return len(sub.get("colors", [])) == 1 and not sub.get("depth")

    for d in replay.frame["draws"]:
        if copy_pass(d):
            made = replay.draw(d)
            if made:
                draws.append(made)
            continue
        if section_of(d) and d.get("kind") == "indexed" and (not args.rp or d.get("rp") in args.rp):
            made = replay.draw(d)
            if made:
                made["key"] = section_of(d)
                draws.append(made)
    mods = sorted({sections[section_of(d)] for d in replay.frame["draws"] if section_of(d)})
    print("bundle %s: Mods %s; %d draws replayed, omitted %s; %d pipelines, %d buffers, %d images, %d bytes of data" % (
        args.bundle.name, mods, len(draws), dict(replay.omitted), len(replay.cases), len(replay.buffers), len(replay.images), len(replay.data)))
    if not draws:
        return 1
    (out / "replay.bin").write_bytes(bytes(replay.data))
    (out / "replay_case.inc").write_text(emit(replay, draws), encoding="ascii")
    exe = build(out)
    results = {}
    for enabled in (False, True):
        r = run(exe, out, dist, args.frames, enabled, validation, args.open_tests, args.two_queues, split_passes=args.split_passes,
                animate_bones=args.animate_bones, rolling_bones=args.rolling_bones)
        results[enabled] = r
        rep = r["status"].get("result", {}).get("replacement", {})
        counters = {k: v for k, v in rep.get("counters", {}).items() if v} if rep else {}
        line = next((l for l in r["output"].splitlines() if l.startswith("replay:")), "replay: no summary")
        print("%s: exit %d; %s; counters %s" % ("enabled" if enabled else "disabled", r["returncode"], line, counters))
    shots = {enabled: pictures(r["folder"]) for enabled, r in results.items() if (r["folder"] / "targets.json").is_file()}
    if len(shots) == 2:
        for name in sorted(shots[True]):
            off, on = shots[False].get(name, {}), shots[True][name]
            print("  %s (format %d): covered %d without the layer's replacement, %d with it%s" % (
                name, on["format"], off.get("covered", 0), on["covered"], "" if off.get("raw") != on["raw"] else "; identical"))
    summary = {"bundle": args.bundle.name, "mods": mods, "draws": len(draws), "omitted": dict(replay.omitted), "frames": args.frames,
               "runs": {("enabled" if enabled else "disabled"): {
                   "returncode": r["returncode"],
                   "replay": next((l for l in r["output"].splitlines() if l.startswith("replay:")), ""),
                   "counters": {k: v for k, v in r["status"].get("result", {}).get("replacement", {}).get("counters", {}).items() if v},
                   "last_reason": r["status"].get("result", {}).get("replacement", {}).get("last_reason", ""),
                   "translucency": r["status"].get("result", {}).get("replacement", {}).get("translucency", {}),
                   "bones": r["status"].get("result", {}).get("replacement", {}).get("bones", {}),
                   "folder": {k: v for k, v in r["status"].get("result", {}).get("replacement", {}).get("folder_generation", {}).items()
                              if k in ("packages", "presents", "ini", "left_out")},
                   "targets": shots.get(enabled, {})} for enabled, r in results.items()}}
    (out / "summary.json").write_text(json.dumps(summary, indent=1), encoding="ascii")
    cpu_bones = None
    if args.bones_identical:
        cpu_bones = run(exe, out, dist, args.frames, True, validation, args.open_tests, args.two_queues, gpu_bones=False, name="enabled-cpu-bones",
                        split_passes=args.split_passes, animate_bones=args.animate_bones, rolling_bones=args.rolling_bones)
        line = next((l for l in cpu_bones["output"].splitlines() if l.startswith("replay:")), "replay: no summary")
        gpu_shots = shots.get(True, {}); cpu_shots = pictures(cpu_bones["folder"]) if (cpu_bones["folder"] / "targets.json").is_file() else {}
        differing = sorted(n for n in gpu_shots if cpu_shots.get(n, {}).get("raw") != gpu_shots[n].get("raw"))
        bones = results[True]["status"].get("result", {}).get("replacement", {}).get("bones", {})
        print("enabled, bones on the CPU: exit %d; %s; pictures %s; GPU run's bones %s" % (
            cpu_bones["returncode"], line, "identical" if not differing and cpu_shots else "differ: %s" % differing, bones))
        summary["runs"]["enabled-cpu-bones"] = {"returncode": cpu_bones["returncode"], "replay": line, "differing": differing}
        (out / "summary.json").write_text(json.dumps(summary, indent=1), encoding="ascii")
    if args.check:
        problems = check(summary, draws, components, args.mod_first)
        if args.bones_identical:
            cpu = summary["runs"]["enabled-cpu-bones"]
            bones = results[True]["status"].get("result", {}).get("replacement", {}).get("bones", {})
            if cpu["returncode"] != 0 or "validation errors=0 " not in cpu["replay"]:
                problems.append("CPU bones run: %s" % cpu["replay"])
            if cpu["differing"] or not shots.get(True):
                problems.append("pictures with GPU and CPU bones differ: %s" % cpu["differing"])
            if not bones.get("jobs") or bones.get("failures") or bones.get("submitted") != bones.get("completed"):
                problems.append("bones status %s" % bones)
        if args.expect_translucency:
            t = summary["runs"].get("enabled", {}).get("translucency") or {}
            if not t.get("captures") or not t.get("composites") or t.get("last_error"):
                problems.append("translucency %s" % t)
        if args.expect_shadows:
            shadows = summary["runs"].get("enabled", {}).get("counters", {}).get("translucent_shadows", 0)
            if not shadows:
                problems.append("no translucent call was drawn in a shadow pass by its opacity (translucent_shadows %s)" % shadows)
        if args.no_refusal:
            refused = {k: v for k, v in summary["runs"].get("enabled", {}).get("counters", {}).items() if k.startswith("refused_") and v}
            if refused:
                problems.append("refused plans %s: %s" % (refused, summary["runs"]["enabled"].get("last_reason", "")))
        for line in problems:
            print("CHECK FAILED: " + line)
        if not problems:
            print("replay checks passed")
        return 1 if problems else 0
    return 0 if all(r["returncode"] == 0 for r in results.values()) else 1


def check(summary: dict, draws: list[dict], components: dict, mod_first: bool = False) -> list[str]:
    problems = []
    runs = summary["runs"]
    if mod_first:
        counters = runs.get("enabled", {}).get("counters", {})
        for name in ("kept_original", "original_before_shown"):
            if counters.get(name):
                problems.append("%d draws drew the game's mesh (%s)" % (counters[name], name))
    for name in ("disabled", "enabled"):
        run = runs.get(name, {})
        if run.get("returncode") != 0:
            problems.append("%s run exited %s" % (name, run.get("returncode")))
        if "validation errors=0 " not in run.get("replay", "") or " 0 failed" not in run.get("replay", ""):
            problems.append("%s run: %s" % (name, run.get("replay") or "no summary"))
    if not runs.get("enabled", {}).get("counters", {}).get("replaced"):
        problems.append("no draw replaced")
    # One generation for the Mods folder (D-131): its INI Present runs once per presentation, not once per package.
    folder = runs.get("enabled", {}).get("folder", {})
    if folder and folder.get("ini", {}).get("presents", 0) > folder.get("presents", 0):
        problems.append("the folder generation's Present ran %d times in %d presentations" % (folder["ini"]["presents"], folder["presents"]))
    # Every component a draw's vertices read bones from was imported on the device by then (BUG-054, D-127).
    unavailable = runs.get("enabled", {}).get("counters", {}).get("bones_unavailable", 0)
    if unavailable:
        problems.append("%d draws refused for bones of a component no device import wrote" % unavailable)
    order, drawn = {}, set()
    for d in draws:
        group = order.setdefault(d["group"], len(order))
        if components.get(d["key"]) is not None:
            drawn.add(group)
    for group in sorted(order.values()):
        names = [n for n in runs.get("enabled", {}).get("targets", {}) if n.startswith("group%d-" % group)]
        off = sum(runs.get("disabled", {}).get("targets", {}).get(n, {}).get("covered", 0) for n in names)
        on = sum(runs["enabled"]["targets"][n]["covered"] for n in names)
        if off:
            problems.append("group %d: the original draws cover %d texels" % (group, off))
        if group in drawn and not on:
            problems.append("group %d has a component draw of the Mod and no covered texel" % group)
    return problems


if __name__ == "__main__":
    raise SystemExit(main())
