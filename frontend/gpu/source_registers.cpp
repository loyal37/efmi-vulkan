#include "source_registers.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace efmi::gpu {
namespace {
constexpr uint32_t kMagic = 0x07230203, kMaxWords = 4u << 20;
enum : uint32_t {
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
    OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29,
    OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpDecorate = 71, OpMemberDecorate = 72,
    DecorationBlock = 2, DecorationBufferBlock = 3, DecorationArrayStride = 6, DecorationMatrixStride = 7, DecorationNonWritable = 24,
    DecorationBinding = 33, DecorationDescriptorSet = 34, DecorationOffset = 35,
    StorageUniformConstant = 0, StorageUniform = 2, StorageStorageBuffer = 12,
};
struct Type { uint32_t op = 0; std::vector<uint32_t> operands; };
} // namespace

bool source_registers(const std::vector<uint32_t>& code, std::vector<SourceRegister>& out, std::string& reason)
{
    reason.clear();
    if (code.size() < 5 || code.size() > kMaxWords || code[0] != kMagic) { reason = "not a SPIR-V module"; return false; }
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, uint32_t> constants, bindings, sets;
    std::set<uint32_t> nonwritable, buffer_blocks;
    std::unordered_map<uint32_t, std::set<uint32_t>> readonly_members;
    std::unordered_map<uint32_t, uint32_t> array_strides;
    std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> member_offsets;
    struct Variable { uint32_t id, pointer, storage; };
    std::vector<Variable> variables;
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xFFFF, count = code[i] >> 16;
        if (!count || i + count > code.size()) { reason = "malformed SPIR-V instruction"; return false; }
        const uint32_t* o = code.data() + i + 1;
        const uint32_t n = count - 1;
        if (op == OpDecorate && n >= 2) {
            if (o[1] == DecorationBinding && n >= 3) bindings[o[0]] = o[2];
            else if (o[1] == DecorationDescriptorSet && n >= 3) sets[o[0]] = o[2];
            else if (o[1] == DecorationNonWritable) nonwritable.insert(o[0]);
            else if (o[1] == DecorationBufferBlock) buffer_blocks.insert(o[0]);
            else if (o[1] == DecorationArrayStride && n >= 3) array_strides[o[0]] = o[2];
        } else if (op == OpMemberDecorate && n >= 3 && o[2] == DecorationNonWritable) {
            readonly_members[o[0]].insert(o[1]);
        } else if (op == OpMemberDecorate && n >= 4 && o[2] == DecorationOffset) {
            member_offsets[o[0]][o[1]] = o[3];
        } else if (op == OpConstant && n >= 3) {
            constants[o[1]] = o[2];
        } else if ((op == OpTypeImage || op == OpTypeSampler || op == OpTypeSampledImage || op == OpTypeArray ||
                    op == OpTypeRuntimeArray || op == OpTypeStruct || op == OpTypePointer || op == OpTypeInt ||
                    op == OpTypeFloat || op == OpTypeVector || op == OpTypeMatrix) && n >= 1) {
            types[o[0]] = {op, std::vector<uint32_t>(o + 1, o + n)};
        } else if (op == OpVariable && n >= 3) {
            variables.push_back({o[1], o[0], o[2]});
        }
        i += count;
    }
    // Sizes as the block layout states them: scalars by width, vectors and matrices by their parts,
    // arrays by their stride, structures by the last member's offset plus its size.
    std::function<uint32_t(uint32_t, unsigned)> size_of = [&](uint32_t id, unsigned depth) -> uint32_t {
        const auto t = types.find(id);
        if (t == types.end() || depth > 16) return 0;
        const auto& a = t->second.operands;
        switch (t->second.op) {
        case OpTypeInt: case OpTypeFloat: return a.empty() ? 0 : a[0] / 8;
        case OpTypeVector: case OpTypeMatrix: return a.size() < 2 ? 0 : a[1] * size_of(a[0], depth + 1);
        case OpTypeArray: {
            if (a.size() < 2) return 0;
            const auto length = constants.find(a[1]);
            const auto stride = array_strides.find(id);
            return length == constants.end() ? 0 : length->second * (stride != array_strides.end() ? stride->second : size_of(a[0], depth + 1));
        }
        case OpTypeStruct: {
            const auto offsets = member_offsets.find(id);
            if (a.empty()) return 0;
            if (offsets == member_offsets.end() || offsets->second.empty()) {
                uint32_t total = 0;
                for (uint32_t member : a) total += size_of(member, depth + 1);
                return total;
            }
            const auto last = std::max_element(offsets->second.begin(), offsets->second.end(),
                                               [](const auto& x, const auto& y) { return x.second < y.second; });
            return last->first < a.size() ? last->second + size_of(a[last->first], depth + 1) : 0;
        }
        default: return 0;
        }
    };
    std::vector<SourceRegister> found;
    for (const auto& v : variables) {
        const auto binding = bindings.find(v.id);
        if (binding == bindings.end()) continue;
        const auto pointer = types.find(v.pointer);
        if (pointer == types.end() || pointer->second.op != OpTypePointer || pointer->second.operands.size() < 2) {
            reason = "resource variable without a pointer type"; return false;
        }
        uint32_t type = pointer->second.operands[1], elements = 1;
        auto t = types.find(type);
        if (t != types.end() && t->second.op == OpTypeRuntimeArray) { reason = "runtime-sized resource array"; return false; }
        if (t != types.end() && t->second.op == OpTypeArray && t->second.operands.size() >= 2) {
            const auto length = constants.find(t->second.operands[1]);
            if (length == constants.end() || !length->second || length->second > 4096) { reason = "resource array of unknown length"; return false; }
            elements = length->second;
            type = t->second.operands[0];
            t = types.find(type);
        }
        if (t == types.end()) { reason = "resource of unknown type"; return false; }
        SourceRegister r;
        r.set = sets.count(v.id) ? sets.at(v.id) : 0;
        r.binding = binding->second;
        r.count = elements;
        const auto& a = t->second.operands;
        if (t->second.op == OpTypeImage || t->second.op == OpTypeSampledImage) {
            const Type* image = &t->second;
            if (t->second.op == OpTypeSampledImage) {
                const auto inner = a.empty() ? types.end() : types.find(a[0]);
                if (inner == types.end() || inner->second.op != OpTypeImage) { reason = "sampled image without an image type"; return false; }
                image = &inner->second;
            }
            if (image->operands.size() < 6) { reason = "malformed image type"; return false; }
            // OpTypeImage: sampled type, Dim, Depth, Arrayed, MS, Sampled (2: storage).
            r.dim = image->operands[1];
            r.arrayed = image->operands[3] != 0;
            r.multisampled = image->operands[4] != 0;
            const bool storage = image->operands[5] == 2;
            r.kind = r.dim == 5 ? (storage ? SourceRegister::TexelUav : SourceRegister::TexelSrv)
                                : (storage ? SourceRegister::ImageUav : SourceRegister::ImageSrv);
        } else if (t->second.op == OpTypeSampler) {
            r.kind = SourceRegister::Sampler;
        } else if (t->second.op == OpTypeStruct) {
            if (v.storage == StorageStorageBuffer || buffer_blocks.count(type)) {
                const auto members = readonly_members.find(type);
                const bool readonly = nonwritable.count(v.id) || nonwritable.count(type) ||
                                      (members != readonly_members.end() && members->second.size() == a.size());
                r.kind = readonly ? SourceRegister::BufferSrv : SourceRegister::BufferUav;
                if (!a.empty()) {
                    const auto last = types.find(a.back());
                    if (last != types.end() && (last->second.op == OpTypeRuntimeArray || last->second.op == OpTypeArray)) {
                        const auto stride = array_strides.find(a.back());
                        if (stride != array_strides.end()) r.stride = stride->second;
                        const auto element = last->second.operands.empty() ? types.end() : types.find(last->second.operands[0]);
                        // DXC compiles a byte address buffer as an array of uint, as a structured buffer of uint.
                        r.words = element != types.end() && element->second.op == OpTypeInt && element->second.operands.size() >= 2 &&
                                  element->second.operands[0] == 32 && element->second.operands[1] == 0;
                    }
                }
            } else if (v.storage == StorageUniform) {
                r.kind = SourceRegister::ConstantBuffer;
                r.bytes = size_of(type, 0);
            } else { reason = "block in an unknown storage class"; return false; }
        } else { reason = "resource of unknown type"; return false; }
        found.push_back(r);
    }
    // Numbering: t for shader resource views (buffers first), then b, s, u, each in module order.
    std::vector<SourceRegister> result;
    auto number = [&](char letter, auto&& wanted) {
        uint32_t next = 0;
        for (auto r : found) {
            if (!wanted(r.kind)) continue;
            r.letter = letter; r.index = next; next += r.count;
            result.push_back(r);
        }
    };
    uint32_t t_next = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (auto r : found) {
            const bool buffer = r.kind == SourceRegister::BufferSrv || r.kind == SourceRegister::TexelSrv;
            if (r.kind != SourceRegister::ImageSrv && !buffer) continue;
            if (buffer != (pass == 0)) continue;
            r.letter = 't'; r.index = t_next; t_next += r.count;
            result.push_back(r);
        }
    number('b', [](SourceRegister::Kind k) { return k == SourceRegister::ConstantBuffer; });
    number('s', [](SourceRegister::Kind k) { return k == SourceRegister::Sampler; });
    number('u', [](SourceRegister::Kind k) { return k == SourceRegister::ImageUav || k == SourceRegister::BufferUav || k == SourceRegister::TexelUav; });
    out = std::move(result);
    return true;
}

