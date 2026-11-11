"""Checks that every writable global of the INI runtime is classified (D-129).

Each generation's original engine state is its own (docs/engine-generations.md): the original's
configuration globals are redirected to the generation the calling thread entered, and a load runs
while other generations draw and present. A writable global that is none of the classes below would
be shared between those threads unnoticed. This lists the writable data symbols (.data, .bss) of the
runtime's objects with dumpbin and fails on any that no rule names.

Run after scripts/build_xxmi_runtime.py. Exit code 0 when every symbol is classified.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build import find_vcvars  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/xxmi-runtime"

# Compiler and standard library internals: constant data the toolchain emits writable, locale
# facet ids and the CRT's per-module option words, all synchronized or never written.
INTERNAL = re.compile(r"RTTI |`string'|__xmm@|__real@|\$TSS|_Init_thread|_Avx2WmemEnabledWeakValue|"
                      r"__local_stdio_(printf|scanf)_options|::id$|_Mbstatet|std::_Facet|`vftable'|`vbtable'")

# (object stem pattern, identifier pattern, class, reason)
RULES = [
    # Tables the original declares without const and never writes.
    (r".*", r"\w+Names|DXGIFormats|ResourceDimensions|ResourceUsage|end_user_conflicting_shader_msg", "constant",
     "the original's enum and format name tables"),
    (r"command_module", r"\w+_operators?|function_tokens|operator_tokens", "constant", "the expression parser's operator tables"),
    (r"runtime_parser", r"AllowLinesWithoutEquals|BlendFactors|BlendOPs|CommandListSections|ComparisonFuncs|CullModes|"
                        r"\w+IniKeys|DepthWriteMasks|FillModes|FrontDirection|PrimitiveTopologies|RegularSections|StencilOps|"
                        r"true_false_overrule", "constant", "the INI parser's section and key tables"),
    (r"runtime_input", r"XInputButtons|VKMappings", "constant", "the key parser's key and button names"),
    (r"Assembler", r"special_purpose_registers|txt_swiz", "constant", "the assembler's register tables"),
    (r"draw_plan|resource_plan|runtime", r"lookup|rows", "constant",
     "the draw state's tables (draw_state.h): const function statics, built once and only read"),
    (r"SignatureParser", r"format_names|global_flag_sfi_map|subshader_feature_comments|system_value_abbreviations", "constant",
     "the signature parser's tables"),
    (r"WICTextureLoader", r"g_WICConvert|g_WICFormats", "constant", "DirectXTK's format tables"),
    (r"crc32c", r"hw_available|table|long_shifts|short_shifts", "constant", "computed at module initialization"),
    (r"character_core|efmittl_bridge", r"core_namespace|library", "constant", "names initialized with the module"),
    (r"hlsl_compiler", r"CLSID_DxcUtils", "constant", "a class identifier"),
    (r"runtime_parser", r"wrapped", "constant", "the discard gate's pattern (D-133), built once and only read"),
    # Only a load uses these, and a load holds the load mutex.
    (r"runtime_parser", r"ini_sections|recursive_includes|ini_warned|enumParseError|buf|log_level|empty_section_vector", "load",
     "the INI parser's state, cleared at each load's start and end"),
    (r"runtime_input", r"keyParseError", "load", "the key parser's error object"),
    (r"command_module", r"deferred_bind_flags", "load", "bind flags propagated while resources are parsed"),
    (r"runtime_override", r"input_dependencies", "load", "lists the key parser collects for the load"),
    (r"ShaderRegex", r"shader_regex_hash", "load", "the regex sections' hash, written while they are parsed"),
    # Under a lock of their own.
    (r"Assembler", r"codeBin|hackMap|ldMap|insMap", "lock", "the assembler's opcode tables, filled by operator[]: assembler_mutex"),
    (r"SignatureParser", r"parseError", "lock", "the signature parser's error object, used inside assembly: assembler_mutex"),
    (r"shader_assembly", r"assembler_mutex", "lock", "the assembler's lock"),
    (r"runtime", r"load_mutex|engine_entries|engine_waits|engine_wait_us", "lock", "the load mutex and the lock counters (atomics)"),
    (r"hlsl_compiler", r"dxc_fault", "lock", "an atomic"),
    (r"runtime_shim", r"resource_creation_mode_lock", "lock", "a critical section"),
    # Written once before any use, or at module initialization only.
    (r"runtime_shim", r"migoto_handle|tls_idx", "once", "set at module initialization"),
    (r"runtime", r"creation_lock", "once", "a once flag"),
    (r"draw_plan|runtime", r"g_layer_gap_sink|gap_sink_installed", "once",
     "LayerGap's sink: the runtime installs it at module initialization; what it writes is the entered generation's"),
    (r"runtime", r"LogFile|gLogDebug|gLogVerbosity", "once", "the original's log settings, never set by the runtime"),
    (r"command_module|runtime_parser", r"fnGetCursorFrameInfo|s", "once",
     "function statics initialized once (thread-safe statics); GetOrigSwapChain1's is never called"),
    (r"WICTextureLoader", r"s_initOnce|g_WIC2", "once", "DirectXTK's factory, created under InitOnce"),
    (r"shader_cache", r"algorithm", "once", "the hash algorithm handle (a thread-safe static)"),
    (r"shader_cache", r"mutex|pruned", "lock", "the folders a cache pruned, under the cache's static mutex"),
    (r"util", r"init_done|fnGetDpiForMonitor|fnGetProcessDpiAwareness|fnSetThreadDpiAwarenessContext", "once",
     "the original's DPI query; the host answers EFFECTIVE_DPI before it (host_values)"),
    (r"resource_hash_module", r"region_hashes_global_cache", "once",
     "GetRegionHash's cache; the runtime takes region hashes from the host (identity.region_hash) and never calls GetRegionHash"),
    (r"util", r"last_fullscreen_swap_chain|crash_handler_level|crash_handler_lock", "once",
     "the original's swap chain and crash handler state, never used by the runtime"),
    (r"runtime_input", r"xinput_lib|_XInputGetState", "once",
     "XInput's library, loaded once when a key names the guide button (an aligned pointer store, another valid function)"),
    # The presenting thread dispatches keys; nothing else reads these.
    (r"runtime_input", r"XInputState|last_time", "present", "XInput polling of the original's key dispatch on the presenting thread"),
    # The original's statistics, never read by the runtime; the original increments them from any thread as well.
    (r"profiling", r".*", "statistics", "the original's profiling counters and texts (Profiling::mode stays off)"),
    (r"command_module", r"profiling_generation", "statistics", "the original's profiling generation"),
    (r"util", r"random_call_counter", "statistics", "the original's random() seed counter, shared by every thread in the original too"),
]

# The objects of the runtime the module links, from the build's stamps.
def runtime_objects() -> list[Path]:
    stamps = json.loads((OUT / "objects.json").read_text())
    return sorted({OUT / (Path(source).stem + ".obj") for source in stamps})


LINE = re.compile(r"^[0-9A-F]{3,} [0-9A-F]{8} (SECT[0-9A-F]+|UNDEF|ABS)\s+(notype(?: \(\))?|\(\))\s+(External|Static)\s+\| (\S+)(?: \((.*)\))?")


def symbols(objects: list[Path]) -> list[tuple[str, str]]:
    with tempfile.TemporaryDirectory() as temp:
        bat = Path(temp) / "dump.bat"
        bat.write_text('@echo off\ncall "%s" >nul 2>nul\nset VSLANG=1033\n' % find_vcvars() +
                       "".join('dumpbin /nologo /symbols "%s"\n' % o for o in objects), encoding="ascii")
        text = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, errors="replace").stdout
    found, current, sections = [], None, {}
    for line in text.splitlines():
        if line.startswith("Dump of file"):
            current, sections = Path(line.split()[-1]).stem, {}
            continue
        m = LINE.match(line)
        if not m:
            continue
        sect, kind, scope, name, demangled = m.groups()
        if name.startswith(".") and scope == "Static":
            sections[sect] = name
            continue
        if "()" in kind or sect in ("UNDEF", "ABS"):
            continue
        section = sections.get(sect, "")
        if not (section.startswith(".data") or section.startswith(".bss")) or section.startswith(".data$r"):
            continue
        full = demangled or name
        # The standard library's own statics (scoped in std, mangled `?name@...@std@@`).
        if INTERNAL.search(full) or re.match(r"\?[^@]+@(?:[^@]+@)*std@@", name):
            continue
        # The variable's own name: the first component of the MSVC mangled name (?name@scope@@...),
        # or the name itself for C linkage.
        own = re.match(r"\?([^@?]+)@", name)
        ident = own.group(1) if own else name
        found.append((current, ident, full))
    return sorted(set(found))


def classify(obj: str, ident: str):
    for objects, pattern, kind, reason in RULES:
        if re.fullmatch(objects, obj) and re.fullmatch(pattern, ident):
            return kind, reason
    return None


def main() -> int:
    objects = runtime_objects()
    missing = [o for o in objects if not o.is_file()]
    if missing:
        print("runtime objects missing; build first:", ", ".join(o.name for o in missing))
        return 1
    found = symbols(objects)
    unclassified = [(o, i, f) for o, i, f in found if not classify(o, i)]
    counts: dict[str, int] = {}
    for o, i, f in found:
        result = classify(o, i)
        if result:
            counts[result[0]] = counts.get(result[0], 0) + 1
    print("runtime writable globals: %d in %d objects; %s" % (len(found), len(objects),
          ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    for o, i, f in unclassified:
        print("UNCLASSIFIED: %s %s (%s)" % (o, i, f[:160]))
    return 1 if unclassified else 0


if __name__ == "__main__":
    sys.exit(main())
