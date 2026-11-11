#include "../frontend/xxmi/runtime.h"
#include "../frontend/xxmi/character_core.h"
#include "../layer/src/character_program.h"
#include <fstream>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <cstring>
using namespace efmi::frontend;
// Independent slow CRC for fixture files, not the production hot path.
namespace efvk { uint32_t crc32c(uint32_t seed, const void* data, size_t size) {
    uint32_t hash = ~seed; const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= p[i]; for (int b = 0; b < 8; ++b) hash = (hash >> 1) ^ (0x82f63b78u & (0u - (hash & 1))); }
    return ~hash;
}
// The plans' notes (BUG-076's missing texture file) to the test's output.
void logf(const char* fmt, ...)
{
    va_list args; va_start(args, fmt); vfprintf(stdout, fmt, args); va_end(args); fputc('\n', stdout);
} }
static int checks = 0;
static void check(bool result, const char* text)
{
    if (!result) throw std::runtime_error(text);
    ++checks;
}
static void write(const std::filesystem::path& path, const char* text)
{
    std::ofstream out(path, std::ios::binary); out << text;
    if (!out) throw std::runtime_error("Cannot write fixture");
}
#include "runtime_match_cases.h"
#include "runtime_shader_cases.h"
#include "runtime_resource_match_cases.h"
#include "runtime_regex_cases.h"
#include "runtime_window_cases.h"
#include "runtime_library_cases.h"
#include "runtime_concurrency_cases.h"
static const char* fixture = R"ini(namespace = runtime_test
[Constants]
global persist $dress = 0
global persist $other = 0
global $active = 0
global $holding = 0
[KeyDress]
condition = $active == 1
key = ctrl 5
type = cycle
$dress = 0,1,2,
[KeyOther]
condition = $active == 1
key = ctrl 5
type = cycle
$other = 0,1,
[KeyHold]
key = alt 6
type = hold
$holding = 1
[Present]
post $active = 0
[TextureOverrideObject]
hash = 12345678
match_index_count = 30
$active = 1
handling = skip
if $dress == 0
 drawindexed = 3,0,0
elif $dress == 1
 drawindexed = 6,3,0
else
 drawindexed = 9,9,0
