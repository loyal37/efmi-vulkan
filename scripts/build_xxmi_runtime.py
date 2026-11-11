"""Build the pinned original XXMI runtime without modifying its dependencies."""
from __future__ import annotations
import argparse
import contextlib
import io
import hashlib
import json
import os
from pathlib import Path
import subprocess

from build import find_vcvars, find_vulkan_include, run_cl
from probe_xxmi_host import DEPS, PINS, ROOT, digest, pinned_checkout, write_ascii


def runtime_flags(out: Path) -> list[str]:
    upstream = DEPS / "XXMI-Libs-Package"
    port = DEPS / "wwmi-dx12/reshade_addon/migoto"
    pristine = DEPS / "efmi-xxmi-host-prototype/pristine"
    includes = [ROOT / "frontend/xxmi", Path(find_vulkan_include()), port, pristine, upstream,
                DEPS / "dxc-1.9.2609/inc", DEPS / "SPIRV-Reflect-1.4.321.0", DEPS / "vkd3d-2.1/include",
                DEPS / "SPIRV-Headers-1.4.321.0/include",
                DEPS / "Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools/include",
                upstream / "HLSLDecompiler", upstream / "D3D_Shaders", upstream / "DirectXTK/Inc",
                upstream / "DirectXTK/Src", upstream / "crc32c-hw-1.0.5/include", upstream / "pcre2"]
    flags = ["/nologo", "/std:c++17", "/permissive", "/W0", "/O2", "/MT", "/EHa", "/Gy", "/Gw",
             "/DUNICODE", "/D_UNICODE", "/D_HAS_STD_BYTE=0", "/DMIGOTO_DX=11",
             "/DPCRE2_CODE_UNIT_WIDTH=8", "/DPCRE2_STATIC", "/DCRC32C_STATIC", "/D_CRT_SECURE_NO_WARNINGS",
             "/D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING", "/D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1",
             "/D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES_COUNT=1", "/FI" + str(port / "port_compat.h"),
             # The original's configuration globals belong to the entered generation (D-129).
             "/FI" + str(ROOT / "frontend/xxmi/engine_globals.h")]
    if os.environ.get("EFMI_VK_COMMAND_TIMING") == "1":
        flags.append("/DEFMI_COMMAND_TIMING")  # per-command timing (D-099), as scripts/build.py
    if os.environ.get("EFMI_VK_PHASE_TIMING") == "1":
        flags.append("/DEFMI_PHASE_TIMING")
    if os.environ.get("EFMI_VK_COMMAND_TIMING") == "1" or os.environ.get("EFMI_VK_PHASE_TIMING") == "1":
        flags.append("/Z7")  # debug information for scripts/sample_profile.py in a profiling build
    return flags + ["/I" + str(path) for path in includes] + ["/Fo" + str(out) + "/"]


