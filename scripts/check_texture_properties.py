"""Check the texture properties the layer answers (D-170) against the DX11 side of the paired catalogue.

    python scripts/check_texture_properties.py [--catalogue DIR] [--dist DIR]

A Mod's `ps-t3->Format`, `->Width`, `->Height`, `->Mips` and `->Array` read the game's texture at a pixel
slot as the original's getters take them off the D3D11 resource and its view (CommandList.cpp,
GetResourceFormat and its neighbours). The layer answers from the Vulkan image and view the draw binds at
the register's binding (layer/src/character_backend_materials.cpp, pixel_texture; the format through
layer/src/texture_identity.cpp, which `efmi_vk_character_test --view-formats` prints).

Here every paired draw of the catalogue (the same scene captured under DX11 and under Vulkan) is gone
through: for each pixel slot its DX11 shader declares and binds, the original's five answers from the DX11
capture, and the layer's from what the Vulkan draw binds at the binding the register rule gives (D-076).

- Where both sides hold the same texture (the CRC-32C of mip 0 is equal), all five answers must be equal.
- Elsewhere the two captures are two sessions: the same slot of the same draw holds the same kind of
  texture (a render target, a shadow map, a placeholder), so the format must be equal; sizes may differ
  with the render size and are counted only. The slots where the two sessions bound different textures
  are listed in KNOWN with why; any other difference fails.
- A view format the layer does not map on such a slot fails: the query would refuse the draw.

Read-only on the catalogue and on the sibling project's capture reader.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
from efmi_vk import source_slots  # noqa: E402

SIBLING = r"F:\EFMI MCP\server"   # the DX11 capture reader (read-only)

# DXGI_FORMAT by number, as dxgiformat.h lists them.
DXGI = ("UNKNOWN R32G32B32A32_TYPELESS R32G32B32A32_FLOAT R32G32B32A32_UINT R32G32B32A32_SINT R32G32B32_TYPELESS "
        "R32G32B32_FLOAT R32G32B32_UINT R32G32B32_SINT R16G16B16A16_TYPELESS R16G16B16A16_FLOAT R16G16B16A16_UNORM "
        "R16G16B16A16_UINT R16G16B16A16_SNORM R16G16B16A16_SINT R32G32_TYPELESS R32G32_FLOAT R32G32_UINT R32G32_SINT "
        "R32G8X24_TYPELESS D32_FLOAT_S8X24_UINT R32_FLOAT_X8X24_TYPELESS X32_TYPELESS_G8X24_UINT R10G10B10A2_TYPELESS "
        "R10G10B10A2_UNORM R10G10B10A2_UINT R11G11B10_FLOAT R8G8B8A8_TYPELESS R8G8B8A8_UNORM R8G8B8A8_UNORM_SRGB "
        "R8G8B8A8_UINT R8G8B8A8_SNORM R8G8B8A8_SINT R16G16_TYPELESS R16G16_FLOAT R16G16_UNORM R16G16_UINT R16G16_SNORM "
        "R16G16_SINT R32_TYPELESS D32_FLOAT R32_FLOAT R32_UINT R32_SINT R24G8_TYPELESS D24_UNORM_S8_UINT "
        "R24_UNORM_X8_TYPELESS X24_TYPELESS_G8_UINT R8G8_TYPELESS R8G8_UNORM R8G8_UINT R8G8_SNORM R8G8_SINT R16_TYPELESS "
        "R16_FLOAT D16_UNORM R16_UNORM R16_UINT R16_SNORM R16_SINT R8_TYPELESS R8_UNORM R8_UINT R8_SNORM R8_SINT A8_UNORM "
        "R1_UNORM R9G9B9E5_SHAREDEXP R8G8_B8G8_UNORM G8R8_G8B8_UNORM BC1_TYPELESS BC1_UNORM BC1_UNORM_SRGB BC2_TYPELESS "
        "BC2_UNORM BC2_UNORM_SRGB BC3_TYPELESS BC3_UNORM BC3_UNORM_SRGB BC4_TYPELESS BC4_UNORM BC4_SNORM BC5_TYPELESS "
        "BC5_UNORM BC5_SNORM B5G6R5_UNORM B5G5R5A1_UNORM B8G8R8A8_UNORM B8G8R8X8_UNORM R10G10B10_XR_BIAS_A2_UNORM "
        "B8G8R8A8_TYPELESS B8G8R8A8_UNORM_SRGB B8G8R8X8_TYPELESS B8G8R8X8_UNORM_SRGB BC6H_TYPELESS BC6H_UF16 BC6H_SF16 "
        "BC7_TYPELESS BC7_UNORM BC7_UNORM_SRGB").split()
DXGI_NUMBER = {name: number for number, name in enumerate(DXGI)}

# Slots of paired draws that are not the same draw after all, by the DX11 pixel shader and slot: (the
# original's format, the layer's). The catalogue pairs draws by their index region and target count; these
# two effect shaders draw one mesh in passes that differ between the two sessions (a 2048 x 1024 BC7 and a
# 4 x 4 placeholder under DX11 where Vulkan holds a 4 x 4 placeholder and a 2048 x 4096 R8), no texture of
# either side has the other's content, and the two builds' sample lists differ as well. Not a difference of
# the rule.
KNOWN = {
    ("b08d318ffeac2dbe", "ps-t0"): (98, 29), ("b08d318ffeac2dbe", "ps-t2"): (29, 61),
    ("5bde6fd4e9643cb5", "ps-t0"): (98, 29), ("5bde6fd4e9643cb5", "ps-t2"): (29, 61),
}


def layer_formats(dist: Path) -> dict[tuple[int, int], int]:
    """(VkFormat, aspect) -> the DXGI view format the layer answers, from the layer's own table."""
    exe = dist / "efmi_vk_character_test.exe"
    run = subprocess.run([str(exe), "--view-formats"], capture_output=True, text=True, timeout=120)
    if run.returncode:
        raise RuntimeError("efmi_vk_character_test --view-formats failed: " + (run.stdout + run.stderr)[-400:])
    table = {}
    for line in run.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            table[(int(parts[0]), int(parts[1]))] = int(parts[2])
    if not table:
        raise RuntimeError("efmi_vk_character_test --view-formats printed no table")
    return table


