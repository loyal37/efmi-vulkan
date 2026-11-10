"""The original as the judge (D-137 step 2, D-150).

Runs small INIs under the pinned XXMI 3DMigoto (d3d11.dll from F:\\vulkan\\XXMI-PACKAGE-v1.2.2.zip, the
version the user's EFMI installation carries) in a D3D11 program of the project's own, and the same INIs
under the layer's owned GPU runtime (resource_gpu.exe --present-ini), and compares what each case leaves in
its persistent variables. A case's [Present] runs commands and puts their results in `global persist`
variables; the original saves them to d3dx_user.ini, the layer's runtime prints them.

The user's installation is never touched: the DLL is extracted into build/dx11-judge/xxmi with a d3dx.ini of
the judge's own whose [Loader] names the judge program.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

from build import COMMON, find_vcvars, find_vulkan_include, run_cl

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(r"F:\vulkan\XXMI-PACKAGE-v1.2.2.zip")
OUT = ROOT / "build" / "dx11-judge"
CASES = ROOT / "sandbox" / "judge_cases"

D3DX = """[Loader]
target = efmi_vk_dx11_judge.exe
[Logging]
calls = 0
input = 0
debug = 0
[Include]
include_recursive = Mods
[System]
allow_check_interface = 1
allow_create_device = 1
allow_platform_update = 1
skip_early_includes_load = 1
config_initialization_delay = 0
settings_auto_save_interval = 1
check_foreground_window = 0
[Device]
[Stereo]
automatic_mode = 0
[Rendering]
[Hunting]
hunting = 0
"""


def build_host() -> Path:
    exe = OUT / "efmi_vk_dx11_judge.exe"
    source = ROOT / "sandbox" / "dx11_judge.cpp"
    scene = ROOT / "sandbox" / "dx11_judge_scene.h"
    if not exe.is_file() or exe.stat().st_mtime < max(source.stat().st_mtime, scene.stat().st_mtime):
        run_cl(find_vcvars(), str(OUT), COMMON + ["/Fo" + str(OUT) + "/", "/Fd" + str(OUT / "judge.pdb"), str(source),
                                                  "/Fe:" + str(exe), "/link", "/NOLOGO", "d3d11.lib", "dxgi.lib", "user32.lib", "d3dcompiler.lib"])
    return exe


def prepare_original() -> Path:
    folder = OUT / "xxmi"
    folder.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(PACKAGE) as z:
        for name in ("d3d11.dll", "d3dcompiler_47.dll"):
            target = folder / name
            if not target.is_file():
                target.write_bytes(z.read(name))
    (folder / "d3dx.ini").write_text(D3DX, encoding="utf-8")
    return folder


def texel(x: int, y: int) -> tuple[int, int, int, int]:
    """The judge's 4x4 test image (images.ini states the same rule)."""
    return 60 * x + 10, 70 * y + 5, 255 - 30 * x - 20 * y, 100 + 10 * x + 30 * y


def dds_rgba8() -> bytes:
    """A 4x4 R8G8B8A8_UNORM DDS with the legacy header (masks), as an RGBA8 file is usually written."""
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8, 4, 4, 16, 0, 1)
    pixel_format = struct.pack("<2I4s5I", 32, 0x1 | 0x40, bytes(4), 32, 0xff, 0xff00, 0xff0000, 0xff000000)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    body = bytes(c for y in range(4) for x in range(4) for c in texel(x, y))
    return header + pixel_format + caps + body


