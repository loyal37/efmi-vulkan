"""A failure stays with its unit: what one Mod, one INI or one Core name gets wrong does not take the folder.

docs/architecture-review-2.1.md, Phase 0. Copies the smallest installed character Mod (read-only source)
into folders of its own, changes one thing in each, and runs the folder generation's every-key-state dry
run (`efmi_vk_character_test --plan-folder-states`) on it:

- the Mod requires the version the installed Core declares: it registers and draws, as under the
  original, whose RegisterMod compares with its own `$version` (P0-2);
- the Mod requires a version the installed Core has not: it stays off, as under the original, and the
  Mod is told (P0-2);
- a Core without one of the lists the layer binds natively: one statement that names the Core and the
  list, not a container's error (P0-3);
- the Mod twice under one namespace: the original keeps the first file's sections and warns about the
  second's; the first draws, the second is left out with that reason (P0-5);
- a plain INI the load refuses (a test's hook): it is left out and named, the Mod draws (P0-5);
- no owned GPU runtime (its tools not deployed, or switched off) and a CustomShader section: the section
  does nothing and the Mod is told; the Mod draws (P0-6).

No GPU. Writes build/mod-containment/.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from check_mod_degradation import CORE, smallest_character_mod

ROOT = Path(__file__).resolve().parents[1]


def states(folder: Path, mods: Path, core: Path, env: dict) -> tuple[int, dict, list[dict], str]:
    exe = Path(os.environ.get("EFMI_VK_DIST", ROOT / "dist")) / "efmi_vk_character_test.exe"
    result = subprocess.run([str(exe), "--plan-folder-states", str(mods), str(core), str(folder / "settings")], cwd=folder,
                            env=dict(os.environ, **env), capture_output=True, text=True, errors="replace", timeout=1800)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    (folder / "states.jsonl").write_text(result.stdout, encoding="utf-8")
    (folder / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    return result.returncode, (lines[0] if lines else {}), lines[1:], result.stderr


def main() -> int:
    source = smallest_character_mod()
    out = ROOT / "build" / "mod-containment"
    if out.exists():
        shutil.rmtree(out)
    ini_name = next(p.name for p in source.glob("*.ini")
                    if re.search(r"(?im)^\s*\[CommandListInitializeMergedSkeleton\]", p.read_text(encoding="utf-8", errors="replace")))
    text = (source / ini_name).read_text(encoding="utf-8", errors="replace")
    installed = re.search(r"(?im)^\s*global\s+\$version\s*=\s*([0-9.]+)", (CORE / "main.ini").read_text(encoding="utf-8", errors="replace"))
    if not installed or not re.search(r"(?im)^\s*global\s+\$required_efmi_version\s*=", text):
        print("mod containment: the installed Core or %s states no version" % source.name)
        return 1
    failed = False

    def variant(label: str, change, verify, core: Path = CORE, env: dict | None = None, copies: tuple[str, ...] = ()) -> None:
        nonlocal failed
        folder = out / label
        mods = folder / "Mods"
        mod = mods / source.name
        shutil.copytree(source, mod)
        for name in copies:
            shutil.copytree(source, mods / name)
        used_core = change(mod, folder) or core
        code, summary, found, stderr = states(folder, mods, used_core, env or {})
        problem = verify(code, summary, found, stderr)
        failed |= bool(problem)
        print("%s %s%s" % ("FAIL" if problem else "ok  ", label, ": " + problem if problem else ""))

    def rewrite(mod: Path, pattern: str, replacement: str) -> None:
        changed, count = re.subn(pattern, replacement, text)
        if not count:
            raise RuntimeError("%s has nothing matching %s" % (ini_name, pattern))
        (mod / ini_name).write_text(changed, encoding="utf-8")

    def draws(found: list[dict], index: int = 0) -> str:
        if len(found) <= index:
            return "the dry run lists no Mod"
        m = found[index]
        if not m.get("loaded"):
            return "not loaded: %s" % m.get("reason", "")
        if m.get("refusals"):
            return "refused: %s" % m["refusals"][0]["reason"]
        return "" if m.get("drawn") else "loaded, but no section draws the Mod in the default state"

    def told(found: list[dict], summary: dict, *words: str) -> str:
        gaps = " | ".join([g for m in found for g in m.get("gaps", [])] + summary.get("gaps", []))
        missing = [w for w in words if w.lower() not in gaps.lower()]
        return "the Mod is not told (%s missing in: %s)" % (", ".join(missing), gaps or "no gap") if missing else ""

    version = installed.group(1)
    variant("requires the installed Core's version",
            lambda mod, folder: rewrite(mod, r"(?im)^(\s*global\s+\$required_efmi_version\s*=\s*)[0-9.]+", r"\g<1>" + version),
            lambda code, summary, found, stderr: draws(found))

    variant("requires a newer Core than installed",
            lambda mod, folder: rewrite(mod, r"(?im)^(\s*global\s+\$required_efmi_version\s*=\s*)[0-9.]+", r"\g<1>9.99"),
            lambda code, summary, found, stderr: ("the dry run failed: %s" % stderr[-300:] if code or not found else
                                                  "the Mod draws although the Core is older than it requires" if found[0].get("drawn") else
                                                  told(found, summary, "requires EFMI", "9.99", version)))

    def renamed_core(mod: Path, folder: Path) -> Path:
        core = folder / "Core" / "EFMI"
        shutil.copytree(CORE, core)
        api = core / "API.ini"
        body = api.read_text(encoding="utf-8", errors="replace")
        changed, count = re.subn(r"(?im)^\[CommandListComponent_DrawInstances\]", "[CommandListComponent_DrawInstancesRenamed]", body)
        if count != 1:
            raise RuntimeError("the installed Core's API.ini has no [CommandListComponent_DrawInstances]")
        api.write_text(changed, encoding="utf-8")
        return core

    def named(code, summary, found, stderr) -> str:
        said = stderr + " ".join(m.get("reason", "") for m in found)
        if found and any(m.get("drawn") for m in found):
            return "a Mod draws through a Core list that is not there"
        missing = [w for w in ("EFMI Core", "component_drawinstances") if w.lower() not in said.lower()]
        return "the refusal does not name %s: %s" % (" and ".join(missing), said.strip()[-300:]) if missing else ""
    variant("a Core without a list the layer binds", renamed_core, named)

    def twice(code, summary, found, stderr) -> str:
        if code or len(found) != 2:
            return "the dry run failed or lists %d Mods: %s" % (len(found), stderr.strip()[-300:])
        first = draws(found, 0)
        if first:
            return "the first Mod: " + first
        second = found[1]
        if second.get("loaded") and second.get("drawn"):
            return "the second file draws although the original keeps the first one's sections"
        reason = second.get("reason", "") + " ".join(second.get("gaps", []))
        return "" if "namespace" in reason.lower() else "the second Mod is not told about its namespace: %s" % reason
    def same_namespace(mod: Path, folder: Path) -> None:
        # Both copies under one declared namespace (a Mod without a namespace line is named by its path,
        # and two copies of it are two Mods to the original).
        if re.search(r"(?im)^\s*namespace\s*=", text.split("[", 1)[0]):
            return
        for copy in (mod, mod.parent / (source.name + " second")):
            (copy / ini_name).write_text("namespace = efvk_twice\n" + text, encoding="utf-8")
    variant("one Mod twice under one namespace", same_namespace, twice, copies=(source.name + " second",))

    def plain(mod: Path, folder: Path) -> None:
        other = folder / "Mods" / "efvk plain"
        other.mkdir(parents=True)
        (other / "efvk_refused.ini").write_text("[Constants]\nglobal $efvk_plain = 1\n", encoding="utf-8")

    def isolated(code, summary, found, stderr) -> str:
        if code:
            return "the folder refused for one plain INI: %s" % stderr.strip()[-300:]
        left = [item for item in summary.get("left_inputs", []) if "efvk_refused.ini" in item.get("ini", "")]
        return draws(found) or ("" if left else "the INI left out is not named: %s" % summary.get("left_inputs"))
    variant("a plain INI the load refuses", plain, isolated, env={"EFMI_VK_TEST_REFUSE_INPUT": "efvk_refused.ini"})

    def custom_shader(mod: Path, folder: Path) -> None:
        (mod / "efvk_probe.hlsl").write_text("[numthreads(1, 1, 1)] void main() {}\n", encoding="ascii")
        (mod / ini_name).write_text(text + "\n[CustomShaderEfvkProbe]\ncs = efvk_probe.hlsl\n", encoding="utf-8")
    variant("no owned GPU runtime and a CustomShader section", custom_shader,
            lambda code, summary, found, stderr: ("the load refused: %s" % stderr.strip()[-300:] if code else
                                                  draws(found) or told(found, summary, "CustomShader")),
            env={"EFMI_VK_CHECK_NO_OWNED": "1"})

    print("mod containment: %s (%s)" % ("FAILED" if failed else "passed", source.name))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
