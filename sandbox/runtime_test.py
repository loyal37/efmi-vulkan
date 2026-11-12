"""Keys, live INI conditions, persistence and transactional F10 through Vulkan hooks."""
import json
import time
import os
from pathlib import Path
import re
import struct
import subprocess
from harness import Checks, DIST, INJECT_DLL, Sandbox, inject_env, layer_env, test_dir
from runtime_fixture import APP_TEXTURE_HASH, CORE, fixture
from efmi_vk.hashes import crc32c
from efmi_vk.probe_client import ProbeClient


def refused_package(checks):
    """BUG-026: a package whose Mod files went missing is refused, and the application runs on."""
    for entry in ("layer", "inject"):
        folder = Path(test_dir("runtime-refused-"))
        package = fixture(folder)
        config = json.loads(Path(str(package) + ".runtime.json").read_text(encoding="ascii"))
        Path(config["stream_0"]).unlink()
        extra = dict(EFMI_VK_M2_PACKAGE=str(package), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500")
        status = {}
        with Sandbox(seconds=4, env_extra=extra, entry=entry) as sb:
            with ProbeClient(sb.pid) as probe:
                status = probe.status().get("replacement", {})
            sb.proc.wait(timeout=60)
            code = sb.proc.returncode
        log = Path(sb.log_path).read_text(encoding="utf-8", errors="replace") if sb.log_path and Path(sb.log_path).is_file() else ""
        label = f"{entry}, missing Mod file"
        checks.equal(code, 0, label + ": the application runs and exits normally (BUG-026)")
        checks.check("character package refused: Runtime resource missing" in log, label + ": the refusal and its reason are logged")
        checks.check(not status.get("ready") and not status.get("enabled"), label + ": the refused backend neither is ready nor enabled")


def outside_mesh(checks):
    """Another INI of the Mod folder draws its own mesh at an object the package has no part for (as
    zhuangfangyi's "fangyi ul.ini"): the original loads it with the character, the package keeps
    working, and that mesh stays with the game's draws."""
    folder = Path(test_dir("runtime-outside-"))
    package = fixture(folder)
    (folder / "other-index.buf").write_bytes(bytes([0, 0, 1, 0, 2, 0]))
    (folder / "other.ini").write_text("""[TextureOverride_OtherMesh]
hash = 0badf00d
match_first_index = 0
match_index_count = 3
handling = skip
ib = ResourceOtherIndex
drawindexed = 3,0,0
[ResourceOtherIndex]
type = Buffer
format = DXGI_FORMAT_R16_UINT
filename = other-index.buf
""", encoding="ascii")
    extra = dict(EFMI_VK_M2_PACKAGE=str(package), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500")
    status = {}
    with Sandbox(seconds=4, env_extra=extra, entry="layer") as sb:
        with ProbeClient(sb.pid) as probe:
            status = probe.status().get("replacement", {})
        sb.proc.wait(timeout=60)
        code = sb.proc.returncode
    log = Path(sb.log_path).read_text(encoding="utf-8", errors="replace") if sb.log_path and Path(sb.log_path).is_file() else ""
    label = "another INI's mesh outside the package"
    checks.equal(code, 0, label + ": the application runs and exits normally")
    checks.check("character package configured" in log and "character package refused" not in log, label + ": the package is configured")
    checks.check("1 referenced mesh file(s) outside the package" in log, label + ": the mesh outside the package is logged")
    checks.check(status.get("enabled") is True, label + ": the backend is enabled")


def shader_sections(checks):
    """A ShaderOverride with only hash and filter_index (perlica's hair sections) runs nothing and
    loads; one that acts on the game's draws (xaihi's handling = skip) needs the game shaders' DX11
    identity, which the layer cannot see: it stays inert, the log names it, and the package loads
    (D-089)."""
    cases = (("declaring", "filter_index = 3737\nallow_duplicate_hash = overrule\n", False),
             ("acting", "handling = skip\n", True))
    for name, body, named in cases:
        folder = Path(test_dir("runtime-shader-"))
        package = fixture(folder)
        (folder / "shaders.ini").write_text("[ShaderOverrideFixture]\nhash = 5d7710a77fb65f25\n" + body, encoding="ascii")
        extra = dict(EFMI_VK_M2_PACKAGE=str(package), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500")
        status = {}
        with Sandbox(seconds=4, env_extra=extra, entry="layer") as sb:
            with ProbeClient(sb.pid) as probe:
                status = probe.status().get("replacement", {})
            sb.proc.wait(timeout=60)
            code = sb.proc.returncode
        log = Path(sb.log_path).read_text(encoding="utf-8", errors="replace") if sb.log_path and Path(sb.log_path).is_file() else ""
        label = f"{name} ShaderOverride in another INI of the Mod folder"
        checks.equal(code, 0, label + ": the application runs and exits normally")
        checks.check("character package configured" in log and status.get("enabled") is True, label + ": the package loads")
        inert = [line for line in log.lower().splitlines() if "fixture] acts on game shaders" in line and "inert" in line]
        checks.check(bool(inert) == named,
                     label + (": the log names it as inert" if named else ": nothing is named as inert"))


def sandbox_shader_hashes():
    """The sandbox program's vertex and fragment modules by the CRC-32C it publishes of their SPIR-V, the hash
    the layer, the probe and the MCP tools show for a module (docs/vulkan-native-mods.md)."""
    with Sandbox(seconds=2, with_layer=False) as sb:
        shaders = {s["stage"]: int(s["hash"], 16) for s in sb.expected["shaders"]}
        sb.proc.wait(timeout=30)
    return shaders[1], shaders[16]


def band_pixels(path):
    """Pixels of the presented image's middle band (where the sandbox draws its five meshes) that differ from
    the clear colour, read off the image's bottom right corner; None without a readback."""
    meta_path = Path(str(path) + ".json")
    if not Path(path).is_file() or not meta_path.is_file():
        return None
    meta = json.loads(meta_path.read_text())
    width, height, data = meta["width"], meta["height"], Path(path).read_bytes()
    def at(x, y):
        o = 4 * (y * width + x)
        return data[o:o + 3]
    clear = at(width - 2, height - 2)
    return sum(1 for y in range(int(height * .46), int(height * .54)) for x in range(width)
               if max(abs(a - b) for a, b in zip(at(x, y), clear)) > 12)


def spirv_shader_overrides(checks):
    """Route 2, docs/vulkan-native-mods.md: a ShaderOverride naming a SPIR-V module by its CRC-32C acts at every draw
    of every pipeline that has the module, as the original's at every draw of a shader. Its pre and post lists run;
    `handling = skip` keeps the application's draws from the GPU (the sandbox's five meshes, recorded on two threads,
    indexed through two index buffers); in a TextureOverride's lists `vs` and `ps` read the entries' filter_index; the
    original's order holds (the vertex shader's entry, the TextureOverride sections, the pixel shader's entry). A
    section with a DX11 hash beside them is still inert and told as before, and nothing else is told (the
    compatibility route). Before, every ShaderOverride was inert."""
    vertex, fragment = sandbox_shader_hashes()
    mesh4 = crc32c(struct.pack("<3I", 14, 15, 16))
    for case in ("skip", "beside"):
        skip = case == "skip"
        root = Path(test_dir("runtime-spirv-override-"))
        mods = root / "Mods"
        (mods / "vulkan").mkdir(parents=True)
        ini = ("namespace = runtime_spirv\n[Constants]\nglobal persist $pre = 0\nglobal persist $post = 0\nglobal persist $vs_runs = 0\n"
               "global persist $to_runs = 0\nglobal persist $order = 0\nglobal persist $order_seen = 0\nglobal persist $ps_value = 0\n"
               "global persist $vs_value = 0\n"
               "[ShaderOverrideSandboxPixel]\nhash = %08x\nfilter_index = 7\n%s$pre = $pre + 1\n$order = 2\npost $post = $post + 1\n"
               "[ShaderOverrideSandboxVertex]\nhash = %08x\nfilter_index = 5\n$vs_runs = $vs_runs + 1\n$order = 1\n"
               "[ShaderOverrideWrittenForDX11]\nhash = 5d7710a77fb65f25\nhandling = skip\n" % (fragment, "handling = skip\n" if skip else "", vertex))
        if not skip:
            ini += ("[TextureOverrideMesh4]\nhash = %08x\nmatch_index_count = 3\n$to_runs = $to_runs + 1\n$ps_value = ps\n$vs_value = vs\n"
                    "$order_seen = $order\n" % mesh4)
        (mods / "vulkan" / "vulkan.ini").write_text(ini, encoding="ascii")
        (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                       "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
        settings = root / "settings"
        settings.mkdir()
        for entry in (("layer", "inject") if skip else ("layer",)):
            readback = root / ("readback-%s.raw" % entry)
            extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1",
                         EFMI_VK_TEST_NOTICE="> test notice", EFMI_VK_TEST_NOTICES_READBACK=str(readback))
            whole = {}
            with Sandbox(seconds=5, env_extra=extra, entry=entry, extra_args=["--readable-swapchain"]) as sb:
                time.sleep(2.5)
                with ProbeClient(sb.pid) as probe:
                    whole = probe.status()
                sb.proc.wait(timeout=60)
                code = sb.proc.returncode
            status, notices = whole.get("replacement", {}), whole.get("notices", {})
            label = f"{entry}, SPIR-V ShaderOverride " + ("with handling = skip" if skip else "beside a TextureOverride")
            checks.equal(code, 0, label + ": the application runs and exits normally")
            variables = status.get("folder_generation", {}).get("ini", {}).get("variables", {}) if skip else status.get("ini", {}).get("variables", {})
            value = lambda name: variables.get("$\\runtime_spirv\\" + name, 0)
            checks.check(value("pre") > 5 and value("post") > 5 and abs(value("pre") - value("post")) <= 5,
                         label + ": the pixel shader's pre and post lists ran at its draws " + str((value("pre"), value("post"))))
            overrides = status.get("shader_overrides", {})
            told = [t for t in notices.get("recent", []) if "acts on game shaders" in t]
            checks.check(len(told) == 1 and "WrittenForDX11" in told[0],
                         label + ": the DX11-hash section is told inert as before, the SPIR-V ones are not " + str(told))
            pixels = band_pixels(readback)
            if skip:
                checks.equal(pixels, 0, label + ": the application's five meshes are not drawn")
                checks.check(overrides.get("skipped", 0) > 5, label + ": the layer counts the draws it did not forward " + str(overrides))
            else:
                checks.check(pixels is not None and pixels > 200, label + ": the application's meshes are drawn (%s pixels)" % pixels)
                checks.equal((value("ps_value"), value("vs_value"), value("order_seen")), (7, 5, 1),
                             label + ": in the TextureOverride's lists `ps` and `vs` read the entries' filter_index, and the vertex "
                             "shader's entry ran before them and the pixel shader's after them")
                checks.check(value("to_runs") > 0 and abs(value("vs_runs") - 5 * value("to_runs")) <= 5,
                             label + ": the lists ran once per draw, also at the draw the TextureOverride matched " +
                             str((value("vs_runs"), value("to_runs"))))
            if code or (skip and pixels != 0):
                print("artifacts:", root)


def hunting_keys(checks):
    """Route 2, docs/vulkan-native-mods.md: hunting as the original's (Hunting.cpp). The installation's d3dx.ini
    [Hunting] keys, dispatched by the original's input code: soft disabled (2) does nothing until toggle_hunting; then
    the frame's vertex and fragment modules are visited, next_pixelshader selects the sandbox's fragment module and
    marking_mode = skip keeps every draw of a pipeline with it from the GPU, mark_pixelshader logs its hash as the
    original's mark does, done_hunting ends the selection. Before, the keys did nothing."""
    vertex, fragment = sandbox_shader_hashes()
    root = Path(test_dir("runtime-hunting-"))
    mods = root / "Mods"
    (mods / "panel").mkdir(parents=True)
    (mods / "panel" / "panel.ini").write_text("namespace = runtime_panel\n[Constants]\nglobal $presents = 0\n[Present]\n$presents = $presents + 1\n",
                                              encoding="ascii")
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n"
                                   "[Hunting]\nhunting = 2\nmarking_mode = skip\nmarking_actions = hlsl\n"
                                   "toggle_hunting = no_modifiers VK_NUMPAD0\nprevious_pixelshader = no_modifiers VK_NUMPAD1\n"
                                   "next_pixelshader = no_modifiers VK_NUMPAD2\nmark_pixelshader = no_modifiers VK_NUMPAD3\n"
                                   "next_vertexshader = no_modifiers VK_NUMPAD5\ndone_hunting = no_modifiers VK_ADD\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    keys = root / "keys.txt"
    keys.write_text("1\n", encoding="ascii")
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1",
                 EFMI_VK_TEST_KEYS=str(keys))
    label = "hunting keys of the installation's d3dx.ini"
    with Sandbox(seconds=90, env_extra=extra, entry="layer") as sb:
        with ProbeClient(sb.pid) as probe:
            def hunting():
                try:
                    return probe.status().get("replacement", {}).get("hunting", {})
                except Exception:  # the program ended (a build without hunting waits every step out)
                    return {}
            def wait(what, seconds=8.0):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    h = hunting()
                    if what(h):
                        return h
                    time.sleep(0.1)
                return hunting()
            def press(key, until):
                keys.write_text("1 %d\n" % key, encoding="ascii")
                h = wait(until)
                keys.write_text("1\n", encoding="ascii")
                time.sleep(0.3)
                return h
            h = wait(lambda h: h.get("keys"), 5)
            checks.check(h.get("keys") and h.get("mode") == 2 and h.get("visited", {}).get("pixel", 0) == 0,
                         label + ": registered and soft disabled, nothing visited " + str(h))
            h = press(0x62, lambda h: h.get("selected", {}).get("pixel"))
            checks.check(not h.get("selected", {}).get("pixel"), label + ": next_pixelshader does nothing while soft disabled " + str(h))
            h = press(0x60, lambda h: h.get("mode") == 1)
            h = wait(lambda h: h.get("visited", {}).get("pixel", 0) >= 1 and h.get("visited", {}).get("vertex", 0) >= 1)
            checks.check(h.get("mode") == 1 and h.get("visited", {}) == {"vertex": 1, "pixel": 1},
                         label + ": toggle_hunting enables it, and the frame's two modules are visited " + str(h))
            h = press(0x62, lambda h: h.get("selected", {}).get("pixel"))
            checks.equal((h.get("selected", {}).get("pixel"), h.get("position", {}).get("pixel")), ("%08x" % fragment, "1/1"),
                         label + ": next_pixelshader selects the sandbox's fragment module, the first of one")
            before = wait(lambda h: h.get("skipped", 0) > 0).get("skipped", 0)
            time.sleep(0.5)
            after = hunting().get("skipped", 0)
            checks.check(0 < before < after, label + ": marking_mode = skip keeps its draws from the GPU, every frame " + str((before, after)))
            h = press(0x63, lambda h: h.get("marks", 0) >= 1)
            checks.equal(h.get("marks"), 1, label + ": mark_pixelshader marks it")
            h = press(0x6B, lambda h: not h.get("selected", {}).get("pixel"))
            first = h.get("skipped", 0)
            time.sleep(0.5)
            checks.check(not h.get("selected", {}).get("pixel") and hunting().get("skipped", 0) == first,
                         label + ": done_hunting ends the selection and the draws are drawn again " + str(h))
            h = press(0x60, lambda h: h.get("mode") == 2)
            checks.equal(h.get("mode"), 2, label + ": toggle_hunting soft disables it again")
        sb.proc.terminate()
        sb.proc.wait(timeout=60)
    log = sb.log()
    checks.check(">>>> pixel shader marked: pixel shader hash = %08x" % fragment in log,
                 label + ": the mark is logged as the original logs it, with the module's hash")


def save_cadence(checks):
    """A persistent variable changed every frame (a key panel's animation) is saved as the original
    saves it: every settings_auto_save_interval seconds (60) and when the program ends, not at each
    change (BUG-058: each save held the engine lock through a flush to disk)."""
    folder = Path(test_dir("runtime-save-"))
    package = fixture(folder)
    (folder / "persist.ini").write_text("[Constants]\nglobal persist $counter = 0\n[Present]\n$counter = $counter + 1\n", encoding="ascii")
    extra = dict(EFMI_VK_M2_PACKAGE=str(package), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1")
    status = {}
    with Sandbox(seconds=4, env_extra=extra, entry="layer") as sb:
        time.sleep(2.5)
        with ProbeClient(sb.pid) as probe:
            status = probe.status().get("replacement", {})
        sb.proc.wait(timeout=60)
        code = sb.proc.returncode
    ini = status.get("ini", {})
    counter = ini.get("variables", {}).get("$\\persist.ini\\counter", 0)
    label = "a persistent variable changed every frame"
    checks.equal(code, 0, label + ": the application runs and exits normally")
    checks.check(counter > 30, label + ": it changed every frame", counter)
    checks.equal(ini.get("saves"), 0, label + ": nothing is saved before the interval passed")
    saved = [p for p in folder.rglob("*.ini") if p.parent.name == "settings"]
    values = [float(line.split("=")[1]) for p in saved for line in p.read_text().splitlines() if line.startswith("$\\persist.ini\\counter")]
    checks.check(bool(values) and max(values) >= counter, label + ": the program's end saved its latest value", (saved, values, counter))


def main():
    checks = Checks("live INI rendering")
    validation = os.environ.get("EFMI_VK_VALIDATION_DIR", "F:/vulkan/validation-1.4.321.0/bin")
    # `runtime_test.py NAME ...`: only those scenes (a failing check first, before the whole step).
    import sys
    named = [a for a in sys.argv[1:] if not a.startswith("-")]
    if named:
        for name in named:
            function = globals()[name]
            function(checks, validation) if function.__code__.co_argcount == 2 else function(checks)
        return checks.finish()
    for entry in ("layer", "inject"):
        for queues in (1, 2):
            folder = Path(test_dir("runtime-"))
            package = fixture(folder, texture_properties=True)
            extra = dict(EFMI_VK_M2_PACKAGE=str(package), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_KEYS=str(folder / "keys.txt"),
                         VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0",
                         XDG_CACHE_HOME=str(folder), TEMP=str(folder), TMP=str(folder), TMPDIR=str(folder))
            env = (layer_env if entry == "layer" else inject_env)(str(folder / "layer.log"), extra=extra)
            env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
            args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation",
                    "--module", str(Path(DIST) / "efmi_vk_backend.dll"), "--bad-module", str(Path(DIST) / "efmi_vk_backend_mismatch.dll")]
            if queues == 2:
                args += ["--graphics-queues", "2", "--split-material-queues"]
            if entry == "inject":
                args += ["--inject", INJECT_DLL]
            result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
            output = result.stdout + result.stderr
            (folder / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
            label = f"{entry}, {queues} queue(s)"
            checks.equal(result.returncode, 0, label + " GPU keys, reload, rollback and retained-command checks")
            checks.check("validation: errors=0 warnings=0" in output, label + " clean core/sync validation including shutdown")
            report = folder / "runtime-final.json"
            checks.check(report.is_file(), label + " final status saved")
            if result.returncode:
                print(output[-10000:]); print("artifacts:", folder)
            if not report.is_file():
                continue
            status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"]
            checks.equal((status["ini_generation"], status["ini_reloads"], status["ini_reload_rejected"], status["ini_reload_pending"], status["ini_reload_error"]),
                         (4, 4, 0, False, ""), label + " four accepted reloads: an original warning, a texture file that does not "
                         "load (D-139), the file repaired, another branch")
            checks.equal(status["ini"]["diagnostics"], 0, label + " the last generation has no warning left")
            checks.equal(status["ini"]["variables"]["$\\runtime_fixture\\tint"], 1, label + " persistent variable survives F10")
            variables = status["ini"]["variables"]
            checks.equal((variables.get("$\\runtime_fixture\\texture_seen"), variables.get("$\\runtime_fixture\\texture_other")), (1, 0),
                         label + " Core's OverrideTextures finds the application texture by its original hash, and only that (D-077)")
            checks.check(status.get("texture_queries", 0) > 0, label + " the layer answered texture queries")
            checks.equal(tuple(variables.get("$\\runtime_fixture\\texture_" + name) for name in ("format", "width", "height", "mips", "array", "absent")),
                         (28, 64, 64, 1, 1, -2), label + " a list reads the application texture's format, size, levels and layers as the "
                         "original's getters, and no resource at a slot without a register (D-170)")
            checks.equal(variables.get("$\\runtime_fixture\\stored"), 7,
                         label + " a store in a replaced draw reads its Mod's buffer, an execution late (D-147)")
            saved = folder / "settings/fixture.ini"
            checks.check(saved.is_file() and "$\\runtime_fixture\\tint = 1" in saved.read_text(), label + " settings atomically saved outside mod source")
            refused = {k: v for k, v in status["counters"].items() if k.startswith(("refused_", "unreadable_", "import_", "exhausted")) and v}
            checks.equal(refused, {}, label + " no refused drawing after successful reload")
    refused_package(checks)
    save_cadence(checks)
    outside_mesh(checks)
    shader_sections(checks)
    spirv_shader_overrides(checks)
    hunting_keys(checks)
    several_packages(checks, validation)
    directory_generation(checks, validation)
    plain_mods(checks, validation)
    folder_without_character(checks)
    shared_spans(checks, validation)
    own_mesh(checks, validation)
    other_names(checks, validation)
    other_folder_image(checks, validation)
    instanced_draw(checks, validation)
    constant_bones(checks, validation)
    computed_component(checks, validation)
    unknown_game(checks, validation)
    implicit_namespace(checks)
    stride_branches(checks)
    lod_left_by_another_mod(checks)
    return checks.finish()


def several_packages(checks, validation):
    """A directory of packages: another character's package, which matches no draw of the scene,
    sorts first, so every draw passes it before the fixture's. The scene must behave as with one
    package, and the other package must never go to the GPU."""
    import shutil
    folder = Path(test_dir("runtime-several-"))
    (folder / "b").mkdir()
    fixture(folder / "b", salt=0x5A5A5A5A, settings="other", name="other.bin")
    package = fixture(folder)
    for suffix in ("", ".runtime.json"):
        shutil.copyfile(folder / "b" / ("other.bin" + suffix), folder / ("0-other.bin" + suffix))
    extra = dict(EFMI_VK_M2_PACKAGE=str(folder), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_KEYS=str(folder / "keys.txt"),
                 VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0",
                 XDG_CACHE_HOME=str(folder), TEMP=str(folder), TMP=str(folder), TMPDIR=str(folder))
    env = layer_env(str(folder / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages",
            "--module", str(Path(DIST) / "efmi_vk_backend.dll"), "--bad-module", str(Path(DIST) / "efmi_vk_backend_mismatch.dll")]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (folder / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "two packages in a directory"
    checks.equal(result.returncode, 0, label + ": the scene's keys, reloads, exchanges and pixels as with one package")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-6000:]); print("artifacts:", folder)
    report = folder / "runtime-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    packages = status.get("packages", [])
    names = [Path(p.get("path", "")).name for p in packages]
    checks.equal(names, ["0-other.bin", "character.bin"], label + ": both packages listed in name order")
    held = [(p.get("package_bytes", 0) > 0, p.get("payload_held_bytes", -1)) for p in packages]
    checks.equal(held, [(True, 0)] * len(packages), label + ": no package holds its mesh bytes in host memory (ISSUE-011)")
    if len(packages) == 2:
        other, own = packages
        checks.equal((other["lazy"], other["activated"], other.get("images_ready", 0), other["counters"].get("replaced", 0)),
                     (True, False, 0, 0), label + ": the other character's package never matched and never went to the GPU")
        checks.check(own["lazy"] and own["activated"] and own["counters"].get("replaced", 0) > 0 and own["images_ready"] == 2,
                     label + ": the fixture's package went to the GPU on its first match and replaced draws")
        checks.equal((own["ini_generation"], own["ini_reloads"]), (4, 4), label + ": its reloads as with one package")
        log = (folder / "layer.log").read_text(encoding="utf-8", errors="replace")
        activations = [line for line in log.splitlines() if "activated on its first match" in line]
        checks.check(activations and all("character.bin" in line and "0-other.bin" not in line for line in activations),
                     label + ": only the fixture's package was activated, in each backend after the exchanges")


def implicit_namespace(checks):
    """An INI without a namespace line (D-078, D-154): the runtime parses it under its path from the
    Mods folder's parent (`Mods\\fixture\\mod.ini`, any script), which names its sections and variables
    as the original's implicit namespace does. The production planner (efmi_vk_character_test
    --plan-mods, no GPU) must read it and draw its components."""
    root = Path(test_dir("runtime-implicit-"))
    folder = root / "Mods" / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    text = (folder / "mod.ini").read_text(encoding="ascii")
    first, rest = text.split("\n", 1)
    checks.equal(first, "namespace = runtime_fixture", "the fixture INI starts with its namespace line")
    (folder / "mod.ini").write_text(rest, encoding="ascii")
    run = subprocess.run([str(Path(DIST) / "efmi_vk_character_test.exe"), "--plan-mods", str(root / "Mods"), str(CORE),
                          str(root / "settings")], cwd=root, capture_output=True, text=True, errors="replace", timeout=120)
    rows = [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]
    label = "INI without a namespace line"
    checks.check(run.returncode == 0 and len(rows) == 1 and rows[0].get("loaded"), label + ": read under its path from the Mods folder's parent " +
                 (rows[0].get("reason", "") if rows else run.stderr[-300:]))
    drawn = [s for s in (rows[0].get("sections", []) if rows else []) if s.get("planned") and s.get("skip") and s.get("draws")]
    checks.check(len(drawn) >= 2, label + ": its components are drawn by their live plans")


def stride_branch_ini(ini, branches):
    """The fixture's first TextureOverride chooses its component and LOD by `vb1->stride`, as an EFMI
    export may (LIMIT-041): `branches` is ((stride, component, lod), ...) in the order of the if/elif
    chain. Returns the section's hash."""
    text = ini.read_text(encoding="ascii")
    head, rest = text.split("[TextureOverride_EntryPoint_0]\n", 1)
    section, tail = rest.split("\n[", 1)
    hashed = re.search(r"^hash = (\w+)$", section, re.M).group(1)
    assignment = "$\\EFMIv1\\component_id = 0\n"
    assert assignment in section, "the fixture's first section assigns component 0"
    chain = "".join("%s vb1->stride == %d\n$lod_level = %d\n$\\EFMIv1\\component_id = %d\n" % ("if" if i == 0 else "elif", stride, lod, component)
                    for i, (stride, component, lod) in enumerate(branches)) + "endif\n"
    section = re.sub(r"^\$lod_level = \d+\n", "", section, flags=re.M).replace(assignment, chain)
    ini.write_text(head + "[TextureOverride_EntryPoint_0]\n" + section + "\n[" + tail, encoding="ascii")
    return hashed


def stride_branches(checks):
    """LIMIT-041: a TextureOverride that tells a component's LODs apart by `if vb1->stride == K`, as an
    EFMI export may (avywenna's face). The original reads the bound vertex buffer's stride per draw;
    discovery makes a section per branch, which the backend matches by the draw's pipeline binding
    and the production planner (efmi_vk_character_test --plan-mods, no GPU) plans with that stride."""
    root = Path(test_dir("runtime-stride-"))
    folder = root / "Mods" / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    hashed = stride_branch_ini(folder / "mod.ini", ((12, 0, 0), (8, 1, 1)))
    run = subprocess.run([str(Path(DIST) / "efmi_vk_character_test.exe"), "--plan-mods", str(root / "Mods"), str(CORE),
                          str(root / "settings")], cwd=root, capture_output=True, text=True, errors="replace", timeout=120)
    rows = [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]
    label = "a component chosen by vb1->stride"
    checks.check(run.returncode == 0 and len(rows) == 1 and rows[0].get("loaded"), label + ": the Mod loads " +
                 (rows[0].get("reason", "") if rows else run.stderr[-300:]))
    found = [s for s in (rows[0].get("sections", []) if rows else []) if s.get("hash") == hashed]
    checks.equal([(s.get("component"), s.get("lod"), s.get("stride_slot"), s.get("stride")) for s in found],
                 [(0, 0, 1, 12), (1, 1, 1, 8)], label + ": a section per branch, with its component, LOD and stride")
    checks.check(len(found) == 2 and all(s.get("planned") and s.get("skip") and s.get("draws") for s in found),
                 label + ": each branch plans and draws with its stride " + str([s.get("reason", "") for s in found]))


def lod_left_by_another_mod(checks):
    """A Mod that never hands its LOD to the Core (Typhoeus: its LOD1 parts are components of their own)
    in the folder generation, after another Mod's draws left the Core's one `$\\EFMIv1\\lod_level` at 1,
    as in the original's one variable space. The Core reads that value only to pick the component's LOD
    remap (MergedSkeleton.ini, `LodRemaps[component * levels + lod_level]`): a component without remaps
    draws the same bones at any LOD and plans; one with a LOD1 remap must agree with the draw's LOD."""
    root = Path(test_dir("runtime-lod-"))
    mods = root / "Mods"
    (mods / "a").mkdir(parents=True)
    (mods / "b").mkdir()
    fixture(mods / "a", settings="a", name="a.bin", namespace="runtime_a")
    fixture(mods / "b", salt=0x5A5A5A5A, settings="b", name="b.bin", namespace="runtime_b")
    a = mods / "a" / "mod.ini"
    a.write_text(re.sub(r"^\$lod_level = \d+$", "$lod_level = 1", a.read_text(encoding="ascii"), flags=re.M), encoding="ascii")
    b = mods / "b" / "mod.ini"
    handed = "$\\EFMIv1\\lod_level = $lod_level\n"
    text = b.read_text(encoding="ascii")
    checks.check(handed in text, "the fixture hands its LOD to the Core")
    b.write_text(re.sub(r"^\$lod_level = \d+\n", "", text, flags=re.M).replace(handed, ""), encoding="ascii")
    run = subprocess.run([str(Path(DIST) / "efmi_vk_character_test.exe"), "--plan-folder", str(mods), str(CORE), str(root / "settings")],
                         cwd=root, capture_output=True, text=True, errors="replace", timeout=120)
    rows = {Path(r.get("ini", "")).parent.name: r for r in (json.loads(line) for line in run.stdout.splitlines() if line.startswith("{"))}
    label = "a Mod that leaves the Core's LOD to the others"
    checks.check(run.returncode == 0 and set(rows) >= {"a", "b"} and rows["b"].get("loaded"), label + ": both Mods load " + run.stderr[-300:])
    sections = rows.get("b", {}).get("sections", [])
    free = [s for s in sections if s.get("component") in (1, 2)]
    remapped = [s for s in sections if s.get("component") == 0]
    # Component 1 draws; component 2 is the fixture's hidden one (planned, skipped, no draw).
    checks.check(free and all(s.get("planned") and s.get("skip") for s in free) and any(s.get("draws") for s in free),
                 label + ": its components without LOD remaps plan whatever LOD the Core holds " + str([s.get("reason", "") for s in free]))
    checks.check(remapped and all(not s.get("planned") and "LOD contract" in s.get("reason", "") for s in remapped),
                 label + ": its component with a LOD1 remap must agree with the draw's LOD")


# Shaped as the installed RabbitFX.ini (2.8): its sections the layer's interface provides natively
# (D-082: [Constants], [CommandListSetTextures]) stay the interface's; its key and its ShaderRegex
# sections join (D-133).
RABBITFX_SHAPED = r"""namespace = RabbitFX
[KeyCensor]
key = no_modifiers \
type = cycle
$censor = 0,1
[Constants]
global persist $censor = 0
global $h = 0.0
[CommandListSetTextures]
ps-t70 = ref ResourceDiffuse
[ShaderRegexMain]
shader_model = ps_4_0 ps_5_0
temps = ini
filter_index = 1718.1
w172 = $censor
if x172 != 0
	post run = CommandListClean
endif
[ShaderRegexMain.Pattern]
(?<discard>discard_nz r\d\.\w\n)
[ShaderRegexMain.Pattern.Replace]
ld_indexable(texture1d)(float,float,float,float) ${ini}.xyzw, l(172, 0), t120.xyzw\n
if_z ${ini}.w\n

	${discard}

endif\n
"""


def plain_mods(checks, validation):
    """D-174 (review R-03, P2-1): an INI outside every character Mod's folder acts at the draws its mesh
    sections match, as under the original, which looks every indexed draw up in the whole registry. The
    scene's Mods folder holds the fixture and `props\\prop.ini`, whose section matches the scene's draw that no
    section of the fixture matches (three indices at 30) and sets a variable: the draw stays the game's and
    the scene's pictures are those of the fixture alone."""
    root = Path(test_dir("runtime-plain-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    (mods / "props").mkdir()
    (mods / "props" / "prop.ini").write_text(
        "namespace = runtime_prop\n[Constants]\nglobal persist $prop_seen = 0\n[TextureOverrideProp]\nhash = %08x\nmatch_index_count = 3\n"
        "match_first_index = 30\n$prop_seen = 1\n" % crc32c(struct.pack("<3H", 0, 1, 2)), encoding="ascii")
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1",
                 VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "Mods folder with an INI outside every character Mod's folder"
    checks.equal(result.returncode, 0, label + ": the scene's pictures as with the fixture alone")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-6000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    packages = status.get("packages", [])
    checks.equal([(Path(p.get("path", "")).parent.name, p.get("components", -1) > 0, p.get("sections")) for p in packages],
                 [("fixture", True, 4), ("props", False, 1)], label + ": the character Mod's package and one without a skeleton for the other INI's folder")
    variables = packages[0].get("ini", {}).get("variables", {}) if packages else {}
    checks.equal(variables.get("$\\runtime_prop\\prop_seen"), 1,
                 label + ": its section ran at the draw it matches, which no section of the character Mod matches")
    if len(packages) == 2:
        prop = packages[1]
        checks.check(not prop.get("activated") and prop["counters"].get("replaced", 0) == 0 and not prop.get("last_reason"),
                     label + ": a section that only sets a variable brings nothing to the GPU and refuses nothing " +
                     str((prop.get("activated"), prop.get("last_reason"))))


def shared_spans(checks, validation):
    """BUG-093 (review R-04): a Mod whose LOD 1 part is a component of its own that shares the LOD 0 part's
    bones, and whose LOD 0 part has a remap, as a tool the user ran on an installed Mod writes it. To the
    Core a component is an offset and a count into the merged skeleton with a remap per LOD; nothing keeps
    the spans apart. The fixture so written: component 0 keeps bones 0 and 1 with a LOD 0 remap (the
    identity, from a file) and none at LOD 1; a fourth component has the same two bones and the LOD 1
    remap, and the LOD 1 section names it. The scene's pictures must be those of the fixture as exported.
    Before, the layer left such a Mod out: "Skeleton components overlap"."""
    root = Path(test_dir("runtime-spans-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    ini = folder / "mod.ini"
    text = ini.read_text(encoding="ascii")

    def change(old, new):
        nonlocal text
        assert text.count(old) == 1, old
        text = text.replace(old, new)

    change("global $component_count = 3\n", "global $component_count = 4\n")
    change("Pool_MergedSkeleton_Component_LodRemaps[0*$levels+0] = null\nPool_MergedSkeleton_Component_LodRemaps[0*$levels+1] = ref ResourceS0\n",
           "Pool_MergedSkeleton_Component_LodRemaps[0*$levels+0] = ref ResourceLod0\nPool_MergedSkeleton_Component_LodRemaps[0*$levels+1] = null\n"
           "$Pool_MergedSkeleton_Component_VertexGroupOffsets[3] = 0\n$Pool_MergedSkeleton_Component_VertexGroupCounts[3] = 2\n"
           "Pool_MergedSkeleton_Component_LodRemaps[3*$levels+0] = null\nPool_MergedSkeleton_Component_LodRemaps[3*$levels+1] = ref ResourceS0\n")
    # The Core takes an object as the Mod's once that many of its components were seen; the scene draws three.
    change("run = CommandList\\EFMIv1\\SpatialIdentity_IdentifyComponentInstances\n",
           "$\\EFMIv1\\identification_min_components = 3\nrun = CommandList\\EFMIv1\\SpatialIdentity_IdentifyComponentInstances\n")
    head, rest = text.split("[TextureOverride_EntryPoint_3]\n", 1)
    section, tail = rest.split("\n[", 1)
    assert "$lod_level = 1\n" in section and "$\\EFMIv1\\component_id = 0\n" in section, "the fixture's fourth section is component 0 at LOD 1"
    section = section.replace("$\\EFMIv1\\component_id = 0\n", "$\\EFMIv1\\component_id = 3\n")
    text = head + "[TextureOverride_EntryPoint_3]\n" + section + "\n[" + tail
    text += "\n[ResourceLod0]\ntype = Buffer\nformat = R16_UINT\nfilename = lod0.buf\n"
    ini.write_text(text, encoding="ascii")
    (folder / "lod0.buf").write_bytes(struct.pack("<2H", 0, 1))
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1",
                 VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    # Once with the bones imported on the device and once on the CPU: each has its own import and remap tables.
    for bones in ("device", "CPU"):
        for name in ("character-final.json", "character-active.json"):
            if (folder / name).is_file():
                (folder / name).unlink()
        env = layer_env(str(root / ("layer-%s.log" % bones)), extra=dict(extra, EFMI_VK_M2_GPU_BONES="1" if bones == "device" else "0"))
        env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
        args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
        result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
        output = result.stdout + result.stderr
        (root / ("stdout-%s.txt" % bones)).write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
        label = "LOD parts sharing bones, bones on the %s" % bones
        checks.equal(result.returncode, 0, label + ": the scene's pictures as with the fixture as exported, its LOD 1 frame among them")
        checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
        if result.returncode:
            print(output[-3000:]); print("artifacts:", root)
        report = folder / "character-final.json"
        status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
        fg = status.get("folder_generation", {})
        checks.equal((fg.get("packages"), [m.get("reason") for m in fg.get("left_out", [])]), (1, []),
                     label + ": the Mod is loaded, not left out")
        checks.equal((status.get("components"), status.get("sections"), status.get("last_reason") or ""), (4, 4, ""),
                     label + ": its four components, two of them on the same bones, and nothing refused")
        counters = status.get("counters", {})
        checks.check(counters.get("replaced", 0) > 0 and counters.get("incomplete", 0) == 0 and counters.get("bones_unavailable", 0) == 0 and
                     (counters.get("bone_jobs", 0) > 0) == (bones == "device"),
                     label + ": draws replaced, none left for bones not imported, the imports where the setting puts them " +
                     str({k: counters.get(k) for k in ("replaced", "incomplete", "bones_unavailable", "bone_jobs")}))


def instanced_draw(checks, validation):
    """P2-4 (review R-05): an instanced draw of a character's component. The original Core decomposes it
    (IterateInstances): per instance the object at that instance's entry is identified, one of the Mod's gets the
    Mod's mesh, any other is drawn alone as the game's. The scene draws component B with two instances whose
    entries lie side by side: B's own object and another one. The layer refused an instanced character draw, and
    the game drew both instances (the character's part as the game's)."""
    root = Path(test_dir("runtime-instanced-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_INSTANCED="1", VK_LAYER_PATH=os.pathsep.join([DIST, validation]),
                 VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "an instanced draw of a component"
    checks.equal(result.returncode, 0, label + ": the character's instance replaced, the other drawn alone as the application's")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-3000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    counters = status.get("counters", {})
    checks.check(counters.get("instanced_draws", 0) > 0 and counters.get("refused_instances", 0) == 0,
                 label + ": decomposed, not refused " + str({k: counters.get(k) for k in ("instanced_draws", "refused_instances", "replaced")}))


def constant_bones(checks, validation):
    """LIMIT-027, D-186: the game's dash after-images draw a character with vertex shaders that have no bone buffer: the
    bones and their offsets are in a uniform block of 4096 vectors at set 3 binding 0 (5370913c, 863d7ba9, 5eb8bfbc). The
    original Core takes that block as the bone source (MergedSkeleton_DetectBoneDataSource: `vs->cb_size(4) == 4096`,
    then cb3), imports from it and binds [its four config rows][the merged bones] in its place. The scene draws the whole
    character with such a pipeline; the layer refused its variant (`vertex_rewrite:spirv_required_buffer_missing`) and the
    character stayed the application's. With the bones imported on the device for the other draws (the default) and on
    the CPU."""
    for bones in ("device", "cpu"):
        root = Path(test_dir("runtime-constant-bones-"))
        mods = root / "Mods"
        folder = mods / "fixture"
        folder.mkdir(parents=True)
        fixture(folder)
        (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                       "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
        settings = root / "settings"
        settings.mkdir()
        extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1",
                     EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_CB_BONES="1",
                     EFMI_VK_M2_GPU_BONES="1" if bones == "device" else "0", VK_LAYER_PATH=os.pathsep.join([DIST, validation]),
                     VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
        env = layer_env(str(root / "layer.log"), extra=extra)
        env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
        args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
        result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
        output = result.stdout + result.stderr
        (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
        label = f"bones in a uniform block of 4096 vectors ({bones} imports elsewhere)"
        checks.equal(result.returncode, 0, label + ": the character drawn by the after-image pipeline is the Mod's and follows its poses")
        checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
        if result.returncode:
            print(output[-3000:]); print("artifacts:", root)
        report = folder / "character-final.json"
        status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
        counters = status.get("counters", {})
        names = ("replaced", "refused_variant", "incomplete", "bones_unavailable", "unreadable_bones", "import_rejected")
        checks.check(counters.get("replaced", 0) > 0 and all(counters.get(name, 0) == 0 for name in names[1:]),
                     label + ": replaced, no variant refused and no bones missing " + str({k: counters.get(k) for k in names}))


def computed_component(checks, validation):
    """D-182 (review P2-3): a section whose lists hand the Core a component computed at the draw
    (`$\\EFMIv1\\component_id = $part_base + N`), which no static reading tells. The original Core reads the variable
    when the Mod's list calls it. The layer read only constants: every such section was one without a component,
    whose Core call failed the contract, and the character stayed the game's."""
    root = Path(test_dir("runtime-computed-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder, computed_component=True)
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", VK_LAYER_PATH=os.pathsep.join([DIST, validation]),
                 VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=180)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "sections that hand the Core a computed component"
    checks.equal(result.returncode, 0, label + ": the whole character scene as with constants")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-3000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    counters = status.get("counters", {})
    checks.check(counters.get("components_handed", 0) > 0 and counters.get("replaced", 0) > 0,
                 label + ": taken from what the lists handed the Core " + str({k: counters.get(k) for k in ("components_handed", "replaced", "refused_state")}))


def unknown_game(checks, validation):
    """D-183 (review L-07): a game whose character draws are not the shape the layer knows (here the instance entries
    lie in memory the application unmapped, which the layer cannot read). The DX11 Core probes the facts of the game
    it runs in and replaces nothing where none fits. The layer refused draw by draw with a reason per Mod and never
    said that the game itself is another; now it says so once."""
    root = Path(test_dir("runtime-unknown-game-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_UNKNOWN_GAME="1", VK_LAYER_PATH=os.pathsep.join([DIST, validation]),
                 VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=180)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "a game whose character draws the layer does not know"
    checks.equal(result.returncode, 0, label + ": every frame the game's")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-3000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    shape = status.get("game_shape", {})
    log = (root / "layer.log").read_text(encoding="utf-8", errors="replace") if (root / "layer.log").is_file() else ""
    checks.check(shape.get("told") and not shape.get("fitting") and "this game version is probably newer than the layer" in log,
                 label + ": said once, with the first reason " + str(shape))


def other_folder_image(checks, validation):
    """D-178 (BUG-091): a section binds a texture whose file lies in another Mod's folder. The original keeps
    one registry: any section a draw matches may bind any declared texture, wherever its file is. The scene's
    Mods folder holds the fixture and `props\\prop.ini`, whose section gives the scene's unmatched draw the
    fixture's red image (`filename = ..\\fixture\\red.dds`) without hiding it: the application's draw again
    with that image (D-083). The layer catalogued a package's images by its folder and refused: "INI image not in
    prepared resource catalogue"."""
    root = Path(test_dir("runtime-image-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    assert (folder / "red.dds").is_file()
    (mods / "props").mkdir()
    (mods / "props" / "prop.ini").write_text(
        "namespace = runtime_prop\n[TextureOverrideProp]\nhash = %08x\nmatch_index_count = 3\nmatch_first_index = 30\n"
        "ps-t1 = ResourceBorrowed\n[ResourceBorrowed]\nfilename = ..\\fixture\\red.dds\n" % crc32c(struct.pack("<3H", 0, 1, 2)), encoding="ascii")
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_UNMATCHED_SHADE="5", VK_LAYER_PATH=os.pathsep.join([DIST, validation]),
                 VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "a texture of another Mod's folder"
    checks.equal(result.returncode, 0, label + ": the application's draw shows that texture, the character as before")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-3000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    packages = status.get("packages", [])
    if len(packages) == 2:
        prop = packages[1]
        counters = prop.get("counters", {})
        checks.check(counters.get("textured", 0) > 0 and not prop.get("last_reason"),
                     label + ": the package without a skeleton drew the application's draw with the borrowed image, nothing refused " +
                     str((counters.get("textured"), prop.get("last_reason"))))
    else:
        checks.check(False, label + ": two packages " + str(len(packages)))


def other_names(checks, validation):
    """D-177 (review R-04, P2-3): the character contract is what the Mod hands to the Core, whatever the Mod
    calls its own lists, variables and pools and wherever its lists assign them. To the Core a character
    is the values of its own inputs when its lists are called: `$\\EFMIv1\\component_id`, `lod_level`,
    `bones_count`, the pools referenced into `Pool\\EFMIv1\\Input_...` and the resource referenced into
    `Resource\\EFMIv1\\Output_MergedSkeleton`. The layer read the contract by the names the exporter gives:
    a section `[CommandListInitializeMergedSkeleton]`, `$bones_count`, `$max_instance_count`, `$lod_level`,
    `Pool_MergedSkeleton_Component_...`, and the component id assigned in the section itself. The fixture
    with every one of those names changed and the component id handed over in a list the section runs:
    the scene's pictures must be those of the fixture as exported."""
    root = Path(test_dir("runtime-names-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    ini = folder / "mod.ini"
    text = ini.read_text(encoding="ascii")
    for old, new in (("CommandListInitializeMergedSkeleton", "CommandListSetupBones"),
                     ("$bones_count", "$nbones"), ("$max_instance_count", "$ninst"), ("$component_count", "$ncomp"), ("$lod_level", "$level"),
                     ("Pool_MergedSkeleton_Component_VertexGroupOffsets", "Pool_Offsets"),
                     ("Pool_MergedSkeleton_Component_VertexGroupCounts", "Pool_Counts"),
                     ("Pool_MergedSkeleton_Component_LodRemaps", "Pool_Remaps"),
                     ("Pool_MergedSkeleton_Instance_UpdateFrame", "Pool_Frames"),
                     ("Pool_MergedSkeleton_Instance_LodLevel", "Pool_Lods"),
                     ("Pool_ObjectSpatialIdentity", "Pool_Places"),
                     ("ResourceMergedSkeletonDataRW", "ResourceBonesRW")):
        assert old in text, old
        text = text.replace(old, new)
    # The Core's own names stay: only the Mod's were changed.
    for kept in ("$\\EFMIv1\\bones_count = $nbones", "$\\EFMIv1\\lod_level = $level", "Pool\\EFMIv1\\Input_MergedSkeleton_Component_LodRemaps = ref Pool_Remaps",
                 "Resource\\EFMIv1\\Output_MergedSkeleton = ref ResourceBonesRW", "run = CommandList\\EFMIv1\\InitializeMergedSkeleton"):
        assert kept in text, kept
    # The component id handed to the Core in a list the section runs, from a variable of the Mod's.
    text, parts = re.subn(r"^\$\\EFMIv1\\component_id = (\d+)$", r"$part = \1\nrun = CommandListPart", text, flags=re.M)
    assert parts == 4, parts
    text = text.replace("[Constants]\nglobal persist $tint = 0\n", "[Constants]\nglobal persist $tint = 0\nglobal $part = 0\n")
    text += "\n[CommandListPart]\n$\\EFMIv1\\component_id = $part\n"
    ini.write_text(text, encoding="ascii")
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1",
                 VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "a character Mod with names and lists of its own"
    checks.equal(result.returncode, 0, label + ": the scene's pictures as with the fixture as exported")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-3000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    fg = status.get("folder_generation", {})
    checks.equal((fg.get("packages"), [m.get("reason") for m in fg.get("left_out", [])]), (1, []), label + ": the Mod is loaded, not left out")
    checks.equal((status.get("components"), status.get("sections"), status.get("skeleton_bones"), status.get("last_reason") or ""), (3, 4, 5, ""),
                 label + ": its skeleton and its parts as the Core is handed them: three components, five bones, four sections, nothing refused")
    counters = status.get("counters", {})
    checks.check(counters.get("replaced", 0) > 0 and counters.get("refused_state", 0) == 0,
                 label + ": draws replaced and none refused " + str({k: counters.get(k) for k in ("replaced", "refused_state", "hidden")}))


def own_mesh(checks, validation):
    own_mesh_case(checks, validation, beside=False)
    # D-179 (review R-09): the same section without `handling = skip`. The original draws the Mod's triangle and
    # then the game's draw; the layer took only a list's textures for the game's draw and dropped its draws.
    own_mesh_case(checks, validation, beside=True)


def own_mesh_case(checks, validation, beside):
    """D-176 (review P2-2): a section without a component that draws a mesh of its own. The original runs
    its lists on the game's draw state: `handling = skip` drops the game's draw, the lists bind the Mod's
    buffers, and their draw goes out with the game's shaders and whatever else the game bound, its bones
    among it. The scene's Mods folder holds the fixture and `props\\prop.ini`, whose section stands for the
    scene's unmatched draw: a triangle moved by (0,.85), weighted to that draw's bone zero, with the
    position, blend and index files of its own and the game's texture coordinates. Before, such a plan
    was refused ("INI hide override unexpectedly emits geometry") and the game's draw stayed."""
    root = Path(test_dir("runtime-own-mesh-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    folder.mkdir(parents=True)
    fixture(folder)
    props = mods / "props"
    props.mkdir()
    base = ((-.22, -.2), (.22, -.2), (0, .22))
    (props / "position.buf").write_bytes(b"".join(struct.pack("<4f", x, y + .85, 0, 0) for x, y in base))
    (props / "blend.buf").write_bytes(struct.pack("<8H", 65535, 0, 0, 0, 0, 0, 0, 0) * 3)
    (props / "index.buf").write_bytes(struct.pack("<3I", 0, 1, 2))
    (props / "prop.ini").write_text(
        "namespace = runtime_prop\n[Constants]\nglobal persist $own_seen = 0\n[TextureOverrideProp]\nhash = %08x\nmatch_index_count = 3\n"
        "match_first_index = 30\n%svb0 = ResourcePosition\nvb2 = ResourceBlend\nvb3 = ResourcePosition\nib = ResourceIndex\n"
        "drawindexed = 3, 0, 0\n$own_seen = 1\n"
        "[ResourcePosition]\ntype = Buffer\nstride = 16\nfilename = position.buf\n"
        "[ResourceBlend]\ntype = Buffer\nstride = 16\nfilename = blend.buf\n"
        "[ResourceIndex]\ntype = Buffer\nformat = R32_UINT\nfilename = index.buf\n" % (crc32c(struct.pack("<3H", 0, 1, 2)), "" if beside else "handling = skip\n"),
        encoding="ascii")
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1",
                 EFMI_VK_M2_IDENTITY_EXPIRY_MS="500", EFMI_VK_TEST_OWN_MESH="2" if beside else "1", VK_LAYER_PATH=os.pathsep.join([DIST, validation]),
                 VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages"]
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "a section without a component that draws a mesh of its own" + (" beside the game's draw" if beside else "")
    checks.equal(result.returncode, 0, label + ": the Mod's triangle stands where the game's draw stood, on the game's bone, and follows it")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-3000:]); print("artifacts:", root)
    report = folder / "character-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    packages = status.get("packages", [])
    checks.equal([(Path(p.get("path", "")).parent.name, p.get("components", -1) > 0) for p in packages], [("fixture", True), ("props", False)],
                 label + ": the character Mod's package and the one without a skeleton")
    if len(packages) == 2:
        prop = packages[1]
        counters = prop.get("counters", {})
        checks.check(prop.get("activated") and counters.get("own_mesh", 0) > 0 and prop.get("package_bytes", 0) > 0 and not prop.get("last_reason"),
                     label + ": its package went to the GPU with its mesh files and drew, nothing refused " +
                     str((prop.get("activated"), counters.get("own_mesh"), prop.get("package_bytes"), prop.get("last_reason"))))
        checks.equal(packages[0].get("ini", {}).get("variables", {}).get("$\\runtime_prop\\own_seen"), 1, label + ": its lists ran to their end")


def folder_without_character(checks):
    """D-174: a Mods folder without any character Mod is a folder as any other. Its INIs load, their Present
    runs, and a mesh section acts at the draw it matches (the sandbox program's fifth mesh: three 32-bit
    indices in a mapped index buffer). Before, such a folder loaded nothing."""
    root = Path(test_dir("runtime-nochar-"))
    mods = root / "Mods"
    (mods / "props").mkdir(parents=True)
    (mods / "props" / "prop.ini").write_text(
        "namespace = runtime_prop\n[Constants]\nglobal persist $prop_seen = 0\nglobal persist $presents = 0\n[Present]\n$presents = 1\n"
        "[TextureOverrideProp]\nhash = %08x\nmatch_index_count = 3\n$prop_seen = 1\n" % crc32c(struct.pack("<3I", 14, 15, 16)), encoding="ascii")
    (mods / "panel").mkdir()
    (mods / "panel" / "panel.ini").write_text("namespace = runtime_panel\n[Constants]\nglobal persist $presents = 0\n[Present]\n$presents = 1\n", encoding="ascii")
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n", encoding="ascii")
    settings = root / "settings"
    settings.mkdir()
    extra = dict(EFMI_VK_M2_MODS=str(mods), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1")
    for entry in ("layer", "inject"):
        status, notices = {}, {}
        with Sandbox(seconds=6, env_extra=extra, entry=entry) as sb:
            time.sleep(3)
            with ProbeClient(sb.pid) as probe:
                whole = probe.status()
                status, notices = whole.get("replacement", {}), whole.get("notices", {})
            sb.proc.wait(timeout=60)
            code = sb.proc.returncode
        label = f"{entry}, Mods folder without a character Mod"
        checks.equal(code, 0, label + ": the application runs and exits normally")
        fg = status.get("folder_generation", {})
        checks.check(fg.get("packages") == 1 and fg.get("left_out") == [] and fg.get("ini", {}).get("presents", 0) > 0,
                     label + ": the folder loads and presents " + str({k: fg.get(k) for k in ("packages", "left_out", "ini")}))
        # One package: the status is that package's.
        checks.equal((status.get("components"), status.get("sections"), status.get("activated"), status.get("last_reason") or ""), (0, 1, False, ""),
                     label + ": the INI with a mesh section has a package without a skeleton, which a section that only sets a variable "
                     "does not bring to the GPU; the panel INI needs none")
        variables = status.get("ini", {}).get("variables", {})
        checks.equal((variables.get("$\\runtime_prop\\prop_seen"), variables.get("$\\runtime_prop\\presents"), variables.get("$\\runtime_panel\\presents")),
                     (1, 1, 1), label + ": the mesh section ran at the draw it matches, and both INIs' Present ran")
        checks.check(not any("no Mod is loaded" in text for text in notices.get("recent", [])),
                     label + ": no notice says that no Mod is loaded " + str(notices.get("recent", [])[:3]))


def directory_generation(checks, validation):
    """D-078: a Mods folder instead of packages. The fixture Mod and another character's Mod (whose
    sections match no draw of the scene) sit in it as an EFMI Mods folder holds them; the layer reads
    both INIs, makes their packages from what the original engine declares, and must then behave as
    with the fixture's package: the scene's keys, reloads, exchanges and pixels."""
    root = Path(test_dir("runtime-mods-"))
    mods = root / "Mods"
    folder = mods / "fixture"
    (mods / "other").mkdir(parents=True)
    folder.mkdir()
    fixture(mods / "other", salt=0x5A5A5A5A, settings="other", name="other.bin", namespace="runtime_other")
    fixture(folder)
    # LIMIT-041: the fixture's first section chooses its component by `vb1->stride`, the branch the scene's
    # pipeline does not bind (stride 12) first: only the draw's own stride (8) may take its draws.
    stride_branch_ini(folder / "mod.ini", ((12, 1, 1), (8, 0, 0)))
    # D-131: a third character Mod the layer cannot take (an export whose TextureOverride sections are
    # missing, so the Core's contract has no mesh section): the folder generation leaves out that Mod
    # alone, with its reason, and the others behave as before.
    (mods / "broken").mkdir()
    fixture(mods / "broken", salt=0x3C3C3C3C, settings="broken", name="broken.bin", namespace="runtime_broken")
    broken_ini = mods / "broken" / "mod.ini"
    broken_ini.write_text(broken_ini.read_text(encoding="ascii").replace("[TextureOverride", "[CommandListUnused"), encoding="ascii")
    (mods / "DISABLED old").mkdir()
    (mods / "DISABLED old" / "mod.ini").write_text((folder / "mod.ini").read_text(encoding="ascii"), encoding="ascii")
    settings = root / "settings"
    # A panel INI without a namespace in the Mod's folder (`k\1.ini`, as several installed Mods ship) with
    # a persistent variable. A generation per Mod named it by its path from the Mod folder and saved its
    # value in the Mod's own settings file; 2.0.0 to 2.0.3 adopted that line under the old name into the
    # folder's file, where the folder generation (which names the INI by its path from the Mods folder's
    # parent, as the original's d3dx_user.ini) never read it.
    (folder / "k").mkdir()
    (folder / "k" / "1.ini").write_text("[Constants]\nglobal persist $z = 0\n", encoding="ascii")
    # LIMIT-042: a panel INI in a folder whose name is outside ASCII (a full-width colon, as Mod folders
    # have): its persistent variable's name is too, and the original writes it as UTF-8 with the rest.
    (folder / "k\uff1apanel").mkdir()
    (folder / "k\uff1apanel" / "1.ini").write_text("[Constants]\nglobal persist $q = 4\n", encoding="ascii")
    # docs/architecture-review-2.1.md R-03, P2-1 (D-173): a section of another INI of the character Mod's
    # folder (as an installed Mod's `Texture\hair.ini` beside its character INI) for a draw no section of
    # the character's INI matches: the scene's unmatched draw, three indices at 30, whose index region has
    # the bytes of component A's. The original looks every indexed draw up in the whole registry by its
    # index count and its region's hash (CheckTextureOverride = ib, BuildTextureOverrideDrawMaps), whichever
    # INI declares the section; the layer matched the sections of the character INI's namespace only.
    (folder / "parts").mkdir()
    (folder / "parts" / "part.ini").write_text(
        "[Constants]\nglobal persist $part_seen = 0\n[TextureOverridePart]\nhash = %08x\nmatch_index_count = 3\nmatch_first_index = 30\n"
        "$part_seen = 1\n" % crc32c(struct.pack("<3H", 0, 1, 2)), encoding="ascii")
    # D-133: RabbitFX's anti-dither. The layer provides RabbitFX natively (D-082); the installed file's key
    # and ShaderRegex sections join: its replacement keeps the shader's discard behind `if_z
    # IniParams[172].w`, its section assigns `w172 = $censor`, a persistent toggle the player left on.
    (mods / "RabbitFX -ENDMI-").mkdir()
    (mods / "RabbitFX -ENDMI-" / "RabbitFX.ini").write_text(RABBITFX_SHAPED, encoding="ascii")
    # The original's include order (IniHandler.cpp ParseIniFilesRecursive) sorts names without regard to
    # case: a Mod in `Zeta` is parsed after `fixture` and `other`, so its [Present] lines run after theirs.
    # Both Mods set the fixture's $order when it is still 0; the first parsed wins.
    (mods / "Zeta").mkdir()
    fixture(mods / "Zeta", salt=0x7E7E7E7E, settings="zeta", name="zeta.bin", namespace="runtime_zeta")
    zeta_ini = mods / "Zeta" / "mod.ini"
    zeta_ini.write_text(zeta_ini.read_text(encoding="ascii").replace(
        "[Present]\npost $active = 0\n", "[Present]\npost $active = 0\nif $\\runtime_fixture\\order == 0\n$\\runtime_fixture\\order = 2\nendif\n"), encoding="ascii")
    own_ini = folder / "mod.ini"
    text = own_ini.read_text(encoding="ascii")
    assert "[Constants]\nglobal persist $tint = 0\n" in text and "[Present]\npost $active = 0\n" in text
    # The fixture also runs a command list the installation's d3dx.ini declares (LIMIT-039).
    own_ini.write_text(text.replace("[Constants]\nglobal persist $tint = 0\n", "[Constants]\nglobal persist $tint = 0\nglobal persist $order = 0\n").replace(
        "[Present]\npost $active = 0\n", "[Present]\npost $active = 0\nif $order == 0\n$order = 1\nendif\nrun = CommandListUnbindAllRenderTargets\n"),
        encoding="ascii")
    # A character Mod in a folder named in another script whose INI declares no namespace: the parser
    # names it by its path (`Mods\<folder>\mod.ini`), in the folder generation and when tried alone (the
    # isolation retry `broken` causes); a byte-by-byte widening of that name left it out (D-154).
    colon = mods / "\uff1aMod"
    colon.mkdir()
    fixture(colon, salt=0x2B2B2B2B, settings="colon", name="colon.bin", namespace="runtime_colon")
    first, rest = (colon / "mod.ini").read_text(encoding="ascii").split("\n", 1)
    assert first == "namespace = runtime_colon"
    (colon / "mod.ini").write_text(rest, encoding="ascii")
    # Every enabled INI of the Mods folder is an input, as the original's include_recursive (D-155): a Mod
    # that is no character Mod runs its [Present] and its TextureOverride by texture hash acts in a
    # replaced draw's pixel slot check; one with a declaration the layer cannot take (ResourceA =
    # copy_desc ResourceB in its post-Constants) is warned about once and loads otherwise, as the
    # original warns and goes on; neither is a package, neither is left out.
    (mods / "plain").mkdir()
    (mods / "plain" / "plain.ini").write_text(
        "namespace = runtime_plain\n[Constants]\nglobal persist $presents = 0\nglobal persist $seen = 0\n[Present]\n$presents = 1\n"
        "[TextureOverride_AppTexture]\nhash = %08x\n$seen = 1\n" % APP_TEXTURE_HASH, encoding="ascii")
    (mods / "bad").mkdir()
    (mods / "bad" / "bad.ini").write_text(
        "namespace = runtime_bad\n[Constants]\nglobal persist $presents = 0\npost ResourceCopied = copy_desc ResourceSource\n"
        "[ResourceSource]\ntype = Buffer\nstride = 16\nfilename = source.buf\n[ResourceCopied]\n[Present]\n$presents = 1\n", encoding="ascii")
    (mods / "bad" / "source.buf").write_bytes(bytes(64))
    # docs/architecture-review-2.1.md R-03, P2-1 (D-174): an INI outside every character Mod's folder whose
    # TextureOverride matches by an index count (a mesh Mod that is no EFMI character Mod). It has a package
    # of its own, without a skeleton; its section here matches no draw of the scene. Before, the layer
    # matched no draw with such an INI and said so in a notice.
    (mods / "meshonly").mkdir()
    (mods / "meshonly" / "mesh.ini").write_text(
        "namespace = runtime_meshonly\n[TextureOverrideMesh]\nhash = 0badf00d\nmatch_index_count = 3\nhandling = skip\n", encoding="ascii")
    # A file named .INI is not an INI to the original (its extension test is case-sensitive): neither a
    # character Mod nor an input, and the folder loads without it.
    (mods / "upper").mkdir()
    fixture(mods / "upper", salt=0x12121212, settings="upper", name="upper.bin", namespace="runtime_upper")
    (mods / "upper" / "mod.ini").rename(mods / "upper" / "mod.INI")
    # The installation's d3dx.ini, the original's first file: its own sections join the generation (a
    # command list Mods call by name, the compiler's recursive_include); its [Include] is the layer's own.
    (root / "d3dx.ini").write_text("[Include]\ninclude = Core\\EFMI\\main.ini\ninclude_recursive = Mods\nexclude_recursive = DISABLED*\n"
                                   "[Rendering]\nrecursive_include = 1\n[CommandListUnbindAllRenderTargets]\nrun = BuiltInCommandListUnbindAllRenderTargets\n",
                                   encoding="ascii")
    settings.mkdir()
    header = "; EFMI Vulkan persistent original INI variables\n[Constants]\n"
    (settings / "runtime_fixture-fixture.ini").write_text(header + "$\\k\\1.ini\\z = 7\n", encoding="ascii")
    (settings / "mods-folder.ini").write_text(header + "$\\k\\1.ini\\z = 3\n$\\rabbitfx\\censor = 1\n", encoding="ascii")
    # The Mods folder relative to the layer's own folder, as the release's configuration names it
    # (`m2_mods = ../Mods` with the release unpacked into EFMI); the scene runs elsewhere.
    extra = dict(EFMI_VK_M2_MODS=os.path.relpath(mods, DIST), EFMI_VK_M2_CORE=str(CORE), EFMI_VK_M2_SETTINGS=str(settings), EFMI_VK_M2_ENABLED="1", EFMI_VK_M2_TEXTURE_PRELOAD="1", EFMI_VK_M2_IDENTITY_EXPIRY_MS="500",
                 EFMI_VK_TEST_KEYS=str(folder / "keys.txt"),
                 # D-133: the scene's application fragment shader dithers; with RabbitFX's gate set (the saved
                 # $censor) every picture must be the one without the dither.
                 EFMI_VK_TEST_DITHER="1",
                 # The scene ends with what a player does around F10 in a Mods folder (docs/f10-operations.md).
                 EFMI_VK_TEST_FOLDER_F10="1",
                 VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0",
                 XDG_CACHE_HOME=str(root), TEMP=str(root), TMP=str(root), TMPDIR=str(root))
    env = layer_env(str(root / "layer.log"), extra=extra)
    env["VK_LAYER_PATH"] = extra["VK_LAYER_PATH"]
    args = [str(Path(DIST) / "efmi_vk_variant_gpu.exe"), "--character", "--material", "--validation", "--lazy-packages",
            "--module", str(Path(DIST) / "efmi_vk_backend.dll"), "--bad-module", str(Path(DIST) / "efmi_vk_backend_mismatch.dll")]
    sources = {p: p.read_bytes() for p in mods.rglob("*") if p.is_file()}
    result = subprocess.run(args, env=env, cwd=folder, capture_output=True, text=True, errors="replace", timeout=120)
    output = result.stdout + result.stderr
    (root / "stdout.txt").write_text(output.encode("ascii", "backslashreplace").decode(), encoding="ascii")
    label = "Mods folder (directory generation)"
    checks.equal(result.returncode, 0, label + ": the scene's keys, reloads, exchanges and pixels as with the fixture's package")
    checks.check("validation: errors=0 warnings=0" in output, label + ": clean core/sync validation")
    if result.returncode:
        print(output[-6000:]); print("artifacts:", root)
    report = folder / "runtime-final.json"
    status = json.loads(report.read_text(encoding="utf-8"))["result"]["replacement"] if report.is_file() else {}
    # D-137: each F10 that reloaded says so as the original's ReloadConfig does.
    recent = json.loads(report.read_text(encoding="utf-8"))["result"].get("notices", {}).get("recent", []) if report.is_file() else []
    checks.check(any(text.startswith("> Reloaded config in ") and text.endswith("s") for text in recent),
                 label + ": an F10 that reloaded queues the original's notice " + str(recent))
    packages = status.get("packages", [])
    names = [Path(p.get("path", "")).parent.name for p in packages]
    checks.equal(names, ["fixture", "other", "Zeta", "\uff1aMod", "meshonly"],
                 label + ": the character INIs in the original's include order, the one named in another script among them, then the "
                 "folder whose INI has a mesh section and no character; the DISABLED folder and the .INI file skipped")
    held = [(p.get("package_bytes", 0) > 0, p.get("payload_held_bytes", -1)) for p in packages if p.get("components")]
    checks.equal(held, [(True, 0)] * 4, label + ": no package holds its mesh bytes in host memory (ISSUE-011)")
    gates = status.get("folder_generation", {}).get("discard_gates", [])
    checks.equal([(g.get("index"), g.get("component"), g.get("value")) for g in gates], [(172, 3, 1.0)],
                 label + ": the library's discard gate, IniParams[172].w, holds $censor (1) after its section's assignment")
    checks.check(status.get("folder_generation", {}).get("dither_off") is True, label + ": a set gate turns the character dither off")
    fg = status.get("folder_generation", {})
    checks.check(fg.get("dither_twins", 0) >= 1 and fg.get("dither_twin_binds", 0) >= 1 and fg.get("dither_twins_failed", 1) == 0,
                 label + ": the application's dithered pipeline is bound as its twin without the dither " +
                 str({k: fg.get(k) for k in ("dither_twins", "dither_twin_binds", "dither_twins_failed")}))
    # 2.0.6 in the game: the key took a second or two, every twin and variant of the other state compiled
    # when the gate turned. The original's replaced shader exists from the game's shader creation.
    checks.check(fg.get("dither_twins_prepared", 0) >= 1,
                 label + ": the dithered pipeline's twin is prepared when the application creates the pipeline, before a bind needs it")
    left_out = status.get("folder_generation", {}).get("left_out", [])
    checks.equal([Path(m.get("ini", "")).parent.name for m in left_out], ["broken"],
                 label + ": the Mod the layer cannot take is left out alone, with its reason " + str([m.get("reason", "") for m in left_out]))
    # R-11: what does not show is said on the screen (a notice), not only in the log and the status.
    log_path = root / "layer.log"
    log_lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines() if log_path.is_file() else []
    checks.check(any("notice: EFMI Vulkan: broken: not loaded" in line for line in log_lines),
                 label + ": the Mod left out is named in a notice with its reason")
    plain = [p for p in packages if Path(p.get("path", "")).parent.name == "meshonly"]
    checks.check(len(plain) == 1 and plain[0].get("components") == 0 and plain[0].get("sections") == 1 and plain[0].get("lazy") and
                 not plain[0].get("activated"),
                 label + ": the INI outside every character Mod's folder has a package without a skeleton for its mesh section, "
                 "which no draw of the scene matches (D-174) " + str([(p.get("components"), p.get("sections"), p.get("activated")) for p in plain]))
    checks.check(not any("its mesh sections replace nothing" in line for line in log_lines),
                 label + ": and no notice says its sections replace nothing")
    # L-02 and L-01: the scene's last steps ran to their end (F10 with no generation loaded reloads; a
    # package whose activation fails once leaves its draws to the game and is brought up again).
    checks.check((folder / "runtime-reactivated.json").is_file(),
                 label + ": F10 without a loaded generation and the retried activation ran to their end")
    if len(packages) == 5:
        own, other, zeta, colon_package = packages[:4]
        checks.check(colon_package.get("lazy") and not colon_package.get("activated"), label + ": the Mod named in another script is a package of the folder, never matched")
        # Its images_ready counts the folder's images another package loaded: every package catalogues every image
        # of the folder, and the residency holds one image per file (D-178).
        checks.equal((other["lazy"], other["activated"], other.get("package_bytes", 0) > 0, other["counters"].get("replaced", 0)),
                     (True, False, True, 0), label + ": the other character's Mod never matched and never went to the GPU")
        checks.check(own["activated"] and own["counters"].get("replaced", 0) > 0 and own["images_ready"] >= 2,
                     label + ": the fixture's Mod went to the GPU on its first match and replaced draws")
        # BUG-089: with RabbitFX's gate set the application's dithered pipeline is bound as its twin at the start of
        # every recording, before the character's first draw. The twin's owner took the generic owner_for, which
        # dropped the character's snapshot batch from the command buffer each time, and the worker made another:
        # about one batch per recording. A recording keeps its batch from one reset to the next, as D3D11 keeps
        # what a pending command list uses whatever else is bound.
        # Read before the scene's reloads, when one generation has drawn some thirty frames with the gate set.
        active_path = folder / "character-active.json"
        active = json.loads(active_path.read_text(encoding="utf-8"))["result"]["replacement"] if active_path.is_file() else {}
        first = (active.get("packages") or [{}])[0]
        checks.check(first.get("batches_made", 10 ** 6) <= 6 and first.get("frame", 0) >= 20,
                     label + ": the character's snapshot batches survive the twin's bind in every recording (BUG-089): %s made in %s frames" %
                     (first.get("batches_made"), first.get("frame")))
        # A broken line, a texture file that does not load, the file repaired, another branch (D-139).
        checks.equal((own["ini_generation"], own["ini_reloads"]), (4, 4), label + ": its reloads as with a package")
        variables = own.get("ini", {}).get("variables", {})
        checks.equal(variables.get("$\\runtime_fixture\\tint"), 1, label + ": the persistent variable survives F10")
        # BUG-090: `vs` in a replaced draw reads 1, as under the original with the installed Core's patternless
        # ShaderRegex over every vertex shader ([ShaderRegexEnableTextureOverrides]); it read 0.
        checks.equal(variables.get("$\\runtime_fixture\\vs_value"), 1, label + ": `vs` reads 1 in a replaced draw, as under the original with the installed Core")
        checks.equal(variables.get("$\\runtime_fixture\\order"), 1, label + ": the [Present] lines of `fixture` ran before those of `Zeta`, the original's include order")
        unrecognised = [m for m in own.get("ini", {}).get("diagnostic_messages", []) if "Unrecognised entry" in m]
        checks.equal(unrecognised, [], label + ": a command list the installation's d3dx.ini declares is known to the Mods (LIMIT-039)")
        checks.equal((variables.get("$\\runtime_plain\\presents"), variables.get("$\\runtime_plain\\seen")), (1, 1),
                     label + ": an enabled INI that is no character Mod's is loaded: its [Present] runs and its TextureOverride acts in a replaced draw (D-155)")
        # With the owned GPU runtime the copy_desc is made as owned work (no warning); offline, without
        # it, the plan leaves the destination null with a warning once (the dry runs, the Core pools test).
        checks.equal(variables.get("$\\runtime_bad\\presents"), 1,
                     label + ": an INI with a copy_desc in its post-Constants loads and its Present runs")
        checks.equal(variables.get("$\\mods\\fixture\\k\\1.ini\\z"), 7,
                     label + ": a panel INI's setting saved by the Mod's own generation is read under the folder's name")
        checks.equal(variables.get("$\\mods\\fixture\\parts\\part.ini\\part_seen"), 1,
                     label + ": a section of another INI of the Mod's folder runs at the draw it matches, which no section of the character INI matches (R-03)")
        checks.check(own.get("sections", 0) == 6,
                     label + ": that section is one of the package's, beside the character INI's five (four, one of them in two branches by stride): %s" % own.get("sections"))
    checks.check(not any(Path(m.get("ini", "")).parent.name in ("plain", "bad") for m in left_out), label + ": neither plain INI is left out")
    # One generation for the folder (D-131) keeps one settings file for every Mod, as the original's
    # d3dx_user.ini; a generation per Mod kept the namespace and the Mod folder (BUG-039).
    saved = settings / "mods-folder.ini"
    checks.check(saved.is_file() and "$\\runtime_fixture\\tint = 1" in saved.read_text(encoding="utf-8"), label + ": settings saved in the configured folder")
    saved_text = saved.read_text(encoding="utf-8") if saved.is_file() else ""
    checks.check("$\\mods\\fixture\\k\uff1apanel\\1.ini\\q = 4" in saved_text,
                 label + ": a persistent variable named outside ASCII is saved, as UTF-8, with the others (LIMIT-042)")
    checks.check("$\\mods\\fixture\\k\\1.ini\\z = 7" in saved_text and "$\\k\\1.ini\\" not in saved_text,
                 label + ": the folder's settings keep the panel's value under its own name, not the old one")
    # 2.0.7 in the game: F10 after a drawn character's Mod was removed stopped its package twice at
    # once (its controller and the generation it left); the second join's error ended the game.
    removed = folder / "runtime-removed.json"
    after_removal = json.loads(removed.read_text(encoding="utf-8"))["result"]["replacement"] if removed.is_file() else {}
    # The disabled character INI's folder still holds `parts\\part.ini`, enabled: with no character Mod above it,
    # its mesh section has a package of its own (D-174), as the original goes on loading that INI.
    checks.equal(after_removal.get("folder_generation", {}).get("packages"), 5,
                 label + ": F10 after the drawn character's Mod was disabled goes on with the other Mods, the removed package stopped once")
    def report_of(name):
        path = folder / name
        return json.loads(path.read_text(encoding="utf-8"))["result"]["replacement"] if path.is_file() else {}
    removed_packages = [(Path(p.get("path", "")).parent.name, p.get("stopped")) for p in after_removal.get("packages", [])]
    checks.equal(removed_packages, [("fixture", True), ("other", False), ("Zeta", False), ("\uff1aMod", False), ("meshonly", False), ("parts", False)],
                 label + ": the disabled Mod's package shows stopped, in its slot")
    # BUG-068: enabled again, the Mod takes its slot back (2.0.7 added a third entry, and so on to 64).
    reenabled = report_of("runtime-reenabled.json")
    back = [(Path(p.get("path", "")).parent.name, p.get("stopped"), p.get("activated")) for p in reenabled.get("packages", [])]
    checks.equal(back, [("fixture", False, True), ("other", False, False), ("Zeta", False, False), ("\uff1aMod", False, False),
                        ("meshonly", False, False), ("parts", True, None)],
                 label + ": a Mod enabled again by F10 is drawn from the slot it had; its folder's other INI is its own again, the package it had meanwhile stopped")
    # D-133, BUG-070: the gate follows the live generation when RabbitFX goes and comes back.
    gates_of = lambda r: [(g.get("index"), g.get("component"), g.get("value")) for g in r.get("folder_generation", {}).get("discard_gates", [])]
    no_rabbit, rabbit_back = report_of("runtime-no-rabbitfx.json"), report_of("runtime-rabbitfx-back.json")
    checks.equal((gates_of(no_rabbit), no_rabbit.get("folder_generation", {}).get("dither_off")), ([], False),
                 label + ": without RabbitFX after F10 no gate is left and the dither is drawn")
    checks.equal((gates_of(rabbit_back), rabbit_back.get("folder_generation", {}).get("dither_off")), ([(172, 3, 1.0)], True),
                 label + ": RabbitFX back by F10 brings its gate back with the saved $censor")
    # The scene itself edits its mod.ini (and disables it last), drives its keys through keys.txt and
    # writes its reports where it runs (the fixture's folder).
    after = {p: p.read_bytes() for p in mods.rglob("*") if p.is_file()}
    changed = [str(p.relative_to(mods)) for p in after if p.name not in ("mod.ini", "keys.txt") and p in sources and sources[p] != after[p]]
    added = [str(p.relative_to(mods)) for p in after if p not in sources and
             not (p.parent == folder and re.fullmatch(r"(character|runtime)-[\w-]+\.json|pixels\.rgba|stdout\.txt|DISABLED_mod\.ini", p.name))]
    checks.equal((changed, added), ([], []), label + ": the layer writes nothing into the Mods folder")
    log = (root / "layer.log").read_text(encoding="utf-8", errors="replace")
    checks.check(log.count("character Mod of the folder generation") >= 2, label + ": every package was made from its INI in the folder generation")
    checks.check("differs from the resolved native identity" not in log,
                 label + ": a section chosen by vb1->stride takes only the draws whose pipeline binds its stride (LIMIT-041)")


if __name__ == "__main__":
    raise SystemExit(main())
