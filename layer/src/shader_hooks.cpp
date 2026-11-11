// What the library's shader hooks stand for, read off SPIR-V (D-113).
#include "shader_hooks.h"
#include "../../frontend/gpu/source_registers.h"
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace efvk {
namespace {
// SPIR-V words of the few instructions read here (SPIR-V 1.0 numbering).
enum Op : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26,
    OpTypeSampledImage = 27, OpTypeStruct = 30, OpTypePointer = 32, OpFunction = 54, OpFunctionEnd = 56, OpFunctionCall = 57,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
    OpSampledImage = 86, OpImageSampleExplicitLod = 88, OpLabel = 248, OpReturn = 253
};
enum Decoration : uint32_t { DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecFlat = 14, DecLocation = 30, DecBinding = 33, DecSet = 34 };
enum Storage : uint32_t { SUniformConstant = 0, SInput = 1, SUniform = 2, SOutput = 3, SStorageBuffer = 12 };
constexpr uint32_t kPosition = 0, kInstanceIndex = 43;

struct Module {
    uint32_t model = UINT32_MAX;
    std::unordered_map<uint32_t, std::vector<uint32_t>> types; // result id -> [opcode, operands after the result]
    std::unordered_map<uint32_t, uint32_t> location, builtin;
    std::unordered_set<uint32_t> flat, block, buffer_block;
    struct Var { uint32_t type = 0, storage = 0; };
    std::unordered_map<uint32_t, Var> vars;
    std::vector<std::vector<uint32_t>> body; // function instructions: [opcode, operands...]
    bool parse(const std::vector<uint32_t>& code)
    {
        if (code.size() < 5 || code[0] != 0x07230203u) return false;
        bool in_function = false;
        for (size_t at = 5; at < code.size();) {
            const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
            if (!count || at + count > code.size()) return false;
            const uint32_t* w = &code[at + 1];
            const uint32_t n = count - 1;
            switch (op) {
            case OpEntryPoint: if (n >= 1 && model == UINT32_MAX) model = w[0]; break;
            case OpDecorate:
                if (n >= 2) {
                    if (w[1] == DecLocation && n >= 3) location[w[0]] = w[2];
                    else if (w[1] == DecBuiltIn && n >= 3) builtin[w[0]] = w[2];
                    else if (w[1] == DecFlat) flat.insert(w[0]);
                    else if (w[1] == DecBlock) block.insert(w[0]);
                    else if (w[1] == DecBufferBlock) buffer_block.insert(w[0]);
                }
                break;
            case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage:
            case OpTypeStruct: case OpTypePointer:
                if (n >= 1) { std::vector<uint32_t> t{op}; t.insert(t.end(), w + 1, w + n); types[w[0]] = std::move(t); }
                break;
            case OpVariable: if (n >= 3 && !in_function) vars[w[1]] = {w[0], w[2]}; break;
            case OpFunction: in_function = true; break;
            case OpFunctionEnd: in_function = false; break;
            default: break;
            }
            if (in_function) { std::vector<uint32_t> i{op}; i.insert(i.end(), w, w + n); body.push_back(std::move(i)); }
            at += count;
        }
        return model != UINT32_MAX;
    }
    const std::vector<uint32_t>* type(uint32_t id) const { const auto t = types.find(id); return t == types.end() ? nullptr : &t->second; }
    // The type a pointer variable points at.
    uint32_t pointee(uint32_t pointer) const { const auto* t = type(pointer); return t && (*t)[0] == OpTypePointer && t->size() >= 3 ? (*t)[2] : 0; }
    // 'f' float, 'u' unsigned, 'i' signed, with components; 0 otherwise.
    std::pair<char, uint32_t> scalar_vector(uint32_t id) const
    {
        const auto* t = type(id);
        if (!t) return {0, 0};
        if ((*t)[0] == OpTypeVector && t->size() >= 3) { const auto inner = scalar_vector((*t)[1]); return inner.second == 1 ? std::make_pair(inner.first, (*t)[2]) : std::make_pair('\0', 0u); }
        if ((*t)[0] == OpTypeFloat) return {'f', 1};
        if ((*t)[0] == OpTypeInt && t->size() >= 3) return {(*t)[2] ? 'i' : 'u', 1};
        return {0, 0};
    }
};
}

