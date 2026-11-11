#include "character_package.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <numeric>
#include <set>
#include <thread>

namespace efvk {
namespace {
const char kMagic[] = "EFMICH01";
const char kMaterialMagic[] = "EFMICH02";
constexpr size_t kHeader = 48;
constexpr uint64_t kMaxPayload = 512ull << 20;

template <class T> T at(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(v)); return v; }

bool widen(const std::string& path, std::wstring& wide)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (count <= 0) return false;
    wide.assign(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &wide[0], count);
    return true;
}
bool open_file(const std::string& path, std::ifstream& in)
{
    std::wstring wide;
    if (!widen(path, wide)) return false;
    in.open(wide.c_str(), std::ios::binary | std::ios::ate);
    return bool(in);
}

// A stream's bytes from its file into `out`, checked against the checksum taken when they were
// validated; read as the original's LoadBufferFromFile reads a buffer file (CreateFile, ReadFile).
// `bounce`: `out` is write-combined memory (the mesh's upload), which the checksum must not read
// back, so the bytes pass through that buffer, a piece at a time while it is warm in the cache.
bool read_source(const CharacterSource& source, uint64_t size, uint8_t* out, std::vector<uint8_t>* bounce, std::string& reason)
{
    std::wstring wide;
    if (!widen(source.path, wide)) { reason = "mesh file path is not UTF-8: " + source.path; return false; }
    struct Handle { HANDLE h; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } file{
        CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (file.h == INVALID_HANDLE_VALUE) { reason = "mesh file missing or busy: " + source.path; return false; }
    LARGE_INTEGER length{}, at{};
    const uint64_t from_file = std::min(size, source.file_bytes);
    if (!GetFileSizeEx(file.h, &length) || static_cast<uint64_t>(length.QuadPart) < source.offset + from_file) {
        reason = "mesh file changed since the load (shorter): " + source.path; return false;
    }
    at.QuadPart = static_cast<LONGLONG>(source.offset);
    if (!SetFilePointerEx(file.h, at, nullptr, FILE_BEGIN)) { reason = "mesh file read failed: " + source.path; return false; }
    uint32_t crc = 0;
    for (uint64_t done = 0; done < from_file;) {
        const DWORD n = static_cast<DWORD>(std::min<uint64_t>(from_file - done, bounce ? bounce->size() : (1u << 30)));
        uint8_t* target = bounce ? bounce->data() : out + done;
        DWORD got = 0;
        if (!ReadFile(file.h, target, n, &got, nullptr) || got != n) { reason = "mesh file read failed: " + source.path; return false; }
        crc = crc32c(crc, target, n);
        if (bounce) std::memcpy(out + done, target, n);
        done += n;
    }
    if (from_file < size) {
        std::memset(out + from_file, 0, static_cast<size_t>(size - from_file));
        std::vector<uint8_t> zero(static_cast<size_t>(std::min<uint64_t>(size - from_file, 1u << 16)), 0);
        for (uint64_t done = from_file; done < size;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(size - done, zero.size()));
            crc = crc32c(crc, zero.data(), n); done += n;
        }
    }
    if (crc != source.crc) { reason = "mesh file changed since the load: " + source.path; return false; }
    return true;
}

