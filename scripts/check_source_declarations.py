"""Check the DX11 declarations read off SPIR-V (D-124) against the DXBC of the paired catalogue.

    python scripts/check_source_declarations.py [--catalogue DIR] [--dist DIR]

A Mod's `vs->srv_stride(0)`, `srv_type` and `srv_dimension` read the declarations of the game's
shader as the original's ByteCodeReader takes them off its DXBC (dcl_resource, dcl_resource_raw,
dcl_resource_structured). The layer answers them from the shader's SPIR-V by the register rule
(frontend/gpu/source_registers.cpp, `efmi_vk_character_test --srv-declarations`). Here every answer
is compared with the DX11 declaration of the same register in the paired scenes: the shader pairs
of docs/source-slot-truth.json, the SPIR-V of catalogue-20261019/vulkan, the DXBC of
catalogue-20261019/dx11. Fails on any answer that differs from a register DX11 declares, and when
a kind of resource the rule leaves undecided turns out to be decided after all. Registers only the
SPIR-V build declares are listed (they read as declared).

A slot below t60 that the SPIR-V has no register for is answered "not declared" (D-159). Here every such
slot of every pair is compared with the DXBC: the DX11 build must declare none there either. The pairs
where it does are listed; the catalogue has three (one pixel register, t3, of two shaders), and the check
fails when there are more. From t60 up the DX11 builds declare registers the SPIR-V lacks, and the rule
does not answer. Read-only on the catalogue.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRUTH = ROOT / "docs" / "source-slot-truth.json"

# D3D10_SB_RESOURCE_DIMENSION to D3D_SRV_DIMENSION, as ByteCodeReader's dxbc_dimension_to_srv.
EXPLICIT_REGISTERS = 60   # frontend/gpu/source_registers.h, kSourceExplicitRegisters
KNOWN_ABSENT_DECLARED = 3  # pairs whose DX11 build declares a register below t60 that the SPIR-V lacks (2026-11-10)
SRV_DIMENSION = {1: 1, 2: 2, 3: 4, 4: 6, 5: 8, 6: 9, 7: 3, 8: 5, 9: 7, 10: 10, 11: 11, 12: 1}
TYPED, STRUCTURED, RAW = 1, 2, 3


def dxbc_resources(data: bytes) -> dict[int, tuple[int, int, int]]:
    """The t declarations of a DXBC container: slot -> (type, dimension, stride)."""
    if data[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    out = {}
    for i in range(struct.unpack_from("<I", data, 0x1C)[0]):
        offset = struct.unpack_from("<I", data, 0x20 + 4 * i)[0]
        if data[offset:offset + 4] not in (b"SHDR", b"SHEX"):
            continue
        count = struct.unpack_from("<I", data, offset + 12)[0]
        tokens = struct.unpack_from("<%dI" % count, data, offset + 8)
        p = 2
        while p < count:
            token = tokens[p]
            op = token & 0x7FF
            length = tokens[p + 1] if op == 53 else (token >> 24) & 0x7F   # 53: custom data, length in the next word
            if not length:
                raise ValueError("malformed DXBC instruction")
            if op == 88:      # dcl_resource
                out[tokens[p + 2]] = (TYPED, SRV_DIMENSION.get((token >> 11) & 0x1F, 0), 0)
            elif op == 161:   # dcl_resource_raw
                out[tokens[p + 2]] = (RAW, 11, 0)
            elif op == 162:   # dcl_resource_structured
                out[tokens[p + 2]] = (STRUCTURED, 1, tokens[p + 3])
            p += length
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalogue", type=Path, default=Path("F:/efmi vulkan/captures/catalogue-20261019"))
    ap.add_argument("--dist", type=Path, default=Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist")))
    args = ap.parse_args()
    truth = json.loads(TRUTH.read_text(encoding="utf-8"))
    spirv, dxbc = {}, {}
    for bundle in sorted((args.catalogue / "vulkan").glob("vk*")):
        for s in json.loads((bundle / "shaders.json").read_text(encoding="utf-8")):
            if s.get("file"):
                spirv.setdefault(s["hash"], bundle / s["file"])
    for folder in sorted((args.catalogue / "dx11").glob("char*.dxbc")):
        for f in folder.glob("*.dxbc"):
            dxbc.setdefault(f.stem, f)
    pairs = [(vk, dx, entry["stage"]) for vk, entry in truth["shaders"].items() if vk in spirv for dx in entry["dx"] if dx in dxbc]
    names = sorted({vk for vk, _, _ in pairs})
    tool = args.dist / "efmi_vk_character_test.exe"
    answers = {}
    for chunk in range(0, len(names), 64):
        part = names[chunk:chunk + 64]
        out = subprocess.run([str(tool), "--srv-declarations"] + [str(spirv[h]) for h in part], capture_output=True, text=True, check=True).stdout
        for h, line in zip(part, out.splitlines()):
            fields = line[len(str(spirv[h])):].split()
            if fields and fields[0] == "refused":
                answers[h] = None
                continue
            slots = {}
            for item in fields[1:]:
                slot, value = item.split(":")
                slots[int(slot)] = None if value == "-" else tuple(int(x) for x in value.split("."))
                # Type 0: a slot the SPIR-V has no register for, answered as not declared (compared below).
                if slots[int(slot)] == (0, 0, 0):
                    del slots[int(slot)]
            answers[h] = slots
    stats, problems, only_spirv, undecided, absent_declared = Counter(), [], [], Counter(), []
    for vk, dx, stage in pairs:
        slots = answers.get(vk)
        if slots is None:
            stats["modules the rule refuses"] += 1
            continue
        declared = dxbc_resources(dxbc[dx].read_bytes())
        stats["pairs " + stage] += 1
        for slot, answer in sorted(slots.items()):
            want = declared.get(slot)
            if answer is None:
                undecided[want[0] if want else 0] += 1
            elif want is None:
                only_spirv.append("%s %s (DX11 %s) t%d" % (stage, vk, dx, slot))
            elif answer != want:
                problems.append("%s %s t%d: answered %s, DX11 %s declares %s" % (stage, vk, slot, answer, dx, want))
            else:
                stats["answers equal to DX11"] += 1
        stats["DX11 registers beyond the SPIR-V's"] += len([s for s in declared if s not in slots])
        for slot in range(EXPLICIT_REGISTERS):
            if slot in slots:
                continue
            if slot in declared:
                absent_declared.append("%s %s (DX11 %s) t%d declared %s" % (stage, vk, dx, slot, declared[slot]))
            else:
                stats["slots without a register that DX11 does not declare either"] += 1
    if len(absent_declared) > KNOWN_ABSENT_DECLARED:
        problems.append("%d slots below t%d answered as not declared are declared by DX11 (%d recorded)" %
                        (len(absent_declared), EXPLICIT_REGISTERS, KNOWN_ABSENT_DECLARED))
    # The undecided registers are buffers of uint words; DX11 declares both kinds there.
    if undecided and not (undecided[RAW] and undecided[STRUCTURED]):
        problems.append("undecided registers are all of one kind %s: the rule could decide them" % dict(undecided))
    if not stats["answers equal to DX11"]:
        problems.append("no answer compared: catalogue or tool missing")
    print("srv declarations: %d pairs (%d vertex, %d pixel), %d answers equal to DX11, undecided by DX11 kind %s, "
          "%d DX11 registers beyond the SPIR-V's, %d registers only the SPIR-V build declares" % (
              stats["pairs vs"] + stats["pairs ps"], stats["pairs vs"], stats["pairs ps"], stats["answers equal to DX11"],
              dict(undecided), stats["DX11 registers beyond the SPIR-V's"], len(only_spirv)))
    print("srv declarations: %d slots below t%d without a register are not declared by DX11 either; %d are (answered as not declared)" %
          (stats["slots without a register that DX11 does not declare either"], EXPLICIT_REGISTERS, len(absent_declared)))
    for line in absent_declared:
        print("  DX11 declares where the SPIR-V has no register: " + line)
    for line in only_spirv:
        print("  only the SPIR-V build declares: " + line)
    for line in problems:
        print("CHECK FAILED: " + line)
    if not problems:
        print("srv declaration checks passed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
