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
// Hunting (Hunting.cpp): the selection runs over what the frame's draws used while hunting is enabled.

namespace {
const char* const kKindName[Hunting::Kinds] = {"vertex shader", "pixel shader", "index buffer", "vertex buffer", "render target"};
const char* const kKindShort[Hunting::Kinds] = {"VS", "PS", "IB", "VB", "RT"};
const char* const kMarkingNames[4] = {"skip", "original", "pink", "mono"};
const char* const kStageName[5] = {"vs", "hs", "ds", "gs", "ps"};

// HashToClipboard with the eight digits; the player's own clipboard, as the original's.
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

// MarkShaderEnd's CopyToFixes for a SPIR-V module: the module's bytes as the game created them, `<hash>-vs.spv` or
// `-ps.spv`, in the layer's ShaderFixes folder (never the installation's). False with the text when there is nothing
// to write; `already` when the file is there.
bool dump_module(Device* device, int kind, uint32_t hash, const std::filesystem::path& folder, std::string& text, bool& already)
{
    const uint32_t stage = kind == Hunting::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    std::shared_ptr<const ShaderInfo> module;
    {
        std::shared_lock<std::shared_mutex> lock(device->res_mx);
        for (const auto& pipe : device->pipes) {
            for (const auto& s : pipe.second.shaders)
                if (s.stage == stage && s.shader && s.shader->hash == hash && !s.shader->code.empty()) { module = s.shader; break; }
            if (module) break;
        }
    }
    char name[64];
    snprintf(name, sizeof(name), "%08x-%s.spv", hash, kind == Hunting::Vertex ? "vs" : "ps");
    const auto path = folder / "ShaderFixes" / name;
    std::error_code error;
    already = std::filesystem::exists(path, error);
    if (already) { text = path.u8string(); return true; }
    if (!module) { text = "the module's bytes are not kept"; return false; }
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(module->code.data()), std::streamsize(module->code.size() * 4));
    if (!out) { text = "cannot write " + path.u8string(); return false; }
    text = path.u8string();
    return true;
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
    char slot[16] = {};
    if (kind == Buffer && buffer_slot_ >= 0) snprintf(slot, sizeof(slot), "%d", buffer_slot_);
    char text[96];
    if (found == visited.end()) snprintf(text, sizeof(text), "%s%s 0/%zu", kKindShort[kind], slot, visited.size());
    else if (kind == Target) snprintf(text, sizeof(text), "%s %zu/%zu: image %u", kKindShort[kind], size_t(std::distance(visited.begin(), found)) + 1,
                                      visited.size(), selected_[kind]);
    else snprintf(text, sizeof(text), "%s%s %zu/%zu: %08x", kKindShort[kind], slot, size_t(std::distance(visited.begin(), found)) + 1, visited.size(),
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
    peers_[kind][0].clear(); peers_[kind][1].clear();
}

void Hunting::act(efmi::frontend::HuntingAction action, Device* device, const std::filesystem::path& folder)
{
    using A = efmi::frontend::HuntingAction;
    std::vector<std::string> told;
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (!keys_) return;
        last_key_us_ = host_time_us();
        const int mode = mode_.load(std::memory_order_relaxed);
        int kind = -1; bool next = false, mark = false;
        switch (action) {
        case A::NextPixel: kind = Pixel; next = true; break;
        case A::PreviousPixel: kind = Pixel; break;
        case A::MarkPixel: kind = Pixel; mark = true; break;
        case A::NextVertex: kind = Vertex; next = true; break;
        case A::PreviousVertex: kind = Vertex; break;
        case A::MarkVertex: kind = Vertex; mark = true; break;
        case A::NextIndex: kind = Index; next = true; break;
        case A::PreviousIndex: kind = Index; break;
        case A::MarkIndex: kind = Index; mark = true; break;
        case A::NextVertexBuffer: kind = Buffer; next = true; break;
        case A::PreviousVertexBuffer: kind = Buffer; break;
        case A::MarkVertexBuffer: kind = Buffer; mark = true; break;
        case A::NextRenderTarget: kind = Target; next = true; break;
        case A::PreviousRenderTarget: kind = Target; break;
        case A::MarkRenderTarget: kind = Target; mark = true; break;
        default: break;
        }
        if (action == A::Toggle) {
            mode_.store(mode == 1 ? 2 : 1, std::memory_order_relaxed);
            logf("> Hunting toggled to %d", mode == 1 ? 2 : 1);
            told.push_back(mode == 1 ? "> hunting off" : "> hunting on");
        } else if (action == A::AnalyseFrame) {
            // AnalyseFrame: a second press during an analysis aborts it; otherwise only while hunting is enabled.
            if (analysing_.load(std::memory_order_relaxed) || armed_) {
                armed_ = false; analysing_.store(false, std::memory_order_relaxed); analysis_.clear();
                told.push_back("Frame analysis aborted");
            } else if (mode == 1) {
                armed_ = true;
            }
        } else if (mode != 1) {
            return; // the original's keys do nothing unless hunting is enabled
        } else if (action == A::NextMarkingMode) {
            marking_mode_ = (marking_mode_ + 1) % 4;
            told.push_back(std::string("> marking mode ") + kMarkingNames[marking_mode_]);
        } else if (action == A::NextVertexBufferSlot || action == A::PreviousVertexBufferSlot) {
            // NextVertexBufferSlot: every slot (-1), then 0 to the last; the visited buffers start again.
            if (action == A::NextVertexBufferSlot) buffer_slot_ = buffer_slot_ >= int(kMaxVb) - 1 ? -1 : buffer_slot_ + 1;
            else buffer_slot_ = buffer_slot_ < 0 ? int(kMaxVb) - 1 : buffer_slot_ - 1;
            visited_[Buffer].clear(); has_selected_[Buffer] = false;
            told.push_back(buffer_slot_ < 0 ? std::string("> vertex buffers of every slot") : "> vertex buffers of slot " + std::to_string(buffer_slot_));
        } else if (kind >= 0 && !mark) {
            select_locked(kind, next);
            told.push_back(line_locked(kind));
        } else if (kind >= 0 && mark) {
            if (!has_selected_[kind]) return;
            const uint32_t hash = selected_[kind];
            char text[160];
            // The original's mark lines, the hash in eight digits (MarkShaderBegin, MarkIndexBuffer, MarkVertexBuffer,
            // LogRenderTarget).
            if (kind == Index) logf(">>>> Index buffer marked: index buffer hash = %08x", hash);
            else if (kind == Buffer) logf(">>>> Vertex buffer marked: vertex buffer hash = %08x", hash);
            else if (kind == Target) logf(">>>> Render target marked: render target image = %u, %s", hash, target_text_[hash].c_str());
            else logf(">>>> %s marked: %s hash = %08x", kKindName[kind], kKindName[kind], hash);
            for (int peer = 0; peer < 2; ++peer)
                for (const uint32_t other : peers_[kind][peer])
                    logf("     visited %s hash = %08x", kind == Vertex || kind == Pixel ? "peer shader" : kKindName[peer], other);
            ++marks_;
            if (kind == Target) snprintf(text, sizeof(text), "> render target marked: image %u, %s", hash, target_text_[hash].c_str());
            else snprintf(text, sizeof(text), "> %s marked: %08x", kKindName[kind], hash);
            told.push_back(text);
            if ((actions_ & 1u) && kind != Target) {
                snprintf(text, sizeof(text), hash_to_clipboard(hash) ? "> %s hash %08x copied to clipboard" : "> error copying %s hash %08x to clipboard",
                         kKindName[kind], hash);
                told.push_back(text);
            }
            // CopyToFixes for `hlsl`, `asm` or `regex` (the original's DUMP_MASK): a module's SPIR-V into ShaderFixes.
            if ((actions_ & 0xEu) && (kind == Vertex || kind == Pixel) && device) {
                std::string where; bool already = false;
                if (dump_module(device, kind, hash, folder, where, already)) {
                    if (!already) ++dumps_;
                    last_dump_ = where;
                    logf("hunting: %s %08x %s %s", kKindName[kind], hash, already ? "already in" : "dumped to", where.c_str());
                    snprintf(text, sizeof(text), "> %s %08x %s ShaderFixes", kKindName[kind], hash, already ? "already in" : "dumped to");
                } else {
                    logf("hunting: %s %08x not dumped: %s", kKindName[kind], hash, where.c_str());
                    snprintf(text, sizeof(text), "> %s %08x not dumped", kKindName[kind], hash);
                }
                told.push_back(text);
            }
        } else if (action == A::Done) {
            // DoneHunting: every selection and every visited set cleared.
            for (int k = 0; k < Kinds; ++k) { visited_[k].clear(); peers_[k][0].clear(); peers_[k][1].clear(); has_selected_[k] = false; }
            buffer_slot_ = -1; target_text_.clear();
            told.push_back("> hunting selection cleared");
        }
    }
    for (const auto& text : told) notice(NoticeLevel::Info, "", text.c_str());
}