// Bounded material envelope. The embedded mesh must be v1, so envelopes cannot recurse.
bool decode_materials(const std::vector<uint8_t>& file, CharacterPackage& output, std::string& reason)
{
    auto fail = [&](const char* text) { reason = text; return false; };
    if (file.size() < 32) return fail("truncated character material envelope");
    const uint64_t mesh_size = at<uint64_t>(file.data() + 8), size = at<uint64_t>(file.data() + 16);
    if (mesh_size < kHeader || mesh_size > kMaxPayload + (4u << 20) || size < 24 || size > (4u << 20) ||
        file.size() != 32 + mesh_size + size || at<uint32_t>(file.data() + 28) ||
        std::memcmp(file.data() + 32, kMagic, 8)) return fail("invalid character material envelope");
    if (at<uint32_t>(file.data() + 24) != crc32c(0, file.data() + 32, file.size() - 32))
        return fail("character material checksum mismatch");
    CharacterPackage next;
    if (!next.decode(std::vector<uint8_t>(file.begin() + 32, file.begin() + 32 + static_cast<size_t>(mesh_size)), reason)) return false;
    const uint8_t* p = file.data() + 32 + mesh_size;
    const uint8_t* end = file.data() + file.size();
    const uint32_t images = at<uint32_t>(p), profiles = at<uint32_t>(p + 4), draws = at<uint32_t>(p + 8);
    next.material_vendor = at<uint32_t>(p + 12); next.material_device = at<uint32_t>(p + 16);
    const uint32_t policy = at<uint32_t>(p + 20); p += 24;
    // No profile is valid: the layer derives every fragment shader's rules from its module (D-076).
    if (images > kCharacterImages || profiles > 256 || draws != next.draws.size() || policy > 1 ||
        (policy ? (!next.material_vendor || !next.material_device) : (next.material_vendor || next.material_device)))
        return fail("character material dimensions or policy invalid");
    for (uint32_t i = 0; i < images; ++i) {
        if (end - p < 4) return fail("truncated image path");
        const uint32_t length = at<uint32_t>(p); p += 4;
        if (!length || length > 4096 || uint64_t(end - p) < length) return fail("image path outside budget");
        std::string path(reinterpret_cast<const char*>(p), length); p += length;
        if (path.find('\0') != std::string::npos ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), length, nullptr, 0) ||
            !std::filesystem::u8path(path).is_absolute()) return fail("image path must be absolute UTF-8");
        next.images.push_back(std::move(path));
    }
    bool uses_policy = false;
    for (uint32_t i = 0; i < profiles; ++i) {
        if (end - p < 8) return fail("truncated material profile");
        const uint32_t words = at<uint32_t>(p), rules = at<uint32_t>(p + 4); p += 8;
        if (words < 5 || words > (1u << 20) || rules > 128 || uint64_t(words) * 4 + uint64_t(rules) * 16 > uint64_t(end - p))
            return fail("material profile outside budget");
        CharacterMaterialProfile profile;
        profile.code.resize(words); std::memcpy(profile.code.data(), p, words * 4); p += words * 4;
        if (profile.code[0] != 0x07230203) return fail("material profile is not SPIR-V");
        for (const auto& other : next.profiles) if (other.code == profile.code) return fail("duplicate material profile");
        std::set<uint32_t> logical;
        std::set<std::pair<uint32_t, uint32_t>> physical;
        for (uint32_t j = 0; j < rules; ++j, p += 16) {
            CharacterMaterialRule r = {at<uint32_t>(p), at<uint32_t>(p + 4), at<uint32_t>(p + 8), at<uint32_t>(p + 12)};
            if (r.slot >= 128 || !logical.insert(r.slot).second || r.kind < 1 || r.kind > 3 ||
                (r.kind == CharacterMaterialRule::Absent ? (r.set || r.binding) :
                 (r.set >= 3 || !physical.emplace(r.set, r.binding).second))) return fail("invalid material rule");
            if (r.kind == CharacterMaterialRule::ZeroFile) {
                if (!policy) return fail("dimension compatibility requires an explicit device policy");
                uses_policy = true;
            }
            profile.rules.push_back(r);
        }
        next.profiles.push_back(std::move(profile));
    }
    if (bool(policy) != uses_policy) return fail("unused material compatibility policy");
    for (uint32_t i = 0; i < draws; ++i) {
        if (end - p < 4) return fail("truncated pixel bindings");
        const uint32_t count = at<uint32_t>(p); p += 4;
        if (count > 128 || uint64_t(count) * 8 > uint64_t(end - p)) return fail("pixel bindings outside budget");
        std::vector<CharacterPixel> pixels;
        std::set<uint32_t> slots;
        for (uint32_t j = 0; j < count; ++j, p += 8) {
            CharacterPixel pixel = {at<uint32_t>(p), at<uint32_t>(p + 4)};
            if (pixel.slot >= 128 || !slots.insert(pixel.slot).second || (pixel.asset != kCharacterNone && pixel.asset >= images))
                return fail("invalid character image reference");
            pixels.push_back(pixel);
        }
        next.pixels.push_back(std::move(pixels));
    }
    if (p != end) return fail("trailing material bytes");
    next.materials = true; output = std::move(next); return true;
}
} // namespace

