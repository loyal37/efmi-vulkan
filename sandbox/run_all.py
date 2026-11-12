"""Builds layer and sandbox, then runs the whole regression suite, or the steps a change touches.

    python sandbox/run_all.py [--no-build] [--only WORDS] [--list]

--only takes words separated by commas: a step runs when its name contains one of them (any case),
or one of the groups below names it. The whole suite runs before a deployment; between, the checks
of the area a change touches (the user, 2026-10-26: the whole suite every time takes too long).
--list prints the steps and groups.

Set EFMI_VK_DIST to build and test in another directory than dist/, for example
while a running game holds the DLL in dist/.

The layer is enabled only inside the child processes the tests start. Nothing is
registered system-wide and the game is not involved.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))

STEPS = [
    ("text check", [os.path.join(ROOT, "scripts", "check_text.py")]),
    ("unit tests", [os.path.join(HERE, "unit_test.py")]),
    ("original frontend host", [os.path.join(ROOT, "scripts", "probe_xxmi_host.py"), "--reuse"]),
    ("directory INI language", [os.path.join(HERE, "ini_folder_test.py")]),
    ("source-defined INI language", [os.path.join(ROOT, "scripts", "check_ini_language.py")]),
    ("installed Mods and Core syntax", [os.path.join(ROOT, "scripts", "check_ini_folder.py")]),
    ("installed INI operation census", [os.path.join(ROOT, "scripts", "ini_operation_census.py")]),
    ("original INI runtime", [os.path.join(ROOT, "scripts", "build_xxmi_runtime.py")]),
    # D-129: every writable global of the runtime is per generation, per thread, the load's, locked or
    # classified as safe to share; run after the runtime's build above.
    ("INI runtime globals classified", [os.path.join(ROOT, "scripts", "check_engine_globals.py")]),
    # Cause C of docs/design-review-1.9.md (D-130): a texture's first use waits no longer than the
    # original's load of it (DirectXTK on D3D11), the installed Mods' largest maps from the file cache.
    ("texture first-use loads against the original", [os.path.join(ROOT, "scripts", "bench_texture_load.py"), "--check"]),
    ("source-defined INI evaluation", [os.path.join(ROOT, "scripts", "check_ini_evaluation.py")]),
    ("INI execution matrix", [os.path.join(ROOT, "scripts", "check_ini_execution_matrix.py")]),
    ("DX11 registers from SPIR-V", [os.path.join(ROOT, "scripts", "check_source_slots.py")]),
    ("DX11 declarations from SPIR-V", [os.path.join(ROOT, "scripts", "check_source_declarations.py")]),
    # D-170: what `ps-tN->Format`, `->Width`, `->Height`, `->Mips` and `->Array` read of the game's texture,
    # against the DX11 side of every paired draw of the catalogue.
    ("DX11 texture properties from Vulkan images", [os.path.join(ROOT, "scripts", "check_texture_properties.py")]),
    ("Core facts from SPIR-V", [os.path.join(ROOT, "scripts", "check_core_facts.py")]),
    ("RabbitFX texture roles", [os.path.join(ROOT, "scripts", "check_texture_roles.py")]),
    ("installed Mods offline plans", [os.path.join(ROOT, "scripts", "check_mods_dry_run.py")]),
    # D-131: the whole Mods folder in one generation gives every character Mod what its own does.
    ("installed Mods offline plans in one folder generation", [os.path.join(ROOT, "scripts", "check_folder_generation.py")]),
    # D-137: every section of every installed Mod plans in every state the Mods' keys reach as a player
    # presses them (the original's own dispatch decides what changes).
    ("installed Mods offline plans in every key state", [os.path.join(ROOT, "scripts", "check_mod_states.py")]),
    # BUG-077: a Mod with a missing, short or twice-declared buffer file loads and degrades as the original.
    ("installed Mod with broken files offline plans", [os.path.join(ROOT, "scripts", "check_mod_degradation.py")]),
    # docs/architecture-review-2.1.md Phase 0: what one Mod, one INI or one name of the Core gets wrong stays
    # with it (the Core's version and names, a second INI under one namespace, an input the load refuses, a
    # CustomShader without the owned runtime).
    ("installed Mod failures contained, offline plans", [os.path.join(ROOT, "scripts", "check_mod_containment.py")]),
    # --mod-first: no matched draw shows the game's mesh, not even while a new object is identified or
    # an EFMI Mod registers with the Core's API (the user's bar; held instead, 2026-10-26).
    ("captured frame replayed with the Mods folder", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                      r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk03", "--frames", "300", "--check",
                                                      "--mod-first"]),
    # Si Lunabelle's optional guard runs the Core's spatial identification itself (D-086). A copy of that
    # Mod alone, so the step holds whichever Si Mod the user has enabled.
    ("captured frame with a Mod's own identification", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                       r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk04", "--frames", "120", "--check",
                                                       "--only-mod", "Si Lunabelle 1.4"]),
    # Si NightveilTrancer drives shape keys from its Present (a compute pass writes the mesh's positions,
    # which its draws bind by reference as vb0) and guards every draw with `if rt_width != 0`, which the
    # original evaluates again in the post phase's fresh state (BUG-053). Every matched draw must plan.
    ("captured frame with Present compute and owned vertex streams", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                                     r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk04", "--frames", "120", "--check",
                                                                     "--no-refusal", "--only-mod", "Si\uff1aNightveilTrancer & Gui"]),
    # The same Mod's parts read bones of several components; on two queues a recording draws parts whose
    # other components another recording imports: every recording dispatches all of them (BUG-054), and
    # the pictures with the bones on the GPU equal those with the CPU's master.
    ("captured frame with bones across recordings", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                     r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk04", "--frames", "120", "--check",
                                                     "--two-queues", "--bones-identical", "--only-mod", "Si\uff1aNightveilTrancer & Gui"]),
    # The game keeps a frame's bone data valid only until that frame completed, then reuses the region
    # (BUG-060): with two copies of every bone buffer, the one of the frame before zeroed before a frame is
    # submitted, the GPU import must read the game's data only in the batch of the recording that drew
    # the component, and its pictures equal the CPU import's (D-127). Typhoeus' parts read bones of
    # several components, which the split passes put in other recordings.
    ("captured frame with the game's bone regions reused", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                           r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk06", "--frames", "60", "--check",
                                                           "--split-passes", "--rolling-bones", "--bones-identical",
                                                           "--only-mod", "Typhoeus\uff1aNya Tech & GUI"]),
    # SiModGB (BUG-064): an index buffer declared with `stride = 6` beside its format, typed vertex buffers,
    # and `if ps-tN == <filter_index>` to find which slot holds one of its textures, all as the original
    # takes them; every matched draw plans.
    ("captured frame with texture filter queries", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                    r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk04", "--frames", "90", "--check",
                                                    "--no-refusal", "--only-mod", "SiModGB"]),
    # Tangtang's draw lists ask the game's shaders `vs->srv_stride(0) == 16`, `ps != 1718.2`: the
    # declarations come from the pipeline's SPIR-V (D-124) and every matched draw plans.
    ("captured frame with shader declaration queries", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                        r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk02", "--frames", "120", "--check",
                                                        "--no-refusal", "--only-mod", "Tangtang - Uniform NSFW"]),
    # A full card (the game leaves almost no device memory free): with pageable device memory the Mod
    # still activates at its first match, as 3DMigoto creates resources whatever the free memory (D-122).
    ("captured frame on a full card", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                       r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk04", "--frames", "120", "--check",
                                       "--reserve-mb", "1000000", "--only-mod", "Si\uff1aNightveilTrancer & Gui"]),
    # The game's two graphics queues (D-119): a frame's passes on two command buffers over both queues,
    # the queues swapping every frame, three frames in flight; the merged skeleton's import on the GPU
    # (D-117) must give the CPU import's pictures and complete on both queues.
    ("captured frame on two queues, bones on the GPU and the CPU", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                                   r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk03", "--frames", "120", "--check",
                                                                   "--two-queues", "--bones-identical"]),
    # ardelia calls the EFMITTL library's translucent draw (D-112): the library is loaded, every plan runs, its
    # shader groups run as the host reads them off SPIR-V (D-171) with the textures' properties it asks (D-170),
    # and with the game's copy pass after the forward pass its calls are captured and composited (D-113). The
    # step used Yvonne until the user's update of 2026-11-10 left out Yvonne's translucent call (BUG-088).
    ("captured frame with a library call", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                            r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk11", "--frames", "120", "--check", "--no-refusal",
                                            "--copy-passes", "--expect-translucency", "--expect-shadows"]),
    # Yvonne calls the library's texture slots only (SetTextures, D-170, D-171): every plan runs.
    ("captured frame with a library's texture slots", [os.path.join(ROOT, "scripts", "replay_capture.py"), "--bundle",
                                                       r"F:\efmi vulkan\captures\catalogue-20261019\vulkan\vk01", "--frames", "120", "--check",
                                                       "--no-refusal", "--copy-passes"]),
    ("EFMITTL hook rules on the paired catalogue", [os.path.join(ROOT, "scripts", "check_shader_hooks.py")]),
    ("original INI GPU resources", [os.path.join(ROOT, "scripts", "check_resource_commands.py")]),
    # D-150: the original as the judge: INI cases under the pinned XXMI 3DMigoto in a D3D11 program and under the
    # owned GPU runtime, their persistent variables compared (after the GPU resource runtime is built).
    ("the original as the judge (XXMI DX11)", [os.path.join(ROOT, "scripts", "check_dx11_judge.py")]),
    # P3-1 of docs/architecture-review-2.1.md: the judge's program draws, the original runs the cases'
    # [TextureOverride] sections matched by the draws' index hashes, and the picture, the primitives and
    # the variables are compared with what the layer's plans of the same lists mean.
    ("the original as the judge of replaced draws (XXMI DX11)", [os.path.join(ROOT, "scripts", "check_dx11_judge_draws.py")]),
    # One working replaced mesh and, one at a time, the lines of sandbox/judge_draw_sweep.json in front of its
    # draw: no single line may make a Mod vanish. The record says which lines differ from the original and why;
    # the step fails on a line that agreed and differs now, and on a recorded difference that agrees now.
    ("one line at a time under the original (XXMI DX11)", [os.path.join(ROOT, "scripts", "check_dx11_judge_draws.py"), "--sweep"]),
    ("EFMITTL owned transparency", [os.path.join(ROOT, "scripts", "check_efmittl.py")]),
    ("original HLSL Vulkan compiler", [os.path.join(ROOT, "scripts", "check_shader_compiler.py")]),
    ("installed Mods and Core HLSL", [os.path.join(ROOT, "scripts", "check_hlsl_folder.py")]),
    ("directory HLSL evidence", [os.path.join(HERE, "hlsl_folder_test.py")]),
    ("original panel HLSL compilation", [os.path.join(ROOT, "scripts", "check_panel_hlsl.py")]),
    ("native tracker tests", [os.path.join(HERE, "native_test.py")]),
    ("CPU skeleton import", [os.path.join(HERE, "bone_test.py")]),
    ("pipeline recipes and variants", [os.path.join(HERE, "variant_test.py")]),
    ("M2 hook rendering", [os.path.join(HERE, "m2_test.py")]),
    ("character replacement", [os.path.join(HERE, "character_test.py")]),
    ("live INI rendering", [os.path.join(HERE, "runtime_test.py")]),
    ("owned Present on a swap chain", [os.path.join(HERE, "owned_present_test.py")]),
    ("probe end to end", [os.path.join(HERE, "probe_test.py")]),
    ("modes", [os.path.join(HERE, "modes_test.py")]),
    ("budgets and reduced tracking", [os.path.join(HERE, "limits_test.py")]),
    ("many-draw timing", [os.path.join(HERE, "timing_test.py")]),
    ("Vulkan validation", [os.path.join(HERE, "validation_test.py")]),
    ("MCP end to end", [os.path.join(HERE, "mcp_test.py")]),
    ("probe evidence", [os.path.join(HERE, "evidence_test.py")]),
]


# Steps by the area of the layer they check, for --only.
GROUPS = {
    "bones": ["bones across recordings", "bone regions reused", "two queues, bones", "CPU skeleton import"],
    "replay": ["captured frame"],
    "textures": ["texture first-use", "live INI rendering", "character replacement", "full card", "Mods folder"],
    "submission": ["two queues, bones", "bone regions reused", "probe end to end", "Vulkan validation", "live INI rendering"],
    "ini": ["INI", "frontend host", "declarations", "registers", "Core facts", "texture roles", "texture properties", "offline plans",
            "as the judge", "one line at a time"],
    "owned": ["owned", "GPU resources", "HLSL", "Present compute", "as the judge"],
    # A minute or so, for every change: text, the Python units, the expression cases, what each command
    # class does in a replaced draw, and the original as the judge.
    "quick": ["text check", "unit tests", "source-defined INI evaluation", "INI execution matrix", "as the judge"],
}


def selected(names: list[str], words: list[str]) -> list[str]:
    patterns = []
    for word in words:
        patterns += GROUPS.get(word.lower(), [word])
    return [n for n in names if any(p.lower() in n.lower() for p in patterns)]


def main() -> int:
    steps = list(STEPS)
    args = sys.argv[1:]
    if "--list" in args:
        for name, _ in STEPS:
            print(name)
        print("groups: " + ", ".join(sorted(GROUPS)))
        return 0
    if "--only" in args:
        at = args.index("--only")
        words = [w.strip() for w in (args[at + 1] if at + 1 < len(args) else "").split(",") if w.strip()]
        names = selected([n for n, _ in STEPS], words)
        if not names:
            print("run_all: --only matched no step")
            return 1
        steps = [(n, a) for n, a in STEPS if n in names]
    if "--no-build" not in args:
        build = [os.path.join(ROOT, "scripts", "build.py")]
        # A running process may hold the DLL in dist/. With EFMI_VK_DIST set, build and
        # test in that directory instead.
        if os.environ.get("EFMI_VK_DIST"):
            build += ["--out", os.environ["EFMI_VK_DIST"]]
        steps.insert(0, ("build", build))
    failed = []
    for name, args in steps:
        start = time.monotonic()
        proc = subprocess.run([sys.executable, "-B"] + args, cwd=HERE, capture_output=True, text=True, errors="replace")
        elapsed = time.monotonic() - start
        output = (proc.stdout or "") + (proc.stderr or "")
        print("[%s] %s (%.1fs)" % ("ok" if proc.returncode == 0 else "FAILED", name, elapsed))
        for line in output.splitlines():
            if proc.returncode != 0 or "checks passed" in line or line.startswith("built ") or "clean" in line:
                print("    " + line)
        if proc.returncode != 0:
            failed.append(name)
            if name == "build":
                break
    print("run_all: %s" % ("all steps passed" if not failed else "FAILED: " + ", ".join(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