bool Hunting::visit(const DrawFacts& facts)
{
    const auto& m = facts.modules;
    std::lock_guard<std::mutex> lock(mx_);
    if (mode_.load(std::memory_order_relaxed) != 1) return false;
    const bool has_vs = (m.mask & 1u) != 0, has_ps = (m.mask & 0x10u) != 0;
    auto insert = [&](int kind, uint32_t value) { if (visited_[kind].size() < 65536) visited_[kind].insert(value); };
    if (has_vs) insert(Vertex, m.hashes[0]);
    if (has_ps) insert(Pixel, m.hashes[4]);
    if (facts.index_known) insert(Index, facts.index_hash);
    for (uint32_t slot = 0; slot < kMaxVb; ++slot)
        if ((facts.vertex_mask >> slot & 1u) && (buffer_slot_ < 0 || int(slot) == buffer_slot_)) insert(Buffer, facts.vertex_hashes[slot]);
    for (uint32_t i = 0; i < facts.target_count; ++i) {
        insert(Target, facts.targets[i]);
        if (target_text_.size() < 4096 && !target_text_.count(facts.targets[i])) {
            char text[96];
            snprintf(text, sizeof(text), "format %u, %ux%u", facts.target_format[i], facts.target_width[i], facts.target_height[i]);
            target_text_[facts.targets[i]] = text;
        }
    }
    // The selection marks the draw (the original's comparison in BeforeDraw), and the selected element's peers.
    bool marked = false;
    auto selected = [&](int kind, uint32_t value) { return has_selected_[kind] && selected_[kind] == value; };
    auto peers = [&](int kind) {
        if (has_vs && peers_[kind][0].size() < 256 && kind != Vertex) peers_[kind][0].insert(m.hashes[0]);
        if (has_ps && peers_[kind][1].size() < 256 && kind != Pixel) peers_[kind][1].insert(m.hashes[4]);
        marked = true;
    };
    if (has_vs && selected(Vertex, m.hashes[0])) peers(Vertex);
    if (has_ps && selected(Pixel, m.hashes[4])) peers(Pixel);
    if (facts.index_known && selected(Index, facts.index_hash)) peers(Index);
    for (uint32_t slot = 0; slot < kMaxVb; ++slot)
        if ((facts.vertex_mask >> slot & 1u) && (buffer_slot_ < 0 || int(slot) == buffer_slot_) && selected(Buffer, facts.vertex_hashes[slot])) {
            peers(Buffer); break;
        }
    for (uint32_t i = 0; i < facts.target_count; ++i)
        if (selected(Target, facts.targets[i])) { peers(Target); break; }
    // marking_mode = skip keeps the draw from the GPU; the other modes need a replaced fragment shader (pink, mono) or
    // a game shader replaced to draw (original): until then the draw is drawn as is, said once.
    if (marked && marking_mode_ == 0) { ++skipped_; return true; }
    if (marked && !told_mode_) { told_mode_ = true; logf("hunting: marking mode %s draws a selected element as it is", kMarkingNames[marking_mode_]); }
    return false;
}