endif
)ini";
static int inspect_character(int argc, wchar_t** argv)
{
    check(argc == 7, "Character inspection expects package, INI, Core, namespace and library mode");
    const auto profile = std::filesystem::absolute(argv[2]);
    const auto ini = std::filesystem::absolute(argv[3]);
    const auto core = std::filesystem::absolute(argv[4]);
    const std::wstring wide_name = argv[5];
    if (wide_name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789_") != std::wstring::npos)
        throw std::runtime_error("Invalid fixture namespace");
    const std::string name(wide_name.begin(), wide_name.end());
    const bool rabbit = !wcscmp(argv[6], L"texture-roles");
    if (!rabbit && wcscmp(argv[6], L"none")) throw std::runtime_error("Unknown fixture library mode");
    std::string reason; efvk::CharacterPackage package;
    if (!package.load(profile.u8string(), reason)) throw std::runtime_error(reason);
    RuntimeConfig config{ini.parent_path(), {core / "API.ini", core / "MergedSkeleton.ini",
        core / "SpatialIdentification.ini", core / "Helpers.ini", ini},
        profile.parent_path() / "inspection-settings", "isolated"};
    auto runtime = IniRuntime::load(config, character_core(name, package, rabbit), reason);
    if (!runtime) throw std::runtime_error("Original character runtime: " + reason);
    RuntimeInput input; efmi::assets::FilePlan plan;
    uint32_t lod = 0; size_t draws = 0, hides = 0;
    for (unsigned round = 0; round < 2; ++round) {
        for (const auto& section : package.sections) {
            if (section.lod != efvk::kCharacterNone) lod = section.lod;
            RuntimeDraw request; request.hash = section.hash; request.count = section.match_count;
            request.first = section.match_first == efvk::kCharacterNone ? 0 : section.match_first;
            request.component = section.component; request.lod = lod; request.frame = round + 1;
            if (section.component != efvk::kCharacterNone) {
                request.identity = 1;
                // Synthetic source-API facts, not measurements of this character.
                request.rolling_bone_source = true; request.source_vs_config_slot = 2;
            }
            if (!runtime->draw(request, plan, reason)) throw std::runtime_error("Original character entry: " + reason);
            if (round) {
                size_t geometry = 0;
                for (const auto& item : plan.items)
                    geometry += item.operation == efmi::assets::FileOperation::DrawIndexed;
                draws += geometry; hides += plan.skip_original && !geometry;
                if (section.component == efvk::kCharacterNone)
                    check(plan.skip_original && !geometry, "Independent original hide is executed without an identity");
            }
        }
        if (!runtime->present(input, round + 1, float(round), reason)) throw std::runtime_error(reason);
    }
    const auto state = runtime->status();
    for (const auto& section : package.sections) if (section.component != efvk::kCharacterNone) {
        RuntimeDraw probe; probe.hash = section.hash; probe.count = section.match_count;
        probe.first = section.match_first == efvk::kCharacterNone ? 0 : section.match_first;
        probe.component = section.component; probe.lod = section.lod == efvk::kCharacterNone ? lod : section.lod;
        probe.identity = 1; probe.source_vs_config_slot = 7;
        check(!runtime->draw(probe, plan, reason), "Invalid source-API slot is refused before execution");
        probe.source_vs_config_slot = -1;
        check(runtime->draw(probe, plan, reason), "Unknown bone-source facts retain original fallback behavior");
        const auto unknown = runtime->status().variables;
        check(unknown.at("$\\efmiv1\\instance_config_vs_cb_slot") == -1 &&
              unknown.at("$\\efmiv1\\bones_data_source") == 0,
              "Absent source-API facts do not become an invented slot or bone source");
        probe.source_vs_config_slot = 2; probe.rolling_bone_source = true;
        check(runtime->draw(probe, plan, reason), "Resolved input recovers after unknown and invalid contracts");
        break;
    }
    printf("{\"scope\":\"Original main INI plans with synthetic native Core inputs; no GPU/shader-profile/game result\","
           "\"sections\":%zu,\"bones\":%u,\"components\":%zu,\"keys_with_f10\":%zu,\"draws\":%zu,\"hidden\":%zu,\"library_texture_roles\":%s}\n",
           package.sections.size(), package.bones, package.components.size(), state.bindings, draws, hides, rabbit ? "true" : "false");
    return 0;
}
static void json_text(const std::string& text)
{
    putchar('"');
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20 || c > 0x7e) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}
// One original INI fixture through a CPU generation: no owned GPU adapters, no shader hooks,
// no native extension, which is how the character renderer in the game hosts the original
// command classes. Load, one present, one draw matching hash 12345678 with 30 indices, a
// second present. Reports the stage that stopped it and what the plan and variables became.
static int probe_command(const std::filesystem::path& file)
{
    const auto path = std::filesystem::absolute(file);
    RuntimeConfig config{path.parent_path(), {path}, path.parent_path() / "settings", "probe"};
    std::string reason, stage = "load";
    auto runtime = IniRuntime::load(config, {}, reason);
    efmi::assets::FilePlan plan;
    if (runtime) {
        RuntimeInput input; input.foreground = true;
        stage = "present";
        if (runtime->present(input, 24, 2.5f, reason)) {
            RuntimeDraw draw; draw.hash = 0x12345678; draw.count = 30; draw.frame = 24; draw.seconds = 2.5f;
            stage = "draw";
            if (runtime->draw(draw, plan, reason)) {
                stage = "post-present";
                if (runtime->present(input, 25, 2.6f, reason)) { stage = "complete"; reason.clear(); }
            }
        }
    }
    static const char* operations[] = {"bind_vertex", "bind_index", "bind_pixel_resource", "draw_indexed", "external"};
    printf("{\"stage\":"); json_text(stage); printf(",\"reason\":"); json_text(reason);
    printf(",\"skip_original\":%s,\"items\":[", plan.skip_original ? "true" : "false");
    for (size_t i = 0; i < plan.items.size(); ++i) {
        const auto& item = plan.items[i];
        printf("%s{\"operation\":\"%s\",\"slot\":%u,\"resource\":%s,\"index_count\":%u,\"first_index\":%u,\"instance_count\":%u,\"layout_edits\":%zu}",
               i ? "," : "", operations[size_t(item.operation)], item.slot, item.resource ? "true" : "false",
               item.index_count, item.first_index, item.instance_count, item.layout.size());
    }
    printf("]");
    if (runtime) {
        const auto state = runtime->status();
        printf(",\"diagnostics\":%zu,\"diagnostic_messages\":[", state.diagnostics);
        for (size_t i = 0; i < state.diagnostic_messages.size(); ++i) { if (i) putchar(','); json_text(state.diagnostic_messages[i]); }
        printf("],\"variables\":{");
        bool comma = false;
        for (const auto& [name, value] : state.variables) {
            if (name.rfind("$\\probe\\", 0)) continue;
            if (comma) putchar(','); comma = true;
            json_text(name.substr(8)); printf(":%.9g", value);
        }
        putchar('}');
    }
    printf("}\n");
    return 0;
}
// The judge's scene on the layer's side (docs/architecture-review-2.1.md, P3-1): the case in a CPU
// generation, as the character renderer hosts a Mod's lists. Each frame the Present, then the two game
// draws of sandbox/dx11_judge_scene.h by their index region hashes (six indices from the first, one
// instance, DrawIndexedInstanced), the game's texture at pixel slot 0 bound and of no known hash. Printed:
// the plan of each draw of the last frame (or why it was refused), the generation's variables and what the
// layer said it did not do as the original does (its notices: a degraded line must have one).
static int judge_scene(const std::filesystem::path& file, int frames, uint32_t first_hash, uint32_t second_hash)
{
    const auto path = std::filesystem::absolute(file);
    RuntimeConfig config{path.parent_path(), {path}, path.parent_path() / "settings", "judge"};
    config.texture_identity = true;
    std::string reason;
    auto runtime = IniRuntime::load(config, {}, reason);
    if (!runtime) { fprintf(stderr, "judge scene: %s\n", reason.c_str()); return 1; }
    const uint32_t hashes[2] = {first_hash, second_hash};
    efmi::assets::FilePlan plans[2];
    std::string refused[2];
    RuntimeInput input; input.foreground = true;
    for (int frame = 1; frame <= frames; ++frame) {
        if (!runtime->present(input, uint64_t(frame), float(frame) / 60.0f, reason)) { fprintf(stderr, "judge scene: present: %s\n", reason.c_str()); return 1; }
        for (int i = 0; i < 2; ++i) {
            RuntimeDraw draw; draw.hash = hashes[i]; draw.count = 6; draw.frame = uint64_t(frame); draw.seconds = float(frame) / 60.0f;
            draw.shader_stages = 0x1 | 0x10;
            // The scene's one colour target, 256 x 256, as the host knows a draw's target.
            draw.targets_known = true; draw.render_targets = 1; draw.target_width = draw.target_height = 256;
            draw.pixel_texture = [](unsigned slot, RuntimeTexture& texture, std::string&) { texture = {}; texture.bound = slot == 0; return true; };
            plans[i] = {}; refused[i].clear();
            if (!runtime->draw(draw, plans[i], refused[i]) && refused[i].empty()) refused[i] = "refused";
        }
    }
    static const char* operations[] = {"bind_vertex", "bind_index", "bind_pixel_resource", "draw_indexed", "external", "translucent"};
    printf("{\"draws\":[");
    for (int i = 0; i < 2; ++i) {
        const auto& plan = plans[i];
        printf("%s{\"refused\":", i ? "," : ""); json_text(refused[i]);
        printf(",\"skip\":%s,\"pre_items\":%lld,\"items\":[", plan.skip_original ? "true" : "false",
               plan.pre_items == SIZE_MAX ? -1ll : static_cast<long long>(plan.pre_items));
        for (size_t k = 0; k < plan.items.size(); ++k) {
            const auto& item = plan.items[k];
            printf("%s{\"op\":\"%s\",\"slot\":%u,\"file\":", k ? "," : "", operations[std::min<size_t>(size_t(item.operation), 5)], item.slot);
            if (item.resource) { auto name = item.resource->path.filename().u8string(); for (auto& c : name) c = char(tolower(static_cast<unsigned char>(c))); json_text(name); }
            else printf("null");
            printf(",\"application\":%d,\"count\":%u,\"first\":%u,\"base\":%d,\"instances\":%u,\"indexed\":%s,\"automatic\":%s}", item.application_slot,
                   item.index_count, item.first_index, item.vertex_offset, item.instance_count, item.indexed ? "true" : "false", item.automatic ? "true" : "false");
        }
        printf("]}");
    }
    printf("],\"variables\":{");
    bool comma = false;
    for (const auto& [name, value] : runtime->status().variables) {
        if (comma) putchar(','); comma = true;
        json_text(name); printf(":%.9g", value);
    }
    printf("},\"gaps\":[");
    comma = false;
    for (const auto& gap : runtime->status().gaps) { if (comma) putchar(','); comma = true; json_text(gap); }
    printf("]}\n");
    return 0;
}
// The draw state's two tables as the source states them (frontend/xxmi/draw_state.h): what a draw makes of
// each slot, and what an operand reads where nothing answers. docs/ini-execution-matrix.json keeps a copy
// that scripts/check_ini_execution_matrix.py compares with this output (--write renews the copy).
static int state_table()
{
    using namespace efmi::frontend;
    printf("{\"draw_state\":[");
    bool comma = false;
    for (const auto& row : honour_table()) {
        printf("%s{\"slots\":", comma ? "," : ""); comma = true; json_text(row.slot_text);
        printf(",\"value\":"); json_text(row.value_text);
        printf(",\"outcome\":"); json_text(honour_name(row.honour));
        printf(",\"note\":"); json_text(row.note); putchar('}');
    }
    printf("],\"operands\":[");
    comma = false;
    for (const auto& row : operand_table()) {
        printf("%s{\"operand\":", comma ? "," : ""); comma = true; json_text(row.operand);
        printf(",\"value\":%g,\"basis\":", double(row.value)); json_text(row.basis); putchar('}');
    }
    printf("]}\n");
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 2 && !wcscmp(argv[1], L"--state-table")) return state_table();
        if (argc == 6 && !wcscmp(argv[1], L"--judge-scene"))
            return judge_scene(argv[2], _wtoi(argv[3]), uint32_t(wcstoul(argv[4], nullptr, 16)), uint32_t(wcstoul(argv[5], nullptr, 16)));
        if (argc > 1 && !wcscmp(argv[1], L"--inspect-character")) return inspect_character(argc, argv);
        if (argc == 3 && !wcscmp(argv[1], L"--probe-command")) return probe_command(argv[2]);
        if (argc == 4 && !wcscmp(argv[1], L"--evaluate-language")) {
            const auto path = std::filesystem::absolute(argv[2]);
            const auto count = std::stoi(argv[3]);
            if (count < 1 || count > 512) throw std::runtime_error("Invalid language fixture size");
            RuntimeConfig config{path.parent_path(), {path}, path.parent_path() / "settings", "evaluation"};
            std::string reason;
            auto runtime = IniRuntime::load(config, {}, reason);
            if (!runtime) throw std::runtime_error(reason);
            RuntimeInput input; input.foreground = true;
            if (!runtime->present(input, 24, 2.5f, reason)) throw std::runtime_error(reason);
            RuntimeDraw draw; draw.hash = 0x12345678; draw.count = 30; draw.frame = 24; draw.seconds = 2.5f;
            efmi::assets::FilePlan plan;
            if (!runtime->draw(draw, plan, reason)) throw std::runtime_error(reason);
            const auto state = runtime->status();
            printf("{\"variable_bits\":[");
            for (int i = 0; i < count; ++i) {
                const auto value = state.variables.at("$\\evaluation\\case" + std::to_string(i));
                uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
                printf("%s%u", i ? "," : "", bits);
            }
            printf("]}\n"); return 0;
        }
        check(argc == 2, "Fixture directory argument");
        const std::filesystem::path folder = std::filesystem::path(argv[1]) / "fixture";
        std::filesystem::create_directories(folder);
        runtime_match_cases(folder);
        runtime_shader_cases(folder);
        runtime_resource_match_cases(folder);
        runtime_regex_cases(folder);
        runtime_regex_assembly_cases(folder);
        runtime_window_cases(folder);
        runtime_library_cases(folder);
        runtime_host_value_cases(folder);
        runtime_concurrency_cases(folder);
        const auto ini = folder / "mod.ini"; write(ini, fixture);
        const auto settings = folder / "settings";
        std::filesystem::remove(settings / "runtime.ini");
        RuntimeConfig config{folder, {ini}, settings, "runtime"}; std::string reason;
        auto first = IniRuntime::load(config, {}, reason);
        if (!first) throw std::runtime_error(reason);
        auto value = [&](const std::shared_ptr<IniRuntime>& r, const char* key) { return r->status().variables.at(std::string("$\\runtime_test\\") + key); };
        RuntimeInput input; input.foreground = true;
        auto present = [&](std::shared_ptr<IniRuntime> r) { if (!r->present(input, 10, 1.f, reason)) throw std::runtime_error(reason); };
        RuntimeDraw draw; draw.hash = 0x12345678; draw.count = 30; draw.frame = 1;
        efmi::assets::FilePlan plan;
        auto render = [&](std::shared_ptr<IniRuntime> r) { if (!r->draw(draw, plan, reason)) throw std::runtime_error(reason); };
        check(first->status().bindings == 4, "Three original keys plus F10");
        input.keys[VK_CONTROL] = true; input.keys['5'] = true; present(first);
        check(value(first, "dress") == 0 && value(first, "other") == 0, "Inactive condition blocks both listeners");
        input.keys.fill(false); present(first); render(first);
        check(plan.skip_original && plan.items.back().index_count == 3, "Original matching and default branch");
        input.keys[VK_CONTROL] = true; input.keys['5'] = true; present(first);
        check(value(first, "dress") == 1 && value(first, "other") == 1, "Both duplicate key listeners activate");
        present(first);
        check(value(first, "dress") == 1, "Held cycle key does not repeat");
        render(first); check(plan.items.back().index_count == 6 && plan.items.back().first_index == 3, "Original condition changes draws");
        check(first->status().dirty, "Original persistent assignment marks settings dirty");
        check(first->save(reason), "Save settings");
        check(!first->status().dirty && first->status().saves == 1, "Atomic settings published");
        auto second = IniRuntime::load(config, {}, reason);
        if (!second) throw std::runtime_error(reason);
        check(value(second, "dress") == 1 && value(second, "other") == 1, "Original constants reload saved values");
        input.keys.fill(false); present(first); render(first);
        input.keys[VK_CONTROL] = true; input.keys['5'] = true; present(first);
        check(value(first, "dress") == 2 && value(first, "other") == 0 && value(second, "dress") == 1, "Generations keep independent variables and key state");
        input.keys.fill(false); present(first);
        input.keys[VK_MENU] = true; input.keys['6'] = true; present(first);
        check(value(first, "holding") == 1, "Original hold key activates");
        input.foreground = false; present(first);
        check(value(first, "holding") == 0, "Focus loss releases original hold");
        input.foreground = true; input.keys.fill(false); input.keys[VK_F10] = true; present(first);
        check(first->status().reload_requested && !second->status().reload_requested, "F10 requests reload only for the active generation");
        check(first->take_reload_request() && !first->take_reload_request(), "F10 consumed once");
        second->inherit_persistent(*first);
        check(value(second, "dress") == 2 && value(second, "other") == 0 && second->status().dirty, "Reload carries latest persistent values, not stale parse snapshot");
        present(second);
        check(!second->take_reload_request(), "F10 held through generation exchange does not repeat");
        input.keys.fill(false); present(second); input.keys[VK_F10] = true; present(second);
        check(second->take_reload_request(), "Releasing and pressing F10 works in the new generation");
        first->suspend_persistence();
        check(first->save(reason) && first->status().dirty, "Retired writer cannot clear or overwrite pending values");
        write(ini, "namespace = runtime_test\n[Constants]\nglobal $x = 0\n[Present]\n$x = (\n");
        {
            // The original loader reports the broken expression and keeps the rest (D-067).
            auto warned = IniRuntime::load(config, {}, reason);
            const auto loaded = warned ? warned->status() : RuntimeStatus{};
            check(warned && loaded.diagnostics >= 1 && !loaded.diagnostic_messages.empty(),
                  "A broken expression is reported and skipped; the rest of the candidate loads");
        }
        // A candidate this host cannot execute is refused instead, and the live one stays.
        // (A CustomShader without shader execution. A ShaderOverride that acts on the application's
        // draws names a DX11 shader the host cannot see: it loads inert and is reported, D-089.)
        write(ini, "namespace = runtime_test\n[ShaderOverrideActing]\nhash = 1234\nhandling = skip\n");
        {
            auto inert = IniRuntime::load(config, {}, reason);
            const auto loaded = inert ? inert->status() : RuntimeStatus{};
            const bool reported = std::any_of(loaded.diagnostic_messages.begin(), loaded.diagnostic_messages.end(),
                                              [](const std::string& text) { return text.find("inert") != std::string::npos; });
            if (!inert) fprintf(stderr, "acting ShaderOverride candidate: %s\n", reason.c_str());
            check(inert && reported, "A ShaderOverride acting on game shaders loads inert and is reported");
        }
        write(ini, "namespace = runtime_test\n[CustomShaderUnadapted]\ncs = absent.hlsl\n");
        {
            // As a CustomShader the original could not compile: declared, its runs doing nothing, the rest of
            // the generation loaded, and said (it refused the whole candidate before; P0-6).
            auto inert = IniRuntime::load(config, {}, reason);
            const auto loaded = inert ? inert->status() : RuntimeStatus{};
            const bool told = std::any_of(loaded.gaps.begin(), loaded.gaps.end(),
                                          [](const std::string& text) { return text.find("CustomShader") != std::string::npos; });
            if (!inert) fprintf(stderr, "CustomShader candidate: %s\n", reason.c_str());
            check(inert && told, "A CustomShader section without shader execution loads inert and is said");
        }
        write(ini, "namespace = runtime_test\n[ShaderOverrideInert]\nhash = 1234\nfilter_index = 5\nallow_duplicate_hash = overrule\n");
        check(bool(IniRuntime::load(config, {}, reason)), "A ShaderOverride with only hash and filter_index runs nothing and loads");
        render(first); check(plan.items.back().index_count == 9, "Failed parse leaves live generation usable");
        first.reset(); render(second);
        check(plan.items.back().index_count == 9, "Retiring one generation preserves another");
        write(ini, "namespace = hide_fixture\n[TextureOverrideHide]\nhash = 12345678\nmatch_index_count = 30\nhandling = skip\nthis = null\ndrawindexed = 3, 0, 0\n");
        auto hide_config = config; hide_config.settings_name = "hide_fixture";
        auto hidden = IniRuntime::load(hide_config, {}, reason);
        if (!hidden) throw std::runtime_error("Independent IB suppression INI: " + reason);
        check(bool(hidden), "Independent IB suppression INI loads without a character skeleton");
        render(hidden);
        // The binding stands before the draw that sees it (D-161); a list that binds and never draws leaves no item.
        check(plan.skip_original && plan.items.size() == 2 && !plan.items[0].resource && plan.items[0].application_slot < 0 &&
              plan.items[0].operation == efmi::assets::FileOperation::BindIndex && plan.items[1].operation == efmi::assets::FileOperation::DrawIndexed,
              "IB override this=null uses the actual triggering index binding");
        render(second); check(plan.items.back().index_count == 9, "Pure hide does not change another generation's draw");
        const auto tree = folder / "directory";
        std::filesystem::create_directories(tree / "Mods/first");
        std::filesystem::create_directories(tree / "Mods/second");
        std::filesystem::create_directories(tree / "Mods/DISABLED-old");
        std::filesystem::create_directories(tree / "library");
        std::filesystem::remove(settings / "directory.ini");
        std::filesystem::remove(tree / "Mods/second/dormant.ini");
        write(tree / "root.ini", "[Include]\ninclude = library/shared.ini\nuser_config = outside-settings.ini\n");
        write(tree / "library/shared.ini", "namespace = shared\n[Constants]\nglobal $n = 3\n[CommandListCount]\nif 1\n$n = $n + 1\nendif\n[CommandListDormant]\nif 0\nif 0\n$n = 123\nendif\nendif\n");
        write(tree / "Mods/first/1.ini", "namespace = first\n[Constants]\nglobal persist $hits = 0\n[TextureOverrideMesh]\nhash = 12345678\nmatch_index_count = 30\nrun = CommandList\\shared\\Count\n$hits = $\\shared\\n\nhandling = skip\ndrawindexed = 3,0,0\n");
        write(tree / "Mods/second/1.ini", "namespace = second\n[Constants]\nglobal $hits = 20\n[TextureOverrideMesh]\nhash = 22345678\nmatch_index_count = 30\n$hits = $hits + 2\nhandling = skip\ndrawindexed = 6,0,0\n");
        write(tree / "Mods/DISABLED-old/bad.ini", "[CommandListBad]\ninvalid_operation = 1\n");
        write(tree / "Mods/conditional.ini", "condition = 0\n[CommandListBad]\ninvalid_operation = 1\n");
        RuntimeConfig folder_config{tree, {tree / "root.ini"}, settings, "directory", tree / "Mods"};
        auto directory = IniRuntime::load(folder_config, {}, reason);
        if (!directory) throw std::runtime_error("Original directory runtime: " + reason);
        render(directory);
        check(plan.skip_original && plan.items.back().index_count == 3 &&
              directory->status().variables.at("$\\first\\hits") == 4,
              "Directory runtime resolves recursive files, original Include and cross-file commands");
        draw.hash = 0x22345678; render(directory);
        check(plan.items.back().index_count == 6 && directory->status().variables.at("$\\second\\hits") == 22,
              "Original directory matcher routes a second namespace independently");
        write(tree / "Mods/second/dormant.ini", "[CommandListUnused]\ninvalid_operation = 1\n");
        {
            // The original loader warns about the line and keeps the rest of the configuration.
            auto warned = IniRuntime::load(folder_config, {}, reason);
            const auto loaded = warned ? warned->status() : RuntimeStatus{};
            bool named = false;
            for (const auto& text : loaded.diagnostic_messages) named |= text.find("invalid_operation") != std::string::npos;
            check(warned && loaded.diagnostics >= 1 && named, "An invalid dormant command is reported and skipped, as by the original loader");
        }
        // What this generation cannot execute is inert and the rest loads, as the original loads past a
        // CustomShader it cannot compile (P0-6; an acting ShaderOverride loads inert since D-089). It refused
        // the whole folder before.
        write(tree / "Mods/second/dormant.ini", "[CustomShaderUnsupported]\ncs = absent.hlsl\n");
        check(bool(IniRuntime::load(folder_config, {}, reason)), "A CustomShader section without shader execution does not refuse the folder");
        render(directory);
        check(plan.items.back().index_count == 6 && directory->status().variables.at("$\\second\\hits") == 24,
              "Another directory load leaves the previous generation usable");
        std::filesystem::remove(tree / "Mods/second/dormant.ini");
        write(tree / "library/shared.ini", "namespace = shared\n[Constants]\nglobal $n = 30\n[CommandListCount]\n$n = $n + 1\n");
        auto directory_next = IniRuntime::load(folder_config, {}, reason);
        if (!directory_next) throw std::runtime_error("Original directory reload: " + reason);
        draw.hash = 0x12345678; render(directory_next);
        check(directory_next->status().variables.at("$\\first\\hits") == 31,
              "Fresh directory generation reloads changed companion commands");
        check(directory_next->save(reason) && std::filesystem::exists(settings / "directory.ini") &&
              !std::filesystem::exists(tree / "outside-settings.ini"), "Original Include user_config cannot redirect project settings writes");
        auto directory_saved = IniRuntime::load(folder_config, {}, reason);
        check(directory_saved && directory_saved->status().variables.at("$\\first\\hits") == 31,
              "Directory persistent values reload only from the explicit project settings");
        write(ini, R"ini(namespace = language_runtime
[Constants]
global $result = 0
global $preset_hits = 0
global $preset_calls = 0
global $clock = 0
global $post_seen = 0
global $key_calls = 0
global $cycle_calls = 0
x = 2
w17 = 3
preset = PresetStart
[PresetStart]
$preset_hits = 7
run = CommandListPresetInit
[CommandListPresetInit]
$preset_calls = $preset_calls + 1
[KeyParameter]
key = no_modifiers VK_F1
type = hold
z3 = 9
run = CommandListOnlyKey
[CommandListOnlyKey]
$key_calls = $key_calls + 1
post $key_calls = $key_calls + 10
[KeyCallbackCycle]
key = no_modifiers VK_F2
type = cycle
smart = false
run = CommandListOnlyCycleOne, CommandListOnlyCycleTwo
[CommandListOnlyCycleOne]
$cycle_calls = $cycle_calls + 1
post $cycle_calls = $cycle_calls + 100
[CommandListOnlyCycleTwo]
$cycle_calls = $cycle_calls + 10
post $cycle_calls = $cycle_calls + 1000
[Present]
$result = x + w17 + z3
$clock = time
[TextureOverrideMesh]
hash = 12345678
match_index_count = 30
handling = skip
drawindexed = 3,0,0
pre run = CommandListAbort
drawindexed = 6,0,0
post $post_seen = 1
[CommandListAbort]
if index_count == 30
handling = abort
endif
$post_seen = 2
)ini");
        RuntimeConfig language_config{folder, {ini}, settings, "language"};
        auto language = IniRuntime::load(language_config, {}, reason);
        if (!language) throw std::runtime_error("Original parameter/preset runtime: " + reason);
        input = {}; input.foreground = true; present(language);
        auto language_value = [&](const char* key) { return language->status().variables.at(std::string("$\\language_runtime\\") + key); };
        check(language_value("result") == 5 && language_value("clock") == 1,
              "Original parameter initialization and time operands evaluate in the runtime");
        check(language_value("preset_hits") == 7 && language_value("preset_calls") == 1,
              "Constants can reference a later preset and its original command callback");
        input.keys[VK_F1] = true; present(language);
        check(language_value("result") == 14, "Original hold key updates an INI parameter without a D3D device");
        check(language_value("key_calls") == 1, "Key-only activation callback is compiled even without a scene reference");
        input.keys.fill(false); present(language);
        check(language_value("result") == 5, "Original hold release restores the earlier parameter value");
        check(language_value("key_calls") == 11, "Hold deactivation executes the original callback post list");
        input.keys[VK_F2] = true; present(language);
        check(language_value("cycle_calls") == 101, "Cycle-only callback executes its pre and deferred post list");
        input.keys.fill(false); present(language); input.keys[VK_F2] = true; present(language);
        // Pinned Override.cpp accumulates earlier run entries in later cycle
        // presets. Preserve that source behavior: 101 + (1+10) + (100+1000).
        check(language_value("cycle_calls") == 1212, "Later cycle callback preserves original accumulated run entries");
        input.keys.fill(false); present(language);
        render(language);
        check(plan.skip_original && plan.items.size() == 1 && plan.items[0].index_count == 3 && language_value("post_seen") == 1,
              "Nested handling=abort stops its root phase; the independent post phase still executes");
        auto language_second = IniRuntime::load(language_config, {}, reason);
        if (!language_second) throw std::runtime_error("Second parameter runtime: " + reason);
        input.keys[VK_F1] = true; present(language); input.keys.fill(false); present(language_second);
        check(language_value("result") == 14 && language_second->status().variables.at("$\\language_runtime\\result") == 5,
              "INI parameter storage and preset activation remain isolated between generations");
        write(ini, R"ini(namespace = pool_runtime
[Constants]
global $cleared = 0
[PoolBindings]
pool_size = 2
[ResourceBuffer]
type = Buffer
stride = 16
filename = pool.buf
[KeyReset]
key = no_modifiers VK_F3
type = toggle
$cleared = 1
[TextureOverrideMesh]
hash = 12345678
match_index_count = 30
handling = skip
if $cleared
PoolBindings[*] = null
else
PoolBindings[0] = ref ResourceBuffer
$PoolBindings[1] = 9
endif
vb0 = ref PoolBindings[0]
drawindexed = 3,0,0
)ini");
        {
            const float data[8]{};
            std::ofstream bytes(folder / "pool.buf", std::ios::binary);
            bytes.write(reinterpret_cast<const char*>(data), sizeof(data));
            if (!bytes) throw std::runtime_error("Cannot write pool buffer fixture");
        }
        RuntimeConfig pool_config{folder, {ini}, settings, "pool"};
        auto pool_first = IniRuntime::load(pool_config, {}, reason);
        auto pool_second = IniRuntime::load(pool_config, {}, reason);
        if (!pool_first || !pool_second) throw std::runtime_error("Pool runtime: " + reason);
        render(pool_first);
        check(plan.items.size() == 2 && plan.items[0].resource && plan.items[0].resource->path.filename() == L"pool.buf",
              "Production file plans retain a pooled file binding across an unrelated scalar write");
        const auto retained_pool_plan = plan;
        input = {}; input.foreground = true; input.keys[VK_F3] = true; present(pool_first);
        render(pool_first);
        check(plan.items.size() == 2 && !plan.items[0].resource && retained_pool_plan.items[0].resource,
              "Pool reset emits a null binding while the previous owned file plan remains intact");
        render(pool_second);
        check(plan.items.size() == 2 && plan.items[0].resource,
              "Pool resource reset in one generation does not change another generation");
        write(ini, fixture);
        // Read-only acceptance of the actual main INI. The panel is a separate
        // input with a CustomShader and remains explicitly outside this fixture.
        const auto project = folder.parent_path().parent_path().parent_path();
        const auto package_path = project / "build/character/arcane-materials.bin";
        if (std::filesystem::exists(package_path)) {
            efvk::CharacterPackage package;
            check(package.load(package_path.u8string(), reason), "Load actual material profile");
            const std::filesystem::path mod = L"D:/XXMI-Launcher-Portable-v2.1.6/EFMI/Mods/arcane fes 1.2";
            const std::filesystem::path core = L"D:/XXMI-Launcher-Portable-v2.1.6/EFMI/Core/EFMI";
            RuntimeConfig actual{mod, {core / "API.ini", core / "MergedSkeleton.ini", core / "SpatialIdentification.ini", core / "Helpers.ini", mod / "arcane.ini"}, settings, "actual"};
            std::filesystem::remove(settings / "actual.ini");
            auto live = IniRuntime::load(actual, character_core("arcane_fes", package), reason);
            if (!live) throw std::runtime_error("Actual INI: " + reason);
            input = {}; input.foreground = true;
            auto render_section = [&](size_t i) {
                const auto& section = package.sections[i]; RuntimeDraw request;
                request.hash = section.hash; request.count = section.match_count;
                request.first = section.match_first == efvk::kCharacterNone ? 0 : section.match_first;
                request.component = section.component; request.lod = section.lod == efvk::kCharacterNone ? 0 : section.lod;
                request.identity = 1; request.frame = 20;
                if (!live->draw(request, plan, reason)) throw std::runtime_error("Actual original entry: " + reason);
            };
            render_section(0); present(live);
            check(live->status().variables.at("$\\arcane_fes\\mod_enabled") == 1, "Original Present registers this mod through Core-VK");
            for (size_t i = 0; i < package.sections.size(); ++i) render_section(i);
            check(live->status().draws == package.sections.size() + 1, "All actual LOD0/LOD1 original entry sections execute");
            render_section(0); input.keys[VK_CONTROL] = true; input.keys['5'] = true; present(live);
            check(live->status().variables.at("$\\arcane_fes\\swapkey0") == 1, "Actual original Ctrl+5 changes outfit variable");
            check(live->save(reason), "Actual original persistent settings save");
            live.reset();
            efvk::CharacterProgram program;
            std::filesystem::remove(settings / "arcane_fes.ini");
            if (!program.init((folder / "actual-live.bin").u8string(), package, reason))
                throw std::runtime_error("Actual renderer catalogue: " + reason);
            check(package.images.size() == 10, "Ten reachable DDS images including alternate; dormant missing declarations excluded");
            auto program_draw = [&](size_t section_index) {
                const auto& section = package.sections[section_index]; RuntimeDraw request;
                request.hash = section.hash; request.count = section.match_count;
                request.first = section.match_first == efvk::kCharacterNone ? 0 : section.match_first;
                request.component = section.component; request.lod = section.lod == efvk::kCharacterNone ? 0 : section.lod;
                request.identity = 1; request.frame = 100;
                efvk::CharacterList list; bool skip = false, added = false;
                if (!program.plan(request, package, list, skip, added, reason)) throw std::runtime_error("Actual renderer draw: " + reason);
                return list;
            };
            input = {}; input.foreground = true;
            auto program_present = [&] { if (!program.present(input, 100, 1.f, reason)) throw std::runtime_error(reason); };
            program_draw(0); program_present();
            for (size_t i = 0; i < package.sections.size(); ++i) program_draw(i);
            check(program.status().draws == package.sections.size() + 1, "Actual commands translate into validated renderer draws for every entry");
            const auto default_outfit = program_draw(1);
            check(default_outfit.count == 9, "Default outfit draws nine independent mesh ranges");
            struct Key { int modifier, code; const char* variable; };
            const Key keys[] = {{VK_CONTROL,'5',"swapkey0"},{VK_CONTROL,'6',"swapkey1"},{VK_CONTROL,'7',"swapkey2"},
                {VK_CONTROL,'8',"swapkey3"},{VK_MENU,'5',"swapkey4"},{VK_MENU,'6',"swapkey5"},{VK_MENU,'7',"swapkey6"},
                {VK_MENU,'8',"swapkey11"},{VK_MENU,'9',"swapkey9"},{VK_CONTROL,'L',"stocking"}};
            for (const auto& binding : keys) {
                input.keys.fill(false); program_present(); program_draw(0);
                input.keys[binding.modifier] = input.keys[binding.code] = true; program_present();
                check(program.status().variables.at(std::string("$\\arcane_fes\\") + binding.variable) == 1, "Actual original binding changes persistent variable");
                for (size_t i = 0; i < package.sections.size(); ++i) program_draw(i);
            }
            const auto state = program.status();
            check(state.bindings == 14 && state.variables.at("$\\arcane_fes\\swapkey7") == 1 &&
                  state.variables.at("$\\arcane_fes\\swapkey8") == 1 && state.variables.at("$\\arcane_fes\\swapkey12") == 1,
                  "All thirteen original listeners run, including three duplicate key pairs");
            check(program_draw(1).count == 2, "Combined outfit conditions leave two mesh ranges");
            const auto stocking = program_draw(2);
            bool alternate = false;
            for (const auto& pixel : package.pixels.at(stocking.first)) if (pixel.asset != efvk::kCharacterNone)
                alternate |= package.images.at(pixel.asset).find("DiffuseMap1.dds") != std::string::npos;
            check(alternate, "Original stocking branch resolves the alternate DDS into the actual draw");
            check(program.save(reason), "Renderer-owned runtime saves settings outside the original mod");
        }
        printf("Original INI runtime: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "Runtime fixture failed: %s\n", e.what()); return 1; }
}