def png(alpha: bool) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(bytes(1) + bytes(c for x in range(4) for c in texel(x, y)[:4 if alpha else 3]) for y in range(4))
    return (bytes([0x89]) + b"PNG" + bytes([0x0d, 0x0a, 0x1a, 0x0a]) + chunk(b"IHDR", struct.pack(">2I5B", 4, 4, 8, 6 if alpha else 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def encoding_files() -> dict[str, bytes]:
    """INI files beside encoding.ini: one with a UTF-8 byte order mark and CRLF line ends; one with a byte
    sequence that is not UTF-8 on a comment line (a file saved in a legacy code page), a variable set
    before that line and one after it."""
    bom = "namespace = judge_bom\r\n[Constants]\r\nglobal persist $crlf = 0\r\n[Present]\r\n$crlf = 1\r\n"
    invalid = (b"namespace = judge_invalid\n[Constants]\nglobal persist $before = 0\nglobal persist $after = 0\n"
               b"[Present]\n$before = 1\n; \xb4\xf3 a comment in a legacy code page\n$after = 1\n")
    return {"encoding.bom.ini": b"\xef\xbb\xbf" + bom.encode("ascii"), "encoding.invalid.ini": invalid}


# Files a case reads that the judge writes rather than the repository keeping binaries.
GENERATED = {"images": lambda: {"images.dds": dds_rgba8(), "images.png": png(True), "images.rgb.png": png(False)},
             "encoding": encoding_files}


def stage(case: Path, mods: Path) -> None:
    """The case and its files (<stem>.*) into a Mods folder."""
    shutil.copy(case, mods / case.name)
    for extra in case.parent.glob(case.stem + ".*"):
        if extra.suffix != ".ini":
            shutil.copy(extra, mods / extra.name)
    for name, data in GENERATED.get(case.stem, dict)().items():
        (mods / name).write_bytes(data)


def persisted(text: str) -> dict[str, float]:
    """`$\\namespace\\name = value` lines of a d3dx_user.ini."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"\s*(\$[^=\s]+)\s*=\s*([-+0-9.eE]+)\s*$", line)
        if m:
            out[m.group(1).lower()] = float(m.group(2))
    return out


def run_original(exe: Path, folder: Path, case: Path, frames: int) -> dict[str, float]:
    mods = folder / "Mods"
    shutil.rmtree(mods, ignore_errors=True)
    mods.mkdir()
    stage(case, mods)
    user = folder / "d3dx_user.ini"
    user.unlink(missing_ok=True)
    run = subprocess.run([str(exe), str(folder), str(frames)], cwd=folder, capture_output=True, text=True, timeout=300)
    if run.returncode:
        raise RuntimeError("the original's program failed: " + (run.stdout + run.stderr)[-800:])
    return persisted(user.read_text(encoding="utf-8", errors="replace")) if user.is_file() else {}


def run_layer(case: Path, frames: int) -> dict[str, float]:
    exe = ROOT / "build" / "resource-commands" / "resource_gpu.exe"
    if not exe.is_file():
        raise RuntimeError("build the layer's GPU resource runtime first (scripts/check_resource_commands.py)")
    work = OUT / "layer" / case.stem
    shutil.rmtree(work, ignore_errors=True)
    (work / "Mods").mkdir(parents=True)
    stage(case, work / "Mods")
    # The test's own child process, with the validation layer as check_resource_commands.py runs it.
    sys.path.insert(0, str(ROOT / "sandbox"))
    from harness import inject_env
    env = inject_env(str(work / "unexpected-injection.log"))
    validation = Path(os.environ.get("EFMI_VK_VALIDATION_DIR", "F:/vulkan/validation-1.4.321.0/bin"))
    env.update(VK_LAYER_PATH=str(validation), VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0",
               XDG_CACHE_HOME=str(work), TMPDIR=str(work), TEMP=str(work), TMP=str(work))
    run = subprocess.run([str(exe), "--present-ini", str(work), str(frames)], cwd=work, env=env, capture_output=True, text=True,
                         timeout=300)
    values = {}
    for line in run.stdout.splitlines():
        if line.startswith("{"):
            values = {k.lower(): float(v) for k, v in json.loads(line).items()}
    if run.returncode or not values:
        raise RuntimeError("the layer's runtime failed: " + (run.stdout + run.stderr)[-800:])
    return values


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--only", help="a case name")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    exe = build_host()
    folder = prepare_original()
    shutil.copy(exe, folder / exe.name)
    failed = 0
    for case in sorted(CASES.glob("*.ini")):
        if args.only and case.stem != args.only:
            continue
        original = run_original(folder / exe.name, folder, case, args.frames)
        try:
            layer = run_layer(case, args.frames)
        except RuntimeError as e:
            print("FAIL %s: %s" % (case.stem, e)); failed += 1; continue
        expected = {k: v for k, v in original.items()}
        differ = {k: (v, layer.get(k)) for k, v in expected.items() if layer.get(k) is None or abs(layer[k] - v) > 1e-4 * max(1, abs(v))}
        if not expected:
            print("FAIL %s: the original saved no variable" % case.stem); failed += 1
        elif differ:
            print("FAIL %s: original vs layer %s" % (case.stem, differ)); failed += 1
        else:
            print("ok   %s: %d values as the original's" % (case.stem, len(expected)))
    print("dx11 judge: %s" % ("FAILED" if failed else "passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