void Hunting::analyse(const DrawRec& r, const DrawFacts& facts)
{
    // FrameAnalysisContext's log, one block per draw with its number: what is bound, then the call as the original
    // names it. Recording threads interleave; the command buffer tells their draws apart.
    std::lock_guard<std::mutex> lock(mx_);
    if (!analysing_.load(std::memory_order_relaxed) || analysis_.size() > (64u << 20)) return;
    const uint64_t n = ++analysed_draws_;
    char line[256];
    const auto& m = facts.modules;
    snprintf(line, sizeof(line), "%06llu command buffer %u, pipeline %llx\n", static_cast<unsigned long long>(n), r.cb_id,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r.pipeline)));
    analysis_ += line;
    for (unsigned i = 0; i < 5; ++i) {
        if (!(m.mask >> i & 1u)) continue;
        snprintf(line, sizeof(line), "%06llu %sSetShader hash=%08x\n", static_cast<unsigned long long>(n), i == 0 ? "VS" : i == 1 ? "HS" : i == 2 ? "DS" : i == 3 ? "GS" : "PS",
                 m.hashes[i]);
        analysis_ += line;
    }
    if (facts.index_known) {
        snprintf(line, sizeof(line), "%06llu IASetIndexBuffer hash=%08x\n", static_cast<unsigned long long>(n), facts.index_hash);
        analysis_ += line;
    }
    for (uint32_t slot = 0; slot < kMaxVb; ++slot)
        if (facts.vertex_mask >> slot & 1u) {
            snprintf(line, sizeof(line), "%06llu IASetVertexBuffers slot %u hash=%08x\n", static_cast<unsigned long long>(n), slot, facts.vertex_hashes[slot]);
            analysis_ += line;
        }
    for (uint32_t i = 0; i < facts.target_count; ++i) {
        snprintf(line, sizeof(line), "%06llu OMSetRenderTargets o%u image=%u format %u, %ux%u\n", static_cast<unsigned long long>(n), i, facts.targets[i],
                 facts.target_format[i], facts.target_width[i], facts.target_height[i]);
        analysis_ += line;
    }
    switch (r.kind) {
    case DK_INDEXED:
        snprintf(line, sizeof(line), "%06llu DrawIndexedInstanced(IndexCountPerInstance:%u, InstanceCount:%u, StartIndexLocation:%u, BaseVertexLocation:%d, StartInstanceLocation:%u)\n",
                 static_cast<unsigned long long>(n), r.count, r.instances, r.first, r.vertex_offset, r.first_instance);
        break;
    case DK_DRAW:
        snprintf(line, sizeof(line), "%06llu DrawInstanced(VertexCountPerInstance:%u, InstanceCount:%u, StartVertexLocation:%u, StartInstanceLocation:%u)\n",
                 static_cast<unsigned long long>(n), r.count, r.instances, r.first, r.first_instance);
        break;
    default:
        snprintf(line, sizeof(line), "%06llu %s(DrawCount:%u)\n", static_cast<unsigned long long>(n),
                 r.kind == DK_INDEXED_INDIRECT ? "DrawIndexedInstancedIndirect" : "DrawInstancedIndirect", r.count);
        break;
    }
    analysis_ += line;
}