bool is_character_package(const std::string& path)
{
    std::ifstream in;
    if (!open_file(path, in) || in.tellg() < static_cast<std::streamoff>(kHeader)) return false;
    char magic[8];
    in.seekg(0);
    in.read(magic, 8);
    return in && (!std::memcmp(magic, kMagic, 8) || !std::memcmp(magic, kMaterialMagic, 8));
}

bool CharacterPackage::load(const std::string& path, std::string& reason)
{
    std::ifstream in;
    if (!open_file(path, in)) { reason = "character package missing or path is not UTF-8"; return false; }
    const auto size = in.tellg();
    if (size < static_cast<std::streamoff>(kHeader) || static_cast<uint64_t>(size) > kMaxPayload + (8u << 20) + 32) {
        reason = "character package outside size budget"; return false;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) { reason = "character package read failed"; return false; }
    if (!decode(data, reason)) return false;
    // The payload ends the v1 package, which an envelope holds after its 32 bytes (decode checked both).
    const uint64_t mesh_end = !std::memcmp(data.data(), kMaterialMagic, 8) ? 32 + at<uint64_t>(data.data() + 8) : data.size();
    std::vector<CharacterSource> files;
    for (const auto& s : streams) files.push_back({path, mesh_end - payload_bytes + s.offset, 0});
    return release_payload(std::move(files), reason);
}

bool CharacterPackage::release_payload(std::vector<CharacterSource> files, std::string& reason)
{
    if (files.size() != streams.size() || payload.size() != payload_bytes) { reason = "mesh sources do not match the package's streams"; return false; }
    for (size_t i = 0; i < streams.size(); ++i)
        files[i].crc = crc32c(0, payload.data() + streams[i].offset, static_cast<size_t>(streams[i].size));
    sources = std::move(files);
    std::vector<uint8_t>().swap(payload);
    return true;
}

bool CharacterPackage::read_stream(uint32_t stream, std::vector<uint8_t>& out, std::string& reason) const
{
    if (stream >= streams.size()) { reason = "stream out of range"; return false; }
    const auto& s = streams[stream];
    out.resize(static_cast<size_t>(s.size));
    if (sources.empty()) {
        if (s.offset + s.size > payload.size()) { reason = "stream outside the payload"; return false; }
        std::memcpy(out.data(), payload.data() + s.offset, static_cast<size_t>(s.size));
        return true;
    }
    return read_source(sources[stream], s.size, out.data(), nullptr, reason);
}

bool CharacterPackage::same_payload(const CharacterPackage& other) const
{
    if (sources.empty() || sources.size() != other.sources.size() || payload_bytes != other.payload_bytes || streams.size() != other.streams.size())
        return false;
    for (size_t i = 0; i < streams.size(); ++i)
        if (streams[i].offset != other.streams[i].offset || streams[i].size != other.streams[i].size || sources[i].crc != other.sources[i].crc ||
            sources[i].offset != other.sources[i].offset || sources[i].path != other.sources[i].path)
            return false;
    return true;
}

bool CharacterPackage::read_payload(uint8_t* out, std::string& reason) const
{
    if (sources.empty()) {
        if (payload.size() != payload_bytes) { reason = "the package holds neither its payload nor its sources"; return false; }
        std::memcpy(out, payload.data(), payload.size());
        return true;
    }
    std::vector<uint32_t> order(streams.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return streams[a].offset < streams[b].offset; });
    // Zero between the streams; streams that share bytes are read by one thread.
    uint64_t cursor = 0;
    bool overlapping = false;
    for (const uint32_t i : order) {
        const auto& s = streams[i];
        if (s.offset > cursor) std::memset(out + cursor, 0, static_cast<size_t>(s.offset - cursor));
        overlapping |= s.offset < cursor;
        cursor = std::max(cursor, s.offset + s.size);
    }
    if (cursor < payload_bytes) std::memset(out + cursor, 0, static_cast<size_t>(payload_bytes - cursor));
    // The files are read by a few threads at once, each into its own streams' bytes: the page cache
    // serves them in parallel, and one thread reading a character's files took three times as long as
    // copying a held payload (ISSUE-011).
    const size_t workers = overlapping ? 1 : std::min<size_t>(streams.size(), std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
    std::vector<std::vector<uint32_t>> groups(workers);
    std::vector<uint64_t> bytes(workers, 0);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return streams[a].size > streams[b].size; });
    for (const uint32_t i : order) {
        const size_t g = std::min_element(bytes.begin(), bytes.end()) - bytes.begin();
        groups[g].push_back(i); bytes[g] += streams[i].size;
    }
    std::vector<std::string> reasons(workers);
    std::vector<char> done(workers, 0);
    auto read_group = [&](size_t g) {
        uint64_t largest = 0;
        for (const uint32_t i : groups[g]) largest = std::max(largest, streams[i].size);
        std::vector<uint8_t> bounce(static_cast<size_t>(std::min<uint64_t>(largest, 1u << 20)));
        for (const uint32_t i : groups[g])
            if (!read_source(sources[i], streams[i].size, out + streams[i].offset, &bounce, reasons[g])) return;
        done[g] = 1;
    };
    std::vector<std::thread> threads;
    try {
        for (size_t g = 1; g < workers; ++g) threads.emplace_back(read_group, g);
    } catch (const std::system_error&) {
        // No thread to spare: this one reads the rest.
        for (size_t g = threads.size() + 1; g < workers; ++g) read_group(g);
    }
    read_group(0);
    for (auto& t : threads) t.join();
    for (size_t g = 0; g < workers; ++g)
        if (!done[g]) { reason = reasons[g]; return false; }
    return true;
}

