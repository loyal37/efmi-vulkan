"""Owned pipeline recipes and real GPU replacement/readback through both layer entries."""

import os
from pathlib import Path
import subprocess
import sys

from harness import Checks, DIST, INJECT_DLL, PINNED_SETTINGS, inject_env, layer_env, test_dir


def main() -> int:
    corpus = "--corpus" in sys.argv[1:]
    c = Checks("captured shader rewrites" if corpus else "pipeline variants")
    folder = test_dir("recipe-")
    env = inject_env(os.path.join(folder, "tracker.log"), extra=PINNED_SETTINGS)
    env.update(XDG_CACHE_HOME=folder, TMPDIR=folder, TEMP=folder, TMP=folder)
    args = [os.path.join(DIST, "efmi_vk_recipe_test.exe")] + (["--corpus"] if corpus else [])
    result = subprocess.run(args, cwd=folder, env=env, capture_output=True, text=True, timeout=180 if corpus else 30)
    Path(folder, "stdout.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    c.equal(result.returncode, 0, "native recipe tests", )
    if corpus: return c.finish()
    result = subprocess.run([os.path.join(DIST, "efmi_vk_cache_test.exe")], cwd=folder, env=env,
                            capture_output=True, text=True, timeout=30)
    Path(folder, "cache.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    c.equal(result.returncode, 0, "asynchronous cache tests")
    result = subprocess.run([os.path.join(DIST, "efmi_vk_snapshot_test.exe")], cwd=folder, env=env,
                            capture_output=True, text=True, timeout=30)
    Path(folder, "snapshots.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    c.equal(result.returncode, 0, "private snapshot storage tests")
    validation = os.environ.get("EFMI_VK_VALIDATION_DIR", "F:/vulkan/validation-1.4.321.0/bin")
    if not Path(validation, "VkLayer_khronos_validation.json").is_file():
        raise RuntimeError("validation layer missing; run scripts/build_validation.py")
    for entry in ("none", "layer", "inject"):
        for scenario in ("canary", "renderpass1", "renderpass2", "material1", "material2"):
            folder = test_dir("variant-")
            log = os.path.join(folder, "layer.log")
            env = layer_env(log) if entry == "layer" else inject_env(log)
            env.update(VK_LAYER_PATH=os.pathsep.join([DIST, validation]), VK_LAYER_VALIDATE_SYNC="1",
                       VK_LAYER_DUPLICATE_MESSAGE_LIMIT="0", XDG_CACHE_HOME=folder, TMPDIR=folder, TEMP=folder, TMP=folder)
            args = [os.path.join(DIST, "efmi_vk_variant_gpu.exe")]
            if entry == "inject":
                args += ["--inject", INJECT_DLL]
            if scenario == "canary":
                args.append("--canary")
            if scenario in ("renderpass2","material2"):
                args.append("--renderpass2")
            if scenario.startswith("material"):
                args.append("--material")
            result = subprocess.run(args, cwd=folder, env=env, capture_output=True, text=True, timeout=45)
            output = result.stdout + result.stderr
            Path(folder, "stdout.txt").write_text(output, encoding="utf-8")
            label = f"{entry}/{scenario}"
            c.equal(result.returncode, 0, label + " exit")
            if result.returncode:
                print(output)
            c.check(f"validation: errors={1 if scenario == 'canary' else 0} warnings=0" in output,
                    label + " validation diagnostics", output[-5000:])
            if scenario == "canary":
                c.check("VUID-VkBufferCreateInfo-size-00912" in output, label + " active validation canary")
            else:
                c.equal(Path(folder, "pixels.rgba").stat().st_size if Path(folder, "pixels.rgba").exists() else 0,
                        64 * 64 * 4, label + " pixels saved")
            if entry != "none":
                c.check(Path(log).exists() and f"entry={entry}" in Path(log).read_text(), label + " intended entry loaded")
            else:
                c.check(not Path(log).exists(), label + " no probe layer")
    return c.finish()


if __name__ == "__main__":
    raise SystemExit(main())