void Hunting::frame(const std::filesystem::path& folder)
{
    std::string written, failed;
    {
        std::lock_guard<std::mutex> lock(mx_);
        // TimeoutHuntingBuffers: after a minute without a hunting key the sets start again (the selection stays).
        if (mode_.load(std::memory_order_relaxed) == 1 && last_key_us_ && host_time_us() - last_key_us_ > 60000000ull) {
            for (auto& visited : visited_) visited.clear();
            last_key_us_ = host_time_us();
        }
        if (analysing_.load(std::memory_order_relaxed)) {
            // The frame's draws are in: the log goes to FrameAnalysis-<time> (the original's folder name).
            analysing_.store(false, std::memory_order_relaxed);
            std::ofstream out(analysis_path_ / "log.txt", std::ios::binary);
            out << analysis_;
            if (out) { written = analysis_path_.u8string(); ++analyses_; } else failed = analysis_path_.u8string();
            analysis_.clear();
        }
        if (armed_) {
            armed_ = false;
            time_t now = time(nullptr);
            struct tm local = {};
            localtime_s(&local, &now);
            char name[64];
            strftime(name, sizeof(name), "FrameAnalysis-%Y-%m-%d-%H%M%S", &local);
            analysis_path_ = folder / name;
            // AnalyseFrame creates the timestamp directory exclusively; an earlier capture is never overwritten.
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            if (std::filesystem::create_directory(analysis_path_, error)) {
                analysis_ = "analyse_options: the draws' shaders, index and vertex buffer hashes and render targets (no resource dumps yet)\n";
                analysed_draws_ = 0;
                analysing_.store(true, std::memory_order_relaxed);
            } else failed = analysis_path_.u8string();
        }
    }
    if (!written.empty()) {
        logf("Frame analysis saved to %s", written.c_str());
        notice(NoticeLevel::Info, "", ("Frame analysis saved to " + written).c_str());
    } else if (!failed.empty()) {
        notice(NoticeLevel::Warning, "", ("Frame analysis could not be written to " + failed).c_str());
    }
}