def build_runtime(out: Path, test: bool = False) -> list[str]:
    upstream, reference = DEPS / "XXMI-Libs-Package", DEPS / "wwmi-dx12"
    pinned_checkout(upstream, PINS["xxmi"])
    pinned_checkout(reference, PINS["reference"])
    reflection = DEPS / "SPIRV-Reflect-1.4.321.0"
    pinned_checkout(reflection, "e55086b044225f9b511ae44dbf9b079d3625943f")
    validator = DEPS / "Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools"
    pinned_checkout(validator, "33e02568181e3312f49a3cf33df470bf96ef293a")
    port = reference / "reshade_addon/migoto"
    pristine = DEPS / "efmi-xxmi-host-prototype/pristine"
    for line in (port / "PRISTINE.txt").read_text().splitlines():
        if (line := line.strip()) and not line.startswith("#"):
            original = upstream / line
            if digest(original) != digest(pristine / original.name):
                raise RuntimeError("Original source staging mismatch; run probe_xxmi_host.py")
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Stage the unchanged declaration reader next to its token header so its
    # globals include resolves through the existing pinned host include paths.
    for name in ("ByteCodeReader.cpp", "ShaderRegex.cpp", "d3d11TokenizedProgramFormat.hpp"):
        original, staged = upstream / "DirectX11" / name, out / name
        if not staged.exists() or digest(original) != digest(staged):
            staged.write_bytes(original.read_bytes())
    legacy = [pristine / (name + ".cpp") for name in ("profiling", "util")]
    legacy += [out / "ByteCodeReader.cpp",out / "ShaderRegex.cpp"]
    legacy += [upstream / "D3D_Shaders" / (name + ".cpp") for name in ("Assembler", "SignatureParser")]
    legacy += [ROOT / "frontend/xxmi" / (name + ".cpp") for name in ("command_module", "resource_hash_module", "runtime_override")]
    sources = [ROOT / "frontend/xxmi" / (name + ".cpp") for name in
               ("runtime", "runtime_input", "runtime_parser", "runtime_shim", "draw_plan", "resource_plan", "shader_state", "shader_regex", "shader_assembly", "gpu_resources", "gpu_bindings", "gpu_images", "gpu_parameters", "gpu_shaders", "gpu_graphics", "input_layout", "gpu_layout", "image_formats", "owned_generation", "owned_runtime")]
    sources += [ROOT / "frontend/xxmi" / (name + ".cpp") for name in ("character_core", "efmittl_bridge", "skeleton_plan", "palette_files")]
    sources += [ROOT / "frontend/core/palette_bank.cpp", ROOT / "frontend/assets/png.cpp"]
    sources += [ROOT / "frontend/xxmi/buffer_queries.cpp", ROOT / "frontend/xxmi/texture_properties.cpp"]
    sources += [ROOT / "frontend/gpu" / (name + ".cpp") for name in
                ("resource_commands", "resource_images", "hlsl_compiler", "shader_cache", "dxbc_compiler", "dxbc_float32", "dxbc_bindings", "graphics_link", "vertex_inputs", "shader_reflection", "shader_usage", "shader_commands", "graphics_commands")]
    sources += [upstream / f"DirectXTK/Src/{name}.cpp" for name in ("DDSTextureLoader", "WICTextureLoader")]
    sources += [upstream / "crc32c-hw-1.0.5/src/crc32c.cpp"]
    flags = runtime_flags(out)
    libraries = ["user32.lib", "gdi32.lib", "advapi32.lib", "d3dcompiler.lib", "dxguid.lib", "ole32.lib", "windowscodecs.lib",
                 "xinput.lib", "dbghelp.lib", str(upstream / "pcre2/pcre2-8-64.lib")]
    objects = [str(out / (path.stem + ".obj")) for path in legacy + sources]
    os.environ["VSLANG"] = "1033"
    vcvars = find_vcvars()
    output = io.StringIO()
    stamp_file = out / "objects.json"
    stamps = json.loads(stamp_file.read_text()) if stamp_file.exists() else {}
    common_files = list(pristine.glob("*")) + list(port.glob("*.h"))
    common_files += [out / "d3d11TokenizedProgramFormat.hpp"]
    common_files += [DEPS / "vkd3d-2.1/include/vkd3d_shader.h", DEPS / "vkd3d-2.1/include/vkd3d_types.h"]
    common_files += [DEPS / "SPIRV-Headers-1.4.321.0/include/spirv/unified1/GLSL.std.450.h"]
    common_files += [upstream / "D3D_Shaders/stdafx.h", upstream / "D3D_Shaders/targetver.h", upstream / "HLSLDecompiler/shader.h"]
    common_files += list((ROOT / "frontend").rglob("*.h")) + [ROOT / "frontend/xxmi/ini_module.cpp"]
    common_files += list((ROOT / "layer/src").glob("*.h"))
    common_files += [DEPS / "dxc-1.9.2609/inc/dxcapi.h", reflection / "spirv_reflect.h",
                     reflection / "spirv_reflect.c", reflection / "include/spirv/unified1/spirv.h",
                     validator / "include/spirv-tools/libspirv.h"]
    common_hash = hashlib.sha256("".join(digest(p) for p in sorted(common_files) if p.is_file()).encode()).hexdigest()
    def compile_sources(units, extra):
        pending, next_stamps = [], {}
        for unit in units:
            stamp = hashlib.sha256((common_hash + digest(unit) + repr(flags + extra)).encode()).hexdigest()
            next_stamps[str(unit)] = stamp
            if stamps.get(str(unit)) != stamp or not (out / (unit.stem + ".obj")).exists():
                pending.append(str(unit))
        if pending:
            run_cl(vcvars, str(out), flags + extra + ["/c"] + pending)
        stamps.update(next_stamps)
        stamp_file.write_text(json.dumps(stamps, indent=2), encoding="ascii")
    try:
        with contextlib.redirect_stdout(output):
            compile_sources(legacy, [])
            compile_sources(sources, ["/DNOMINMAX"])
            if test:
                run_cl(vcvars, str(out), flags + ["/DNOMINMAX", str(ROOT / "sandbox/xxmi_runtime.cpp"),
                       str(ROOT / "layer/src/character_package.cpp"), str(ROOT / "layer/src/merged_skeleton.cpp"),
                       str(ROOT / "layer/src/pipeline_recipe.cpp"), str(ROOT / "layer/src/character_program.cpp"), str(ROOT / "frontend/assets/dds.cpp"),
                       *objects, "/Fe:" + str(out / "runtime_test.exe"), "/link", "/NOLOGO", "/OPT:REF", "/OPT:ICF", *libraries])
    except Exception as exc:
        output.write(str(exc))
        raise
    finally:
        write_ascii(out / "build.txt", output.getvalue())
    return objects + libraries


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/xxmi-runtime")
    args = parser.parse_args()
    build_runtime(args.out, test=True)
    package = ROOT / "build/character/arcane-materials.bin"
    if package.is_file():
        from prepare_runtime import prepare
        mod = Path("D:/XXMI-Launcher-Portable-v2.1.6/EFMI/Mods/arcane fes 1.2")
        prepare(package, package.with_suffix(".json"), mod / "arcane.ini", mod.parents[1] / "Core/EFMI",
                "arcane_fes", args.out / "fixture/actual-live.bin")
    result = subprocess.run([str(args.out.resolve() / "runtime_test.exe"), str(args.out.resolve())],
                            capture_output=True, text=True, timeout=40)
    write_ascii(args.out / "runtime.txt", result.stdout + result.stderr)
    print(result.stdout)
    if result.returncode:
        print(result.stderr[-6000:])
    if result.returncode == 0:
        second = ROOT / "build/characters/yvonne/mesh.bin"
        source = Path("D:/XXMI-Launcher-Portable-v2.1.6/EFMI/Mods/Yvonne Ensnared Heart 1.6/yvonne.ini")
        if second.is_file() and source.is_file():
            result = subprocess.run([str(args.out.resolve() / "runtime_test.exe"), "--inspect-character",
                str(second), str(source), str(source.parents[2] / "Core/EFMI"), "yvonne", "texture-roles"],
                capture_output=True, text=True, timeout=40)
            write_ascii(second.parent / "runtime-inspection.out", result.stdout + result.stderr)
            print(result.stdout if not result.returncode else result.stderr[-6000:])
    raise SystemExit(result.returncode)
