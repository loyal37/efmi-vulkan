// A whole character as data: meshes, bone ranges, LOD remaps and what to draw per matched section.
// Built offline by scripts/package_character.py from one scenario of the hosted frontend's plans.
// Optional static materials; no keys or conditions, not an INI interpreter.
#pragma once
#include "pipeline_recipe.h"

namespace efvk {

constexpr uint32_t kCharacterNone = UINT32_MAX;
// A draw's vertex slot bound to an owned GPU buffer of a live plan (CharacterDraw::owned).
constexpr uint32_t kCharacterOwned = UINT32_MAX - 1;
constexpr uint32_t kCharacterOvershoot = 64;
// DDS images one package may declare: they load only when a draw binds them, so this is a sanity
// budget, not memory (a Mod with a watch GUI declares 83; 64 until 1.8.1).
constexpr uint32_t kCharacterImages = 1024;
// A live generation's catalogue holds every image the Mods folder declares (D-178).
constexpr uint32_t kCharacterRuntimeImages = 65536;
// What one character's contract holds (D-141). The original Core sizes its pools by the Mod's counts and has
// no limit of its own; these are the widths of the layer's indices and masks: a component a bit of a 64-bit
// mask, a bone a 16-bit index after the blend-index edit, sections and mesh files far beyond any export.
constexpr uint32_t kCharacterComponents = 64;
constexpr uint32_t kCharacterBones = 65535;
constexpr uint32_t kCharacterSections = 4096;
constexpr uint32_t kCharacterStreams = 4096;
// The formats a pipeline declares BLENDINDICES and BLENDWEIGHT with in binding 2: four unsigned
// integers of 8, 16 or 32 bits, and four normalized 16-bit or float weights. A Mod's blend-index
// edit names the replacement format; the game's own may be any of these (BUG-038).
inline bool blend_index_format(VkFormat f)
{
    return f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R32G32B32A32_UINT;
}
inline uint32_t blend_weight_bytes(VkFormat f)
{
    return f == VK_FORMAT_R16G16B16A16_UNORM ? 2 : f == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 0;
}

struct CharacterStream {
    enum Kind : uint32_t { Vertex = 0, Index = 1, Remap = 2 };
    uint32_t kind = Vertex, stride = 0;
    uint64_t offset = 0, size = 0; // into the payload
    // For a vertex stream that carries bone indices: byte offsets of four 16-bit indices and,
    // when present, of four 16-bit normalized weights inside one element.
    uint32_t bone_index_offset = kCharacterNone, bone_weight_offset = kCharacterNone;
    uint32_t bone_index_bytes = 2; // width of one bone index: 2 (16-bit), 4 under a 32-bit blend-index edit, 1 as the game's own 8-bit element
    uint32_t bone_weight_bytes = 2; // width of one weight: 2 (16-bit normalized) or 4 (float, BUG-038)
    uint64_t spans = 0; // bit per bone span (CharacterPackage::spans) this stream's bones lie in (computed at load)
};
struct CharacterDraw {
    uint32_t index_stream = 0, count = 0, first = 0;
    int32_t base = 0;
    uint32_t vb[4] = {kCharacterNone, kCharacterNone, kCharacterNone, kCharacterNone};
    // A live plan's instances and non-indexed draws (D-142): `indexed` false draws `count` vertices from
    // `first` without the index stream. Not in the file.
    uint32_t instances = 1, first_instance = 0;
    bool indexed = true;
    // A live plan's blend-index format for binding 2 (R16G16B16A16_UINT or R32G32B32A32_UINT);
    // undefined keeps the package's edits. Not part of the package file.
    VkFormat blend = VK_FORMAT_UNDEFINED;
    // A live plan whose list has no blend-index edit (R-08): the pipeline's own BLENDINDICES element of
    // binding 2 is read as the game declares it, and the package's edits do not apply to it. Not in the file.
    bool pipeline_blend = false;
    // A live plan that leaves the application's draw but binds textures for it (D-083): no geometry
    // of the package; the draw is made again with its own geometry and these pixels. Not in the file.
    bool texture_only = false;
    // Element offset edits of a live plan (D-085): the attribute of `binding` at the game's offset
    // `from` reads the Mod's stream at `to`. Not in the file.
    struct Offset { uint32_t binding = 0, from = 0, to = 0; };
    std::vector<Offset> offsets;
    // Vertex slots a live plan binds to a buffer its owned GPU work wrote (P2, D-087): vb[slot] is
    // kCharacterOwned; the Vulkan buffer is bound as it is and `keep` holds it. Not in the file.
    struct OwnedStream { uint32_t slot = 0; uint64_t buffer = 0, offset = 0, size = 0; uint32_t stride = 0; std::shared_ptr<void> keep; };
    std::vector<OwnedStream> owned;
    // A live plan's translucent draw (D-113): not drawn in the pass but captured into a translucency
    // layer at its end, with this opacity, mask image (an index of the image catalogue, or
    // kCharacterNone), channel and inversion. Not in the file.
    bool translucent = false;
    float alpha = 1;
    uint32_t mask_image = kCharacterNone, mask_channel = 0;
    bool mask_invert = false;
    const OwnedStream* owned_stream(uint32_t slot) const
    {
        for (const auto& o : owned) if (o.slot == slot) return &o;
        return nullptr;
    }
};
struct CharacterList {
    uint32_t first = 0, count = 0; // into draws; an empty list hides the original geometry
    uint64_t required = 0;         // bone spans the list's vertices use (computed at load)
};
// A part of the character as the Core's pools give it: an offset and a count into the merged skeleton, and a
// remap per LOD. Nothing keeps two components' bones apart (BUG-093): a LOD 1 part may be a component of its
// own on the LOD 0 part's bones, and then either one's import fills them.
struct CharacterComponent {
    uint32_t first = 0, count = 0;     // its bones in the merged skeleton
    bool has_remap = false;
    std::vector<uint16_t> remap_lod1;  // destination bone -> source bone at LOD 1, when has_remap
    std::vector<uint16_t> remap_lod0;  // the same at LOD 0; empty: the identity
    uint32_t lists[2] = {0, 0};        // draw list at LOD 0 and LOD 1
    uint32_t remap_stream = kCharacterNone;  // LOD 1's
    uint32_t remap_stream0 = kCharacterNone; // LOD 0's
    const std::vector<uint16_t>& remap(uint32_t lod) const { return lod == 1 ? remap_lod1 : remap_lod0; }
};
// The merged skeleton cut at every component's ends: a span lies wholly inside or outside each component,
// and `cover` has a bit per component it lies in. With components apart, a span is a component.
struct BoneSpan {
    uint32_t first = 0, count = 0;
    uint64_t cover = 0;
};
struct CharacterSection {
    uint32_t hash = 0, match_count = 0;
    uint32_t match_first = kCharacterNone; // none: any first index
    uint32_t component = 0;
    uint32_t lod = kCharacterNone;         // none: the section leaves the mod's LOD variable as it is
    // A draw matches only while its vertex binding `stride_slot` has this stride: one branch of a
    // TextureOverride that tells a component's LODs apart by `if vbN->stride == K` (LIMIT-041). Not in
    // the file.
    uint32_t stride_slot = kCharacterNone, stride = 0;
};
struct CharacterEdit {
    uint32_t binding = 0;
    VkFormat source = VK_FORMAT_UNDEFINED, format = VK_FORMAT_UNDEFINED;
};
struct CharacterPixel {
    uint32_t slot = 0, asset = kCharacterNone; // none is an explicit null; missing slots inherit
};
struct CharacterMaterialRule {
    // ZeroFile is an explicit, device-bound compatibility policy for invalid source-API
    // image dimensions. It is never inferred merely because a relocation failed.
    enum Kind : uint32_t { Absent = 1, Image = 2, ZeroFile = 3 };
    uint32_t slot = 0, kind = Absent, set = 0, binding = 0;
};
struct CharacterMaterialProfile {
    std::vector<uint32_t> code;
    std::vector<CharacterMaterialRule> rules;
};

// Where a stream's bytes are once the package released its payload (ISSUE-011): a region of a file,
// and the checksum of the bytes validated at load.
struct CharacterSource {
    std::string path; // UTF-8
    uint64_t offset = 0;
    uint32_t crc = 0;
    // Bytes the stream takes from the file; the rest of the stream is zero (a last element the file holds
    // only part of reads zero, as the original's out-of-bounds vertex fetch does; BUG-077). All by default.
    uint64_t file_bytes = UINT64_MAX;
};

struct CharacterPackage {
    uint32_t bones = 0;
    std::vector<CharacterComponent> components; // in the order of their first bones; bones between them belong to none
    // The bones some component imports, cut at the components' ends (make_spans): what a stream's bones and a
    // list's needs are counted in. At most 64, the width of the masks.
    std::vector<BoneSpan> spans;
    bool make_spans(std::string& reason);
    std::vector<CharacterStream> streams;
    std::vector<CharacterSection> sections;
    std::vector<CharacterList> lists;
    std::vector<CharacterDraw> draws;
    std::vector<CharacterEdit> edits;
    // The mesh bytes, from decode until release_payload; payload_bytes stays their size.
    std::vector<uint8_t> payload;
    uint64_t payload_bytes = 0;
    std::vector<CharacterSource> sources; // per stream, once released
    bool materials = false;
    uint32_t material_vendor = 0, material_device = 0;
    std::vector<std::string> images; // absolute UTF-8 paths, immutable after load
    // Per image, the loader flags of its declaration's colour space (D-140); missing ones are 0.
    std::vector<uint32_t> image_flags;
    std::vector<CharacterMaterialProfile> profiles;
    std::vector<std::vector<CharacterPixel>> pixels; // one binding snapshot per mesh draw