namespace {
// A character vertex shader by its output interface: outputs of these widths at locations 0 on, all float
// but the last (the instance); a pipeline leaves out those it does not pass on, the first (the UV) and the
// last kept. The instance index is read, a storage and a uniform buffer are bound.
bool character_outputs(const std::vector<uint32_t>& code, const uint32_t* widths, uint32_t count)
{
    Module m;
    if (!m.parse(code) || m.model != 0) return false;
    bool instance = false, position = false, storage = false, uniform = false;
    uint32_t seen = 0;
    for (const auto& [id, var] : m.vars) {
        const uint32_t pointee = m.pointee(var.type);
        const auto builtin = m.builtin.find(id);
        if (var.storage == SInput && builtin != m.builtin.end() && builtin->second == kInstanceIndex) instance = true;
        if (var.storage == SOutput) {
            if (builtin != m.builtin.end()) { position |= builtin->second == kPosition; continue; }
            const auto location = m.location.find(id);
            if (location == m.location.end() || location->second >= count) return false;
            const auto sv = m.scalar_vector(pointee);
            if (sv.second != widths[location->second] || (location->second + 1 < count && sv.first != 'f')) return false;
            seen |= 1u << location->second;
        }
        if (var.storage == SStorageBuffer || (var.storage == SUniform && m.buffer_block.count(pointee))) storage = true;
        else if (var.storage == SUniform && m.block.count(pointee)) uniform = true;
    }
    return instance && position && storage && uniform && (seen & 1u) && (seen & (1u << (count - 1)));
}
}

bool lit_character_vertex(const std::vector<uint32_t>& code)
{
    // DX11 o1..o9 of the lit vertex shader at locations 0..8: float2, float3, float3, float4, float3 x4, one component.
    static const uint32_t widths[9] = {2, 3, 3, 4, 3, 3, 3, 3, 1};
    return character_outputs(code, widths, 9);
}

bool lit_skin_vertex(const std::vector<uint32_t>& code)
{
    // DX11 o1..o10 of the lit vertex shader of skin at locations 0..9: two more interpolants ahead of the instance.
    static const uint32_t widths[10] = {2, 3, 3, 4, 3, 3, 3, 4, 3, 1};
    return character_outputs(code, widths, 10);
}

bool outline_vertex(const std::vector<uint32_t>& code)
{
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    constexpr uint32_t kOpConstant = 43, kOutlineRamp = 0x42E52EE2u; // 114.59157 as the game's modules hold it
    std::unordered_set<uint32_t> floats;
    uint32_t model = UINT32_MAX;
    bool found = false;
    for (size_t at = 5; at < code.size();) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || at + count > code.size()) return false;
        if (op == OpFunction) break; // types and constants come ahead of the functions
        if (op == OpEntryPoint && count >= 2 && model == UINT32_MAX) model = code[at + 1];
        else if (op == OpTypeFloat && count >= 3 && code[at + 2] == 32) floats.insert(code[at + 1]);
        else if (op == kOpConstant && count >= 4 && code[at + 3] == kOutlineRamp && floats.count(code[at + 1])) found = true;
        at += count;
    }
    return model == 0 && found;
}

uint32_t character_vertex_hook(const std::vector<uint32_t>& code)
{
    return lit_character_vertex(code) ? 1u : lit_skin_vertex(code) ? 2u : outline_vertex(code) ? 3u : 0u;
}

