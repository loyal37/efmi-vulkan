"""Builds the layer and the sandbox with the MSVC x64 toolchain.

    python scripts/build.py [layer] [sandbox] [--out DIR]

Without a target both are built. Output goes to dist/ (or --out), objects to
<component>/build/. The Vulkan headers are looked for in this order:
%VULKAN_SDK%\\Include, F:\\vulkan\\Include, F:\\vulkan\\Vulkan-Headers\\include.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VULKAN_ROOT = os.environ.get("EFMI_VK_VULKAN_ROOT", r"F:\vulkan")

LAYER_NAME = "VK_LAYER_EFMI_probe"
LAYER_DLL = "efmi_vk_layer.dll"
INJECT_DLL = "efmi_vk_inject.dll"
LAYER_JSON = "efmi_vk_layer.json"
SANDBOX_EXE = "efmi_vk_sandbox.exe"


class BuildError(RuntimeError):
    pass


def find_vcvars() -> str:
    explicit = os.environ.get("VCVARS")
    if explicit and os.path.isfile(explicit):
        return explicit
    vswhere = os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Microsoft Visual Studio", "Installer", "vswhere.exe",
    )
    if os.path.isfile(vswhere):
        out = subprocess.run(
            [vswhere, "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
            capture_output=True, text=True,
        ).stdout.strip().splitlines()
        if out:
            candidate = os.path.join(out[0], "VC", "Auxiliary", "Build", "vcvars64.bat")
            if os.path.isfile(candidate):
                return candidate
    raise BuildError("vcvars64.bat not found; set VCVARS to its full path")


def find_vulkan_include() -> str:
    candidates = []
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidates.append(os.path.join(sdk, "Include"))
    candidates.append(os.path.join(VULKAN_ROOT, "Include"))
    candidates.append(os.path.join(VULKAN_ROOT, "Vulkan-Headers", "include"))
    for c in candidates:
        if os.path.isfile(os.path.join(c, "vulkan", "vulkan_core.h")):
            return c
    raise BuildError(
        "Vulkan headers not found (looked in: %s); run scripts/fetch_vulkan_headers.ps1" % ", ".join(candidates)
    )


def run_cl(vcvars: str, cwd: str, args: list[str]) -> None:
    # vcvars64.bat has to run in the same cmd instance as cl. The command line is
    # written to a temporary batch file so that no quoting passes through cmd /c.
    quoted = " ".join('"%s"' % a if (" " in a or "(" in a) else a for a in args)
    script = os.path.join(cwd, "build", "_build.bat")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    with open(script, "w", encoding="ascii", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write('call "%s" >nul 2>&1\n' % vcvars)
        f.write("if errorlevel 1 exit /b 1\n")
        f.write("cl %s\n" % quoted)
        f.write("exit /b %errorlevel%\n")
    env = dict(os.environ)
    scratch = os.path.join(cwd, "build", "tmp")
    os.makedirs(scratch, exist_ok=True)
    env.update(TEMP=scratch, TMP=scratch)
    proc = subprocess.run(["cmd.exe", "/d", "/c", script], cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    output = (proc.stdout or "") + (proc.stderr or "")
    lines = [ln for ln in output.splitlines() if ln.strip()]
    noise = (".cpp", "Generating Code...", "Compiling...", "Creating library")
    shown = [ln for ln in lines if not (ln.strip().endswith(noise[0]) and ":" not in ln) and not ln.strip().startswith(noise[1:])]
    if proc.returncode != 0:
        raise BuildError("cl failed in %s:\n%s" % (cwd, "\n".join(lines)))
    for ln in shown:
        if "warning" in ln.lower():
            print("  " + ln)


def sources(folder: str) -> list[str]:
    src = os.path.join(folder, "src")
    return [os.path.join("src", n) for n in sorted(os.listdir(src)) if n.endswith(".cpp")]


COMMON = [
    "/nologo", "/std:c++17", "/O2", "/MT", "/EHsc", "/W3", "/GS", "/Zi", "/DNDEBUG",
    "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DVK_USE_PLATFORM_WIN32_KHR", "/D_CRT_SECURE_NO_WARNINGS",
]
# EFMI_VK_COMMAND_TIMING=1: the INI runtime times every command class (D-099), for a profiling build
# into a folder of its own; the measurement costs about as much as the commands.
if os.environ.get("EFMI_VK_COMMAND_TIMING") == "1":
    COMMON.append("/DEFMI_COMMAND_TIMING")
if os.environ.get("EFMI_VK_PHASE_TIMING") == "1":
    COMMON.append("/DEFMI_PHASE_TIMING")  # a few phase timers per draw and presentation, nearly free


# The replacement code as a module of its own (docs/hot-swap.md): what decides and records
# character replacements, without hooks and tracker. The layer DLL has the same code built in.
MODULE_DLL = "efmi_vk_backend.dll"
MODULE_SOURCES = [
    "module/module_main.cpp", "src/character_backend.cpp", "src/translucency.cpp", "src/shader_hooks.cpp", "src/character_package.cpp", "src/character_materials.cpp",
    "src/character_program.cpp", "src/character_present.cpp",
    "src/merged_skeleton.cpp", "src/pipeline_variant.cpp", "src/spirv_repoint.cpp",
    "src/variant_cache.cpp", "src/draw_snapshots.cpp", "src/pipeline_recipe.cpp", "../frontend/gpu/fragment_images.cpp", "../frontend/gpu/dither_kills.cpp",
    "src/texture_identity.cpp", "src/texture_residency.cpp",
    "src/bone_merge.cpp", "build/bone_merge_spirv.cpp",
]
DXC_EXE = "F:/vulkan/dxc-1.9.2609/bin/x64/dxc.exe"


def generate_bone_merge_shader() -> str:
    """The merged skeleton's compute shader (layer/shaders/bone_merge.hlsl, D-117) as SPIR-V words in a
    source of the module, made with the project's DXC; written only when its contents change."""
    folder = os.path.join(ROOT, "layer")
    source = os.path.join(folder, "shaders", "bone_merge.hlsl")
    out_dir = os.path.join(folder, "build")
    os.makedirs(out_dir, exist_ok=True)
    spv = os.path.join(out_dir, "bone_merge.spv")
    cpp = os.path.join(out_dir, "bone_merge_spirv.cpp")
    subprocess.run([DXC_EXE, "-T", "cs_6_0", "-E", "main", "-spirv", "-fspv-target-env=vulkan1.1", "-O3", "-Fo", spv, source], check=True)
    with open(spv, "rb") as f:
        data = f.read()
    if len(data) % 4:
        raise RuntimeError("bone_merge.spv is not a whole number of words")
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    lines = [", ".join("0x%08x" % w for w in words[i:i + 8]) for i in range(0, len(words), 8)]
    text = ("// Generated by scripts/build.py from layer/shaders/bone_merge.hlsl (D-117); do not edit.\n"
            "#include <cstddef>\n#include <cstdint>\nnamespace efvk {\nextern const uint32_t bone_merge_spirv[] = {\n"
            + ",\n".join(lines) + "\n};\nextern const size_t bone_merge_spirv_words = %d;\n}\n" % len(words))
    try:
        with open(cpp, "r", encoding="ascii") as f:
            current = f.read()
    except OSError:
        current = None
    if current != text:
        with open(cpp, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
    return cpp


def generate_overlay_shaders() -> str:
    """The overlay's vertex and fragment shaders (layer/shaders/overlay.hlsl, D-137) as SPIR-V words in a
    source of the layer, made with the project's DXC; written only when its contents change."""
    folder = os.path.join(ROOT, "layer")
    source = os.path.join(folder, "shaders", "overlay.hlsl")
    out_dir = os.path.join(folder, "build")
    os.makedirs(out_dir, exist_ok=True)
    cpp = os.path.join(out_dir, "overlay_spirv.cpp")
    parts = ["// Generated by scripts/build.py from layer/shaders/overlay.hlsl (D-137); do not edit.\n"
             "#include <cstddef>\n#include <cstdint>\nnamespace efvk {\n"]
    for stage, profile, entry in (("vertex", "vs_6_0", "vs_main"), ("fragment", "ps_6_0", "ps_main")):
        spv = os.path.join(out_dir, "overlay_%s.spv" % stage)
        subprocess.run([DXC_EXE, "-T", profile, "-E", entry, "-spirv", "-fspv-target-env=vulkan1.1", "-O3", "-Fo", spv, source],
                       check=True)
        with open(spv, "rb") as f:
            data = f.read()
        if len(data) % 4:
            raise RuntimeError("overlay_%s.spv is not a whole number of words" % stage)
        words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
        lines = [", ".join("0x%08x" % w for w in words[i:i + 8]) for i in range(0, len(words), 8)]
        parts.append("extern const uint32_t overlay_%s_spirv[] = {\n%s\n};\nextern const size_t overlay_%s_spirv_words = %d;\n"
                     % (stage, ",\n".join(lines), stage, len(words)))
    text = "".join(parts) + "}\n"
    try:
        with open(cpp, "r", encoding="ascii") as f:
            current = f.read()
    except OSError:
        current = None
    if current != text:
        with open(cpp, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
    return cpp
# Headers whose structures cross between the layer and a module. A module built from other
# contents of these is refused by the layer.
ABI_ROOTS = ["state.h", "character_module.h", "command_owner.h", "json.h"]

_runtime_link = None


def runtime_link() -> list[str]:
    global _runtime_link
    if _runtime_link is None:
        from pathlib import Path
        from build_xxmi_runtime import build_runtime
        print("Building original INI runtime...")
        _runtime_link = build_runtime(Path(ROOT) / "build/xxmi-runtime", test=False)
    return _runtime_link


def abi_stamp() -> str:
    """64-bit stamp over the shared headers and what they include from layer/src."""
    import hashlib
    import re
    src = os.path.join(ROOT, "layer", "src")
    seen, todo = [], list(ABI_ROOTS)
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.append(name)
        with open(os.path.join(src, name), encoding="utf-8") as f:
            text = f.read()
        todo += [m for m in re.findall(r'^#include "([^"/]+)"', text, re.M) if os.path.isfile(os.path.join(src, m))]
    digest = hashlib.sha256()
    for name in sorted(seen):
        with open(os.path.join(src, name), encoding="utf-8") as f:
            # The version text changes with every build of the replacement code and is no structure.
            lines = [ln for ln in f.read().splitlines() if not ln.startswith("#define EFVK_VERSION ")]
        digest.update(name.encode() + b"\0" + "\n".join(lines).encode() + b"\0")
    digest.update(" ".join(f for f in COMMON if not f.startswith("/DEFVK_ABI=")).encode())
    return "0x" + digest.hexdigest()[:16] + "ull"


def abi_headers() -> list[str]:
    import re
    src = os.path.join(ROOT, "layer", "src")
    seen, todo = [], list(ABI_ROOTS)
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.append(name)
        with open(os.path.join(src, name), encoding="utf-8") as f:
            todo += [m for m in re.findall(r'^#include "([^"/]+)"', f.read(), re.M) if os.path.isfile(os.path.join(src, m))]
    return sorted(seen)


def build_module(vcvars: str, include: str, out: str, mismatch: bool = False) -> str:
    """The module DLL. `mismatch`: a copy with another stamp, for the test that it is refused."""
    folder = os.path.join(ROOT, "layer")
    name = "module_mismatch" if mismatch else "module"
    os.makedirs(os.path.join(folder, "build", name), exist_ok=True)
    generate_bone_merge_shader()
    dll = os.path.join(out, "efmi_vk_backend_mismatch.dll" if mismatch else MODULE_DLL)
    flags = [f for f in COMMON if not f.startswith("/DEFVK_ABI=")]
    stamp = abi_stamp()
    flags.append("/DEFVK_ABI=" + (stamp[:-4] + ("0" if stamp[-4] != "0" else "1") + "ull" if mismatch else stamp))
    args = flags + ["/I" + include, "/Fobuild\\" + name + "\\", "/Fdbuild\\" + name + "\\module_cl.pdb", "/LD"]
    args += MODULE_SOURCES + MATERIAL_SOURCES + runtime_link() + [
        "/Fe:" + dll, "/link", "/NOLOGO", "/DLL", "/OPT:REF", "/OPT:ICF", "/DEBUG",
        "/PDB:build\\" + name + "\\" + name + ".pdb", "/IMPLIB:build\\" + name + "\\" + name + ".lib", "advapi32.lib", "user32.lib",
    ]
    run_cl(vcvars, folder, args)
    return dll


# Shared static material path also linked by tracker/recipe fixtures with the backend.
MATERIAL_SOURCES = ["../frontend/gpu/material_plan.cpp", "../frontend/gpu/source_registers.cpp", "../frontend/gpu/texture_roles.cpp", "../frontend/assets/dds.cpp",
                    "../frontend/assets/image_upload.cpp", "../frontend/assets/buffer_upload.cpp",
                    "../frontend/assets/resource_cache.cpp"]


def build_layer(vcvars: str, include: str, out: str) -> str:
    folder = os.path.join(ROOT, "layer")
    os.makedirs(os.path.join(folder, "build"), exist_ok=True)
    dll = os.path.join(out, LAYER_DLL)
    args = COMMON + [
        "/I" + include, "/Fobuild\\", "/Fdbuild\\efmi_vk_layer_cl.pdb", "/LD",
    ] + sources(folder) + [generate_bone_merge_shader(), generate_overlay_shaders(), "../frontend/gpu/fragment_images.cpp",
                           "../frontend/gpu/dither_kills.cpp"] + MATERIAL_SOURCES + runtime_link() + [
        "/Fe:" + dll,
        "/link", "/NOLOGO", "/DLL", "/OPT:REF", "/OPT:ICF", "/DEBUG",
        "/PDB:build\\efmi_vk_layer.pdb", "/IMPLIB:build\\efmi_vk_layer.lib",
        "advapi32.lib", "user32.lib", "gdi32.lib",
    ]
    run_cl(vcvars, folder, args)
    manifest = {
        "file_format_version": "1.2.0",
        "layer": {
            "name": LAYER_NAME,
            "type": "GLOBAL",
            "library_path": ".\\" + LAYER_DLL,
            "api_version": "1.4.321",
            "implementation_version": "1",
            "description": "EFMI Vulkan development probe",
            "functions": {
                "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
            },
        },
    }
    with open(os.path.join(out, LAYER_JSON), "w", encoding="ascii", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    # The file name selects the entry (see layer/src/util.cpp): the loader is given the layer
    # DLL through the manifest, an injector is given this copy.
    shutil.copyfile(dll, os.path.join(out, INJECT_DLL))
    return dll


def build_sandbox(vcvars: str, include: str, out: str) -> str:
    folder = os.path.join(ROOT, "sandbox")
    os.makedirs(os.path.join(folder, "build"), exist_ok=True)
    exe = os.path.join(out, SANDBOX_EXE)
    args = COMMON + [
        "/I" + include, "/Fobuild\\", "/Fdbuild\\efmi_vk_sandbox_cl.pdb",
    ] + sources(folder) + [
        "/Fe:" + exe,
        "/link", "/NOLOGO", "/OPT:REF", "/OPT:ICF", "/DEBUG", "/SUBSYSTEM:CONSOLE",
        "/PDB:build\\efmi_vk_sandbox.pdb",
        "user32.lib",
    ]
    run_cl(vcvars, folder, args)
    return exe


def exported_names(path: str) -> list[str]:
    """The names a PE32+ file exports."""
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    export_rva, = struct.unpack_from("<I", data, optional + 112)
    table = optional + optional_size

    def offset(rva: int) -> int:
        for i in range(sections):
            virtual_size, virtual_address, raw_size, raw = struct.unpack_from("<IIII", data, table + 40 * i + 8)
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return rva - virtual_address + raw
        raise BuildError("%s: address 0x%x is in no section" % (path, rva))

    directory = offset(export_rva)
    count, = struct.unpack_from("<I", data, directory + 24)
    names = offset(struct.unpack_from("<I", data, directory + 32)[0])
    out = []
    for i in range(count):
        start = offset(struct.unpack_from("<I", data, names + 4 * i)[0])
        out.append(data[start:data.index(b"\0", start)].decode("ascii"))
    return out


def build_loader_proxy(vcvars: str) -> list[str]:
    """Test fixtures (BUG-074): DLLs named vulkan-1.dll holding no code, every export of the system's
    loader forwarded, as proxy DLLs placed next to a game do: loader_proxy to the system's loader by
    its full path, loader_proxy_renamed to a copy of it named vulkan-1-real.dll beside the program."""
    folder = os.path.join(ROOT, "sandbox")
    system = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    names = exported_names(os.path.join(system, "vulkan-1.dll"))
    built = []
    for kind, module in (("loader_proxy", os.path.join(system, "vulkan-1")), ("loader_proxy_renamed", "vulkan-1-real")):
        out = os.path.join(folder, "build", kind)
        os.makedirs(out, exist_ok=True)
        lines = ["// Generated by scripts/build.py: forwarders only (BUG-074).\n"]
        for name in names:
            lines.append('#pragma comment(linker, "/export:%s=%s")\n' % (name, (module + "." + name).replace("\\", "\\\\")))
        with open(os.path.join(folder, "build", kind + ".c"), "w", encoding="ascii") as f:
            f.writelines(lines)
        dll = os.path.join(out, "vulkan-1.dll")
        run_cl(vcvars, folder, ["/nologo", "/LD", "/GS-", "build\\%s.c" % kind, "/Fobuild\\", "/Fe:" + dll,
                                "/link", "/NOLOGO", "/NOENTRY", "/NODEFAULTLIB"])
        built.append(dll)
    return built


def build_tracker_test(vcvars: str, include: str, out: str) -> str:
    folder = os.path.join(ROOT, "sandbox")
    scratch = os.path.join(folder, "build", "tracker")
    os.makedirs(scratch, exist_ok=True)
    exe = os.path.join(out, "efmi_vk_tracker_test.exe")
    args = COMMON + ["/I" + include, "/Fobuild\\tracker\\", "/Fdbuild\\tracker\\test_cl.pdb",
                     "/DEFVK_CHARACTER_CACHE_TESTS", "tracker_test.cpp", "../layer/src/state.cpp", "../layer/src/resources.cpp", "../layer/src/util.cpp",
                     "../layer/src/pipeline_recipe.cpp", "../layer/src/render_targets.cpp",
                     "../layer/src/pipeline_variant.cpp", "../layer/src/spirv_repoint.cpp",
                     "../frontend/gpu/fragment_images.cpp", "../frontend/gpu/dither_kills.cpp",
                     "../layer/src/variant_cache.cpp", "../layer/src/draw_snapshots.cpp",
                     "../layer/src/merged_skeleton.cpp", "../layer/src/m2_mesh.cpp", "../layer/src/m2_backend.cpp",
                     "../layer/src/character_package.cpp", "../layer/src/character_backend.cpp", "../layer/src/translucency.cpp", "../layer/src/shader_hooks.cpp", "../layer/src/character_program.cpp", "../layer/src/character_present.cpp", "../layer/src/texture_identity.cpp", "../layer/src/texture_residency.cpp", "../layer/src/character_materials.cpp",
                     "../layer/src/bone_merge.cpp", "../layer/build/bone_merge_spirv.cpp", "../layer/src/notices.cpp", generate_overlay_shaders()] + MATERIAL_SOURCES + runtime_link() + [
                     "/Fe:" + exe, "/link", "/NOLOGO", "/DEBUG", "/SUBSYSTEM:CONSOLE",
                     "/PDB:build\\tracker\\test.pdb", "user32.lib", "gdi32.lib"]
    run_cl(vcvars, folder, args)
    return exe


def build_recipe_tests(vcvars: str, include: str, out: str) -> dict:
    folder = os.path.join(ROOT, "sandbox")
    result = {}
    generate_bone_merge_shader()
    for name, tracker in (("recipe_test", True), ("variant_gpu", False), ("cache_test", False), ("snapshot_test", False),
                          ("mesh_test", False), ("character_test", False)):
        os.makedirs(os.path.join(folder, "build", name), exist_ok=True)
        exe = os.path.join(out, "efmi_vk_" + name + ".exe")
        sources = [name + ".cpp", "../layer/src/pipeline_recipe.cpp", "../layer/src/pipeline_variant.cpp",
                   "../layer/src/spirv_repoint.cpp", "../frontend/gpu/fragment_images.cpp", "../frontend/gpu/dither_kills.cpp"]
        if name in ("cache_test", "variant_gpu"):
            sources.append("../layer/src/variant_cache.cpp")
        if name in ("variant_gpu", "snapshot_test"):
            sources.append("../layer/src/draw_snapshots.cpp")
        if name == "mesh_test":
            sources += ["../layer/src/m2_mesh.cpp", "../layer/src/util.cpp"]
        if name == "character_test":
            # --plan-mods runs the directory generation's planning (D-078) offline: the program and the
            # original INI runtime join the decoder.
            sources += ["../layer/src/character_package.cpp", "../layer/src/character_materials.cpp", "../layer/src/util.cpp",
                        "../layer/src/character_program.cpp", "../layer/src/merged_skeleton.cpp", "../layer/src/shader_hooks.cpp"] + MATERIAL_SOURCES + runtime_link()
        if tracker:
            sources += ["../layer/src/state.cpp", "../layer/src/resources.cpp", "../layer/src/render_targets.cpp", "../layer/src/util.cpp",
                        "../layer/src/variant_cache.cpp", "../layer/src/draw_snapshots.cpp",
                        "../layer/src/merged_skeleton.cpp", "../layer/src/m2_mesh.cpp", "../layer/src/m2_backend.cpp",
                        "../layer/src/character_package.cpp", "../layer/src/character_backend.cpp", "../layer/src/translucency.cpp", "../layer/src/shader_hooks.cpp", "../layer/src/character_program.cpp", "../layer/src/character_present.cpp", "../layer/src/texture_identity.cpp", "../layer/src/texture_residency.cpp", "../layer/src/character_materials.cpp",
                        "../layer/src/bone_merge.cpp", "../layer/build/bone_merge_spirv.cpp", "../layer/src/notices.cpp", generate_overlay_shaders()] + MATERIAL_SOURCES + runtime_link()
        args = COMMON + ["/I" + include, "/Fobuild\\" + name + "\\", "/Fdbuild\\" + name + "\\test_cl.pdb"]
        if name == "recipe_test":
            # The dither patch's modules are validated with SPIRV-Tools (D-133).
            args.append("/IF:/vulkan/Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools/include")
        args += sources + ["/Fe:" + exe, "/link", "/NOLOGO", "/DEBUG", "/SUBSYSTEM:CONSOLE",
                           "/PDB:build\\" + name + "\\test.pdb", "user32.lib", "gdi32.lib"]
        run_cl(vcvars, folder, args)
        result[name] = exe
    return result


def build_bone_test(vcvars: str, include: str, out: str) -> str:
    folder = os.path.join(ROOT, "sandbox")
    os.makedirs(os.path.join(folder, "build", "bones"), exist_ok=True)
    exe = os.path.join(out, "efmi_vk_bone_test.exe")
    args = COMMON + ["/I" + include, "/Fobuild\\bones\\", "/Fdbuild\\bones\\test_cl.pdb",
                     "bone_test.cpp", "../layer/src/merged_skeleton.cpp", "/Fe:" + exe,
                     "/link", "/NOLOGO", "/DEBUG", "/SUBSYSTEM:CONSOLE", "/PDB:build\\bones\\test.pdb"]
    run_cl(vcvars, folder, args)
    return exe


# Tools owned GPU work loads from the layer's own folder (D-072): the HLSL compiler, the exact
# DXBC translation and the SPIR-V validator. Copied unchanged from the project's F:\vulkan.
OWNED_GPU_TOOLS = [
    "F:/vulkan/dxc-1.9.2609/bin/x64/dxcompiler.dll",
    "F:/vulkan/dxc-1.9.2609/bin/x64/dxil.dll",
    "F:/vulkan/vkd3d-2.1-build/efmi-vkd3d-shader.dll",
    "F:/vulkan/Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools/build/source/SPIRV-Tools-shared.dll",
]


def deploy_owned_gpu_tools(out: str) -> list[str]:
    copied = []
    for tool in OWNED_GPU_TOOLS:
        target = os.path.join(out, os.path.basename(tool))
        if not os.path.isfile(tool):
            raise BuildError("owned GPU tool missing: " + tool)
        if not os.path.isfile(target) or os.path.getsize(target) != os.path.getsize(tool) or os.path.getmtime(target) < os.path.getmtime(tool):
            shutil.copy2(tool, target)
        copied.append(target)
    return copied


# The playable release (docs/release.md): the layer with the inject entry and without the probe, the
# GPU tools it loads, its configuration, the README and the licences. Nothing for development: no
# probe pipe (EFVK_NO_PROBE, ipc.cpp left out), no tests, no sandbox, no replacement module files, no
# layer manifest, no symbols (they stay in layer/build/release for crash logs), no dxil.dll.
RELEASE_NAME = "EFMI-Vulkan"
RELEASE_TOOLS = [tool for tool in OWNED_GPU_TOOLS if os.path.basename(tool) != "dxil.dll"]
RELEASE_LICENSES = {
    "vkd3d-LGPL-2.1.txt": "F:/vulkan/vkd3d-2.1/LICENSE",
    "vkd3d-COPYING.txt": "F:/vulkan/vkd3d-2.1/COPYING",
    "SPIRV-Tools-Apache-2.0.txt": "F:/vulkan/Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools/LICENSE",
    "DirectXShaderCompiler-LLVM.txt": "F:/vulkan/dxc-1.9.2609/LICENSE-LLVM.txt",
    "DirectXShaderCompiler-MIT.txt": "F:/vulkan/dxc-1.9.2609/LICENCE-MIT.txt",
    "DirectXTK-MIT.txt": "F:/vulkan/XXMI-Libs-Package/DirectXTK/LICENSE",
    "SPIRV-Reflect-Apache-2.0.txt": "F:/vulkan/SPIRV-Reflect-1.4.321.0/LICENSE",
    "Vulkan-Headers-Apache-2.0.txt": "F:/vulkan/Vulkan-Headers/LICENSES/Apache-2.0.txt",
    "Vulkan-Headers-MIT.txt": "F:/vulkan/Vulkan-Headers/LICENSES/MIT.txt",
    "SPIRV-Headers-MIT.txt": "F:/vulkan/SPIRV-Headers-1.4.321.0/LICENSE",
}
# Licences that exist only as a source file's leading comment: (file, first line, last line of it).
RELEASE_LICENSE_COMMENTS = {
    "crc32c-hw-zlib.txt": ("F:/vulkan/XXMI-Libs-Package/crc32c-hw-1.0.5/src/crc32c.cpp", "/*", "*/"),
    "PCRE2-BSD.txt": ("F:/vulkan/XXMI-Libs-Package/pcre2/pcre2.h", "           Copyright (c) 2016-2017 University of Cambridge",
                      "-----------------------------------------------------------------------------"),
}
RELEASE_FILES = sorted([INJECT_DLL, "efmi_vk.cfg", "README.txt", "LICENSE.txt", "THIRD_PARTY_NOTICES.md",
                        "switch_renderer.bat", "switch_renderer.ps1"] +
                       [os.path.basename(tool) for tool in RELEASE_TOOLS] +
                       ["licenses/" + name for name in list(RELEASE_LICENSES) + list(RELEASE_LICENSE_COMMENTS)])


def license_comment(path: str, first: str, last: str) -> str:
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    begin = next(i for i, line in enumerate(lines) if line.rstrip() == first.rstrip())
    end = next(i for i in range(begin + 1, len(lines)) if lines[i].strip() == last.strip() and i > begin + 2)
    return "\n".join(lines[begin:end + 1]) + "\n"


def build_release(vcvars: str, include: str, out: str) -> dict:
    import hashlib
    import re
    import zipfile
    folder = os.path.join(ROOT, "layer")
    objects = os.path.join(folder, "build", "release")
    os.makedirs(objects, exist_ok=True)
    out = os.path.abspath(out)
    if os.path.isdir(out):
        if not os.path.abspath(out).startswith(os.path.join(ROOT, "build") + os.sep):
            raise BuildError("release output must be inside build/: " + out)
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "licenses"))
    dll = os.path.join(out, INJECT_DLL)
    layer_sources = [name for name in sources(folder) if os.path.basename(name) != "ipc.cpp"]
    args = COMMON + [
        "/DEFVK_NO_PROBE", "/I" + include, "/Fobuild\\release\\", "/Fdbuild\\release\\efmi_vk_inject_cl.pdb", "/LD",
    ] + layer_sources + [generate_bone_merge_shader(), generate_overlay_shaders(), "../frontend/gpu/fragment_images.cpp",
                         "../frontend/gpu/dither_kills.cpp"] + MATERIAL_SOURCES + runtime_link() + [
        "/Fe:" + dll,
        "/link", "/NOLOGO", "/DLL", "/OPT:REF", "/OPT:ICF", "/DEBUG",
        "/PDB:build\\release\\efmi_vk_inject.pdb", "/IMPLIB:build\\release\\efmi_vk_inject.lib",
        "advapi32.lib", "user32.lib", "gdi32.lib",
    ]
    run_cl(vcvars, folder, args)
    for leftover in ("efmi_vk_inject.exp", "efmi_vk_inject.lib"):
        if os.path.isfile(os.path.join(out, leftover)):
            os.remove(os.path.join(out, leftover))
    for tool in RELEASE_TOOLS:
        shutil.copy2(tool, os.path.join(out, os.path.basename(tool)))
    shutil.copy2(os.path.join(ROOT, "release", "efmi_vk.cfg"), os.path.join(out, "efmi_vk.cfg"))
    shutil.copy2(os.path.join(ROOT, "release", "README.txt"), os.path.join(out, "README.txt"))
    # The DX11/Vulkan switch for the launcher's EFMI profile (release/README.txt, "Switching").
    shutil.copy2(os.path.join(ROOT, "release", "switch_renderer.bat"), os.path.join(out, "switch_renderer.bat"))
    shutil.copy2(os.path.join(ROOT, "release", "switch_renderer.ps1"), os.path.join(out, "switch_renderer.ps1"))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE.txt"))
    shutil.copy2(os.path.join(ROOT, "THIRD_PARTY_NOTICES.md"), os.path.join(out, "THIRD_PARTY_NOTICES.md"))
    for name, path in RELEASE_LICENSES.items():
        shutil.copy2(path, os.path.join(out, "licenses", name))
    for name, (path, first, last) in RELEASE_LICENSE_COMMENTS.items():
        with open(os.path.join(out, "licenses", name), "w", encoding="utf-8", newline="\n") as f:
            f.write(license_comment(path, first, last))
    found = sorted(os.path.relpath(os.path.join(d, n), out).replace(os.sep, "/") for d, _, names in os.walk(out) for n in names)
    if found != RELEASE_FILES:
        raise BuildError("release files differ from the list: extra %s, missing %s" %
                         (sorted(set(found) - set(RELEASE_FILES)), sorted(set(RELEASE_FILES) - set(found))))
    with open(os.path.join(folder, "src", "common.h"), encoding="utf-8") as f:
        version = re.search(r'#define EFVK_VERSION "([0-9.]+)"', f.read()).group(1)
    archive = os.path.join(os.path.dirname(out), "%s-%s.zip" % (RELEASE_NAME, version))
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for name in RELEASE_FILES:
            z.write(os.path.join(out, name), RELEASE_NAME + "/" + name)
    digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
    with open(archive + ".sha256", "w", encoding="ascii", newline="\n") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(archive)))
    return {"release": out, "release_archive": archive, "release_sha256": digest}