// The component mask of a stream with bone indices (and optional weights), every index inside the
// skeleton and the weights normalized.
static bool stream_bones(const std::vector<CharacterComponent>& components, uint32_t bone_count, const uint8_t* payload,
                         CharacterStream& s, std::string& reason)
{
    const uint32_t component_count = static_cast<uint32_t>(components.size());
    // A bone between components belongs to none: nothing imports it, so no draw waits for it.
    auto component_of = [&](uint32_t bone) {
        for (uint32_t i = 0; i < component_count; ++i)
            if (bone >= components[i].first && bone < components[i].first + components[i].count) return i;
        return component_count;
    };
    s.components = 0;
    const uint64_t count = s.size / s.stride;
    for (uint64_t v = 0; v < count; ++v) {
        const uint8_t* element = payload + s.offset + v * s.stride;
        uint32_t total = 0;
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t bone = s.bone_index_bytes == 4 ? at<uint32_t>(element + s.bone_index_offset + k * 4)
                                : s.bone_index_bytes == 1 ? uint32_t(element[s.bone_index_offset + k])
                                                          : at<uint16_t>(element + s.bone_index_offset + k * 2);
            // The shader may fetch every index, weighted or not: all of them stay inside the skeleton.
            if (bone >= bone_count) { reason = "bone index outside the skeleton"; return false; }
            uint32_t weight = 1;
            if (s.bone_weight_offset != kCharacterNone) {
                if (s.bone_weight_bytes == 4) {
                    // Float weights (BUG-038): counted in 1/65536 as the normalized ones are.
                    const float f = at<float>(element + s.bone_weight_offset + k * 4);
                    if (!(f >= 0.0f && f <= 1.0f)) { reason = "bone weight outside 0..1"; return false; }
                    weight = static_cast<uint32_t>(f * 65536.0f + 0.5f);
                } else weight = at<uint16_t>(element + s.bone_weight_offset + k * 2);
                total += weight;
            }
            if (const uint32_t c = component_of(bone); weight && c < component_count) s.components |= 1ull << c;
        }
        if (s.bone_weight_offset != kCharacterNone && (total < 65533 - (s.bone_weight_bytes == 4 ? 64 : 0) || total > 65537 + (s.bone_weight_bytes == 4 ? 64 : 0))) {
            reason = "bone weights are not normalized"; return false;
        }
    }
    return true;
}