PixelHook character_pixel_hook(const std::vector<uint32_t>& code)
{
    PixelHook out;
    std::vector<efmi::gpu::SourceRegister> registers;
    std::string reason;
    if (code.size() < 5 || code[0] != 0x07230203u || !efmi::gpu::source_registers(code, registers, reason)) return out;
    // SPIR-V words read here beyond the enumeration above.
    enum : uint32_t { ExtInst = 12, VectorShuffle = 79, CompositeConstruct = 80, CompositeExtract = 81, Image = 100, CopyMemory = 63,
                      SampleFirst = 87, SampleLast = 94, LoopMerge = 246, SelectionMerge = 247, Branch = 249, BranchConditional = 250,
                      Switch = 251, Kill = 252, ReturnValue = 254, Unreachable = 255, ImageWrite = 99, Line = 8, NoLine = 317 };
    struct Inst { uint32_t op = 0; const uint32_t* w = nullptr; uint32_t n = 0; };
    std::vector<Inst> body;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> bound; // variable -> (set, binding)
    std::unordered_map<uint32_t, uint32_t> origin;                      // value -> the variable it was loaded from
    std::unordered_set<uint32_t> inputs;
    uint32_t model = UINT32_MAX;
    bool in_function = false;
    for (size_t at = 5; at < code.size();) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (!count || at + count > code.size()) return out;
        const uint32_t* w = &code[at + 1];
        const uint32_t n = count - 1;
        if (op == OpEntryPoint && n >= 1 && model == UINT32_MAX) model = w[0];
        else if (op == OpDecorate && n >= 3 && w[1] == DecBinding) bound[w[0]].second = w[2];
        else if (op == OpDecorate && n >= 3 && w[1] == DecSet) bound[w[0]].first = w[2];
        else if (op == OpVariable && n >= 3 && !in_function) { origin[w[1]] = w[1]; if (w[2] == SInput) inputs.insert(w[1]); }
        else if (op == OpFunction) in_function = true;
        if (in_function) body.push_back({op, w, n});
        at += count;
    }
    if (model != 4) return out;
    const auto has_result = [](uint32_t op) {
        switch (op) {
        case OpLabel: case Branch: case BranchConditional: case Switch: case Kill: case OpReturn: case ReturnValue: case Unreachable:
        case OpStore: case CopyMemory: case LoopMerge: case SelectionMerge: case OpFunctionEnd: case ImageWrite: case Line: case NoLine:
            return false;
        default: return true;
        }
    };
    // The ids an instruction reads (its literals left out where an instruction of the game's modules has any).
    const auto reads = [&](const Inst& i, std::vector<uint32_t>& ids) {
        ids.clear();
        if (!has_result(i.op)) { if (i.op == BranchConditional && i.n) ids.push_back(i.w[0]); else if (i.op == OpStore && i.n >= 2) ids.push_back(i.w[1]); else if (i.op == ReturnValue && i.n) ids.push_back(i.w[0]); else if (i.op == Switch && i.n) ids.push_back(i.w[0]); return; }
        if (i.n < 3) return;
        if (i.op == CompositeExtract || i.op == OpLoad) ids.push_back(i.w[2]);
        else if (i.op == VectorShuffle) { ids.push_back(i.w[2]); if (i.n >= 4) ids.push_back(i.w[3]); }
        else if (i.op == ExtInst) ids.insert(ids.end(), i.w + std::min<uint32_t>(4, i.n), i.w + i.n);
        else if (i.op >= SampleFirst && i.op <= SampleLast) { ids.push_back(i.w[2]); if (i.n >= 4) ids.push_back(i.w[3]); if (i.n > 5) ids.insert(ids.end(), i.w + 5, i.w + i.n); }
        else ids.insert(ids.end(), i.w + 2, i.w + i.n);
    };
    std::unordered_map<uint32_t, uint32_t> producer;                 // result -> index in body
    std::unordered_map<uint32_t, std::vector<uint32_t>> users;       // id -> indices of the instructions that read it
    std::unordered_map<uint32_t, uint32_t> label_at;
    std::vector<uint32_t> ids;
    for (uint32_t at = 0; at < body.size(); ++at) {
        const Inst& i = body[at];
        if (i.op == OpLabel && i.n) label_at[i.w[0]] = at;
        if (has_result(i.op) && i.n >= 2) {
            producer[i.w[1]] = at;
            if ((i.op == OpLoad || i.op == OpAccessChain || i.op == OpInBoundsAccessChain || i.op == OpSampledImage || i.op == Image) && i.n >= 3) {
                const auto from = origin.find(i.w[2]);
                if (from != origin.end()) origin[i.w[1]] = from->second;
            }
        }
        reads(i, ids);
        for (uint32_t id : ids) users[id].push_back(at);
    }
    // A coordinate that is an interpolated input as it is: loaded, with at most its channels picked.
    std::function<bool(uint32_t, unsigned)> direct_input = [&](uint32_t value, unsigned depth) {
        const auto made = producer.find(value);
        if (depth > 8 || made == producer.end()) return false;
        const Inst& i = body[made->second];
        if (i.op == OpLoad && i.n >= 3) {
            if (inputs.count(i.w[2])) return true;
            const auto chain = producer.find(i.w[2]);
            return chain != producer.end() && (body[chain->second].op == OpAccessChain || body[chain->second].op == OpInBoundsAccessChain) &&
                   body[chain->second].n >= 3 && inputs.count(body[chain->second].w[2]) != 0;
        }
        if (i.op == VectorShuffle && i.n >= 4) return direct_input(i.w[2], depth + 1) && direct_input(i.w[3], depth + 1);
        if (i.op == CompositeExtract && i.n >= 3) return direct_input(i.w[2], depth + 1);
        if (i.op == CompositeConstruct) { for (uint32_t k = 2; k < i.n; ++k) if (!direct_input(i.w[k], depth + 1)) return false; return i.n > 2; }
        return false;
    };
    // The channels of a sample's result the module reads: those its extracts and shuffles pick, all four
    // when anything takes the whole value.
    const auto channels = [&](uint32_t value) {
        uint32_t mask = 0;
        const auto used = users.find(value);
        if (used == users.end()) return 0u;
        for (uint32_t at : used->second) {
            const Inst& i = body[at];
            if (i.op == CompositeExtract && i.n >= 4) { if (i.w[3] < 4) mask |= 1u << i.w[3]; }
            else if (i.op == VectorShuffle && i.n >= 4) {
                for (uint32_t k = 4; k < i.n; ++k) {
                    if (i.w[2] == value && i.w[k] < 4) mask |= 1u << i.w[k];
                    if (i.w[3] == value && i.w[k] >= 4 && i.w[k] < 8) mask |= 1u << (i.w[k] - 4);
                }
            } else return 4u;
        }
        uint32_t n = 0;
        for (uint32_t bit = 0; bit < 4; ++bit) n += mask >> bit & 1u;
        return n;
    };
    // The conditional branches that lead to a discard: one of whose targets is a block of OpKill alone.
    const auto kills = [&](uint32_t label) {
        const auto at = label_at.find(label);
        return at != label_at.end() && at->second + 1 < body.size() && body[at->second + 1].op == Kill;
    };
    std::vector<std::pair<uint32_t, uint32_t>> discards; // (index, condition)
    for (uint32_t at = 0; at < body.size(); ++at)
        if (body[at].op == BranchConditional && body[at].n >= 3 && (kills(body[at].w[1]) || kills(body[at].w[2]))) discards.emplace_back(at, body[at].w[0]);
    // Whether `target` is computed from `value`.
    const auto feeds = [&](uint32_t value, uint32_t target) {
        std::unordered_set<uint32_t> seen{value};
        std::vector<uint32_t> frontier{value};
        while (!frontier.empty()) {
            const uint32_t x = frontier.back(); frontier.pop_back();
            if (x == target) return true;
            const auto used = users.find(x);
            if (used == users.end()) continue;
            for (uint32_t at : used->second) {
                const Inst& i = body[at];
                if (has_result(i.op) && i.n >= 2 && seen.insert(i.w[1]).second) frontier.push_back(i.w[1]);
            }
            if (seen.size() > 65536) return true; // a value read everywhere: taken as read by the condition too
        }
        return false;
    };
    // The first instruction that computes with a value, past the shuffles that only pick its channels.
    const auto first_use = [&](uint32_t value) {
        uint32_t best = UINT32_MAX;
        std::unordered_set<uint32_t> seen{value};
        std::vector<uint32_t> frontier{value};
        while (!frontier.empty()) {
            const uint32_t x = frontier.back(); frontier.pop_back();
            const auto used = users.find(x);
            if (used == users.end()) continue;
            for (uint32_t at : used->second) {
                const Inst& i = body[at];
                if ((i.op == VectorShuffle || i.op == CompositeExtract || i.op == CompositeConstruct) && i.n >= 2) { if (seen.insert(i.w[1]).second) frontier.push_back(i.w[1]); }
                else best = std::min(best, at);
            }
        }
        return best;
    };
    struct Sample { uint32_t slot = 0, width = 0, at = 0, listed = 0; };
    std::vector<Sample> samples;
    for (uint32_t at = 0; at < body.size(); ++at) {
        const Inst& i = body[at];
        if (i.op < SampleFirst || i.op > SampleLast || i.n < 4) continue;
        const auto from = origin.find(i.w[2]);
        if (from == origin.end()) continue;
        const auto where = bound.find(from->second);
        if (where == bound.end()) continue;
        const efmi::gpu::SourceRegister* found = nullptr;
        for (const auto& r : registers)
            if (r.letter == 't' && r.kind == efmi::gpu::SourceRegister::ImageSrv && r.set == where->second.first && r.binding == where->second.second) found = &r;
        // A 2D texture below t24, as the patterns' `(texture2d)` and their slot range.
        if (!found || found->count != 1 || found->dim != 1 || found->arrayed || found->multisampled || found->index > 23 || !direct_input(i.w[3], 0)) continue;
        Sample s;
        s.slot = found->index; s.width = channels(i.w[1]); s.at = at; s.listed = at;
        const uint32_t use = first_use(i.w[1]);
        for (const auto& discard : discards)
            if (at < discard.first && discard.first < use && !feeds(i.w[1], discard.second)) s.listed = std::max(s.listed, discard.first);
        samples.push_back(s);
    }
    std::stable_sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.listed < b.listed; });
    out.known = true;
    out.narrow_first = !samples.empty() && samples.front().width <= 2;
    for (const auto& s : samples)
        if (s.width >= 3) { out.colour_slot = int32_t(s.slot); break; }
    return out;
}