void Hunting::status(JsonWriter& w)
{
    std::lock_guard<std::mutex> lock(mx_);
    static const char* const keys[Kinds] = {"vertex", "pixel", "index", "buffer", "target"};
    w.key("hunting"); w.begin_object();
    w.kb("keys", keys_); w.kn("mode", uint64_t(mode_.load(std::memory_order_relaxed)));
    w.kv("marking_mode", kMarkingNames[marking_mode_]); w.kn("marking_actions", actions_);
    w.key("visited"); w.begin_object();
    for (int k = 0; k < Kinds; ++k) w.kn(keys[k], visited_[k].size());
    w.end_object();
    w.key("selected"); w.begin_object();
    for (int k = 0; k < Kinds; ++k) {
        char text[16] = {};
        if (has_selected_[k]) snprintf(text, sizeof(text), k == Target ? "%u" : "%08x", selected_[k]);
        w.kv(keys[k], text);
    }
    w.end_object();
    w.key("position"); w.begin_object();
    for (int k = 0; k < Kinds; ++k) {
        const auto& visited = visited_[k];
        const auto found = has_selected_[k] ? visited.find(selected_[k]) : visited.end();
        const size_t at = found == visited.end() ? 0 : size_t(std::distance(visited.begin(), found)) + 1;
        w.kv(keys[k], std::to_string(at) + "/" + std::to_string(visited.size()));
    }
    w.end_object();
    w.kn("buffer_slot", uint64_t(buffer_slot_ + 1)); // 0: every slot
    w.kn("skipped", skipped_.load()); w.kn("marks", marks_); w.kn("dumps", dumps_); w.kv("last_dump", last_dump_);
    w.kn("analyses", analyses_); w.kb("analysing", analysing_.load(std::memory_order_relaxed));
    w.kv("last_analysis", analyses_ ? analysis_path_.u8string() : std::string());
    w.end_object();
}