def build(targets, out: str | None = None) -> dict:
    release_out = out or os.path.join(ROOT, "build", "release", RELEASE_NAME)
    out = os.path.abspath(out or os.path.join(ROOT, "dist"))
    os.makedirs(out, exist_ok=True)
    vcvars = find_vcvars()
    include = find_vulkan_include()
    result = {"out": out, "include": include}
    # Every binary of one build carries the stamp of the shared headers it was built from.
    COMMON[:] = [f for f in COMMON if not f.startswith("/DEFVK_ABI=")] + ["/DEFVK_ABI=" + abi_stamp()]
    result["abi"] = abi_stamp()
    if "module" in targets:
        result["module"] = build_module(vcvars, include, out)
    if "layer" in targets:
        result["layer"] = build_layer(vcvars, include, out)
        result["module"] = build_module(vcvars, include, out)
        result["module_mismatch"] = build_module(vcvars, include, out, mismatch=True)
        result["tracker_test"] = build_tracker_test(vcvars, include, out)
        result.update(build_recipe_tests(vcvars, include, out))
        result["bone_test"] = build_bone_test(vcvars, include, out)
        result["owned_gpu_tools"] = deploy_owned_gpu_tools(out)
    if "sandbox" in targets:
        result["sandbox"] = build_sandbox(vcvars, include, out)
        result["loader_proxy"] = build_loader_proxy(vcvars)
    if "release" in targets:
        result.update(build_release(vcvars, include, release_out))
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the EFMI Vulkan layer and sandbox")
    ap.add_argument("targets", nargs="*", choices=["layer", "sandbox", "module", "release"],
                    help="what to build (default: layer and sandbox; 'module' builds the replacement module alone; "
                         "'release' the playable package in build/release)")
    ap.add_argument("--out", help="output directory (default: dist)")
    args = ap.parse_args()
    targets = args.targets or ["layer", "sandbox"]
    try:
        result = build(targets, args.out)
    except BuildError as e:
        print(str(e))
        return 1
    print("shared header stamp %s" % result["abi"])
    for key in ("layer", "module", "sandbox", "tracker_test", "recipe_test", "variant_gpu", "bone_test", "release", "release_archive"):
        if key in result:
            print("built %s" % result[key])
    if "release_sha256" in result:
        print("release sha256 %s" % result["release_sha256"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