bool copy_fragment(const std::vector<uint32_t>& code)
{
    Module m;
    if (!m.parse(code) || m.model != 4) return false;
    uint32_t input = 0, output = 0, images = 0, samplers = 0, blocks = 0;
    for (const auto& [id, var] : m.vars) {
        const uint32_t pointee = m.pointee(var.type);
        const bool builtin = m.builtin.count(id) != 0;
        if (var.storage == SInput && !builtin) {
            if (input || m.scalar_vector(pointee) != std::make_pair('f', 2u)) return false;
            input = id;
        } else if (var.storage == SOutput) {
            if (builtin || output || m.scalar_vector(pointee) != std::make_pair('f', 4u)) return false;
            const auto location = m.location.find(id);
            if (location == m.location.end() || location->second) return false;
            output = id;
        } else if (var.storage == SUniformConstant) {
            const auto* t = m.type(pointee);
            if (!t) return false;
            if ((*t)[0] == OpTypeImage && t->size() >= 3 && (*t)[2] == 1 && m.scalar_vector((*t)[1]).first == 'f') ++images;
            else if ((*t)[0] == OpTypeSampler) ++samplers;
            else return false;
        } else if (var.storage == SUniform && m.block.count(pointee)) ++blocks;
        else if (var.storage != SInput) return false;
    }
    if (!input || !output || images != 1 || samplers != 1 || blocks > 1) return false;
    // The body: loads, one sampled image, one explicit-LOD sample of the loaded UV, stored as it is.
    std::unordered_map<uint32_t, uint32_t> loaded; // load result -> variable
    uint32_t sample = 0, samples = 0, stores = 0;
    for (const auto& i : m.body) {
        switch (i[0]) {
        case OpFunction: case OpFunctionEnd: case OpLabel: case OpReturn: case OpAccessChain: case OpInBoundsAccessChain: case OpSampledImage: break;
        case OpLoad: if (i.size() >= 4) loaded[i[2]] = i[3]; break;
        case OpImageSampleExplicitLod:
            // result type, result, sampled image, coordinate, image operands (Lod), lod
            if (i.size() < 7 || !(i[5] & 0x2) || loaded.count(i[4]) == 0 || loaded[i[4]] != input) return false;
            sample = i[2]; ++samples; break;
        case OpStore:
            if (i.size() < 3 || i[1] != output || i[2] != sample) return false;
            ++stores; break;
        default: return false;
        }
    }
    return samples == 1 && stores == 1;
}

} // namespace efvk
