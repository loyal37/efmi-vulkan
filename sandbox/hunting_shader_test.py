"""Selected native shaders and buffer/target draws: original/pink, restoration and real validation."""
from pathlib import Path
import sys
import time
import struct

from harness import Checks, Sandbox, test_dir
from runtime_fixture import CORE
from runtime_test import sandbox_shader_hashes, band_pixels
from native_shader_test import compile_pixel, compile_shader, colored_pixels
from efmi_vk.probe_client import ProbeClient
from efmi_vk.hashes import crc32c


def main():
    checks = Checks("hunting shader modes")
    vertex, fragment = sandbox_shader_hashes()
    selected = set(sys.argv[1:])
    for case in ("original_pixel", "original_vertex", "pink_pixel", "pink_index", "pink_target", "pink_clear", "pink_mesh", "pink_clear_mesh", "pink_direct", "pink_indirect"):
        if selected and case not in selected: continue
        root = Path(test_dir("hunting-" + case + "-"))
        mods = root / "Mods"
        (mods / "native").mkdir(parents=True)
        (mods / "native/mod.ini").write_text("namespace = native_test\n[Constants]\nglobal $loaded = 1\n", encoding="ascii")
        if case.endswith("mesh"):
            folder = mods / "native"
            # The last application's mesh uses the persistently mapped index buffer, as the replacement path requires.
            (folder / "vertex.buf").write_bytes(struct.pack("<6f", .70, .10, .90, .10, .80, -.10))
            (folder / "index.buf").write_bytes(struct.pack("<3H", 0, 1, 2))
            (folder / "mod.ini").write_text(
                "namespace = hunting_mesh\n[TextureOverrideMesh]\nhash = %08x\nmatch_first_index = 6\nmatch_index_count = 3\n"
                "handling = skip\nvb0 = ResourceVertex\nib = ResourceIndex\ndrawindexed = 3,0,0\n"
                "[ResourceVertex]\ntype = Buffer\nstride = 8\nfilename = vertex.buf\n"
                "[ResourceIndex]\ntype = Buffer\nformat = R16_UINT\nfilename = index.buf\n" % crc32c(struct.pack("<3I", 14, 15, 16)), encoding="ascii")
        (root / "d3dx.ini").write_text(
            "[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\n[Rendering]\nrecursive_include = 1\n"
            "[Hunting]\nhunting = 1\nmarking_mode = " + case.split("_")[0] + "\n"
            "next_pixelshader = no_modifiers VK_NUMPAD2\nnext_vertexshader = no_modifiers VK_NUMPAD5\n"
            "next_indexbuffer = no_modifiers VK_NUMPAD8\nnext_rendertarget = no_modifiers VK_NUMPAD7\n"
            "done_hunting = no_modifiers VK_ADD\n", encoding="ascii")
        settings = root / "settings"
        fixes = settings / "ShaderFixes"
        fixes.mkdir(parents=True)
        if case.startswith("original"):
            (fixes / ("%08x-ps_replace.spv" % fragment)).write_bytes(compile_pixel(root, "1, 0, 1"))
        if case == "original_vertex":
            source = root / "vertex.hlsl"
            source.write_text("float4 main([[vk::location(0)]] float2 p : POSITION) : SV_Position { return float4(p + float2(0,4),0,1); }", encoding="ascii")
            compile_shader(source, fixes, "%08x" % vertex, "vs")
        keys = root / "keys.txt"
        keys.write_text("1\n", encoding="ascii")
        readback, trigger = root / "pixels.raw", root / "capture.trigger"
        extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings),
                     EFMI_VK_M2_ENABLED="1", EFMI_VK_TEST_KEYS=str(keys), EFMI_VK_TEST_NOTICES_READBACK=str(readback),
                     EFMI_VK_TEST_NOTICES_READBACK_TRIGGER=str(trigger))
        args = ["--readable-swapchain"]
        if case == "pink_direct": args.append("--non-indexed")
        if case == "pink_indirect": args.append("--indexed-indirect")
        with Sandbox(seconds=40, threads=0, entry="inject" if case == "pink_index" else "layer",
                     validation=True, env_extra=extra, extra_args=args) as sb:
            with ProbeClient(sb.pid) as probe:
                time.sleep(1)
                kind = "vertex" if case == "original_vertex" else "index" if case == "pink_index" else "target" if case == "pink_target" else "pixel"
                key = {"pixel": 0x62, "vertex": 0x65, "index": 0x68, "target": 0x67}[kind]
                keys.write_text("1 %d\n" % key, encoding="ascii")
                deadline = time.monotonic() + 8
                status = {}
                while time.monotonic() < deadline:
                    status = probe.status().get("replacement", {}).get("hunting", {})
                    if status.get("selected", {}).get(kind): break
                    time.sleep(.1)
                keys.write_text("1\n", encoding="ascii")
                checks.check(bool(status.get("selected", {}).get(kind)), case + ": selection active")
                if case.startswith("pink_clear"):
                    time.sleep(.5)
                    keys.write_text("1 107\n", encoding="ascii")
                    time.sleep(.5)
                    keys.write_text("1\n", encoding="ascii")
                time.sleep(.7)
                trigger.touch()
                deadline = time.monotonic() + 8
                while not readback.is_file() and time.monotonic() < deadline: time.sleep(.1)
                replacement = probe.status().get("replacement", {})
                if case.endswith("mesh"):
                    counters = replacement.get("counters", {})
                    checks.check(counters.get("own_mesh", 0) > 0, case + ": the Mod actually records its own mesh: " + str(counters))
            pixels, pink = band_pixels(readback) or 0, colored_pixels(readback, (255, 0, 255))
            checks.check(pixels > 200, case + ": visible mesh image")
            if case == "pink_target":
                # A render target selection is one swapchain image, not every presented image. The one-shot readback
                # can belong to another image; prove selected draws bound a twin and validation accepted it.
                checks.check(replacement.get("folder_generation", {}).get("dither_twin_binds", 0) > 0,
                             case + ": selected target draws bind the pink pipeline")
            else:
                checks.equal(pink > 200, case != "original_pixel" and not case.startswith("pink_clear"), case + ": expected marking color")
            if case == "pink_index": checks.check(pixels - pink > 200, case + ": unselected draws retain their original colors")
            if case.endswith("mesh"):
                own_pink = colored_pixels(readback, (255, 0, 255), (.84, .96))
                checks.equal(own_pink > 50, not case.startswith("pink_clear"), case + ": the Mod's mesh region follows the marking state")
            sb.proc.terminate(); sb.proc.wait(timeout=30)
        checks.check("Validation Error" not in sb.output() and "VUID-" not in sb.output(), case + ": Vulkan validation clean")
        print("artifacts:", root)
    return checks.finish()


if __name__ == "__main__":
    raise SystemExit(main())