    bool load(const std::string& utf8_path, std::string& reason);
    // Everything is checked before anything is accepted: sizes, ranges, index bounds of every
    // draw against its vertex streams, and every bone index against the bone count.
    bool decode(const std::vector<uint8_t>& file, std::string& reason);
    // The declaration a replaced draw needs. A stream the draw binds takes the file's stride;
    // an edit changes the format of matching elements of its binding in place. Streams the draw
    // does not bind, the application's default attributes among them, stay as they are.
    // An element may end up to kCharacterOvershoot bytes past the file's stride: the source API
    // binds such a file all the same and the shader reads into the next vertex.
    bool vertex_input(const CharacterDraw&, const VertexInputRecipe& source, VertexInputRecipe& output,
                      std::string& reason) const;
    // A vertex stream found to carry bone indices only at run time (the live plan binds it where the
    // blend-index edit applies): the same checks as decode, then its component mask. Once per stream.
    bool adopt_bones(uint32_t stream, uint32_t index_offset, uint32_t weight_offset, std::string& reason, uint32_t index_bytes = 2,
                     uint32_t weight_bytes = 2);
    // 3DMigoto keeps no host copy of a buffer it made from a file: LoadBufferFromFile frees the bytes
    // once CreateBuffer took them. The package does the same once validated: `files` names each
    // stream's file and where its bytes begin; the payload goes and the streams are read again from
    // there (the upload when the package activates, a stream a live plan adopts for bones).
    bool release_payload(std::vector<CharacterSource> files, std::string& reason);
    // The payload, zero between streams, into `out` (payload_bytes long): held, or read from the files.
    // A stream whose bytes are not the ones validated at load refuses (its file changed since).
    bool read_payload(uint8_t* out, std::string& reason) const;
    bool read_stream(uint32_t stream, std::vector<uint8_t>& out, std::string& reason) const;
    // The same bytes at the same places as `other`'s: both released, every stream at the same offset,
    // of the same size, from the same file region with the same validated checksum (BUG-061: an F10
    // that changed no mesh file keeps the mesh on the device).
    bool same_payload(const CharacterPackage& other) const;
};

// True when the file starts with the character package magic. No other validation.
bool is_character_package(const std::string& utf8_path);

} // namespace efvk