bool CharacterPackage::adopt_bones(uint32_t stream, uint32_t index_offset, uint32_t weight_offset, std::string& reason, uint32_t index_bytes,
                                   uint32_t weight_bytes)
{
    if (stream >= streams.size()) { reason = "bone stream out of range"; return false; }
    CharacterStream s = streams[stream];
    if (s.kind != CharacterStream::Vertex || s.bone_index_offset != kCharacterNone) { reason = "bone stream already known or not a vertex stream"; return false; }
    if (index_bytes != 1 && index_bytes != 2 && index_bytes != 4) { reason = "bone index width unsupported"; return false; }
    if (weight_bytes != 2 && weight_bytes != 4) { reason = "bone weight width unsupported"; return false; }
    s.bone_index_offset = index_offset; s.bone_weight_offset = weight_offset; s.bone_index_bytes = index_bytes; s.bone_weight_bytes = weight_bytes;
    const uint32_t element = 4 * index_bytes, weights = 4 * weight_bytes;
    if (uint64_t(index_offset) + element > s.stride || (weight_offset != kCharacterNone &&
        (uint64_t(weight_offset) + weights > s.stride || (weight_offset < index_offset + element && index_offset < weight_offset + weights)))) {
        reason = "bone elements outside the stream or overlapping"; return false;
    }
    // The stream's bytes, held or read again from its file (ISSUE-011).
    std::vector<uint8_t> bytes;
    if (!read_stream(stream, bytes, reason)) return false;
    CharacterStream read = s;
    read.offset = 0;
    if (!stream_bones(components, bones, bytes.data(), read, reason)) return false;
    s.components = read.components;
    streams[stream] = s;
    return true;
}

