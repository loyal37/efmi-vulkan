// Route 2's first step (docs/vulkan-native-mods.md): ShaderOverride sections named by a SPIR-V module's hash, run at
// every draw of every pipeline with that module as 3DMigoto runs them at every draw of a shader (HackerContext.cpp
// BeforeDraw and AfterDraw), and hunting over the frame's modules as Hunting.cpp hunts over its shaders.
#include "character_backend_internal.h"

namespace efvk {

// The original's stage order of a draw (vs, hs, ds, gs, ps) for a Vulkan stage bit, -1 for another stage.
static int original_stage(uint32_t bit)
{
    switch (bit) {
    case VK_SHADER_STAGE_VERTEX_BIT: return 0;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return 1;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return 2;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return 3;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return 4;
    default: return -1;
    }
}

bool pipeline_modules(Device* device, VkPipeline pipeline, PipelineModules& out)
{
    std::shared_lock<std::shared_mutex> lock(device->res_mx);
    const auto pipe = device->pipes.find(pipeline);
    if (pipe == device->pipes.end() || pipe->second.compute) return false;
    const PipeInfo& info = pipe->second;
    out = {};
    for (const auto& stage : info.shaders) {
        const int i = original_stage(stage.stage);
        if (i < 0 || !stage.shader) continue;
        out.hashes[i] = stage.shader->hash; out.mask |= 1u << i; out.stages |= stage.stage;
    }
    // The render targets for `oN` as a replaced draw's (CharacterGeneration::render_targets): the pipeline's subpass.
    if (info.recipe && info.recipe->render_pass && info.recipe->subpass < info.recipe->render_pass->subpasses.size()) {
        const auto& sub = info.recipe->render_pass->subpasses[info.recipe->subpass];
        for (size_t i = 0; i < sub.colors.size() && i < 8; ++i)
            if (sub.colors[i].attachment != VK_ATTACHMENT_UNUSED) out.targets |= 1u << i;
        if (sub.has_depth && sub.depth.attachment != VK_ATTACHMENT_UNUSED) out.targets |= 1u << 8;
        out.targets_known = true; out.render_pass = uint64_t(info.recipe->render_pass->id) << 8 | info.recipe->subpass;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Hunting (Hunting.cpp): the selection runs over the modules the frame's draws had while hunting is enabled.

namespace {
const char* const kKindName[2] = {"vertex shader", "pixel shader"};
const char* const kKindShort[2] = {"VS", "PS"};
const char* const kMarkingNames[4] = {"skip", "original", "pink", "mono"};

// HashToClipboard with the module's eight digits; the player's own clipboard, as the original's.
bool hash_to_clipboard(uint32_t hash)
{
    char text[16];
    snprintf(text, sizeof(text), "%08x", hash);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(text));
    if (!memory) return false;
    memcpy(GlobalLock(memory), text, sizeof(text));
    GlobalUnlock(memory);
    if (!OpenClipboard(nullptr)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    const bool kept = SetClipboardData(CF_TEXT, memory) != nullptr;
    CloseClipboard();
    if (!kept) GlobalFree(memory);
    return kept;
}
} // namespace

void Hunting::configure(const efmi::frontend::HuntingSettings& settings)
{
    std::lock_guard<std::mutex> lock(mx_);
    // `hunting = 0` registers no key: nothing to hunt with. An F10 reads `hunting` again, as the original's reload does.
    keys_ = settings.keys;
    mode_.store(settings.keys ? settings.hunting : 0, std::memory_order_relaxed);
    marking_mode_ = settings.marking_mode >= 0 && settings.marking_mode < 4 ? settings.marking_mode : 0;
    actions_ = settings.marking_actions;
}

std::string Hunting::line_locked(int kind) const
{
    const auto& visited = visited_[kind];
    const auto found = has_selected_[kind] ? visited.find(selected_[kind]) : visited.end();
    char text[96];
    if (found == visited.end()) snprintf(text, sizeof(text), "%s 0/%zu", kKindShort[kind], visited.size());
    else snprintf(text, sizeof(text), "%s %zu/%zu: %08x", kKindShort[kind], size_t(std::distance(visited.begin(), found)) + 1, visited.size(),
                  selected_[kind]);
    return text;
}

void Hunting::select_locked(int kind, bool next)
{
    // HuntNext and HuntPrev: the element after (before) the selected one, wrapping; the first (last) one when the
    // selected one is not in the set.
    auto& visited = visited_[kind];
    if (visited.empty()) return;
    auto found = has_selected_[kind] ? visited.find(selected_[kind]) : visited.end();
    if (next) {
        if (found != visited.end() && ++found != visited.end()) selected_[kind] = *found;
        else selected_[kind] = *visited.begin();
    } else {
        if (found != visited.end() && found != visited.begin()) selected_[kind] = *--found;
        else selected_[kind] = *std::prev(visited.end());
    }
    has_selected_[kind] = true;
    peers_[kind].clear();
}

void Hunting::act(efmi::frontend::HuntingAction action)
{
    using A = efmi::frontend::HuntingAction;
    std::string told;
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (!keys_) return;
        last_key_us_ = host_time_us();
        const int mode = mode_.load(std::memory_order_relaxed);
        if (action == A::Toggle) {
            mode_.store(mode == 1 ? 2 : 1, std::memory_order_relaxed);
            logf("> Hunting toggled to %d", mode == 1 ? 2 : 1);
            told = mode == 1 ? "> hunting off" : "> hunting on";
        } else if (mode != 1) {
            return; // the original's keys do nothing unless hunting is enabled
        } else if (action == A::NextMarkingMode) {
            marking_mode_ = (marking_mode_ + 1) % 4;
            told = std::string("> marking mode ") + kMarkingNames[marking_mode_];
        } else if (action == A::NextPixel || action == A::PreviousPixel || action == A::NextVertex || action == A::PreviousVertex) {
            const int kind = action == A::NextVertex || action == A::PreviousVertex ? 0 : 1;
            select_locked(kind, action == A::NextPixel || action == A::NextVertex);
            told = line_locked(kind);
        } else if (action == A::MarkPixel || action == A::MarkVertex) {
            const int kind = action == A::MarkVertex ? 0 : 1;
            if (!has_selected_[kind]) return;
            const uint32_t hash = selected_[kind];
            // MarkShaderBegin and the peers' lines: what the original logs of a marked shader, with the module's hash.
            logf(">>>> %s marked: %s hash = %08x", kKindName[kind], kKindName[kind], hash);
            for (const uint32_t peer : peers_[kind]) logf("     visited peer shader hash = %08x", peer);
            ++marks_;
            char text[96];
            snprintf(text, sizeof(text), "> %s marked: %08x", kKindName[kind], hash);
            told = text;
            if (actions_ & 1u) {
                snprintf(text, sizeof(text), hash_to_clipboard(hash) ? "> %s hash %08x copied to clipboard" : "> error copying %s hash %08x to clipboard",
                         kKindName[kind], hash);
                told += "\n"; told += text;
            }
        } else if (action == A::Done) {
            // DoneHunting: every selection and every visited set cleared.
            for (int kind = 0; kind < 2; ++kind) {
                visited_[kind].clear(); peers_[kind].clear(); has_selected_[kind] = false;
            }
            told = "> hunting selection cleared";
        }
    }
    size_t at = 0;
    while (!told.empty() && at != std::string::npos) {
        const size_t end = told.find('\n', at);
        notice(NoticeLevel::Info, "", told.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
        at = end == std::string::npos ? end : end + 1;
    }
}

bool Hunting::visit(const PipelineModules& modules)
{
    const bool has[2] = {(modules.mask & 1u) != 0, (modules.mask & 0x10u) != 0};
    const uint32_t hash[2] = {modules.hashes[0], modules.hashes[4]};
    std::lock_guard<std::mutex> lock(mx_);
    if (mode_.load(std::memory_order_relaxed) != 1) return false;
    for (int kind = 0; kind < 2; ++kind) if (has[kind] && visited_[kind].size() < 65536) visited_[kind].insert(hash[kind]);
    bool marked = false;
    for (int kind = 0; kind < 2; ++kind) {
        if (!has_selected_[kind] || !has[kind] || hash[kind] != selected_[kind]) continue;
        // The selected shader's peers, for the mark's log; the draw marked.
        if (has[1 - kind] && peers_[kind].size() < 256) peers_[kind].insert(hash[1 - kind]);
        marked = true;
    }
    // marking_mode = skip keeps the draw from the GPU; the other modes need a replaced fragment shader (the next step).
    if (marked && marking_mode_ == 0) { ++skipped_; return true; }
    if (marked && !told_mode_) { told_mode_ = true; logf("hunting: marking mode %s draws a selected shader as it is (no game shader is replaced yet)", kMarkingNames[marking_mode_]); }
    return false;
}

void Hunting::frame()
{
    std::lock_guard<std::mutex> lock(mx_);
    // TimeoutHuntingBuffers: after a minute without a hunting key the sets start again (the selection stays).
    if (mode_.load(std::memory_order_relaxed) == 1 && last_key_us_ && host_time_us() - last_key_us_ > 60000000ull) {
        visited_[0].clear(); visited_[1].clear(); last_key_us_ = host_time_us();
    }
}

void Hunting::status(JsonWriter& w)
{
    std::lock_guard<std::mutex> lock(mx_);
    w.key("hunting"); w.begin_object();
    w.kb("keys", keys_); w.kn("mode", uint64_t(mode_.load(std::memory_order_relaxed)));
    w.kv("marking_mode", kMarkingNames[marking_mode_]); w.kn("marking_actions", actions_);
    w.key("visited"); w.begin_object(); w.kn("vertex", visited_[0].size()); w.kn("pixel", visited_[1].size()); w.end_object();
    w.key("selected"); w.begin_object();
    for (int kind = 0; kind < 2; ++kind) {
        char text[16] = {};
        if (has_selected_[kind]) snprintf(text, sizeof(text), "%08x", selected_[kind]);
        w.kv(kind ? "pixel" : "vertex", text);
    }
    w.end_object();
    w.key("position"); w.begin_object();
    for (int kind = 0; kind < 2; ++kind) {
        const auto& visited = visited_[kind];
        const auto found = has_selected_[kind] ? visited.find(selected_[kind]) : visited.end();
        const size_t at = found == visited.end() ? 0 : size_t(std::distance(visited.begin(), found)) + 1;
        w.kv(kind ? "pixel" : "vertex", std::to_string(at) + "/" + std::to_string(visited.size()));
    }
    w.end_object();
    w.kn("skipped", skipped_.load()); w.kn("marks", marks_);
    w.end_object();
}

// ---------------------------------------------------------------------------------------------------------------------
// ShaderOverride by SPIR-V hash.

// After the folder generation's presentation, on the presenting thread: the acting hashes of the generation in place
// (a new one after an F10 exchange), its hunting settings, and the hunting keys its input dispatcher met.
void shader_hooks_frame(CharacterBackend& b)
{
    CharacterProgram* program = b.folder ? b.folder->program : nullptr;
    std::shared_ptr<const CharacterBackend::ShaderHooks> current;
    { std::shared_lock<std::shared_mutex> lock(b.index_mx); current = b.shader_hooks; }
    if (!current ? program != nullptr : current->folder != b.folder) {
        auto next = std::make_shared<CharacterBackend::ShaderHooks>();
        next->folder = b.folder;
        if (program && program->runtime()) {
            next->program = program;
            next->hashes = program->runtime()->shader_hashes();
            b.hunting.configure(program->runtime()->hunting());
            if (!next->hashes.empty()) logf("ShaderOverride: %zu SPIR-V module hash(es) act at the draws of the pipelines that have them", next->hashes.size());
        }
        std::unique_lock<std::shared_mutex> lock(b.index_mx);
        b.shader_hooks = std::move(next);
        current = b.shader_hooks;
    }
    if (program && program->runtime())
        for (const auto action : program->runtime()->take_hunting_actions()) b.hunting.act(action);
    b.hunting.frame();
    if (b.device) b.device->shader_draws.store((current && !current->hashes.empty()) || b.hunting.enabled(), std::memory_order_relaxed);
}

// A draw no Mod section made the Mod's (it matched none, or its lists left it to the game): hunting's mark, and the
// ShaderOverride entries of its modules unless they ran with the matched sections. True: the draw is not forwarded.
bool shader_draw_unmatched(CharacterBackend& b, CmdBuf* cb, const DrawRec& r, const PipelineModules& modules, bool run_lists, bool marked)
{
    (void)cb;
    bool skip = marked;
    std::shared_ptr<const CharacterBackend::ShaderHooks> hooks;
    if (run_lists) { std::shared_lock<std::shared_mutex> lock(b.index_mx); hooks = b.shader_hooks; }
    bool named = false;
    if (hooks && hooks->program)
        for (unsigned i = 0; i < 5 && !named; ++i)
            named = (modules.mask >> i & 1u) && std::binary_search(hooks->hashes.begin(), hooks->hashes.end(), modules.hashes[i]);
    if (named) {
        efmi::frontend::RuntimeDraw request;
        request.count = r.count; request.first = r.first; request.base = r.vertex_offset;
        request.instances = r.instances; request.first_instance = r.first_instance;
        request.indexed = r.kind == DK_INDEXED || r.kind == DK_INDEXED_INDIRECT;
        request.indirect = r.kind == DK_INDIRECT || r.kind == DK_INDEXED_INDIRECT;
        request.frame = hooks->folder->frame.load(std::memory_order_relaxed); request.seconds = ini_seconds();
        request.frame_seconds = hooks->folder->frame_seconds.load(std::memory_order_relaxed);
        request.shader_stages = modules.stages;
        for (unsigned i = 0; i < 5; ++i) request.stage_hashes[i] = modules.hashes[i];
        request.stage_hash_mask = modules.mask;
        request.targets_known = modules.targets_known; request.render_targets = modules.targets; request.render_pass = modules.render_pass;
        efmi::assets::FilePlan plan;
        std::string reason;
        bool ran = false;
        {
            // The folder's generation is one program: one plan at a time with every package's (D-131).
            std::lock_guard<std::mutex> lock(*hooks->folder->plan_mx);
            ran = hooks->program->runtime()->shader_draw(request, plan, reason);
        }
        b.shader_draws_run.fetch_add(1, std::memory_order_relaxed);
        if (!ran) {
            // D-162: what the host cannot do leaves the draw to the game, the lists having run to their end.
            b.shader_draws_refused.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(b.shader_note_mx);
            if (b.shader_note.empty()) { b.shader_note = reason; logf("ShaderOverride at a draw no Mod section matched: %s; the game's draw stays", reason.c_str()); }
        } else if (!plan.items.empty()) {
            // Bindings and draws of the lists at such a draw are a later step's (route 2's slots): the game's draw stays.
            b.shader_draws_kept.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(b.shader_note_mx);
            if (!b.shader_bindings_told) {
                b.shader_bindings_told = true;
                notice(NoticeLevel::Warning, "shader override bindings",
                       "A ShaderOverride binds or draws at a draw no Mod section matched: not done yet, the game's draw stays as it is");
            }
        } else if (plan.skip_original) {
            skip = true;
        }
    }
    if (skip) b.shader_draws_skipped.fetch_add(1, std::memory_order_relaxed);
    return skip;
}

void shader_hooks_status(CharacterBackend& b, JsonWriter& w)
{
    std::shared_ptr<const CharacterBackend::ShaderHooks> hooks;
    { std::shared_lock<std::shared_mutex> lock(b.index_mx); hooks = b.shader_hooks; }
    w.key("shader_overrides"); w.begin_object();
    w.key("hashes"); w.begin_array();
    if (hooks) for (const uint32_t hash : hooks->hashes) w.hex32(hash);
    w.end_array();
    w.kn("draws", b.shader_draws_run.load()); w.kn("skipped", b.shader_draws_skipped.load());
    w.kn("kept", b.shader_draws_kept.load()); w.kn("refused", b.shader_draws_refused.load());
    { std::lock_guard<std::mutex> lock(b.shader_note_mx); w.kv("last_reason", b.shader_note); }
    w.end_object();
    b.hunting.status(w);
}

// A draw the host records outside the index's path (non-indexed and indirect draws): hunting and ShaderOverride only.
bool character_shader_draw(CharacterBackend& b, CmdBuf* cb, const DrawRec& r)
{
    if (!b.device || !b.device->shader_draws.load(std::memory_order_relaxed) || !b.enabled.load(std::memory_order_relaxed)) return false;
    PipelineModules modules;
    if (!pipeline_modules(b.device, r.pipeline, modules)) return false;
    const bool marked = b.hunting.enabled() && b.hunting.visit(modules);
    return shader_draw_unmatched(b, cb, r, modules, true, marked);
}

} // namespace efvk
