"""Compile Vulkan-native HLSL to a validated, atomically published ShaderFixes file."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

DXC = Path("F:/vulkan/dxc-1.9.2609/bin/x64/dxc.exe")
VALIDATOR = Path("F:/vulkan/Vulkan-ValidationLayers/external/Release/64/SPIRV-Tools/build/tools/spirv-val.exe")


def compile_shader(source: Path, folder: Path, shader_hash: str, stage: str, *, entry="main",
                   model="6_0", target="vulkan1.1", dxc=DXC, validator=VALIDATOR) -> Path:
    if not re.fullmatch(r"[0-9a-fA-F]{8}", shader_hash) or stage not in ("vs", "hs", "ds", "gs", "ps"):
        raise ValueError("Use the original eight-digit SPIR-V hash and a graphics stage")
    folder.mkdir(parents=True, exist_ok=True)
    output = folder / (shader_hash.lower() + "-" + stage + "_replace.spv")
    # No legacy register shifts or DX11 layout assumptions: declare Vulkan set/binding explicitly.
    with tempfile.TemporaryDirectory(prefix="compile-", dir=folder) as temp:
        pending = Path(temp) / "pending.spv"
        commands = ([str(dxc), "-spirv", "-fspv-target-env=" + target, "-T", stage + "_" + model,
                     "-E", entry, "-Fo", str(pending), str(source.resolve())],
                    [str(validator), "--target-env", target, str(pending)])
        for command in commands:
            run = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace", timeout=120)
            if run.returncode:
                raise RuntimeError((run.stdout + run.stderr).strip() or "Shader tool failed")
        pending.replace(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--hash", required=True, help="original SPIR-V CRC-32C from hunting")
    parser.add_argument("--stage", required=True, choices=("vs", "hs", "ds", "gs", "ps"))
    parser.add_argument("--output-dir", required=True, type=Path, help="the layer settings/ShaderFixes directory")
    parser.add_argument("--entry", default="main", help="must match the game's entry point")
    parser.add_argument("--model", default="6_0", choices=("6_0", "6_1", "6_2", "6_3", "6_4", "6_5", "6_6", "6_7", "6_8"))
    parser.add_argument("--target", default="vulkan1.1", choices=("vulkan1.1", "vulkan1.2", "vulkan1.3"))
    args = parser.parse_args()
    try:
        output = compile_shader(args.source, args.output_dir, args.hash, args.stage, entry=args.entry, model=args.model, target=args.target)
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, str(error) + "\n")
    print(str(output.resolve()))
    print("Compiled and validated offline. F10 loads this file; device features and the game interface are checked there.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
