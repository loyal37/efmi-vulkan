"""Build and run the pinned, headless XXMI CPU host prototype.

This does not build/deploy the layer or load a mod into the game. Upstream
checkouts are read-only; staged sources stay under F:/vulkan and generated
objects, diagnostics and the executable stay under this project's build/.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from build import BuildError, find_vcvars, find_vulkan_include, run_cl

ROOT = Path(__file__).resolve().parents[1]
DEPS = Path("F:/vulkan")
PINS = {
    "xxmi": "a7847eae82aaac32175c9a7d199673d5c9323bcd",
    "reference": "113f42cc2dc5702dbba624ee16abac96d68ed3b5",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pinned_checkout(path: Path, commit: str) -> dict:
    head = subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if head != commit:
        raise BuildError(f"Wrong dependency revision in {path}: {head}")
    changed = subprocess.run(
        ["git", "-C", str(path), "diff", "--quiet", "HEAD", "--"], check=False)
    if changed.returncode:
        raise BuildError(f"Dependency has tracked changes or cannot be verified: {path}")
    return {"path": str(path), "commit": head, "tracked_tree_clean": True}


def write_ascii(path: Path, text: str) -> None:
    path.write_text(text.encode("ascii", "backslashreplace").decode("ascii"),
                    encoding="ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xxmi", type=Path, default=DEPS / "XXMI-Libs-Package")
    parser.add_argument("--reference", type=Path, default=DEPS / "wwmi-dx12")
    parser.add_argument("--reuse", action="store_true", help="reuse a passing executable only when every recorded input still matches")
    args = parser.parse_args()
    out = ROOT / "build/xxmi-host"
    out.mkdir(parents=True, exist_ok=True)
    report = {"scope": "standalone CPU host; no GPU/layer/game compatibility claim",
              "pins": PINS, "compiled": False, "executed": False, "passed": False}
    try:
        upstream, reference = args.xxmi.resolve(), args.reference.resolve()
        report["inputs"] = {
            "xxmi": pinned_checkout(upstream, PINS["xxmi"]),
            "reference": pinned_checkout(reference, PINS["reference"]),
        }
        port = reference / "reshade_addon/migoto"
        pristine = DEPS / "efmi-xxmi-host-prototype/pristine"
        pristine.mkdir(parents=True, exist_ok=True)
        inputs = [port / "PRISTINE.txt"]
        for line in inputs[0].read_text(encoding="utf-8").splitlines():
            if not (line := line.strip()) or line.startswith("#"):
                continue
            source = (upstream / line).resolve()
            if not source.is_relative_to(upstream):
                raise BuildError("Invalid path in the pinned pristine-source manifest")
            destination = pristine / source.name
            shutil.copyfile(source, destination)
            if digest(source) != digest(destination):
                raise BuildError(f"Staging mismatch: {source}")
            inputs.append(source)
        (pristine / "version.generated.h").write_text(
            "#define VERSION_MAJOR 1\n#define VERSION_MINOR 2\n"
            "#define VERSION_REVISION 2\n#define VERSION_BUILD 0\n", encoding="ascii")
        fixture = ROOT / "sandbox/xxmi_host"
        legacy_sources = [pristine / f"{name}.cpp" for name in
                          ("profiling", "util", "Override")]
        legacy_sources += [ROOT / "frontend/xxmi/command_module.cpp", ROOT / "frontend/xxmi/resource_hash_module.cpp"]
        sources = [fixture / f"{name}.cpp" for name in ("main", "shim", "ini", "host_input")]
        sources += [fixture / "plan_test.cpp", fixture / "mod_plan.cpp", ROOT / "frontend/xxmi/draw_plan.cpp"]
        sources += [fixture / "folder_parse.cpp"]
        sources += [fixture / "resource_plan_test.cpp", ROOT / "frontend/xxmi/resource_plan.cpp"]
        sources += [fixture / "dds_test.cpp", ROOT / "frontend/assets/dds.cpp"]
        sources += [fixture / "core_pool_test.cpp"]
        sources += [fixture / "mod_skeleton.cpp", ROOT / "frontend/xxmi/skeleton_plan.cpp"]
        sources += [fixture / "skeleton_plan_test.cpp"]
        sources += [fixture / "core_spatial_test.cpp", ROOT / "frontend/xxmi/buffer_queries.cpp"]
        sources += [ROOT / "frontend/xxmi/palette_files.cpp", ROOT / "frontend/core/palette_bank.cpp",
                    ROOT / "layer/src/merged_skeleton.cpp"]
        sources += [ROOT / "frontend/xxmi/spatial_palettes.cpp"]
        sources += [fixture / "core_native_test.cpp"]
        sources += [fixture / "texture_query_test.cpp"]
        sources += [fixture / "core_component_test.cpp", ROOT / "frontend/xxmi/component_bridge.cpp"]
        sources += [fixture / "vertex_plan_test.cpp", ROOT / "frontend/xxmi/vertex_plan.cpp",
                    ROOT / "layer/src/pipeline_recipe.cpp"]
        sources += [upstream / f"DirectXTK/Src/{name}.cpp" for name in
                    ("DDSTextureLoader", "WICTextureLoader")]
        sources += [upstream / "crc32c-hw-1.0.5/src/crc32c.cpp"]
        regex_lib = upstream / "pcre2/pcre2-8-64.lib"
        inputs += legacy_sources + sources + list(port.glob("*.h")) + [regex_lib, Path(__file__),
                  ROOT / "frontend/xxmi/draw_plan.h", ROOT / "frontend/xxmi/draw_state.h", ROOT / "frontend/assets/dds.h"]
        inputs += [ROOT / "frontend/xxmi/resource_plan.h", ROOT / "frontend/xxmi/resource_match.h"] + list((ROOT / "frontend/assets").glob("*.h"))
        inputs += [ROOT / "frontend/xxmi/ini_module.cpp", ROOT / "frontend/xxmi/rabbit_textures.h"]
        inputs += [ROOT / "frontend/xxmi/skeleton_plan.h"]
        inputs += [ROOT / "frontend/xxmi/buffer_queries.h"]
        inputs += [ROOT / "frontend/xxmi/palette_files.h", ROOT / "frontend/core/palette_bank.h",
                   ROOT / "layer/src/merged_skeleton.h"]
        inputs += [ROOT / "frontend/xxmi/spatial_palettes.h"]
        inputs += [ROOT / "frontend/xxmi/component_bridge.h"]
        inputs += [ROOT / "frontend/xxmi/vertex_plan.h", ROOT / "frontend/assets/file_draws.h",
                   ROOT / "layer/src/pipeline_recipe.h", ROOT / "sandbox/material_fixture.h", ROOT / "sandbox/src/spirv.h"]
        inputs += [ROOT / "layer/src" / name for name in ("common.h", "state.h", "json.h")]
        report["source_sha256"] = {str(p): digest(p) for p in sorted(set(inputs))}
        includes = [ROOT / "frontend/xxmi", ROOT / "frontend/assets", Path(find_vulkan_include()),
                    port, pristine, upstream, upstream / "HLSLDecompiler",
                    upstream / "D3D_Shaders", upstream / "DirectXTK/Inc",
                    upstream / "DirectXTK/Src", upstream / "crc32c-hw-1.0.5/include",
                    upstream / "pcre2"]
        vcvars = find_vcvars()
        report["vcvars"] = vcvars
        exe = out / "host_probe.exe"
        if args.reuse and exe.is_file() and (out / "report.json").is_file():
            previous = json.loads((out / "report.json").read_text())
            if (previous.get("passed") and previous.get("source_sha256") == report["source_sha256"]
                    and previous.get("executable_sha256") == digest(exe) and previous.get("vcvars") == vcvars):
                report = previous
                print("Pinned XXMI host checks passed; verified unchanged inputs and executable")
                return 0
        flags = ["/nologo", "/std:c++17", "/permissive", "/W0", "/Od", "/MT", "/EHa",
                 "/Gy", "/Gw", "/DUNICODE", "/D_UNICODE", "/D_HAS_STD_BYTE=0",
                 "/DMIGOTO_DX=11", "/DPCRE2_CODE_UNIT_WIDTH=8", "/DPCRE2_STATIC",
                 "/DCRC32C_STATIC", "/D_CRT_SECURE_NO_WARNINGS",
                 "/D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",
                 "/D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1",
                 "/D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES_COUNT=1",
                 "/FI" + str(port / "port_compat.h")]
        flags += ["/I" + str(p) for p in includes]
        flags += ["/Fo" + str(out) + "/"]
        compile_flags = flags + ["/c"] + [str(p) for p in legacy_sources]
        # The legacy units depend on Win32 min/max accepting mixed integer
        # types; the CRC source needs NOMINMAX before the forced Win32 header.
        link_flags = flags + ["/DNOMINMAX"] + [str(p) for p in sources]
        link_flags += [str(out / (p.stem + ".obj")) for p in legacy_sources]
        link_flags += ["/Fe:" + str(exe), "/link", "/NOLOGO",
                  "/OPT:REF", "/OPT:ICF", "user32.lib", "gdi32.lib", "advapi32.lib",
                  "d3dcompiler.lib", "dxguid.lib", "ole32.lib", "xinput.lib", "dbghelp.lib",
                  str(regex_lib)]
        # run_cl uses a batch file. Reject shell metacharacters in path overrides.
        if any(any(c in value for c in '\"&|<>^%!\r\n') for value in [vcvars, *compile_flags, *link_flags]):
            raise BuildError("A compiler argument contains unsupported shell characters")
        os.environ["VSLANG"] = "1033"  # Only this script and its compiler children.
        print("Building pinned XXMI CPU host (no graphics device or hooks)...", flush=True)
        build_output = io.StringIO()
        try:
            with contextlib.redirect_stdout(build_output):
                run_cl(vcvars, str(out), compile_flags)
                run_cl(vcvars, str(out), link_flags)
        except BuildError as exc:
            build_output.write(str(exc))
            raise
        finally:
            write_ascii(out / "build.txt", build_output.getvalue())
        report.update(compiled=True, executable_sha256=digest(exe))
        env = dict(os.environ, TEMP=str(out / "build/tmp"), TMP=str(out / "build/tmp"))
        result = subprocess.run([str(exe)], cwd=out, env=env, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=30)
        output = result.stdout + result.stderr
        write_ascii(out / "runtime.txt", output)
        report.update(executed=True, returncode=result.returncode)
        counts = re.search(r"XXMI hosted CPU prototype: (\d+) checks passed, (\d+) failed", output)
        report["checks_passed"] = int(counts[1]) if counts else 0
        report["checks_failed"] = int(counts[2]) if counts else None
        planner = re.search(r"XXMI draw planner: (\d+) checks passed, (\d+) failed", output)
        report["planner_passed"] = int(planner[1]) if planner else 0
        report["planner_failed"] = int(planner[2]) if planner else None
        dds = re.search(r"DDS asset metadata: (\d+) checks passed, (\d+) failed", output)
        report["dds_passed"] = int(dds[1]) if dds else 0
        report["dds_failed"] = int(dds[2]) if dds else None
        resources = re.search(r"XXMI resource planner: (\d+) checks passed, (\d+) failed", output)
        report["resources_passed"] = int(resources[1]) if resources else 0
        report["resources_failed"] = int(resources[2]) if resources else None
        pools = re.search(r"XXMI Core pools: (\d+) checks passed, (\d+) failed", output)
        report["pools_passed"] = int(pools[1]) if pools else 0
        report["pools_failed"] = int(pools[2]) if pools else None
        skeleton = re.search(r"XXMI skeleton metadata: (\d+) checks passed, (\d+) failed", output)
        report["skeleton_passed"] = int(skeleton[1]) if skeleton else 0
        report["skeleton_failed"] = int(skeleton[2]) if skeleton else None
        native = re.search(r"XXMI native Core bridge: (\d+) checks passed, (\d+) failed", output)
        report["native_core_passed"] = int(native[1]) if native else 0
        report["native_core_failed"] = int(native[2]) if native else None
        vertex = re.search(r"XXMI vertex plans: (\d+) checks passed, (\d+) failed", output)
        report["vertex_passed"] = int(vertex[1]) if vertex else 0
        report["vertex_failed"] = int(vertex[2]) if vertex else None
        texture = re.search(r"XXMI texture queries: (\d+) checks passed, (\d+) failed", output)
        report["texture_queries_passed"] = int(texture[1]) if texture else 0
        report["texture_queries_failed"] = int(texture[2]) if texture else None
        report["passed"] = (result.returncode == 0 and bool(counts) and int(counts[2]) == 0
                            and bool(planner) and int(planner[2]) == 0 and bool(dds) and int(dds[2]) == 0
                            and bool(resources) and int(resources[2]) == 0 and bool(pools) and int(pools[2]) == 0
                            and bool(skeleton) and int(skeleton[2]) == 0 and bool(native) and int(native[2]) == 0
                            and bool(vertex) and int(vertex[2]) == 0 and bool(texture) and int(texture[2]) == 0)
        print(counts[0] if counts else f"Host process failed: {result.returncode}", flush=True)
        if planner:
            print(planner[0], flush=True)
        if dds:
            print(dds[0], flush=True)
        if resources:
            print(resources[0], flush=True)
        if pools:
            print(pools[0], flush=True)
        if skeleton:
            print(skeleton[0], flush=True)
        if native:
            print(native[0], flush=True)
        if vertex:
            print(vertex[0], flush=True)
        if texture:
            print(texture[0], flush=True)
        return 0 if report["passed"] else 1
    except (BuildError, OSError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        print(str(exc).encode("ascii", "backslashreplace").decode("ascii"), file=sys.stderr)
        return 1
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n",
                                         encoding="ascii")
        print(f"Report: {out / 'report.json'}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
