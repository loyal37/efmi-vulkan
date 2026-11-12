#include "merged_skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <emmintrin.h>

namespace efvk {
namespace {
uint32_t word(const InstanceEntry& entry, size_t offset)
{
    uint32_t value;
    std::memcpy(&value, entry.data() + offset, sizeof(value));
    return value;
}
bool reject(std::string& reason, const char* text) { reason = text; return false; }
}

bool MergedSkeleton::configure(uint32_t bones, uint32_t instances,
                               const std::vector<BoneComponent>& components, std::string& reason)
{
    reason.clear();
    if (!bones || bones > 65536 || !instances || components.empty() || components.size() > bones)
        return reject(reason, "invalid skeleton dimensions");
    const uint64_t total = (uint64_t(kImplicitBones) + 2ull * bones * instances) * kBytesPerBone;
    if (total > kMaxBytes) return reject(reason, "skeleton exceeds byte budget");
    uint64_t cursor = 0;
    uint32_t largest = 0;
    for (const auto& c : components) {
        if (!c.count || c.first < cursor || uint64_t(c.first) + c.count > bones)
            return reject(reason, "components must be ranges of the skeleton in the order of their first bones");
        cursor = c.first;
        largest = std::max(largest, c.count);
    }
    try {
        auto new_components = components;
        std::vector<Stamp> new_stamps(size_t(instances) * components.size());
        std::vector<uint8_t> new_master(static_cast<size_t>(total), 0);
        std::vector<uint8_t> new_pending(size_t(largest) * kBytesPerBone * 2);
        const float identity[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
        for (uint32_t i = 0; i < kImplicitBones; ++i)
            std::memcpy(new_master.data() + i * kBytesPerBone, identity, kBytesPerBone);
        components_.swap(new_components); stamps_.swap(new_stamps); master_.swap(new_master);
        pending_.swap(new_pending);
    } catch (const std::bad_alloc&) { return reject(reason, "skeleton allocation failed"); }
    bones_ = bones; instances_ = instances; serial_ = 0; begun_ = false;
    return true;
}

bool MergedSkeleton::begin_update(uint64_t serial, std::string& reason)
{
    reason.clear();
    if (!bones_) return reject(reason, "skeleton is not configured");
    if (begun_ && serial < serial_) return reject(reason, "update serial went backwards");
    serial_ = serial; begun_ = true;
    return true;
}

uint32_t MergedSkeleton::rows_for_instance(uint32_t instance) const
{
    if (instance >= instances_) return 0;
    return kStaticRows + 2 * bones_ * kRowsPerBone * instance;
}

bool MergedSkeleton::updated(uint32_t instance, uint32_t component) const
{
    if (!begun_ || instance >= instances_ || component >= components_.size()) return false;
    const auto& stamp = stamps_[size_t(instance) * components_.size() + component];
    return stamp.valid && stamp.serial == serial_;
}

bool MergedSkeleton::needs_import(uint32_t instance, uint32_t component, uint32_t lod) const
{
    if (!begun_ || instance >= instances_ || component >= components_.size()) return true;
    const auto& stamp = stamps_[size_t(instance) * components_.size() + component];
    return !(stamp.valid && stamp.serial == serial_ && lod >= stamp.lod);
}

bool MergedSkeleton::note_imported(uint32_t instance, uint32_t component, uint32_t lod)
{
    if (!begun_ || instance >= instances_ || component >= components_.size() || lod >= 4) return false;
    auto& stamp = stamps_[size_t(instance) * components_.size() + component];
    if (stamp.valid && stamp.serial == serial_ && lod >= stamp.lod) return false;
    stamp = {serial_, lod, true};
    return true;
}

BoneImportResult MergedSkeleton::import_component(uint32_t instance, uint32_t component, uint32_t lod,
    const InstanceEntry& entry, BoneWindow current, BoneWindow previous,
    const std::vector<uint16_t>& remap, float scale, std::string& reason)
{
    const uint8_t poses = (word(entry, 80) >= kStaticRows ? 1 : 0) | (word(entry, 84) >= kStaticRows ? 2 : 0);
    return import_poses(instance, component, lod, poses, current, previous, remap, scale, reason);
}

BoneImportResult MergedSkeleton::import_poses(uint32_t instance, uint32_t component, uint32_t lod, uint8_t poses,
    BoneWindow current, BoneWindow previous, const std::vector<uint16_t>& remap, float scale, std::string& reason)
{
    reason.clear();
    auto fail = [&](const char* text) { reason = text; return BoneImportResult::Rejected; };
    if (!begun_ || instance >= instances_ || component >= components_.size())
        return fail("invalid instance/component or update not begun");
    if (lod >= 4) return fail("unsupported LOD level");
    auto& stamp = stamps_[size_t(instance) * components_.size() + component];
    if (stamp.valid && stamp.serial == serial_ && lod >= stamp.lod)
        return BoneImportResult::AlreadyCurrent;
    const auto& c = components_[component];
    if (!remap.empty() && remap.size() != c.count) return fail("remap length differs from component");
    if (!std::isfinite(scale)) return fail("nonfinite mesh scale");
    uint32_t last = c.count - 1;
    if (!remap.empty()) last = *std::max_element(remap.begin(), remap.end());
    // Core skips one matrix before source bone zero, then copies three float4 rows per bone.
    const size_t needed = (size_t(last) + 2) * kBytesPerBone;
    const bool dynamic[2] = {(poses & 1) != 0, (poses & 2) != 0};
    const BoneWindow sources[2] = {current, previous};
    for (uint32_t pose = 0; pose < 2; ++pose)
        if (dynamic[pose] && (!sources[pose].data || sources[pose].size < needed))
            return fail("source bone window is incomplete");
    // Staging also makes aliasing safe and prevents a failed previous pose from partially
    // changing the current one. Only exact mapped bone rows are read, never a full ring buffer.
    const __m128 scale4 = _mm_set1_ps(scale);
    const __m128i exponent = _mm_set1_epi32(0x7f800000);
    for (uint32_t pose = 0; pose < 2; ++pose) {
        if (!dynamic[pose]) continue;
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t source = remap.empty() ? i : remap[i];
            const auto* src = sources[pose].data + (size_t(source) + 1) * kBytesPerBone;
            auto* dst = pending_.data() + (size_t(pose) * c.count + i) * kBytesPerBone;
            for (uint32_t row = 0; row < kRowsPerBone; ++row) {
                // memcpy permits unaligned windows without aliasing assumptions. Packed multiply
                // preserves scalar rounding/FTZ, even for scale 1; NaN/Inf reject the entire import.
                __m128 value; std::memcpy(&value, src + row * 16, 16);
                value = _mm_mul_ps(value, scale4);
                const auto bits = _mm_and_si128(_mm_castps_si128(value), exponent);
                if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(bits, exponent))))
                    return fail("nonfinite bone data after scaling");
                std::memcpy(dst + row * 16, &value, 16);
            }
        }
    }
    const size_t base = size_t(rows_for_instance(instance)) * 16;
    for (uint32_t pose = 0; pose < 2; ++pose)
        if (dynamic[pose])
            std::memcpy(master_.data() + base + (size_t(pose) * bones_ + c.first) * kBytesPerBone,
                        pending_.data() + size_t(pose) * c.count * kBytesPerBone, size_t(c.count) * kBytesPerBone);
    stamp = {serial_, lod, true};
    return BoneImportResult::Imported;
}

bool MergedSkeleton::patch_entry(uint32_t instance, const InstanceEntry& input,
                                InstanceEntry& output, std::string& reason) const
{
    reason.clear();
    if (instance >= instances_) return reject(reason, "invalid instance for entry patch");
    InstanceEntry patched = input;
    // The offset in an entry names the matrix before bone zero: the game's vertex shaders read bone i
    // at offset + 3 + 3 * i, which is why the importer above skips one source matrix. The core passes
    // "$merged_skeleton_offset - 3" to its instance config overrider for the same reason.
    const uint32_t first = rows_for_instance(instance) - kRowsPerBone;
    const uint32_t offsets[2] = {first, first + bones_ * kRowsPerBone};
    for (size_t i = 0; i < 2; ++i)
        if (word(input, 80 + i * 4) >= kStaticRows)
            std::memcpy(patched.data() + 80 + i * 4, &offsets[i], sizeof(uint32_t));
    output = patched;
    return true;
}

} // namespace efvk