const SourceRegister* find_source_register(const std::vector<SourceRegister>& registers, char letter, uint32_t index)
{
    for (const auto& r : registers)
        if (r.letter == letter && index >= r.index && index - r.index < r.count) return &r;
    return nullptr;
}

bool source_srv_declaration(const std::vector<SourceRegister>& registers, uint32_t slot, SourceSrvDeclaration& out, std::string& reason)
{
    const auto* r = find_source_register(registers, 't', slot);
    if (!r) {
        // A slot the draw's shader has no register for. The original answers a slot its DXBC does not declare
        // with type NONE (srv_stride and srv_dimension then read -1). Below t60 the Vulkan build lacks a
        // register exactly where the DX11 build declares none: the paired catalogue's 180 shader pairs differ
        // in one register (a pixel t3 of two shaders, scripts/check_source_declarations.py). From t60 up the
        // DX11 pixel shaders declare registers by explicit assignment that the Vulkan builds do not have
        // (551 registers in 84 pairs): not decided there. A library that asks every slot of a range
        // (EFMITTL's `ps->srv_type(8)` ... `(21)`) refused every draw it ran on while this was not answered.
        if (slot < kSourceExplicitRegisters) { out = {}; return true; }
        reason = "no t register in the SPIR-V at t60 or above (the DX11 builds declare more there)"; return false;
    }
    out = {};
    if (r->kind == SourceRegister::TexelSrv) { out.type = 1; out.dimension = 1; return true; }
    if (r->kind == SourceRegister::BufferSrv) {
        if (r->words && r->stride) { out.words = true; out.type = 2; out.dimension = 1; out.stride = r->stride; return true; }
        if (!r->stride) { reason = "a buffer whose element stride the SPIR-V does not give"; return false; }
        out.type = 2; out.dimension = 1; out.stride = r->stride; return true;
    }
    // SPIR-V Dim 0 1D, 1 2D, 2 3D, 3 cube; arrayed and multisampled as DX11's view dimensions.
    static const struct { uint32_t dim; bool arrayed, multisampled; uint32_t dimension; } views[] = {
        {0, false, false, 2}, {0, true, false, 3}, {1, false, false, 4}, {1, true, false, 5}, {1, false, true, 6}, {1, true, true, 7},
        {2, false, false, 8}, {3, false, false, 9}, {3, true, false, 10}};
    for (const auto& v : views)
        if (r->kind == SourceRegister::ImageSrv && v.dim == r->dim && v.arrayed == r->arrayed && v.multisampled == r->multisampled) {
            out.type = 1; out.dimension = v.dimension; return true;
        }
    reason = "an image of a kind DX11 has no view for"; return false;
}

CoreSourceFacts core_source_facts(const std::vector<SourceRegister>& registers)
{
    CoreSourceFacts facts;
    for (uint32_t slot : {2u, 1u, 3u}) {
        const auto* cb = find_source_register(registers, 'b', slot);
        if (cb && cb->kind == SourceRegister::ConstantBuffer && cb->count == 1 && cb->bytes == 4096 * 16) {
            facts.vs_config_slot = int32_t(slot); facts.config = cb; break;
        }
    }
    const auto* t0 = find_source_register(registers, 't', 0);
    if (t0 && t0->kind == SourceRegister::BufferSrv && t0->count == 1 && t0->stride == 16) facts.bones = t0;
    facts.rolling_bones = facts.bones && facts.vs_config_slot != -1;
    if (!facts.rolling_bones)
        for (uint32_t slot : {4u, 3u}) {
            const auto* cb = find_source_register(registers, 'b', slot);
            if (cb && cb->kind == SourceRegister::ConstantBuffer && cb->count == 1 && cb->bytes == 4096 * 16) {
                facts.constant_bones = cb; facts.constant_slot = slot; break;
            }
        }
    return facts;
}

} // namespace efmi::gpu