def original_answers(res: dict, view: dict) -> tuple | None:
    """Format, Width, Height, Mips, Array as the original's getters read a texture behind a view."""
    kind = res.get("kind")
    if kind not in ("tex1d", "tex2d", "tex3d"):
        return None
    name = view.get("format")
    if name in (None, "UNKNOWN"):
        name = res.get("fmt_name")
    fmt = DXGI_NUMBER.get(name, -1)
    height = 1 if kind == "tex1d" else res.get("h")
    array = -4 if kind == "tex3d" else res.get("array")
    return (fmt, res.get("w"), height, res.get("mips"), array)


def layer_answers(element: dict, formats: dict) -> tuple:
    """The same from the Vulkan element, as pixel_texture fills RuntimeTexture and texture_property reads it."""
    image_type = element.get("image_type")
    aspect = (element.get("subresource") or {}).get("aspect")
    fmt = formats.get((element.get("format"), aspect), 0)
    height = 1 if image_type == 0 else element["extent"][1]
    array = -4 if image_type == 2 else element.get("array_layers")
    return (fmt, element["extent"][0], height, element.get("mip_levels"), array)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalogue", type=Path, default=Path("F:/efmi vulkan/captures/catalogue-20261019"))
    ap.add_argument("--dist", type=Path, default=Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist")))
    args = ap.parse_args()
    sys.path.insert(0, SIBLING)
    from efmi_mcp import capture_model  # noqa: E402
    formats = layer_formats(args.dist)
    counts = Counter()
    by_format = Counter()
    failures, known_seen = [], Counter()
    declarations = {}
    for summary_path in sorted((args.catalogue / "dx11").glob("*.summary.json")):
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        label = summary["dx11"]["label"]
        base = args.catalogue / "dx11" / label
        cap = capture_model.Capture(json.loads(base.with_suffix(".json").read_text(encoding="utf-8")), base.with_suffix(".bin").read_bytes())
        shaders = json.loads(Path(str(base) + ".shaders.json").read_text(encoding="utf-8"))
        contents = json.loads(Path(str(base) + ".textures.json").read_text(encoding="utf-8"))
        dx_draws = {d["index"]: d for d in cap.draws()}
        bundle = args.catalogue / "vulkan" / Path(summary["vulkan"]["bundle"]).name
        vk_draws = {d["n"]: d for d in json.loads((bundle / "frame.json").read_text(encoding="utf-8"))["draws"]}
        modules = {s["hash"]: bundle / s["file"] for s in json.loads((bundle / "shaders.json").read_text(encoding="utf-8")) if s.get("file")}
        for obj in summary["objects"]:
            for pair in obj["pairs"]:
                dx, vk, fs = dx_draws.get(pair["dx_draw"]), vk_draws.get(pair["vk_draw"]), pair.get("vk_fs")
                if not dx or not vk or fs not in modules:
                    counts["pairs without both draws"] += 1
                    continue
                counts["paired draws"] += 1
                if fs not in declarations:
                    declarations[fs] = source_slots.declarations(modules[fs].read_bytes())
                ps = dx["shaders"].get("ps")
                declared = (shaders.get(ps) or {}).get("t")
                for slot, bound in (dx.get("tex") or {}).items():
                    if not slot.startswith("ps-t") or int(slot[4:]) >= 100:
                        continue
                    number = int(slot[4:])
                    if declared is not None and number not in declared:
                        continue          # left bound by an earlier draw; the shader does not read it
                    res = cap.resource(bound["res"]) or {}
                    where = source_slots.binding_of(declarations[fs], "t%d" % number)
                    if where is None:
                        counts["slots the SPIR-V has no register for"] += 1
                        continue
                    element, descriptor = None, None
                    sets = vk.get("sets") or []
                    if where[0] < len(sets):
                        for binding in (sets[where[0]].get("contents") or {}).get("bindings") or []:
                            if binding["binding"] == where[1] and binding.get("elements"):
                                element, descriptor = binding["elements"][0], binding["type"]
                    original = original_answers(res, bound.get("view") or {})
                    if original is None:
                        # A buffer behind the slot: the layer answers "not a texture" for a buffer register.
                        counts["buffers" if descriptor in (4, 7) else "buffers at another kind of descriptor"] += 1
                        if descriptor not in (4, 7):
                            failures.append("%s draw %s %s: a DX11 buffer where Vulkan binds descriptor type %s" % (label, pair["dx_draw"], slot, descriptor))
                        continue
                    if element is None or not element.get("image_known"):
                        counts["textures the Vulkan capture does not describe"] += 1
                        continue
                    layer = layer_answers(element, formats)
                    dx_content = (contents.get(bound.get("hash")) or {}).get("crc32c")
                    held = element.get("content") or {}
                    same = bool(dx_content) and held.get("state") == "hashed" and held.get("crc") == dx_content
                    where_text = "%s draw %s %s (ps %s)" % (label, pair["dx_draw"], slot, ps)
                    if same:
                        counts["slots with the same texture"] += 1
                        by_format[DXGI[original[0]] if original[0] >= 0 else "?"] += 1
                        if layer != original:
                            failures.append("%s: the same texture reads %s under the original and %s in the layer" % (where_text, original, layer))
                        continue
                    counts["slots of the same draw, other content"] += 1
                    if layer[0] == original[0]:
                        by_format[DXGI[original[0]]] += 1
                        counts["  of them with the size, levels and layers equal too" if layer == original else "  of them with another size (two sessions)"] += 1
                    elif KNOWN.get((ps, slot)) == (original[0], layer[0]):
                        known_seen[(ps, slot)] += 1
                    elif not layer[0]:
                        failures.append("%s: the layer does not map Vulkan view format %s (aspect %s); the original reads %s" % (
                            where_text, element.get("format"), (element.get("subresource") or {}).get("aspect"), DXGI[original[0]]))
                    else:
                        failures.append("%s: the original reads format %s, the layer %s" % (where_text, original[0], layer[0]))
    for key in KNOWN:
        if not known_seen[key]:
            failures.append("known difference %s %s was not met: take it off KNOWN" % key)
    for name, n in counts.items():
        print("%6d %s" % (n, name))
    print("       formats that agreed: " + ", ".join("%s %d" % item for item in sorted(by_format.items())))
    if known_seen:
        print("%6d slots where the two sessions bound different textures (%d shader slots, listed in KNOWN)" % (sum(known_seen.values()), len(known_seen)))
    shown = sorted(set(failures))
    for line in shown[:20]:
        print("FAIL " + line)
    if len(shown) > 20:
        print("FAIL ... and %d more" % (len(shown) - 20))
    print("texture properties against the paired catalogue: %s" % ("FAILED" if failures else "passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
