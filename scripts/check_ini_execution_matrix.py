"""Check the INI execution matrix against the pinned source, the hosted runtime and the sandbox.

`docs/ini-execution-matrix.json` states for every original command class how it executes in a
CPU generation (the configuration of the character renderer in the game) and in the owned GPU
domain of the sandbox. This check fails when a class of the pinned source has no entry, when a
class's stated origin disagrees with the source, when a cpu decision disagrees with its probe
(a fixture run through `runtime_test.exe --probe-command`), or when cited owned evidence is gone.
It runs no GPU work and reads no installed file; the installed census is only reported beside.

The matrix also carries the two tables of the draw state a replaced draw's lists run against
(frontend/xxmi/draw_state.h; docs/architecture-review-2.1.md P1-2 and P1-4): `draw_state`, what a draw
makes of each kind of slot holding each kind of value, and `operands`, what an operand reads where nothing
answers. They are generated from the source (`runtime_test.exe --state-table`); the check fails when the
copy in the matrix is not the source's, and `--write` renews the copy.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys

from ini_language import UPSTREAM, inventory

ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "docs/ini-execution-matrix.json"
OUT = ROOT / "build/ini-execution"
PROBE = ROOT / "build/xxmi-runtime/runtime_test.exe"
CENSUS = ROOT / "build/ini-operations/report.json"
CPU = {"original", "plan", "noop", "removed", "degraded", "refused", "not-created"}
OWNED = CPU | {"gpu"}
ORIGINS = {"parser", "optimiser", "shader-regex", "base", "none"}
# Placeholder files: a CPU plan names file resources and never reads them.
FILES = {"probe.buf": bytes(36), "probe.ib": bytes(6), "probe.dds": b"DDS " + bytes(124), "other.dds": b"DDS " + bytes(124)}


GENERATED = {
    "draw_state": "What a draw of a replaced draw's lists makes of one slot of the draw state, by the kind of slot and the kind of value "
                  "the lists left there (frontend/xxmi/draw_state.h, honour()). Every resource assignment is a total operation on that "
                  "state; this table is the one place that says what the layer gives a draw in Vulkan terms. Generated: "
                  "scripts/check_ini_execution_matrix.py --write.",
    "operands": "What an operand reads where neither the host nor an owned domain answers it: the expression goes on with this value "
                "and a notice says so once per operand, instead of the list being refused (frontend/xxmi/draw_state.h, operand_table()). "
                "Generated: scripts/check_ini_execution_matrix.py --write.",
}


def state_tables() -> dict:
    run = subprocess.run([str(PROBE), "--state-table"], capture_output=True, text=True, timeout=30)
    if run.returncode:
        raise SystemExit("runtime_test.exe --state-table failed: " + run.stderr[-400:])
    return json.loads(run.stdout)


def write_generated(tables: dict) -> None:
    """The two generated sections stand last in the file; everything before them is kept as it is written."""
    text = MATRIX.read_text(encoding="ascii")
    at = text.find(' "draw_state": {')
    if at < 0:
        end = text.rstrip().rfind("\n }")
        head = text[:end] + "\n },\n"
    else:
        head = text[:at]
    lines = []
    for key in ("draw_state", "operands"):
        rows = tables[key]
        lines += [' "%s": {' % key, '  "scope": %s,' % json.dumps(GENERATED[key]), '  "rows": [']
        lines += ['   ' + json.dumps(row, ensure_ascii=True) + ("," if i + 1 < len(rows) else "") for i, row in enumerate(rows)]
        lines += ['  ]', ' }' + ("," if key == "draw_state" else "")]
    MATRIX.write_text(head + "\n".join(lines) + "\n}\n", encoding="ascii", newline="\n")


def constructed(name: str, sources: str) -> bool:
    return re.search(rf"\bnew {name}\b|make_(?:shared|unique)<{name}>", sources) is not None


def derived(name: str, header: str) -> list[str]:
    return re.findall(rf"^class (\w+)\s*:\s*public {name}\b", header, re.M)


def probe(folder: Path, preamble: list[str], case: dict) -> dict:
    folder.mkdir(parents=True, exist_ok=True)
    for name, data in FILES.items():
        (folder / name).write_bytes(data)
    (folder / "probe.hlsl").write_text("[numthreads(1, 1, 1)] void main() {}\n", encoding="ascii")
    lines = [*preamble, "[TextureOverrideProbe]", "hash = 12345678", "match_index_count = 30", "$hit = 1",
             *case["lines"], *case.get("extra", [])]
    ini = folder / "probe.ini"
    ini.write_text("\n".join(lines) + "\n", encoding="ascii")
    for stale in (folder / "settings").glob("*.ini"):
        stale.unlink()
    run = subprocess.run([str(PROBE), "--probe-command", str(ini)], cwd=folder, capture_output=True,
                         text=True, timeout=30)
    if run.returncode:
        return dict(stage="crash", reason=f"exit {run.returncode:#x}: {run.stderr[-400:]}")
    return json.loads(run.stdout)


def compare(decision: str, expect: dict, actual: dict) -> list[str]:
    problems = []
    complete = actual.get("stage") == "complete"
    if decision == "refused" and complete:
        problems.append("decision refused, but the probe completed")
    if decision in ("original", "plan", "noop", "removed", "degraded") and not complete:
        problems.append(f"decision {decision}, but the probe stopped at {actual.get('stage')}: {actual.get('reason')}")
    # A degraded command says so through the original's warning channel (D-137); nothing is silent.
    if decision == "degraded" and "warning" not in expect:
        problems.append("decision degraded needs the warning it leaves")
    if "warning" in expect and not any(expect["warning"] in text for text in actual.get("diagnostic_messages", [])):
        problems.append(f"warnings {actual.get('diagnostic_messages')} lack {expect['warning']!r}")
    if expect.get("stage") != actual.get("stage"):
        problems.append(f"stage {actual.get('stage')!r}, expected {expect.get('stage')!r}")
    if "reason" in expect and expect["reason"] not in actual.get("reason", ""):
        problems.append(f"reason {actual.get('reason')!r} lacks {expect['reason']!r}")
    for name, value in expect.get("variables", {}).items():
        if actual.get("variables", {}).get(name) != value:
            problems.append(f"${name} = {actual.get('variables', {}).get(name)}, expected {value}")
    operations = [item["operation"] for item in actual.get("items", [])]
    if "items" in expect and operations != expect["items"]:
        problems.append(f"plan {operations}, expected {expect['items']}")
    if "layout_edits" in expect:
        edits = sum(item["layout_edits"] for item in actual.get("items", []) if item["operation"] == "draw_indexed")
        if edits != expect["layout_edits"]:
            problems.append(f"{edits} layout edits on the draw, expected {expect['layout_edits']}")
    if "skip_original" in expect and actual.get("skip_original") != expect["skip_original"]:
        problems.append(f"skip_original {actual.get('skip_original')}, expected {expect['skip_original']}")
    if complete and actual.get("diagnostics", 0) != expect.get("diagnostics", 0):
        problems.append(f"{actual.get('diagnostics')} original warnings: {actual.get('diagnostic_messages')}")
    return problems


def main():
    matrix = json.loads(MATRIX.read_text(encoding="ascii"))
    if not PROBE.is_file():
        raise SystemExit("runtime_test.exe is missing; run scripts/build_xxmi_runtime.py first")
    OUT.mkdir(parents=True, exist_ok=True)
    tables = state_tables()
    if "--write" in sys.argv:
        write_generated(tables)
        matrix = json.loads(MATRIX.read_text(encoding="ascii"))
    sources = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in sorted(UPSTREAM.glob("*.cpp")))
    header = (UPSTREAM / "CommandList.h").read_text(encoding="utf-8", errors="replace")
    regex = (UPSTREAM / "ShaderRegex.cpp").read_text(encoding="utf-8", errors="replace")
    source_classes = set(inventory()["command_classes"])
    classes = matrix["classes"]
    failures = []
    for key in ("draw_state", "operands"):
        if matrix.get(key, {}).get("rows") != tables[key]:
            failures.append(f"{key}: the table in frontend/xxmi/draw_state.h is not the copy in the matrix; run this script with --write")
    for name in sorted(source_classes - classes.keys()):
        failures.append(f"{name}: original command class without a stated decision")
    for name in sorted(classes.keys() - source_classes):
        failures.append(f"{name}: matrix entry for a class the pinned source no longer has")
    census = {}
    if CENSUS.is_file():
        census = json.loads(CENSUS.read_text(encoding="ascii")).get("families", {})
    rows = []
    for name, entry in sorted(classes.items()):
        origin, uses = entry.get("origin"), entry.get("uses", [])
        fail = lambda text: failures.append(f"{name}: {text}")
        if origin not in ORIGINS:
            fail(f"unknown origin {origin!r}")
        made = constructed(name, sources)
        if origin in ("parser", "optimiser", "shader-regex") and not made:
            fail(f"origin {origin}, but the pinned source never constructs it")
        if origin in ("base", "none") and made:
            fail(f"origin {origin}, but the pinned source constructs it")
        if origin == "base" and not derived(name, header):
            fail("origin base, but no class derives from it")
        if origin == "shader-regex" and "LinkCommandLists(" not in regex:
            fail("origin shader-regex, but ShaderRegex.cpp links no command lists")
        if origin in ("base", "none") and uses:
            fail("a class that is never constructed has no uses")
        if origin not in ("base", "none") and not uses:
            fail("a constructed class needs at least one stated use")
        results = []
        for index, use in enumerate(uses):
            label = f"use {index} ({use.get('syntax', '?')})"
            if use.get("cpu") not in CPU or use.get("owned") not in OWNED:
                fail(f"{label}: unknown decision cpu={use.get('cpu')!r} owned={use.get('owned')!r}")
                continue
            if use["owned"] == "gpu" and not use.get("evidence"):
                fail(f"{label}: owned gpu needs cited evidence")
            for cited in use.get("evidence", []):
                path = ROOT / cited["file"]
                if not path.is_file() or cited["text"] not in path.read_text(encoding="utf-8", errors="replace"):
                    fail(f"{label}: evidence {cited['text']!r} is not in {cited['file']}")
            case = use.get("probe")
            if use["cpu"] == "not-created":
                if case:
                    fail(f"{label}: cpu not-created cannot have a probe")
                results.append(dict(use=use["syntax"], cpu=use["cpu"], owned=use["owned"], probe=None))
                continue
            if not case:
                fail(f"{label}: cpu decision {use['cpu']} has no probe")
                continue
            actual = probe(OUT / f"{name}-{index}", matrix["probe"]["preamble"], case)
            problems = compare(use["cpu"], case.get("expect", {}), actual)
            for problem in problems:
                fail(f"{label}: {problem}")
            results.append(dict(use=use["syntax"], cpu=use["cpu"], owned=use["owned"], passed=not problems,
                                stage=actual.get("stage"), reason=actual.get("reason")))
        rows.append(dict(name=name, origin=origin, installed=census.get(name), uses=results))
    report = dict(scope=__doc__.strip(), passed=not failures, classes=len(rows), failures=failures, rows=rows)
    (OUT / "report.json").write_text(json.dumps(report, indent=1, ensure_ascii=True) + "\n", encoding="ascii")
    for row in rows:
        decisions = ", ".join(f"{u['cpu']}/{u['owned']}" for u in row["uses"]) or row["origin"]
        installed = "" if row["installed"] is None else f"  installed {row['installed']}"
        print(f"{row['name']:34s} {decisions}{installed}")
    for failure in failures:
        print("FAIL:", failure)
    probes = sum(1 for row in rows for use in row["uses"] if use.get("probe", True) is not None)
    print(f"INI execution matrix: {len(rows)} classes, {probes} probed uses, {len(tables['draw_state'])} draw state rows, "
          f"{len(tables['operands'])} operand rows, {len(failures)} failures")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
