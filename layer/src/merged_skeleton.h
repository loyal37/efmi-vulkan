// CPU implementation of EFMI's rolling-buffer skeleton import. No GPU or live hooks here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace efvk {

using InstanceEntry = std::array<uint8_t, 256>;
struct BoneWindow {
    // Begins at the source offset in constant 5, including the leading implicit matrix.
    const uint8_t* data = nullptr;
    size_t size = 0;
};
struct BoneComponent { uint32_t first = 0, count = 0; };
enum class BoneImportResult { Rejected, Imported, AlreadyCurrent };

class MergedSkeleton {
public:
    static constexpr uint32_t kImplicitBones = 512;
    static constexpr uint32_t kRowsPerBone = 3;
    static constexpr uint32_t kStaticRows = kImplicitBones * kRowsPerBone;
    static constexpr size_t kBytesPerBone = 48;
    static constexpr size_t kMaxBytes = 64u << 20;

    // Components are ranges of [0,bones) in the order of their first bones. Two may share bones, as the
    // Core's pools allow (BUG-093): each one's import writes its own range, and the later one stays. Bones
    // between them keep their initial value (zero, the implicit ones identity), as the Core's merged
    // skeleton leaves them. Bounds and allocation are checked before replacing state.
    bool configure(uint32_t bones, uint32_t instances, const std::vector<BoneComponent>&,
                   std::string& reason);
    // A caller-owned logical update serial, not necessarily a present counter. Increasing it
    // permits new imports while retaining the previous master for components not seen yet.
    bool begin_update(uint64_t serial, std::string& reason);
    // source remap maps each destination component bone to a source bone. Empty means identity.
    // Both poses and all float values are validated before any byte or update stamp changes.
    // Repeated components import once per update, except that a lower LOD number replaces them.
    BoneImportResult import_component(uint32_t instance, uint32_t component, uint32_t lod,
                                      const InstanceEntry&, BoneWindow current, BoneWindow previous,
                                      const std::vector<uint16_t>& remap, float scale,
                                      std::string& reason);
    // Copies all 256 bytes, replacing only nonstatic x/y offsets of constant 5. The new offsets are
    // one matrix below the instance's first merged bone, as the vertex shader expects.
    bool patch_entry(uint32_t instance, const InstanceEntry&, InstanceEntry& output,
                     std::string& reason) const;
    const std::vector<uint8_t>& bytes() const { return master_; }
    uint32_t bones() const { return bones_; }
    uint32_t instances() const { return instances_; }
    uint32_t rows_for_instance(uint32_t instance) const;
    bool updated(uint32_t instance, uint32_t component) const;
    // False when import_component would answer AlreadyCurrent: the caller need not read the source.
    bool needs_import(uint32_t instance, uint32_t component, uint32_t lod) const;
    // The stamp of an import done elsewhere (the device's merge, D-117): false when it was current.
    bool note_imported(uint32_t instance, uint32_t component, uint32_t lod);

private:
    struct Stamp { uint64_t serial = 0; uint32_t lod = 0; bool valid = false; };
    uint32_t bones_ = 0, instances_ = 0;
    uint64_t serial_ = 0;
    bool begun_ = false;
    std::vector<BoneComponent> components_;
    std::vector<Stamp> stamps_;
    std::vector<uint8_t> master_;
    // One component's two poses, reused across imports; never published before full validation.
    std::vector<uint8_t> pending_;
};

} // namespace efvk