bool CharacterPackage::decode(const std::vector<uint8_t>& file, std::string& reason)
{
    reason.clear();
    if (file.size() >= 8 && !std::memcmp(file.data(), kMaterialMagic, 8)) return decode_materials(file, *this, reason);
    auto fail = [&](const char* text) { reason = text; return false; };
    if (file.size() < kHeader || std::memcmp(file.data(), kMagic, 8)) return fail("unsupported character package");
    uint32_t words[8];
    std::memcpy(words, file.data() + 8, sizeof(words));
    const uint32_t bone_count = words[0], component_count = words[1], stream_count = words[2], section_count = words[3];
    const uint32_t list_count = words[4], draw_count = words[5], edit_count = words[6], checksum = words[7];
    const uint64_t payload_bytes = at<uint64_t>(file.data() + 40);
    // The original Core sizes its pools by the Mod's own counts; these bound what the format's indices and the
    // component masks hold (D-141): 16-bit bone indices, 64 components.
    if (!bone_count || bone_count > kCharacterBones || !component_count || component_count > kCharacterComponents || !stream_count ||
        stream_count > kCharacterStreams || !section_count || section_count > kCharacterSections || list_count != component_count * 2 ||
        draw_count > 16384 || edit_count > 16 ||
        !payload_bytes || payload_bytes > kMaxPayload)
        return fail("character package dimensions outside limits");
    const uint64_t tables = uint64_t(component_count) * 24 + uint64_t(stream_count) * 32 + uint64_t(section_count) * 20 +
                            uint64_t(list_count) * 8 + uint64_t(draw_count) * 32 + uint64_t(edit_count) * 12;
    if (file.size() != kHeader + tables + payload_bytes) return fail("character package size disagrees with its header");
    if (crc32c(0, file.data() + kHeader, file.size() - kHeader) != checksum) return fail("character package checksum mismatch");

    CharacterPackage next;
    next.bones = bone_count;
    const uint8_t* p = file.data() + kHeader;
    struct RawComponent { uint32_t first, count, remap, list0, list1; };
    std::vector<RawComponent> raw_components;
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < component_count; ++i, p += 24) {
        RawComponent c = {at<uint32_t>(p), at<uint32_t>(p + 4), at<uint32_t>(p + 8), at<uint32_t>(p + 12), at<uint32_t>(p + 16)};
        // Disjoint and in order; bones between components stay as initialized (BUG-048).
        if (!c.count || c.first < cursor || uint64_t(c.first) + c.count > bone_count) return fail("components overlap or leave the skeleton");
        if (c.list0 >= list_count || c.list1 >= list_count) return fail("component draw list out of range");
        cursor = uint64_t(c.first) + c.count;
        raw_components.push_back(c);
    }
    for (uint32_t i = 0; i < stream_count; ++i, p += 32) {
        CharacterStream s;
        s.kind = at<uint32_t>(p); s.stride = at<uint32_t>(p + 4); s.offset = at<uint64_t>(p + 8); s.size = at<uint64_t>(p + 16);
        s.bone_index_offset = at<uint32_t>(p + 24); s.bone_weight_offset = at<uint32_t>(p + 28);
        if (s.kind > CharacterStream::Remap || !s.size || s.offset > payload_bytes || s.size > payload_bytes - s.offset)
            return fail("stream outside the payload");
        const bool stride_ok = s.kind == CharacterStream::Vertex ? (s.stride >= 1 && s.stride <= 256) :
                               s.kind == CharacterStream::Index ? (s.stride == 2 || s.stride == 4) : s.stride == 2;
        if (!stride_ok || s.size % s.stride || (s.kind == CharacterStream::Index && s.offset % 4))
            return fail("stream stride, size or alignment unsupported");
        if (s.bone_index_offset != kCharacterNone) {
            if (s.kind != CharacterStream::Vertex || uint64_t(s.bone_index_offset) + 8 > s.stride) return fail("bone index element outside its stream");
            if (s.bone_weight_offset != kCharacterNone &&
                (uint64_t(s.bone_weight_offset) + 8 > s.stride ||
                 (s.bone_weight_offset < s.bone_index_offset + 8 && s.bone_index_offset < s.bone_weight_offset + 8)))
                return fail("bone weight element outside its stream or overlapping the indices");
        } else if (s.bone_weight_offset != kCharacterNone) {
            return fail("bone weights without bone indices");
        }
        next.streams.push_back(s);
    }
    for (uint32_t i = 0; i < section_count; ++i, p += 20) {
        CharacterSection s = {at<uint32_t>(p), at<uint32_t>(p + 4), at<uint32_t>(p + 8), at<uint32_t>(p + 12), at<uint32_t>(p + 16)};
        if (!s.match_count || (s.component != kCharacterNone && s.component >= component_count) ||
            (s.lod != kCharacterNone && s.lod > 1) || (s.component == kCharacterNone && s.lod != kCharacterNone))
            return fail("section out of range");
        next.sections.push_back(s);
    }
    for (uint32_t i = 0; i < list_count; ++i, p += 8) {
        CharacterList l;
        l.first = at<uint32_t>(p); l.count = at<uint32_t>(p + 4);
        if (l.first > draw_count || l.count > draw_count - l.first || l.count > 64) return fail("draw list out of range");
        next.lists.push_back(l);
    }
    for (uint32_t i = 0; i < draw_count; ++i, p += 32) {
        CharacterDraw d;
        d.index_stream = at<uint32_t>(p); d.count = at<uint32_t>(p + 4); d.first = at<uint32_t>(p + 8); d.base = at<int32_t>(p + 12);
        for (uint32_t s = 0; s < 4; ++s) d.vb[s] = at<uint32_t>(p + 16 + s * 4);
        next.draws.push_back(d);
    }
    for (uint32_t i = 0; i < edit_count; ++i, p += 12) {
        CharacterEdit e = {at<uint32_t>(p), static_cast<VkFormat>(at<uint32_t>(p + 4)), static_cast<VkFormat>(at<uint32_t>(p + 8))};
        if (e.binding >= 4 || e.source != VK_FORMAT_R8G8B8A8_UINT || e.format != VK_FORMAT_R16G16B16A16_UINT)
            return fail("layout edit outside the supported rule");
        for (const auto& other : next.edits)
            if (other.binding == e.binding && other.source == e.source) return fail("duplicate layout edit");
        next.edits.push_back(e);
    }
    const uint8_t* payload = p;

    for (uint32_t i = 0; i < component_count; ++i) {
        const auto& raw = raw_components[i];
        CharacterComponent c;
        c.first = raw.first; c.count = raw.count; c.lists[0] = raw.list0; c.lists[1] = raw.list1;
        if (raw.remap != kCharacterNone) {
            c.remap_stream = raw.remap;
            if (raw.remap >= stream_count) return fail("remap stream out of range");
            const auto& s = next.streams[raw.remap];
            if (s.kind != CharacterStream::Remap || s.size != uint64_t(c.count) * 2) return fail("remap stream has the wrong kind or length");
            c.has_remap = true;
            c.remap_lod1.resize(c.count);
            std::memcpy(c.remap_lod1.data(), payload + s.offset, static_cast<size_t>(s.size));
            // A source index selects a matrix of the game's palette; bound what the importer may read.
            for (uint16_t source : c.remap_lod1) if (source >= 1024) return fail("remap source bone outside the supported window");
        }
        next.components.push_back(std::move(c));
    }
    for (auto& s : next.streams) {
        if (s.bone_index_offset == kCharacterNone) continue;
        if (!stream_bones(next.components, bone_count, payload, s, reason)) return false;
    }
    for (const auto& d : next.draws) {
        if (d.index_stream >= stream_count || next.streams[d.index_stream].kind != CharacterStream::Index) return fail("draw without an index stream");
        const auto& index = next.streams[d.index_stream];
        const uint64_t available = index.size / index.stride;
        if (!d.count || d.count > (64u << 20) || d.first > available || d.count > available - d.first) return fail("draw reads past its index stream");
        int64_t low = INT64_MAX, high = INT64_MIN;
        const uint8_t* data = payload + index.offset + uint64_t(d.first) * index.stride;
        for (uint32_t i = 0; i < d.count; ++i) {
            const int64_t value = index.stride == 2 ? at<uint16_t>(data + i * 2ull) : at<uint32_t>(data + i * 4ull);
            low = std::min(low, value); high = std::max(high, value);
        }
        low += d.base; high += d.base;
        bool any = false;
        for (uint32_t slot = 0; slot < 4; ++slot) {
            if (d.vb[slot] == kCharacterNone) continue;
            if (d.vb[slot] >= stream_count || next.streams[d.vb[slot]].kind != CharacterStream::Vertex) return fail("draw binds a stream that is not a vertex stream");
            const auto& s = next.streams[d.vb[slot]];
            if (low < 0 || high >= static_cast<int64_t>(s.size / s.stride)) return fail("draw reads past a vertex stream");
            any = true;
        }
        if (!any) return fail("draw binds no vertex stream");
    }
    for (auto& l : next.lists)
        for (uint32_t i = 0; i < l.count; ++i)
            for (uint32_t slot = 0; slot < 4; ++slot) {
                const uint32_t stream = next.draws[l.first + i].vb[slot];
                if (stream != kCharacterNone) l.required |= next.streams[stream].components;
            }
    next.payload.assign(payload, payload + payload_bytes);
    next.payload_bytes = payload_bytes;
    *this = std::move(next);
    return true;
}

