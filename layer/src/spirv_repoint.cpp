#include "spirv_repoint.h"

#include <functional>
#include <map>
#include <set>

namespace efvk {

bool repoint_vertex(const std::vector<uint32_t>& code, uint32_t own_set, VertexRepoint& out)
{
    out = VertexRepoint(); out.own_set = own_set;
    auto fail = [&](const char* why) { out.reason = why; return false; };
    if (code.size() < 5 || code[0] != 0x07230203 || code[1] > 0x00010600 || code[4] != 0)
        return fail("spirv_header");
    struct Decor {
        size_t set_word = 0, binding_word = 0;
        size_t block_word = 0, block_end = 0; // the Block decoration's kind word and the word after its instruction
        bool block = false, buffer_block = false;
        uint32_t stride = 0;                  // ArrayStride
    };
    struct Pointer { uint32_t storage, pointee; };
    struct Variable { uint32_t pointer, storage; };
    struct Type { uint32_t op = 0; std::vector<uint32_t> operands; };
    std::map<uint32_t, Decor> decorations;
    std::map<uint32_t, Pointer> pointers;
    std::map<uint32_t, Variable> variables;
    std::vector<uint32_t> order;                                 // the variables in module order
    std::map<uint32_t, Type> types;                              // numeric, vector, matrix, array and structure types
    std::map<uint32_t, uint32_t> constants;                      // 32-bit constants
    std::map<uint32_t, std::map<uint32_t, uint32_t>> offsets;    // structure member offsets
    std::map<uint32_t, uint32_t> named;                          // how often a type is named by another type or a pointer
    bool vertex_entry = false;
    for (size_t i = 5; i < code.size();) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (n == 0 || n > code.size() - i) return fail("spirv_instruction_bounds");
        if (op == 15) { // OpEntryPoint; this helper must never be used on a fragment or mixed module.
            if (n < 4 || code[i + 1] != 0) return fail("spirv_not_vertex");
            vertex_entry = true;
        } else if (op == 71) { // OpDecorate
            if (n < 3 || code[i + 1] >= code[3]) return fail("spirv_decoration");
            auto& d = decorations[code[i + 1]];
            uint32_t kind = code[i + 2];
            if (kind == 33 || kind == 34) {
                if (n != 4) return fail("spirv_descriptor_decoration");
                size_t& word = kind == 33 ? d.binding_word : d.set_word;
                if (word) return fail("spirv_duplicate_decoration");
                word = i + 3;
            } else if (kind == 2) { d.block = true; d.block_word = i + 2; d.block_end = i + n; }
            else if (kind == 3) d.buffer_block = true;
            else if (kind == 6 && n == 4) d.stride = code[i + 3];
        } else if (op == 72) { // OpMemberDecorate
            if (n == 5 && code[i + 3] == 35) offsets[code[i + 1]][code[i + 2]] = code[i + 4];
        } else if (op == 73 || op == 74 || op == 75 || op == 332) {
            // Group/ID decorations need full expansion to prove there are no hidden aliases.
            return fail("spirv_indirect_decoration");
        } else if (op == 32) { // OpTypePointer
            if (n != 4 || code[i + 1] >= code[3]) return fail("spirv_pointer");
            pointers[code[i + 1]] = {code[i + 2], code[i + 3]};
            ++named[code[i + 3]];
        } else if (op == 59) { // OpVariable
            if (n < 4 || code[i + 2] >= code[3]) return fail("spirv_variable");
            variables[code[i + 2]] = {code[i + 1], code[i + 3]};
            order.push_back(code[i + 2]);
        } else if (op == 43) { // OpConstant
            if (n == 4) constants[code[i + 2]] = code[i + 3];
        } else if (op >= 21 && op <= 30 && op != 25 && op != 26 && op != 27 && n >= 2) {
            // OpTypeInt, Float, Vector, Matrix, Array, RuntimeArray, Struct: what the bone block is made of.
            Type& t = types[code[i + 1]];
            t.op = op; t.operands.assign(code.begin() + i + 2, code.begin() + i + n);
            if (op == 23 || op == 24 || op == 28 || op == 29) ++named[t.operands[0]];
            if (op == 30) for (uint32_t member : t.operands) ++named[member];
        } else if (op == 33) { // OpTypeFunction: a structure passed or returned by value is named there
            for (size_t k = i + 2; k < i + n; ++k) ++named[code[k]];
        }
        i += n;
    }
    if (!vertex_entry) return fail("spirv_vertex_entry_missing");
    uint32_t bone_id = 0, instance_id = 0;
    for (const auto& item : variables) {
        uint32_t id = item.first;
        const auto& v = item.second;
        auto dit = decorations.find(id);
        if (dit == decorations.end() || !dit->second.set_word || !dit->second.binding_word) continue;
        const auto& d = dit->second;
        const uint32_t set = code[d.set_word], binding = code[d.binding_word];
        if (set == own_set && binding <= 1) return fail("spirv_target_collision");
        auto p = pointers.find(v.pointer);
        if (p == pointers.end() || p->second.storage != v.storage) return fail("spirv_variable_pointer");
        auto type = decorations.find(p->second.pointee);
        const bool storage = v.storage == 12 || (v.storage == 2 && type != decorations.end() && type->second.buffer_block);
        const bool uniform = v.storage == 2 && type != decorations.end() && type->second.block;
        if (storage) {
            if (bone_id) return fail("spirv_multiple_storage_buffers");
            bone_id = id; out.bone_set = set; out.bone_binding = binding;
        }
        if (set == 2 && binding == 0) {
            if (!uniform || instance_id) return fail("spirv_instance_ambiguous");
            instance_id = id;
        }
    }
    // No bone storage buffer: the bones may be in a uniform block of 4096 vectors, as in the game's dash after-images
    // (LIMIT-027, D-186). The original Core's MergedSkeleton_DetectBoneDataSource takes, after the rolling buffer, the
    // block at vs-cb4 whose declared size is 4096 vectors, then the one at vs-cb3 ("Sometimes used for FX shaders. Both
    // bone data offsets and bone data entries are supplied via vs-cb4"). The registers are numbered as D-076 does
    // (frontend/gpu/source_registers.cpp): a constant buffer is a variable with a binding in the Uniform storage class
    // whose block is not a BufferBlock, numbered in module order, an array taking one register per element.
    uint32_t block_struct = 0;
    if (!bone_id && instance_id) {
        std::function<uint64_t(uint32_t, unsigned)> size_of = [&](uint32_t id, unsigned depth) -> uint64_t {
            const auto t = types.find(id);
            if (t == types.end() || depth > 16) return 0;
            const auto& a = t->second.operands;
            switch (t->second.op) {
            case 21: case 22: return a.empty() ? 0 : a[0] / 8;
            case 23: case 24: return a.size() < 2 ? 0 : a[1] * size_of(a[0], depth + 1);
            case 28: {
                const auto length = a.size() < 2 ? constants.end() : constants.find(a[1]);
                if (length == constants.end()) return 0;
                const auto d = decorations.find(id);
                return uint64_t(length->second) * (d != decorations.end() && d->second.stride ? d->second.stride : size_of(a[0], depth + 1));
            }
            case 30: {
                const auto o = offsets.find(id);
                if (a.empty() || o == offsets.end() || o->second.empty()) return 0;
                uint32_t last = 0, at = 0;
                for (const auto& m : o->second) if (m.second >= at) { at = m.second; last = m.first; }
                return last < a.size() ? at + size_of(a[last], depth + 1) : 0;
            }
            default: return 0;
            }
        };
        uint32_t next = 0, at_register[5] = {};
        for (uint32_t id : order) {
            const auto d = decorations.find(id);
            if (d == decorations.end() || !d->second.binding_word) continue;
            const auto& v = variables[id];
            const auto p = pointers.find(v.pointer);
            if (p == pointers.end()) continue;
            uint32_t type = p->second.pointee, count = 1;
            auto t = types.find(type);
            if (t != types.end() && t->second.op == 29) return fail("spirv_constant_bones_registers");
            if (t != types.end() && t->second.op == 28) {
                const auto length = constants.find(t->second.operands.size() > 1 ? t->second.operands[1] : 0);
                if (length == constants.end() || !length->second || length->second > 4096) return fail("spirv_constant_bones_registers");
                count = length->second; type = t->second.operands[0]; t = types.find(type);
            }
            if (v.storage != 2 || t == types.end() || t->second.op != 30) continue;
            const auto td = decorations.find(type);
            if (td != decorations.end() && td->second.buffer_block) continue;
            if (count == 1 && next <= 4) at_register[next] = id;
            next += count;
        }
        for (uint32_t slot : {4u, 3u}) {
            const uint32_t id = at_register[slot];
            if (!id || size_of(pointers[variables[id].pointer].pointee, 0) != 4096 * 16) continue;
            // Moved to the private set, so it must name its set (the instance window's search above needs both too).
            if (!decorations[id].set_word) return fail("spirv_constant_bones_set");
            bone_id = id; out.constant_slot = slot;
            out.bone_set = code[decorations[id].set_word]; out.bone_binding = code[decorations[id].binding_word];
            break;
        }
        if (bone_id) {
            // Read as a storage buffer, the block must lie as a uniform block: vectors of four 32-bit values and arrays
            // of them with a stride of 16, one after another, which std140 and std430 lay out alike.
            const uint32_t s = pointers[variables[bone_id].pointer].pointee;
            const auto& members = types[s].operands;
            uint32_t rows = 0;
            auto vec4 = [&](uint32_t id) {
                const auto v = types.find(id);
                if (v == types.end() || v->second.op != 23 || v->second.operands.size() != 2 || v->second.operands[1] != 4) return false;
                const auto e = types.find(v->second.operands[0]);
                return e != types.end() && (e->second.op == 21 || e->second.op == 22) && !e->second.operands.empty() && e->second.operands[0] == 32;
            };
            const auto member_offsets = offsets.find(s);
            bool laid = member_offsets != offsets.end() && member_offsets->second.size() == members.size();
            for (uint32_t k = 0; laid && k < members.size(); ++k) {
                const auto at = member_offsets->second.find(k);
                if (at == member_offsets->second.end() || at->second != rows * 16) { laid = false; break; }
                const auto m = types.find(members[k]);
                if (vec4(members[k])) { ++rows; continue; }
                const auto length = m != types.end() && m->second.op == 28 && m->second.operands.size() == 2 ? constants.find(m->second.operands[1]) : constants.end();
                if (length == constants.end() || !vec4(m->second.operands[0]) || decorations[members[k]].stride != 16) { laid = false; break; }
                rows += length->second;
            }
            if (!laid || rows != 4096) return fail("spirv_constant_bones_layout");
            // BufferBlock is SPIR-V 1.0 to 1.3's storage buffer; the game's after-image modules are 1.0.
            if (code[1] > 0x00010300) return fail("spirv_constant_bones_version");
            const auto& sd = decorations[s];
            if (!sd.block || !sd.block_word || sd.buffer_block) return fail("spirv_constant_bones_block");
            // Its block type and pointer belong to this variable alone, since the rewrite changes the type.
            uint32_t users = 0;
            for (const auto& v : variables) users += v.second.pointer == variables[bone_id].pointer;
            if (named[s] != 1 || users != 1) return fail("spirv_constant_bones_shared");
            block_struct = s;
        }
    }
    if (!bone_id || !instance_id || bone_id == instance_id) return fail("spirv_required_buffer_missing");
    // A second variable at a source binding would make moving just one declaration ambiguous.
    for (const auto& item : variables) {
        auto it = decorations.find(item.first);
        if (it == decorations.end() || !it->second.set_word || !it->second.binding_word) continue;
        uint32_t s = code[it->second.set_word], b = code[it->second.binding_word];
        if ((s == out.bone_set && b == out.bone_binding && item.first != bone_id) ||
            (s == 2 && b == 0 && item.first != instance_id)) return fail("spirv_source_alias");
    }
    out.words = code;
    out.words[decorations[bone_id].set_word] = own_set;
    out.words[decorations[bone_id].binding_word] = 0;
    out.words[decorations[instance_id].set_word] = own_set;
    out.words[decorations[instance_id].binding_word] = 1;
    if (block_struct) {
        // The uniform block becomes a read-only storage block of the same layout (D-186): its Block decoration a
        // BufferBlock, each member NonWritable (a vertex stage may not write a storage buffer without
        // vertexPipelineStoresAndAtomics), inserted right after that decoration.
        const auto& sd = decorations[block_struct];
        out.words[sd.block_word] = 3;
        std::vector<uint32_t> readonly;
        for (uint32_t k = 0; k < types[block_struct].operands.size(); ++k)
            readonly.insert(readonly.end(), {(4u << 16) | 72u, block_struct, k, 24u});
        out.words.insert(out.words.begin() + sd.block_end, readonly.begin(), readonly.end());
    }
    return true;
}

} // namespace efvk