// What hunting and frame analysis read of a draw (its modules are in `facts` already).
void draw_facts(CharacterBackend& b, CmdBuf* cb, const DrawRec& r, bool buffers, DrawFacts& facts)
{
    facts.indexed = r.kind == DK_INDEXED;
    Device* device = b.device;
    // The colour targets the pipeline's subpass writes, as the pass instance names their images.
    if (cb && cb->pass.info && cb->pass.info->known && cb->pass.recipe) {
        std::shared_lock<std::shared_mutex> lock(device->res_mx);
        const auto pipe = device->pipes.find(r.pipeline);
        const uint32_t sub = pipe != device->pipes.end() && pipe->second.recipe ? pipe->second.recipe->subpass : 0;
        if (sub < cb->pass.recipe->subpasses.size())
            for (const auto& color : cb->pass.recipe->subpasses[sub].colors) {
                if (facts.target_count >= 8) break;
                if (color.attachment == VK_ATTACHMENT_UNUSED || color.attachment >= cb->pass.info->views.size()) continue;
                const auto& view = cb->pass.info->views[color.attachment];
                if (!view.known) continue;
                const uint32_t i = facts.target_count++;
                facts.targets[i] = view.image.id; facts.target_format[i] = uint32_t(view.image.format);
                facts.target_width[i] = view.image.extent.width; facts.target_height[i] = view.image.extent.height;
            }
    }
    if (!buffers) return;
    std::shared_lock<std::shared_mutex> lock(device->res_mx);
    std::vector<uint8_t> bytes;
    // The index region's hash, the TextureOverride hash (GetRegionHash over the draw's indices), read from the
    // application's mapping or the layer's shadow copy, as the original reads its copy of the buffer.
    if (facts.indexed && (r.index_size == 2 || r.index_size == 4)) {
        const auto buffer = device->bufs.find(r.ib);
        const uint64_t offset = r.ib_off + uint64_t(r.first) * r.index_size, size = uint64_t(r.count) * r.index_size;
        if (buffer != device->bufs.end() && size && size <= (64u << 20) && offset <= buffer->second.size && size <= buffer->second.size - offset) {
            bytes.resize(size_t(size));
            if (read_buffer_locked(device, buffer->second, offset, size, bytes.data()) != SRC_NONE) {
                facts.index_hash = crc32c(0, bytes.data(), bytes.size());
                facts.index_known = true;
            }
        }
    }
    // The vertex buffers' regions (GetVertexBufferRegionOffset and Size): from the first vertex (the base vertex of an
    // indexed draw), the vertex count, or for an indexed draw (3 * IndexCount + 10) / 20 + 3 vertices; from mapped memory.
    const auto pipe = device->pipes.find(r.pipeline);
    if (pipe == device->pipes.end()) return;
    for (const auto& binding : pipe->second.bindings) {
        if (binding.binding >= kMaxVb || !binding.stride || !r.vb[binding.binding].buf) continue;
        const auto buffer = device->bufs.find(r.vb[binding.binding].buf);
        if (buffer == device->bufs.end()) continue;
        const int64_t first = facts.indexed ? r.vertex_offset : int64_t(r.first);
        const uint64_t vertices = facts.indexed ? (3ull * r.count + 10) / 20 + 3 : r.count;
        if (first < 0) continue;
        const uint64_t offset = r.vb[binding.binding].off + uint64_t(first) * binding.stride;
        uint64_t size = vertices * binding.stride;
        if (offset >= buffer->second.size) continue;
        size = std::min<uint64_t>(size, buffer->second.size - offset);
        if (!size || size > (16u << 20)) continue;
        bytes.resize(size_t(size));
        if (read_buffer_locked(device, buffer->second, offset, size, bytes.data()) == SRC_NONE) continue;
        facts.vertex_hashes[binding.binding] = crc32c(0, bytes.data(), bytes.size());
        facts.vertex_mask |= 1u << binding.binding;
    }
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
    if (!current ? b.folder != nullptr : current->folder != b.folder) {
        auto next = std::make_shared<CharacterBackend::ShaderHooks>();
        next->folder = b.folder;
        if (b.variants) {
            const auto fixes = b.folder ? b.folder->shader_fixes : nullptr;
            const auto previous = b.variants->native_shaders();
            b.variants->native_shaders(fixes);
            if ((fixes ? fixes->revision : 0) != (previous ? previous->revision : 0)) {
                decltype(b.twins) retired;
                { std::unique_lock<std::shared_mutex> lock(b.twins_mx); retired.swap(b.twins); b.twins_failed.clear(); }
                // A staged generation may have warmed its local lookup before this revision was published.
                // Its next draw must ask the revision-keyed variant cache again.
                for (const auto& controller : b.controllers) {
                    std::shared_lock<std::shared_mutex> lock(controller->mutex);
                    if (controller->active) {
                        std::lock_guard<std::mutex> mod(controller->active->mod_mx);
                        controller->active->variant_lookup.clear();
                    }
                }
                prepare_twins(b);
            }
            if (fixes) {
                if (!fixes->modules.empty()) logf("ShaderFixes: %zu native module(s), revision %llu", fixes->modules.size(), static_cast<unsigned long long>(fixes->revision));
                for (const auto& warning : fixes->warnings) {
                    logf("ShaderFixes: %s", warning.c_str());
                    notice(NoticeLevel::Warning, "shader replacement", warning.c_str());
                }
            }
        }
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
    // The layer's own folder for what hunting writes (dumped modules, frame analyses), never the installation's.
    const auto folder = g_cfg.m2_settings.empty() ? module_folder() / "settings" : absolute_folder(g_cfg.m2_settings);
    if (program && program->runtime())
        for (const auto action : program->runtime()->take_hunting_actions()) b.hunting.act(action, b.device, folder);
    b.hunting.frame(folder);
    // The `hunting` operand reads the original's mode (docs/vulkan-native-mods.md section 6).
    if (program && program->runtime()) program->runtime()->hunting_mode(b.hunting.operand());
    if (b.device)
        b.device->shader_draws.store((current && !current->hashes.empty()) || b.hunting.enabled() || b.hunting.analysing(), std::memory_order_relaxed);
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
    const auto fixes = b.variants ? b.variants->native_shaders() : nullptr;
    w.key("shader_fixes"); w.begin_object();
    w.kn("loaded", fixes ? fixes->modules.size() : 0); w.kn("revision", fixes ? fixes->revision : 0);
    w.kn("binds", b.shader_fix_binds.load());
    w.key("warnings"); w.begin_array();
    if (fixes) for (const auto& warning : fixes->warnings) w.str(warning.c_str());
    w.end_array(); w.end_object();
    b.hunting.status(w);
}

// Hunting and frame analysis at a draw whose modules are in `facts`: true when hunting's selection keeps it from the GPU.
bool hunting_draw(CharacterBackend& b, CmdBuf* cb, const DrawRec& r, DrawFacts& facts)
{
    const bool hunting = b.hunting.enabled(), analysing = b.hunting.analysing();
    if (!hunting && !analysing) return false;
    draw_facts(b, cb, r, true, facts);
    if (analysing) b.hunting.analyse(r, facts);
    return hunting && b.hunting.visit(facts);
}

// A draw the host records outside the index's path (non-indexed and indirect draws): hunting and ShaderOverride only.
bool character_shader_draw(CharacterBackend& b, CmdBuf* cb, const DrawRec& r)
{
    if (!b.device || !b.device->shader_draws.load(std::memory_order_relaxed) || !b.enabled.load(std::memory_order_relaxed)) return false;
    DrawFacts facts;
    if (!pipeline_modules(b.device, r.pipeline, facts.modules)) return false;
    return shader_draw_unmatched(b, cb, r, facts.modules, true, hunting_draw(b, cb, r, facts));
}

} // namespace efvk