bool CharacterPackage::vertex_input(const CharacterDraw& draw, const VertexInputRecipe& source, VertexInputRecipe& output,
                                    std::string& reason) const
{
    reason.clear();
    VertexInputRecipe next;
    // No stream patches: this only checks the source declaration (unique bindings and locations).
    if (!patch_vertex_streams(source, {}, next, reason)) return false;
    for (uint32_t slot = 0; slot < 4; ++slot) {
        if (draw.vb[slot] == kCharacterNone) continue;
        const auto binding = std::find_if(next.bindings.begin(), next.bindings.end(), [&](const auto& b) { return b.binding == slot; });
        // A stream the pipeline does not declare is bound and ignored, as under the source API.
        if (binding == next.bindings.end()) continue;
        if (binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX) { reason = "replaced stream is per instance in the pipeline"; return false; }
        const auto* owned = draw.vb[slot] == kCharacterOwned ? draw.owned_stream(slot) : nullptr;
        if (draw.vb[slot] == kCharacterOwned && !owned) { reason = "owned vertex slot without its buffer"; return false; }
        const uint32_t stride = owned ? owned->stride : streams[draw.vb[slot]].stride;
        for (auto& a : next.attributes) {
            if (a.binding != slot) continue;
            for (const auto& o : draw.offsets)
                if (o.binding == slot && o.from == a.offset) { a.offset = o.to; break; }
            // A live plan's blend-index edit replaces the pipeline's BLENDINDICES element whatever width
            // the game declares it with (8-bit mostly, 32-bit for some pipelines: BUG-038); the
            // package's own edits name their source format.
            if (slot == 2 && draw.pipeline_blend && blend_index_format(a.format)) {} // the game's own element, as it is
            else if (slot == 2 && draw.blend != VK_FORMAT_UNDEFINED && blend_index_format(a.format)) a.format = draw.blend;
            else
                for (const auto& e : edits)
                    if (e.binding == slot && e.source == a.format) { a.format = e.format; break; }
            const uint32_t bytes = vertex_format_bytes(a.format);
            if (!bytes) { reason = "replaced stream has an element of an unknown format"; return false; }
            if (uint64_t(a.offset) + bytes > uint64_t(stride) + kCharacterOvershoot) {
                reason = "element far outside the file's stride"; return false;
            }
        }
        binding->stride = stride;
    }
    output = std::move(next);
    return true;
}

} // namespace efvk
