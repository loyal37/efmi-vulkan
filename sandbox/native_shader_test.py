"""Game shader replacement by SPIR-V hash, through the real layer's pipeline hooks."""
from pathlib import Path
import json
import subprocess
import struct
import sys
import time

from harness import Checks, Sandbox, test_dir
from runtime_fixture import CORE
from runtime_test import sandbox_shader_hashes, band_pixels
from efmi_vk.probe_client import ProbeClient
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compile_game_shader import compile_shader


def compile_pixel(root, color):
    source = root / "replacement.hlsl"
    source.write_text("float4 main() : SV_Target0 { return float4(%s, 1); }\n" % color, encoding="ascii")
    output = compile_shader(source, root / "compiled", "12345678", "ps")
    return output.read_bytes()


def colored_pixels(path, color):
    meta_path = Path(str(path) + ".json")
    if not path.exists() or not meta_path.exists():
        return 0
    meta = json.loads(meta_path.read_text())
    width, height, data = meta["width"], meta["height"], path.read_bytes()
    return sum(1 for y in range(int(height * .46), int(height * .54)) for x in range(width)
               if all(abs(data[4 * (y * width + x) + i] - color[i]) < 8 for i in range(3)))


def main():
    checks = Checks("native game shaders")
    selected = set(sys.argv[1:])
    compiler_root = Path(test_dir("native-compiler-"))
    good = compile_pixel(compiler_root, "1, 0, 1")
    source = compiler_root / "replacement.hlsl"
    source.write_text("not a shader", encoding="ascii")
    rejected = False
    try: compile_shader(source, compiler_root / "compiled", "12345678", "ps")
    except RuntimeError: rejected = True
    checks.check(rejected and (compiler_root / "compiled/12345678-ps_replace.spv").read_bytes() == good,
                 "failed HLSL compilation preserves the published last good shader")
    vertex, fragment = sandbox_shader_hashes()
    for entry, case in (("layer", "pixel"), ("inject", "pixel"), ("layer", "empty"),
                        ("layer", "vertex"), ("layer", "interface"), ("layer", "stage"), ("layer", "skip"), ("layer", "image_type")):
        if selected and case not in selected: continue
        label = entry + " " + case
        root = Path(test_dir("native-shaders-"))
        mods = root / "Mods"
        (mods / "native").mkdir(parents=True)
        if case != "empty":
            (mods / "native" / "mod.ini").write_text("namespace = native_test\n[Constants]\nglobal $loaded = 1\n", encoding="ascii")
        if case == "skip":
            with (mods / "native" / "mod.ini").open("a", encoding="ascii") as ini:
                ini.write("[ShaderOverrideOriginalHash]\nhash = %08x\nhandling = skip\n" % fragment)
        (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\n"
                                        "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
        settings = root / "settings"
        fixes = settings / "ShaderFixes"
        fixes.mkdir(parents=True)
        path = fixes / ("%08x-%s_replace.spv" % (vertex if case == "vertex" else fragment, "vs" if case == "vertex" else "ps"))
        if case in ("vertex", "stage", "interface", "image_type"):
            source = root / "replacement.hlsl"
            code = ("float4 main([[vk::location(0)]] float2 pos : POSITION) : SV_Position { return float4(pos + float2(0,4),0,1); }"
                    if case != "interface" else "float4 main() : SV_Target3 { return float4(1,0,1,1); }")
            if case == "image_type":
                code = ("[[vk::binding(5,1)]] [[vk::combinedImageSampler]] Texture2D<int4> tex;\n"
                        "[[vk::binding(5,1)]] [[vk::combinedImageSampler]] SamplerState samp;\n"
                        "float4 main() : SV_Target0 { return float4(tex.Load(int3(0,0,0))); }\n")
            source.write_text(code, encoding="ascii")
            data = compile_shader(source, root / "compiled", "12345678", "ps" if case in ("interface", "image_type") else "vs").read_bytes()
            if case == "image_type":
                words = list(struct.unpack("<%dI" % (len(data) // 4), data))
                at = 5
                while at < len(words):
                    if words[at] & 65535 == 25: words[at + 4] = 0  # Declare non-depth as the original; change only the sampled scalar type.
                    at += words[at] >> 16
                data = struct.pack("<%dI" % len(words), *words)
            path.write_bytes(data)
        else: path.write_bytes(compile_pixel(root, "1, 0, 1"))
        readback = root / "pixels.raw"
        extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings),
                     EFMI_VK_M2_ENABLED="1", EFMI_VK_TEST_NOTICE="> native shader test",
                     EFMI_VK_TEST_NOTICES_READBACK=str(readback))
        with Sandbox(seconds=5, entry=entry, validation=True, env_extra=extra,
                     extra_args=["--readable-swapchain"]) as sb:
            time.sleep(2)
            with ProbeClient(sb.pid) as probe:
                status = probe.status().get("replacement", {}).get("shader_fixes", {})
            sb.proc.wait(timeout=60)
            checks.equal(sb.proc.returncode, 0, label + ": normal exit")
        if case == "vertex":
            checks.equal(band_pixels(readback), 0, label + ": replacement vertex shader moves every mesh off screen")
        else:
            checks.equal(colored_pixels(readback, (255, 0, 255)) > 200, case not in ("interface", "stage", "skip", "image_type"),
                         label + ": replacement image or original fallback as expected")
        if case in ("interface", "stage", "image_type"):
            checks.check((band_pixels(readback) or 0) > 200, label + ": the game's original meshes still draw")
        if case == "skip": checks.equal(band_pixels(readback), 0, label + ": ShaderOverride still matches the original hash after replacement")
        checks.equal(status.get("loaded", 0), 0 if case == "stage" else 1, label + ": expected validated files")
        checks.equal(status.get("binds", 0) > 0, case not in ("interface", "stage", "image_type"),
                     label + ": compatible replacement pipeline bound: " + str(status))
        checks.check("Validation Error" not in sb.output() and "VUID-" not in sb.output(), label + ": Vulkan validation clean")
        print("artifacts:", root)
    # The existing readback is one shot. These processes show their first notice only after F10,
    # so their first image proves what was drawn after the replacement changed or disappeared.
    for change in ("reload", "invalid", "interface", "removed", "unchanged"):
        if selected and "f10:" + change not in selected: continue
        root = Path(test_dir("native-shader-" + change + "-"))
        mods = root / "Mods"
        (mods / "native").mkdir(parents=True)
        (mods / "native" / "mod.ini").write_text("namespace = native_test\n[Constants]\nglobal $loaded = 1\n", encoding="ascii")
        (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\n"
                                        "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
        settings = root / "settings"
        fixes = settings / "ShaderFixes"
        fixes.mkdir(parents=True)
        path = fixes / ("%08x-ps_replace.spv" % fragment)
        path.write_bytes(compile_pixel(root, "1, 0, 1"))
        readback = root / "pixels.raw"
        trigger = root / "capture.trigger"
        keys = root / "keys.txt"
        keys.write_text("1\n", encoding="ascii")
        extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings),
                     EFMI_VK_M2_ENABLED="1", EFMI_VK_TEST_KEYS=str(keys), EFMI_VK_TEST_NOTICES_READBACK=str(readback),
                     EFMI_VK_TEST_NOTICES_READBACK_TRIGGER=str(trigger))
        with Sandbox(seconds=40, entry="layer", validation=True, env_extra=extra,
                     extra_args=["--readable-swapchain"]) as sb:
            with ProbeClient(sb.pid) as probe:
                time.sleep(1)
                before = probe.status().get("replacement", {}).get("shader_fixes", {})
                if change == "reload": path.write_bytes(compile_pixel(root, "0, 1, 0"))
                elif change == "invalid": path.write_bytes(b"invalid shader")
                elif change == "interface":
                    source = root / "replacement.hlsl"
                    source.write_text("float4 main() : SV_Target3 { return float4(0,1,0,1); }", encoding="ascii")
                    path.write_bytes(compile_shader(source, root / "compiled", "12345678", "ps").read_bytes())
                elif change == "removed": path.unlink()
                keys.write_text("1 121\n", encoding="ascii")
                time.sleep(0.5)
                keys.write_text("1\n", encoding="ascii")
                deadline = time.monotonic() + 20
                after = {}
                while time.monotonic() < deadline:
                    status = probe.status().get("replacement", {})
                    after = status.get("shader_fixes", {})
                    if status.get("folder_generation", {}).get("reloads", 0) >= 1:
                        if not trigger.exists():
                            time.sleep(.5)  # Drain the already recorded presentation frames before asking for the image.
                            trigger.touch()
                        if readback.is_file(): break
                    time.sleep(.1)
                checks.check(before.get("loaded") == 1 and before.get("binds", 0) > 0, change + ": initial replacement active")
                if change == "removed":
                    checks.equal(after.get("loaded"), 0, "removed: F10 restores the game's shader")
                    checks.equal(colored_pixels(readback, (255, 0, 255)), 0, "removed: replacement color gone after F10")
                else:
                    color = (0, 255, 0) if change == "reload" else (255, 0, 255)
                    checks.check(colored_pixels(readback, color) > 200, change + ": correct post-F10 image")
                    checks.equal(after.get("loaded"), 1, change + ": one shader remains active")
                    checks.equal(after.get("revision") != before.get("revision"), change == "reload",
                                 change + ": changed code alone advances the shader revision")
                if change in ("invalid", "interface"): checks.check(bool(after.get("warnings")), change + ": bad shader reported while last good shader remains")
            sb.proc.terminate(); sb.proc.wait(timeout=30)
        checks.check("Validation Error" not in sb.output() and "VUID-" not in sb.output(), change + ": Vulkan validation clean")
        print("artifacts:", root)
    return checks.finish()


if __name__ == "__main__":
    raise SystemExit(main())
