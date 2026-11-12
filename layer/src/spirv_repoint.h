// The narrow vertex-buffer rewrite decided in D-022, not a general SPIR-V optimizer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace efvk {

struct VertexRepoint {
    uint32_t bone_set = 0, bone_binding = 0;
    uint32_t own_set = 0;
    // 3 or 4 when the bones are in a uniform block of 4096 vectors, the original Core's vs-cb3 / vs-cb4 bone
    // source (D-186): the rewrite made that block a read-only storage block. 0: the bone storage buffer.
    uint32_t constant_slot = 0;
    std::vector<uint32_t> words;
    std::string reason;
};

// Find exactly one storage block and the uniform block at (2, 0). Change only their
// Binding/DescriptorSet literals to (own_set, 0/1), preserving all fragment code.
// Without a storage block, the bones are the uniform block of 4096 vectors at register b4, else b3, as the
// original Core's MergedSkeleton_DetectBoneDataSource takes them (D-186); that block's Block decoration becomes
// BufferBlock and its members NonWritable, so that it is read at (own_set, 0) as the bone storage buffer is.
bool repoint_vertex(const std::vector<uint32_t>& code, uint32_t own_set, VertexRepoint& out);

} // namespace efvk
