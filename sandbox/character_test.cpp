// Bounded decoding of a character package and the vertex declaration it asks for.
// "--inspect <file>" runs the production decoder on a real package and prints what it holds.
// "--derived <package> <fragment.spv> <slot:kind:set:binding>..." prepares that fragment module with
// those rules as the backend does for a shader without a profile, and prints what each draw gets and
// the DX11 registers the rule of D-076 gives the module's resources.
#include "../layer/src/character_package.h"
#include "../layer/src/character_materials.h"
#include "../layer/src/character_program.h"
#include "../layer/src/shader_hooks.h"
#include "../layer/src/texture_identity.h"
#include "../frontend/gpu/source_registers.h"
#include "../frontend/gpu/texture_roles.h"
#include "../layer/src/json.h"
#include "variant_fixture.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

using namespace efvk;
// A path argument as the process received it: argv holds the ANSI code page, which cannot carry
// every folder name a Mod has (a full-width colon, D-116's non-ASCII paths).
static std::filesystem::path path_argument(int index)
{
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::filesystem::path out = args && index < count ? std::filesystem::path(args[index]) : std::filesystem::path();
    LocalFree(args);
    return out;
}
static int passed = 0, failed = 0;
static void check(bool ok, const char* text) { if (ok) ++passed; else { ++failed; printf("FAIL: %s\n", text); } }
// The game's common character vertex shader by the register rule (D-079): constant buffers at
// vs-cb0 to vs-cb3 with the instance window at vs-cb2 (set 2, binding 0), the rolling bones at vs-t0.
static std::vector<efmi::gpu::SourceRegister> common_vertex_registers()
{
    using efmi::gpu::SourceRegister;
    auto cb = [](uint32_t index, uint32_t bytes, uint32_t set, uint32_t binding) {
        SourceRegister r; r.kind = SourceRegister::ConstantBuffer; r.letter = 'b'; r.index = index; r.bytes = bytes; r.set = set; r.binding = binding; return r;
    };
    SourceRegister bones; bones.kind = SourceRegister::BufferSrv; bones.letter = 't'; bones.index = 0; bones.stride = 16; bones.binding = 17;
    return {cb(0, 1312, 0, 11), cb(1, 3328, 0, 15), cb(2, 65536, 2, 0), cb(3, 448, 1, 0), bones};
}
// RuntimeDraw::source_srv over a stage's registers, as the backend answers it (D-124).
static bool srv_of(const std::vector<efmi::gpu::SourceRegister>& registers, unsigned slot, uint32_t& type, uint32_t& dimension, uint32_t& stride,
                   std::string& why)
{
    efmi::gpu::SourceSrvDeclaration declaration;
    if (!efmi::gpu::source_srv_declaration(registers, slot, declaration, why)) return false;
    type = declaration.words ? efmi::frontend::kWordBufferDeclaration : declaration.type;
    dimension = declaration.dimension; stride = declaration.stride;
    return true;
}
template <class T> static void put(std::vector<uint8_t>& data, T value)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    data.insert(data.end(), p, p + sizeof(value));
}
// A refusal that is this offline stand-in's limit, not the Mod's: what it answers with "offline ...",
// and the services it has none of (the owned GPU runtime, the game's instance windows, the draw's
// render targets), which the layer has in the game.
static bool offline_limit(const std::string& why)
{
    for (const char* marker : {"offline ", "needs the owned GPU domain", "Original buffer window unavailable", "render targets are not known"})
        if (why.find(marker) != std::string::npos) return true;
    return false;
}
template <class T> static void poke(std::vector<uint8_t>& data, size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }

// Two components (bones 0-1 and 2-4), the first with a LOD 1 remap. One draw list of two draws on
// the first component, whose second draw reads a bone of the second component; the second is hidden.
struct Model {
    std::vector<uint8_t> payload, tables;
    uint32_t components = 2, streams = 0, sections = 2, lists = 4, draws = 2, edits = 1, bones = 5;
    size_t stream_table = 0, draw_table = 0, component_table = 0, list_table = 0, section_table = 0, edit_table = 0;
    size_t blend_a = 0, blend_b = 0, index_data = 0, remap_data = 0;
};
// `extra`: that many more components of one bone each after the two, the second draw reading the last of
// them instead of bone 3 (D-141: more components than a 32-bit mask holds).
static std::vector<uint8_t> package(Model* out = nullptr, uint32_t extra = 0)
{
    Model m;
    m.components += extra; m.lists += 2 * extra; m.bones += extra;
    struct Entry { uint32_t kind, stride; uint64_t offset, size; uint32_t index_offset, weight_offset; };
    std::vector<Entry> table;
    auto add = [&](uint32_t kind, uint32_t stride, const std::vector<uint8_t>& bytes, uint32_t io = kCharacterNone, uint32_t wo = kCharacterNone) {
        while (m.payload.size() % 16) m.payload.push_back(0);
        table.push_back({kind, stride, m.payload.size(), bytes.size(), io, wo});
        m.payload.insert(m.payload.end(), bytes.begin(), bytes.end());
        return static_cast<uint32_t>(table.size() - 1);
    };
    std::vector<uint8_t> position(3 * 16, 0), texcoord(3 * 8, 0), index, remap, blend_a, blend_b;
    for (uint32_t v = 0; v < 3; ++v) {
        for (uint16_t value : {uint16_t(65535), uint16_t(0), uint16_t(0), uint16_t(0), uint16_t(1), uint16_t(0), uint16_t(0), uint16_t(0)}) put(blend_a, value);
        const uint16_t second = extra ? uint16_t(4 + extra) : uint16_t(3);
        for (uint16_t value : {uint16_t(40000), uint16_t(25535), uint16_t(0), uint16_t(0), uint16_t(0), second, uint16_t(0), uint16_t(0)}) put(blend_b, value);
        put(index, v);
    }
    put(remap, uint16_t(1)); put(remap, uint16_t(0));
    const uint32_t s_remap = add(CharacterStream::Remap, 2, remap);
    const uint32_t s_index = add(CharacterStream::Index, 4, index);
    const uint32_t s_position = add(CharacterStream::Vertex, 16, position);
    const uint32_t s_texcoord = add(CharacterStream::Vertex, 8, texcoord);
    const uint32_t s_blend_a = add(CharacterStream::Vertex, 16, blend_a, 8, 0);
    const uint32_t s_blend_b = add(CharacterStream::Vertex, 16, blend_b, 8, 0);
    m.remap_data = static_cast<size_t>(table[s_remap].offset); m.index_data = static_cast<size_t>(table[s_index].offset);
    m.blend_a = static_cast<size_t>(table[s_blend_a].offset); m.blend_b = static_cast<size_t>(table[s_blend_b].offset);
    m.streams = static_cast<uint32_t>(table.size());
    auto& t = m.tables;
    m.component_table = t.size();
    for (uint32_t value : {0u, 2u, s_remap, 0u, 1u, 0u}) put(t, value);
    for (uint32_t value : {2u, 3u, kCharacterNone, 2u, 3u, 0u}) put(t, value);
    for (uint32_t i = 0; i < extra; ++i)
        for (uint32_t value : {5u + i, 1u, kCharacterNone, 4u + 2 * i, 5u + 2 * i, 0u}) put(t, value);
    m.stream_table = t.size();
    for (const auto& e : table) { put(t, e.kind); put(t, e.stride); put(t, e.offset); put(t, e.size); put(t, e.index_offset); put(t, e.weight_offset); }
    m.section_table = t.size();
    for (uint32_t value : {0x11111111u, 3u, 0u, 0u, 0u}) put(t, value);
    for (uint32_t value : {0x22222222u, 12u, kCharacterNone, 1u, kCharacterNone}) put(t, value);
    m.list_table = t.size();
    for (uint32_t value : {0u, 2u, 0u, 2u, 2u, 0u, 2u, 0u}) put(t, value);
    for (uint32_t i = 0; i < 2 * extra; ++i) for (uint32_t value : {2u, 0u}) put(t, value);
    m.draw_table = t.size();
    for (uint32_t blend : {s_blend_a, s_blend_b}) {
        for (uint32_t value : {s_index, 3u, 0u}) put(t, value);
        put(t, int32_t(0));
        for (uint32_t value : {s_position, s_texcoord, blend, s_position}) put(t, value);
    }
    m.edit_table = t.size();
    for (uint32_t value : {2u, uint32_t(VK_FORMAT_R8G8B8A8_UINT), uint32_t(VK_FORMAT_R16G16B16A16_UINT)}) put(t, value);
    std::vector<uint8_t> data(8);
    std::memcpy(data.data(), "EFMICH01", 8);
    for (uint32_t value : {m.bones, m.components, m.streams, m.sections, m.lists, m.draws, m.edits, 0u}) put(data, value);
    put(data, uint64_t(m.payload.size()));
    data.insert(data.end(), t.begin(), t.end());
    data.insert(data.end(), m.payload.begin(), m.payload.end());
    poke(data, 36, crc32c(0, data.data() + 48, data.size() - 48));
    if (out) *out = m;
    return data;
}
static void seal(std::vector<uint8_t>& data) { poke(data, 36, crc32c(0, data.data() + 48, data.size() - 48)); }
// A package file of the test's own, in the temporary folder.
static std::filesystem::path package_file(const char* name, const std::vector<uint8_t>& bytes)
{
    const auto path = std::filesystem::temp_directory_path() / (std::string("efvk-") + name + "-" + std::to_string(GetCurrentProcessId()) + ".bin");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

// ISSUE-011: 3DMigoto keeps no host copy of a buffer it made from a file (LoadBufferFromFile frees its
// bytes once CreateBuffer took them). A loaded package holds no payload either: where each stream is
// in its file, and the checksum of the bytes validated at load; its streams are read again from there.
static void payload_tests(const std::vector<uint8_t>& valid, const Model& m)
{
    const auto path = package_file("payload", valid);
    CharacterPackage p;
    std::string why;
    check(p.load(path.u8string(), why) && p.payload.empty() && p.payload_bytes == m.payload.size() && p.sources.size() == p.streams.size(),
          "a loaded package holds no payload, only where each stream is in its file");
    std::vector<uint8_t> read(m.payload.size() + 1, 0xCD);
    check(p.read_payload(read.data(), why) && std::equal(m.payload.begin(), m.payload.end(), read.begin()) && read.back() == 0xCD,
          "the payload read again from the file is the one validated, zero between streams");
    // The texcoord stream (3) carries no bone indices in the file; a live plan that binds it under the
    // blend-index edit adopts it, its bytes read from the file.
    check(p.adopt_bones(3, 0, kCharacterNone, why) && p.streams[3].components == 1, "a stream adopted for bones is read from its file");
    // The position stream (2) changed in the file since the load.
    auto changed = valid;
    changed[48 + m.tables.size() + static_cast<size_t>(p.streams[2].offset) + 2] ^= 1;
    package_file("payload", changed);
    check(!p.read_payload(read.data(), why) && why.find("changed") != std::string::npos,
          ("a stream whose file changed since the load refuses: " + why).c_str());
    check(!p.adopt_bones(2, 0, kCharacterNone, why) && why.find("changed") != std::string::npos && p.streams[2].bone_index_offset == kCharacterNone,
          "a stream adopted for bones whose file changed refuses and stays as it was");
    std::filesystem::remove(path);
    check(!p.read_payload(read.data(), why) && !why.empty(), "a stream whose file is gone refuses");
    CharacterPackage held;
    check(held.decode(valid, why) && held.payload == m.payload && held.payload_bytes == m.payload.size() && held.read_payload(read.data(), why) &&
          std::equal(m.payload.begin(), m.payload.end(), read.begin()), "a decoded package holds its payload until released");
}

static void material_tests(const std::vector<uint8_t>& mesh)
{
    const auto code = variant_fixture::shader(false, true);
    const std::string path = "F:/fixture.dds";
    std::vector<uint8_t> data{'E','F','M','I','C','H','0','2'};
    put(data, uint64_t(mesh.size())); put(data, uint64_t(0)); put(data, uint32_t(0)); put(data, uint32_t(0));
    data.insert(data.end(), mesh.begin(), mesh.end());
    const size_t table = data.size();
    for (uint32_t word : {1u, 1u, 2u, 0u, 0u, 0u}) put(data, word);
    put(data, uint32_t(path.size())); data.insert(data.end(), path.begin(), path.end());
    const size_t profile = data.size();
    put(data, uint32_t(code.size())); put(data, uint32_t(1));
    for (uint32_t word : code) put(data, word);
    const size_t rule = data.size();
    for (uint32_t word : {15u, 2u, 1u, 6u}) put(data, word);
    const size_t pixels = data.size();
    for (uint32_t word : {1u, 15u, 0u, 1u, 15u, kCharacterNone}) put(data, word);
    poke(data, 16, uint64_t(data.size() - table));
    auto reseal = [](std::vector<uint8_t>& bytes) { poke(bytes, 24, crc32c(0, bytes.data() + 32, bytes.size() - 32)); };
    reseal(data);
    CharacterPackage package;
    std::string reason;
    check(package.decode(data, reason) && package.materials && package.images == std::vector<std::string>{path} &&
          package.profiles[0].code == code && package.pixels[0][0].asset == 0 && package.pixels[1][0].asset == kCharacterNone,
          "material envelope preserves exact code, per-draw file/null and mesh bytes");
    {
        // The envelope's mesh streams are read again from the envelope's file (ISSUE-011).
        const auto file = package_file("envelope", data);
        CharacterPackage loaded;
        std::vector<uint8_t> read(package.payload.size());
        check(loaded.load(file.u8string(), reason) && loaded.payload.empty() && loaded.read_payload(read.data(), reason) && read == package.payload,
              "a material envelope's mesh is read again from its file");
        std::filesystem::remove(file);
    }
    {
        // A package without profiles: every fragment shader's rules come from its module (D-076).
        std::vector<uint8_t> bare(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(profile));
        bare.insert(bare.end(), data.begin() + static_cast<std::ptrdiff_t>(pixels), data.end());
        poke(bare, table + 4, uint32_t(0)); poke(bare, 16, uint64_t(bare.size() - table)); reseal(bare);
        CharacterPackage plain;
        check(plain.decode(bare, reason) && plain.materials && plain.profiles.empty() && plain.pixels.size() == 2 &&
              plain.pixels[0][0].slot == 15 && plain.pixels[0][0].asset == 0, "a material envelope without profiles keeps its per-draw bindings");
    }
    std::vector<std::vector<CharacterMaterial>> plans;
    std::set<uint32_t> counts;
    check(prepare_character_materials(package, plans, counts, reason) && plans[0][0].supported && plans[0][1].supported &&
          plans[0][0].assets == std::vector<uint32_t>{0} && plans[0][0].patch.images.size()==1 &&
          plans[0][1].patch.images.empty() && plans[0][1].patch.nulls.size()==1 && counts==std::set<uint32_t>{0,1},
          "production material preparation preserves per-draw file and explicit null");
    auto unsupported=package;unsupported.pixels[1].push_back({42,0});
    check(prepare_character_materials(unsupported,plans,counts,reason) && plans[0][0].supported && !plans[0][1].supported &&
          plans[0][1].patch.images.empty() && plans[0][1].patch.nulls.empty() && !plans[0][1].reason.empty(),
          "unknown late pixel slot has an explicit refusal and no partial contract");
    auto invalid=package;invalid.profiles[0].rules.push_back(invalid.profiles[0].rules[0]);
    check(!prepare_character_materials(invalid,plans,counts,reason) && plans[0][0].supported && !plans[0][1].supported,
          "invalid profile preserves all previously prepared contracts");
    auto reject = [&](std::vector<uint8_t> bytes, const char* text, bool seal = true) {
        if (seal) reseal(bytes);
        check(!package.decode(bytes, reason) && !reason.empty() && package.materials && package.images[0] == path &&
              package.profiles[0].code == code, text);
    };
    {auto b=data;poke(b,8,UINT64_MAX);reject(b,"material mesh length overflow refused");}
    {auto b=data;poke(b,16,uint64_t(4u<<20)+1);reject(b,"material byte budget enforced");}
    {auto b=data;b.back()^=1;reject(b,"material checksum covers bindings",false);}
    {auto b=data;b[28]=1;reject(b,"material reserved flags refused");}
    {auto b=data;b[39]='2';reject(b,"nested material envelope refused");}
    {auto b=data;poke(b,table,uint32_t(65));reject(b,"too many images refused");}
    {auto b=data;poke(b,table+8,uint32_t(1));reject(b,"material and mesh draw counts must agree");}
    {auto b=data;poke(b,table+24,uint32_t(4097));reject(b,"image path byte budget enforced");}
    {auto b=data;b[table+28]=0;reject(b,"embedded path NUL refused");}
    {auto b=data;b[table+29]='x';reject(b,"relative image path refused");}
    {auto b=data;b[table+28]=0xff;reject(b,"invalid UTF-8 path refused");}
    {auto b=data;poke(b,profile,uint32_t(1u<<22));reject(b,"shader byte budget enforced");}
    {auto b=data;poke(b,profile+8,uint32_t(0));reject(b,"shader magic checked");}
    {auto b=data;poke(b,rule,uint32_t(128));reject(b,"logical rule slot bound checked");}
    {auto b=data;poke(b,rule+4,uint32_t(0));reject(b,"unknown rule kind refused");}
    {auto b=data;poke(b,rule+8,uint32_t(3));reject(b,"profile cannot address private set");}
    {auto b=data;poke(b,rule+4,uint32_t(3));reject(b,"dimension zero cannot be implicit");}
    {auto b=data;poke(b,table+12,uint32_t(1));reject(b,"device restriction requires a policy");}
    {auto b=data;poke(b,pixels,uint32_t(129));reject(b,"pixel count bounded");}
    {auto b=data;poke(b,pixels+8,uint32_t(1));reject(b,"missing image asset refused");}
    {auto b=data;b.pop_back();poke(b,16,uint64_t(b.size()-table));reject(b,"truncated pixel binding refused");}
    {auto b=data;b.push_back(0);poke(b,16,uint64_t(b.size()-table));reject(b,"trailing material bytes refused");}
    check(package.decode(mesh, reason) && !package.materials && package.images.empty() && package.pixels.empty(),
          "loading a legacy package clears previous material state");
}

static void declaration_tests(const CharacterPackage& p)
{
    VertexInputRecipe source, result;
    std::string why;
    source.bindings = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 8, VK_VERTEX_INPUT_RATE_VERTEX}, {2, 12, VK_VERTEX_INPUT_RATE_VERTEX},
                       {3, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {4, 0, VK_VERTEX_INPUT_RATE_VERTEX}};
    source.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32_SFLOAT, 0}, {2, 2, VK_FORMAT_R16G16B16A16_UNORM, 0},
                         {3, 2, VK_FORMAT_R8G8B8A8_UINT, 8}, {4, 4, VK_FORMAT_R8G8B8A8_UNORM, 12}};
    check(p.vertex_input(p.draws[0], source, result, why) && result.bindings[2].stride == 16 && result.bindings[1].stride == 8 &&
          result.bindings[4].stride == 0 && result.attributes[3].format == VK_FORMAT_R16G16B16A16_UINT && result.attributes[3].offset == 8 &&
          result.attributes[2].format == VK_FORMAT_R16G16B16A16_UNORM && result.attributes[4].binding == 4,
          "12-byte blend stream widened to 16; other streams and the default attribute untouched");
    {
        // An element offset edit (D-085): the attribute at the game's offset reads the Mod's stream at
        // the new one; an edit that names no attribute of the pipeline changes nothing.
        auto moved = p.draws[0];
        moved.offsets = {{2, 8, 12}, {3, 12, 40}};
        auto wide = p;
        wide.streams[wide.draws[0].vb[2]].stride = 16;
        check(wide.vertex_input(moved, source, result, why) && result.attributes[3].offset == 12 && result.attributes[2].offset == 0 &&
              result.attributes[0].offset == 0, "an offset edit moves the attribute at the game's offset only");
        moved.offsets = {{2, 8, 100}};
        check(!wide.vertex_input(moved, source, result, why), "an offset edit far past the Mod's stride refuses");
    }
    auto narrow = source;
    narrow.bindings[2].stride = 4; narrow.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {3, 2, VK_FORMAT_R8G8B8A8_UINT, 0}};
    auto small = p;
    small.streams[small.draws[0].vb[2]].stride = 8;
    check(small.vertex_input(small.draws[0], narrow, result, why) && result.bindings[2].stride == 8 &&
          result.attributes[1].format == VK_FORMAT_R16G16B16A16_UINT, "4-byte index-only stream widened to 8");
    {
        // A live draw under a 32-bit blend-index edit (R32G32B32A32_UINT): the same element, four 32-bit indices.
        auto wide32 = p;
        wide32.streams[wide32.draws[0].vb[2]].stride = 24;
        auto draw = wide32.draws[0];
        draw.blend = VK_FORMAT_R32G32B32A32_UINT;
        check(wide32.vertex_input(draw, source, result, why) && result.bindings[2].stride == 24 &&
              result.attributes[3].format == VK_FORMAT_R32G32B32A32_UINT && result.attributes[3].offset == 8 &&
              result.attributes[2].format == VK_FORMAT_R16G16B16A16_UNORM,
              "a 32-bit blend-index edit widens the element to R32G32B32A32_UINT for that draw");
        CharacterPackage bones;
        bones.bones = 5;
        CharacterComponent a, b; a.first = 0; a.count = 2; b.first = 2; b.count = 3;
        bones.components = {a, b};
        CharacterStream s; s.kind = CharacterStream::Vertex; s.stride = 24; s.offset = 0; s.size = 48;
        bones.streams = {s};
        bones.payload.assign(48, 0);
        for (uint32_t v = 0; v < 2; ++v) {
            const uint16_t weights[4] = {65535, 0, 0, 0};
            const uint32_t indices[4] = {v == 0 ? 1u : 3u, 4, 4, 4};
            std::memcpy(bones.payload.data() + v * 24, weights, 8);
            std::memcpy(bones.payload.data() + v * 24 + 8, indices, 16);
        }
        auto adopted = bones;
        check(adopted.adopt_bones(0, 8, 0, why, 4) && adopted.streams[0].components == 3 && adopted.streams[0].bone_index_bytes == 4,
              "32-bit bone indices are read as such; only weighted bones count for the component mask");
        auto outside = bones;
        const uint32_t beyond = 5;
        std::memcpy(outside.payload.data() + 8 + 4, &beyond, 4);
        check(!outside.adopt_bones(0, 8, 0, why, 4) && why == "bone index outside the skeleton",
              "a 32-bit bone index outside the skeleton refuses, weighted or not");
        check(!bones.adopt_bones(0, 16, 0, why, 4), "32-bit indices that do not fit the element refuse");
    }
    {
        // A live draw whose list has no blend-index edit (R-08, D-161): the pipeline's own BLENDINDICES element
        // is read as the game declares it (8-bit here); the package's own widening does not apply to that draw.
        auto own = p;
        own.streams[own.draws[0].vb[2]].stride = 12;
        auto draw = own.draws[0];
        draw.pipeline_blend = true;
        check(own.vertex_input(draw, source, result, why) && result.bindings[2].stride == 12 &&
              result.attributes[3].format == VK_FORMAT_R8G8B8A8_UINT && result.attributes[3].offset == 8,
              "a draw without a blend-index edit keeps the pipeline's own 8-bit element");
        CharacterPackage bones;
        bones.bones = 5;
        CharacterComponent a, b; a.first = 0; a.count = 2; b.first = 2; b.count = 3;
        bones.components = {a, b};
        CharacterStream s; s.kind = CharacterStream::Vertex; s.stride = 12; s.offset = 0; s.size = 24;
        bones.streams = {s};
        bones.payload.assign(24, 0);
        for (uint32_t v = 0; v < 2; ++v) {
            const uint16_t weights[4] = {65535, 0, 0, 0};
            const uint8_t indices[4] = {uint8_t(v == 0 ? 1 : 3), 4, 4, 4};
            std::memcpy(bones.payload.data() + v * 12, weights, 8);
            std::memcpy(bones.payload.data() + v * 12 + 8, indices, 4);
        }
        auto adopted = bones;
        check(adopted.adopt_bones(0, 8, 0, why, 1) && adopted.streams[0].components == 3 && adopted.streams[0].bone_index_bytes == 1,
              "8-bit bone indices are read as such");
        auto outside = bones;
        outside.payload[8 + 1] = 5;
        check(!outside.adopt_bones(0, 8, 0, why, 1) && why == "bone index outside the skeleton", "an 8-bit bone index outside the skeleton refuses");
    }
    auto depth_only = source;
    depth_only.bindings = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
    depth_only.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    check(p.vertex_input(p.draws[0], depth_only, result, why) && result.bindings.size() == 1 && result.bindings[0].stride == 16,
          "streams the pipeline does not declare are ignored");
    auto wide = source;
    wide.bindings[1].stride = 20; wide.attributes[1] = {1, 1, VK_FORMAT_R32G32_SFLOAT, 12};
    check(p.vertex_input(p.draws[0], wide, result, why) && result.bindings[1].stride == 8 && result.attributes[1].offset == 12,
          "an element a little past the file's stride is kept where it is, as the source API reads it");
    wide.attributes[1].offset = 100;
    check(!p.vertex_input(p.draws[0], wide, result, why) && !why.empty(), "an element far outside the file's stride refuses");
    auto unknown = source;
    unknown.attributes[1].format = VK_FORMAT_R64G64_SFLOAT;
    check(!p.vertex_input(p.draws[0], unknown, result, why) && !why.empty(), "an element of an unknown format in a replaced stream refuses");
    auto twice = source;
    twice.bindings.push_back(twice.bindings[0]);
    check(!p.vertex_input(p.draws[0], twice, result, why), "a declaration with a repeated binding refuses");
    auto instanced = source;
    instanced.bindings[2].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    check(!p.vertex_input(p.draws[0], instanced, result, why), "a per-instance stream is not replaced");
    auto same = source;
    same.bindings[2].stride = 16; same.attributes[3].format = VK_FORMAT_R16G16B16A16_UINT;
    check(p.vertex_input(p.draws[0], same, result, why) && result.bindings[2].stride == 16 && result.attributes[3].format == VK_FORMAT_R16G16B16A16_UINT,
          "a declaration that already has the file's layout passes unchanged");
}

// RabbitFX's roles by the bound textures (D-082): the decisions server/efmi_vk/texture_roles.py makes.
static void texture_role_tests()
{
    using efmi::gpu::RoleSample; using efmi::gpu::RoleTexture;
    const RoleTexture srgb{true, 146, 2048, 2048, 12}, linear{true, 145, 2048, 2048, 12}, small{true, 145, 512, 512, 10},
                      bc5{true, 141, 2048, 2048, 12}, target{true, 122, 1708, 1068, 1}, placeholder{true, 43, 4, 4, 1};
    auto run = [](std::vector<std::pair<RoleSample, RoleTexture>> items) {
        std::vector<RoleSample> samples;
        for (const auto& item : items) samples.push_back(item.first);
        return efmi::gpu::texture_roles(samples, [&](const RoleSample& s) {
            for (const auto& item : items) if (item.first.t == s.t) return item.second;
            return RoleTexture{};
        });
    };
    auto sample = [](uint32_t t, uint32_t at, bool decoded = false) { RoleSample s; s.t = t; s.at = at; s.decoded = decoded; return s; };
    check(efmi::gpu::texture_class(srgb) == 'S' && efmi::gpu::texture_class(linear) == 'U' && efmi::gpu::texture_class(bc5) == 'N' &&
          efmi::gpu::texture_class(target) == 'o' && efmi::gpu::texture_class(placeholder) == 'o' &&
          efmi::gpu::texture_class(RoleTexture{true, 146, 2048, 2048, 11}) == 'o',
          "texture classes: sRGB, linear BC, BC5 asset textures; targets, placeholders and short mip chains are none");
    auto r = run({{sample(16, 10), srgb}, {sample(17, 20), linear}, {sample(18, 50, true), bc5}});
    check(r.diffuse == 16 && r.lightmap == 17 && r.normal == 18, "diffuse, a lightmap of its size, then a BC5 normal map");
    r = run({{sample(16, 10), srgb}, {sample(17, 20), linear}, {sample(18, 50, true), linear}, {sample(12, 90), small}});
    check(r.diffuse == 16 && r.lightmap == 17 && r.normal == 18, "a linear normal map decoded right after the lightmap");
    r = run({{sample(0, 10), srgb}, {sample(1, 40, true), srgb}});
    check(r.diffuse == 0 && r.lightmap == -1 && r.normal == 1, "an sRGB texture decoded as a normal near the diffuse");
    r = run({{sample(14, 10), srgb}, {sample(12, 20), target}, {sample(13, 700, true), linear}});
    check(r.diffuse == 14 && r.lightmap == -1 && r.normal == -1, "a normal decode far from the diffuse is not a role");
    r = run({{sample(13, 10), srgb}, {sample(14, 20), small}});
    check(r.diffuse == 13 && r.lightmap == -1 && r.normal == 14, "a linear texture of another size is the normal map, not a lightmap");
    r = run({{sample(5, 10), target}, {sample(6, 20), linear}});
    check(r.diffuse == -1 && r.lightmap == -1 && r.normal == -1, "no sRGB asset texture: no role");
    r = run({{sample(16, 10), srgb}, {sample(16, 15), srgb}, {sample(17, 20), RoleTexture{}}});
    check(r.diffuse == 16 && r.lightmap == -1 && r.normal == -1, "a register sampled twice counts once; an unknown texture has no class");
}

// The DX11 register rule (D-076) on the fixture's fragment modules. Their bone block is a read-only
// buffer and comes first (t0); the image follows (t1) wherever its binding is, as for a feature
// variant of a game shader that moves its bindings.
static void register_tests()
{
    using efmi::gpu::SourceRegister;
    std::vector<SourceRegister> r;
    std::string reason;
    const auto at = [&](char letter, uint32_t index) { return efmi::gpu::find_source_register(r, letter, index); };
    check(efmi::gpu::source_registers(variant_fixture::shader(false, true), r, reason) && at('t', 0) && at('t', 1) &&
          at('t', 0)->kind == SourceRegister::BufferSrv && at('t', 0)->set == 0 && at('t', 0)->binding == 7 &&
          at('t', 1)->kind == SourceRegister::ImageSrv && at('t', 1)->set == 1 && at('t', 1)->binding == 6 && at('t', 1)->dim == 1 && !at('t', 2),
          "the read-only bone block is t0 and the image t1 of the material fragment module");
    check(efmi::gpu::source_registers(variant_fixture::shader(false, true, 8), r, reason) && at('t', 1) && at('t', 1)->binding == 8,
          "the variant with the image at another binding keeps it at t1");
    check(efmi::gpu::source_registers(variant_fixture::shader(false, false), r, reason) && at('t', 0) && !at('t', 1) &&
          at('b', 0) && at('b', 0)->kind == SourceRegister::ConstantBuffer && at('b', 0)->set == 2 && at('b', 0)->binding == 0,
          "a module without an image has its buffer at t0 and its uniform block at b0");
    auto cut = variant_fixture::shader(false, true);
    cut.resize(cut.size() - 3);
    r = {SourceRegister{}};
    check(!efmi::gpu::source_registers(cut, r, reason) && !efmi::gpu::source_registers({1, 2, 3}, r, reason) && r.size() == 1,
          "a truncated or foreign module is refused and the output stays");
}

// The first index of the draw a section is planned with (D-173): its own; for a section without a component
// that takes any, the lowest no section with a component of the same hash and count takes, because the backend
// hands a draw such a section matches to it (its plan runs this section's lists as well). False when every
// draw of the section is one of those: it has no plan of its own.
template <typename Sections, typename Section>
static bool section_first(const Sections& all, const Section& s, uint32_t& first)
{
    first = s.match_first == kCharacterNone ? 0 : s.match_first;
    if (s.component != kCharacterNone) return true;
    for (;;) {
        bool taken = false;
        for (const auto& other : all) {
            if (other.component == kCharacterNone || other.hash != s.hash || other.match_count != s.match_count) continue;
            if (other.match_first == kCharacterNone) return false;
            taken |= other.match_first == first;
        }
        if (!taken) return true;
        if (s.match_first != kCharacterNone) return false;
        first += 3;
    }
}

// A section's draw as the backend hands it to the plan, with what offline stands in for the game.
template <typename Section>
static efmi::frontend::RuntimeDraw section_draw(const Section& s, uint32_t first, uint32_t lod, uint64_t frame, CharacterProgram& program,
                                                const std::vector<efmi::gpu::SourceRegister>& vertex)
{
    efmi::frontend::RuntimeDraw request;
    request.hash = s.hash; request.count = s.match_count == kCharacterNone ? 0 : s.match_count;
    request.first = first;
    request.frame = frame; request.seconds = float(frame) / 60.0f;
    request.mod = program.folder_index();
    if (s.component != kCharacterNone) { request.component = s.component; request.lod = lod; request.identity = 1; }
    if (s.component != kCharacterNone) { request.rolling_bone_source = true; request.source_vs_config_slot = 2; }
    request.shader_stages = 0x1 | 0x10;
    request.source_srv = [&vertex](wchar_t stage, unsigned slot, uint32_t& type, uint32_t& dimension, uint32_t& stride, std::string& why) {
        if (stage != L'v') { why = "offline the pixel shader's declarations are not known"; return false; }
        return srv_of(vertex, slot, type, dimension, stride, why);
    };
    request.pixel_texture = [](unsigned, efmi::frontend::RuntimeTexture& texture, std::string&) { texture = {}; texture.bound = true; return true; };
    request.vertex_stride = [](unsigned, uint32_t&, std::string& why) { why = "offline the game's vertex buffers are not known"; return false; };
    if (s.stride_slot != kCharacterNone)
        request.vertex_stride = [&s](unsigned slot, uint32_t& value, std::string& why) {
            if (slot != s.stride_slot) { why = "offline the game's vertex buffers are not known"; return false; }
            value = s.stride; return true;
        };
    return request;
}

// D-137: the folder generation's dry run in every state its keys reach as a player presses them. Each
// key action's keys (as the original parsed them) are held for a presentation and released, up to eight
// presses or until the variables repeat a state seen; the original's own dispatch decides what changes.
// After the default state and after each hold and release, every Mod's sections are planned. A refusal is
// reported with the first state it appeared in; the offline stand-ins' own limits are not.
static int plan_folder_states(const std::filesystem::path& mods, const std::filesystem::path& core, const std::filesystem::path& settings)
{
    const auto vertex = common_vertex_registers();
    CharacterProgram::FolderSource source{std::filesystem::absolute(mods), std::filesystem::absolute(core), std::filesystem::absolute(settings)};
    // EFMI_VK_CHECK_NO_OWNED: as in the game where the owned GPU runtime is not available (its tools not
    // deployed, m2_owned = 0): no CustomShader execution and none declared.
    source.declarations_only = !getenv("EFMI_VK_CHECK_NO_OWNED");
    std::vector<std::pair<std::filesystem::path, std::string>> left_inputs;
    source.left_inputs = &left_inputs;
    std::vector<CharacterProgram::FolderMod> found;
    std::string reason;
    if (!CharacterProgram::discover_folder(source, found, reason)) {
        fprintf(stderr, "folder generation refused: %s\n", reason.c_str());
        for (const auto& m : found)
            if (!m.reason.empty()) fprintf(stderr, "  %s: %s\n", m.ini.lexically_relative(mods).u8string().c_str(), m.reason.c_str());
        return 1;
    }
    CharacterProgram* presenter = nullptr;
    for (auto& m : found) if (m.program) { presenter = m.program.get(); break; }
    if (!presenter) { fprintf(stderr, "no character Mod\n"); return 1; }
    struct Refusal { std::string state, reason; uint32_t hash = 0, component = kCharacterNone, lod = 0; };
    std::vector<std::vector<Refusal>> refusals(found.size());
    std::vector<std::set<std::string>> seen_refusals(found.size());
    std::vector<uint64_t> plans(found.size());
    // Sections whose plan in the default state hides the game's draw and draws the Mod's own: what a
    // player sees of the Mod without a key (a plan that is made and draws nothing is no evidence).
    std::vector<uint64_t> drawn(found.size());
    // docs/architecture-review-2.1.md P3-3: what a Mod's plans are, in one number. Every section's plan in
    // every state, in order: whether the game's draw is hidden, each draw of the list (streams, range,
    // instances, layout edits, translucency) with the textures bound for it, and a refusal with its reason.
    // The same Mod files under the same layer give the same number; a change of the plan path that changes
    // any plan of an installed Mod changes it (scripts/check_mod_states.py compares with the accepted ones).
    std::vector<uint64_t> digests(found.size(), 1469598103934665603ull);
    // EFMI_VK_PLAN_OUTCOMES=<file>: per Mod and section, every distinct outcome over all states with the
    // number of states it had (a plan in words, or the refusal's reason). Two builds' files, compared, say
    // which sections' plans a change of the plan path changed, and how (scripts/check_mod_states.py --explain).
    const char* const outcomes_file = getenv("EFMI_VK_PLAN_OUTCOMES");
    std::vector<std::map<std::string, std::map<std::string, uint64_t>>> outcomes(found.size());
    auto mix = [](uint64_t& h, uint64_t v) { for (int b = 0; b < 8; ++b) { h ^= (v >> (b * 8)) & 0xff; h *= 1099511628211ull; } };
    std::vector<std::string> present_errors;
    uint64_t frame = 0, states = 0;
    efmi::frontend::RuntimeInput released;
    released.foreground = true;
    auto present = [&](const efmi::frontend::RuntimeInput& input, const std::string& state) {
        std::string why;
        ++frame;
        // A Present whose draws have no executor is this stand-in's limit (no GPU runtime), not the Mods'.
        if (!presenter->present(input, frame, float(frame) / 60.0f, why) && why.find("no executor") == std::string::npos && present_errors.size() < 16)
            present_errors.push_back(state + ": " + why);
    };
    auto plan_all = [&](const std::string& state) {
        ++states;
        for (size_t i = 0; i < found.size(); ++i) {
            auto& m = found[i];
            if (!m.program) continue;
            uint32_t current_lod = 0;
            for (const auto& s : m.package.sections) {
                if (s.lod != kCharacterNone) current_lod = s.lod;
                const uint32_t lod = s.component == kCharacterNone ? kCharacterNone : current_lod;
                uint32_t first = 0;
                if (!section_first(m.package.sections, s, first)) continue;
                char section_key[96];
                snprintf(section_key, sizeof(section_key), "%04zu %08x count %u first %u c%u lod%u", size_t(&s - m.package.sections.data()), s.hash,
                         s.match_count, s.match_first, s.component, lod);
                auto request = section_draw(s, first, lod, frame, *m.program, vertex);
                CharacterList list; bool skip = false, added = false, textures = false; std::string why;
                ++plans[i];
                auto& digest = digests[i];
                mix(digest, s.hash); mix(digest, uint64_t(s.component) << 32 | lod);
                if (m.program->plan(request, m.package, list, skip, added, why, nullptr, &textures)) {
                    if (state == "default" && skip && list.count) ++drawn[i];
                    mix(digest, uint64_t(skip) | uint64_t(textures) << 1); mix(digest, list.count);
                    // A stream and an image by what they are, not by their number in the package (numbers are
                    // given at first use and need not be the same in two runs).
                    auto stream_of = [&](uint32_t index) -> uint64_t {
                        if (index >= m.package.streams.size()) return 0xffff000000000000ull | index;
                        const auto& stream = m.package.streams[index];
                        return uint64_t(stream.kind) << 60 | uint64_t(stream.stride) << 48 | (stream.size & 0xffffffffffffull);
                    };
                    auto image_of = [&](uint32_t index) -> uint64_t {
                        if (index >= m.package.images.size()) return 0xffff000000000000ull | index;
                        uint64_t h = 1469598103934665603ull;
                        for (unsigned char c : m.package.images[index]) { h ^= c; h *= 1099511628211ull; }
                        return h;
                    };
                    for (uint32_t d = 0; d < list.count && size_t(list.first) + d < m.package.draws.size(); ++d) {
                        const auto& draw = m.package.draws[list.first + d];
                        mix(digest, stream_of(draw.index_stream)); mix(digest, draw.count); mix(digest, uint64_t(draw.first) << 32 | uint32_t(draw.base));
                        for (uint32_t stream : draw.vb) mix(digest, stream_of(stream));
                        mix(digest, uint64_t(draw.instances) << 32 | draw.first_instance);
                        mix(digest, uint64_t(draw.indexed) | uint64_t(draw.texture_only) << 1 | uint64_t(draw.translucent) << 2 | uint64_t(draw.mask_invert) << 3 |
                                    uint64_t(draw.pipeline_blend) << 4 | uint64_t(uint32_t(draw.blend)) << 8);
                        for (const auto& o : draw.offsets) { mix(digest, uint64_t(o.binding) << 32 | o.from); mix(digest, o.to); }
                        for (const auto& o : draw.owned) mix(digest, uint64_t(o.slot) << 32 | o.stride);
                        if (draw.translucent) { uint32_t bits = 0; memcpy(&bits, &draw.alpha, sizeof(bits)); mix(digest, bits); mix(digest, image_of(draw.mask_image)); mix(digest, draw.mask_channel); }
                        if (size_t(list.first) + d < m.package.pixels.size())
                            for (const auto& pixel : m.package.pixels[list.first + d]) { mix(digest, pixel.slot); mix(digest, image_of(pixel.asset)); }
                    }
                    if (outcomes_file) {
                        std::string text = std::string(skip ? "skip" : "keep") + (textures ? " textures" : "") + " draws=" + std::to_string(list.count);
                        for (uint32_t d = 0; d < list.count && size_t(list.first) + d < m.package.draws.size(); ++d) {
                            const auto& draw = m.package.draws[list.first + d];
                            char part[320];
                            snprintf(part, sizeof(part), " [ib %llx n%u f%u b%d vb %llx %llx %llx %llx inst %u+%u%s%s%s blend %u",
                                     static_cast<unsigned long long>(stream_of(draw.index_stream)), draw.count, draw.first, draw.base,
                                     static_cast<unsigned long long>(stream_of(draw.vb[0])), static_cast<unsigned long long>(stream_of(draw.vb[1])),
                                     static_cast<unsigned long long>(stream_of(draw.vb[2])), static_cast<unsigned long long>(stream_of(draw.vb[3])),
                                     draw.instances, draw.first_instance, draw.indexed ? "" : " nonindexed", draw.texture_only ? " texture-only" : "",
                                     draw.translucent ? " translucent" : "", unsigned(draw.blend));
                            text += part;
                            for (const auto& o : draw.offsets) { snprintf(part, sizeof(part), " off %u:%u>%u", o.binding, o.from, o.to); text += part; }
                            if (size_t(list.first) + d < m.package.pixels.size())
                                for (const auto& pixel : m.package.pixels[list.first + d]) {
                                    snprintf(part, sizeof(part), " t%u=%08x", pixel.slot, unsigned(image_of(pixel.asset) & 0xffffffffu)); text += part;
                                }
                            text += "]";
                        }
                        ++outcomes[i][section_key][text];
                    }
                    continue;
                }
                if (outcomes_file) ++outcomes[i][section_key]["REFUSED: " + why];
                mix(digest, 0xdeadull); for (unsigned char c : why) mix(digest, c);
                if (offline_limit(why)) continue; // the stand-ins' limits, not the Mod's
                char key[64]; snprintf(key, sizeof(key), "%08x/%u/%u/", s.hash, s.component, lod);
                if (!seen_refusals[i].insert(key + why).second) continue;
                refusals[i].push_back({state, why, s.hash, s.component, lod});
            }
        }
    };
    auto snapshot = [&] {
        std::string text;
        for (const auto& v : presenter->status().variables) text += v.first + "=" + std::to_string(v.second) + ";";
        return text;
    };
    // EFMI's exports register at a Present and draw after it: the default state is the second one.
    present(released, "start"); plan_all("start");
    present(released, "default"); plan_all("default");
    const auto combos = presenter->key_combos();
    for (size_t c = 0; c < combos.size(); ++c) {
        std::set<std::string> seen{snapshot()};
        std::string keys;
        for (int k : combos[c]) { char name[16]; snprintf(name, sizeof(name), "%s0x%02x", keys.empty() ? "" : "+", k); keys += name; }
        efmi::frontend::RuntimeInput held = released;
        for (int k : combos[c]) if (k >= 0 && k < 256) held.keys[k] = true;
        for (int press = 1; press <= 8; ++press) {
            const std::string state = "key " + std::to_string(c) + " (" + keys + ") press " + std::to_string(press);
            present(held, state + " held"); plan_all(state + " held");
            present(released, state); plan_all(state);
            if (!seen.insert(snapshot()).second) break;
        }
    }
    // Then the time a player spends without a key: a Mod's Present may change its state every frame
    // (timers, transitions), and a long session meets every state that reaches.
    for (int f = 1; f <= 1200; ++f) {
        const std::string state = "frame " + std::to_string(f) + " after the keys";
        present(released, state); plan_all(state);
    }
    if (outcomes_file) {
        if (FILE* out = fopen(outcomes_file, "wb")) {
            for (size_t i = 0; i < found.size(); ++i) {
                fprintf(out, "MOD %s\n", found[i].ini.lexically_relative(mods).u8string().c_str());
                for (const auto& section : outcomes[i])
                    for (const auto& outcome : section.second)
                        fprintf(out, "  %s | %llu | %s\n", section.first.c_str(), static_cast<unsigned long long>(outcome.second), outcome.first.c_str());
            }
            fclose(out);
        }
    }
    JsonWriter summary; summary.begin_object();
    summary.kn("key_actions", combos.size()); summary.kn("states", states);
    summary.key("present_errors"); summary.begin_array(); for (const auto& e : present_errors) summary.str(e); summary.end_array();
    // The inputs the folder was loaded without, and what the layer did not do as the original does while
    // the generation ran (the host shows both).
    summary.key("left_inputs"); summary.begin_array();
    for (const auto& item : left_inputs) {
        summary.begin_object(); summary.kv("ini", item.first.lexically_relative(mods).u8string()); summary.kv("reason", item.second); summary.end_object();
    }
    summary.end_array();
    summary.key("gaps"); summary.begin_array(); for (const auto& gap : presenter->status().gaps) summary.str(gap); summary.end_array();
    summary.end_object();
    printf("%s\n", summary.out.c_str());
    for (size_t i = 0; i < found.size(); ++i) {
        JsonWriter w; w.begin_object();
        w.kv("ini", found[i].ini.lexically_relative(mods).u8string());
        w.kb("loaded", found[i].program != nullptr);
        if (!found[i].program) w.kv("reason", found[i].reason);
        else { w.key("gaps"); w.begin_array(); for (const auto& gap : found[i].program->gaps()) w.str(gap); w.end_array(); }
        w.kn("plans", plans[i]); w.kn("drawn", drawn[i]);
        { char text[24]; snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(digests[i])); w.kv("digest", text); }
        w.key("refusals"); w.begin_array();
        for (const auto& r : refusals[i]) {
            w.begin_object();
            char hash[16]; snprintf(hash, sizeof(hash), "%08x", r.hash);
            w.kv("hash", hash);
            if (r.component != kCharacterNone) { w.kn("component", r.component); w.kn("lod", r.lod); }
            w.kv("state", r.state); w.kv("reason", r.reason);
            w.end_object();
        }
        w.end_array();
        w.end_object();
        printf("%s\n", w.out.c_str());
    }
    return 0;
}

// The live plan of each section of a discovered Mod in package order, with the LOD the backend would
// pass, written as the dry run's JSON (D-078). `presenter` runs the generation's Present before each
// plan: the Mod's own program, or one program of a folder generation (D-131), whose Present is shared.
static void write_plans(JsonWriter& w, CharacterProgram& program, CharacterProgram& presenter, CharacterPackage& package,
                        const std::vector<efmi::gpu::SourceRegister>& vertex, uint64_t& frame)
{
    w.kn("streams", package.streams.size()); w.kn("images", package.images.size());
    w.kn("components", package.components.size()); w.kn("bones", package.bones);
    w.kn("left_to_game", program.left_to_game());
    w.kn("identify_components", program.identify_components());
    // The streams and images by file name, which a folder generation must give each Mod as its own does.
    w.key("image_files"); w.begin_array();
    for (const auto& image : package.images) w.str(std::filesystem::u8path(image).filename().u8string());
    w.end_array();
    w.key("sections"); w.begin_array();
    uint32_t current_lod = 0; // as the backend: a section that assigns the LOD changes it for those that do not
    for (const auto& s : package.sections) {
        if (s.lod != kCharacterNone) current_lod = s.lod;
        uint32_t first = 0;
        if (!section_first(package.sections, s, first)) continue;
        for (uint32_t lod : {s.component == kCharacterNone ? kCharacterNone : current_lod}) {
            efmi::frontend::RuntimeInput input;
            ++frame;
            std::string presented;
            const bool present_ok = presenter.present(input, frame, float(frame) / 60.0f, presented);
            // Offline there is no pipeline: the facts the backend reads off each vertex module (D-079) are
            // taken as the game's common ones, the rolling vs-t0 bones and vs-cb2; the stages a vertex and a
            // pixel shader with the vertex shader's declarations (D-124), the pixel shader's not known; every
            // pixel slot bound without a hash, as the backend answers for a resource whose hash it cannot know.
            // Nor are its vertex buffers: a list that tells LODs apart by `vbN->stride` is refused here, unless
            // the section is one of its branches (LIMIT-041), which the backend matches only to a draw whose
            // pipeline binds that stride.
            auto request = section_draw(s, first, lod, frame, program, vertex);
            CharacterList list; bool skip = false, added = false, textures = false; std::string refused;
            const bool ok = program.plan(request, package, list, skip, added, refused, nullptr, &textures);
            // A list that tells LODs apart by `vbN->stride` plans once the host answers it, as the game's
            // pipeline does: the strides of the game's vertex layouts with which it plans.
            std::vector<uint32_t> strides;
            if (!ok && refused.find("vertex buffers are not known") != std::string::npos)
                for (uint32_t stride : {4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u, 48u, 64u}) {
                    request.vertex_stride = [stride](unsigned, uint32_t& value, std::string&) { value = stride; return true; };
                    ++frame;
                    std::string ignored;
                    presenter.present(input, frame, float(frame) / 60.0f, ignored);
                    request.frame = frame; request.seconds = float(frame) / 60.0f;
                    CharacterList probe; bool probe_skip = false, probe_added = false, probe_textures = false; std::string why;
                    if (program.plan(request, package, probe, probe_skip, probe_added, why, nullptr, &probe_textures)) strides.push_back(stride);
                }
            w.begin_object();
            char hash[16]; snprintf(hash, sizeof(hash), "%08x", s.hash);
            w.kv("hash", hash); w.kn("count", s.match_count);
            if (s.component != kCharacterNone) { w.kn("component", s.component); w.kn("lod", lod); }
            if (s.stride_slot != kCharacterNone) { w.kn("stride_slot", s.stride_slot); w.kn("stride", s.stride); }
            if (!present_ok) w.kv("present_refused", presented);
            w.kb("planned", ok);
            if (!ok) w.kv("reason", refused);
            if (!strides.empty()) { w.key("planned_with_vertex_strides"); w.begin_array(); for (uint32_t v : strides) w.num(v); w.end_array(); }
            else {
                w.kb("skip", skip); w.kn("draws", list.count); w.kn("required", list.required);
                if (textures) w.kb("textures", true); // the application's draw with the Mod's textures (D-083)
                std::set<uint32_t> slots;
                std::set<std::string> files;
                for (uint32_t i = 0; i < list.count; ++i)
                    for (const auto& pixel : package.pixels[list.first + i]) {
                        slots.insert(pixel.slot);
                        if (pixel.asset != kCharacterNone) files.insert(std::filesystem::u8path(package.images[pixel.asset]).filename().u8string());
                    }
                w.key("pixel_slots"); w.begin_array(); for (uint32_t slot : slots) w.num(slot); w.end_array();
                w.kn("images_bound", files.size());
                w.key("bound_files"); w.begin_array(); for (const auto& f : files) w.str(f); w.end_array();
            }
            w.end_object();
        }
    }
    w.end_array();
}
int main(int argc, char** argv)
{
    SetEnvironmentVariableW(L"EFMI_VK_LOG", L"character-test.log");
    util_init(GetModuleHandleW(nullptr));
    CharacterPackage p;
    std::string why;
    if (argc == 3 && !strcmp(argv[1], "--material-shader")) {
        const auto code = variant_fixture::shader(false, true);
        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        return out ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--materials")) {
        std::vector<std::vector<CharacterMaterial>> plans;
        std::set<uint32_t> counts;
        if (!p.load(argv[2], why) || !p.materials || !prepare_character_materials(p, plans, counts, why)) {
            fprintf(stderr, "materials rejected: %s\n", why.c_str()); return 1;
        }
        JsonWriter w;w.begin_object();w.kn("vendor",p.material_vendor);w.kn("device",p.material_device);
        w.key("profiles");w.begin_array();
        for(size_t i=0;i<plans.size();++i){
            const auto& code=p.profiles[i].code;
            w.begin_object();w.kn("crc",crc32c(0,code.data(),code.size()*4));w.key("draws");w.begin_array();
            for(const auto& plan:plans[i]){
                w.begin_object();w.kb("ok",plan.supported);w.kb("compatibility",plan.compatibility);w.kv("reason",plan.reason);
                w.key("images");w.begin_array();
                for(size_t j=0;j<plan.patch.images.size();++j){
                    w.begin_array();w.num(plan.assets[j]);w.num(plan.patch.images[j].set);w.num(plan.patch.images[j].binding);w.end_array();
                }
                w.end_array();w.key("nulls");w.begin_array();
                for(const auto& image:plan.patch.nulls){w.begin_array();w.num(image.set);w.num(image.binding);w.end_array();}
                w.end_array();w.end_object();
            }
            w.end_array();w.end_object();
        }
        w.end_array();w.end_object();printf("%s\n",w.out.c_str());return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--derived")) {
        std::vector<std::vector<CharacterMaterial>> known;
        std::set<uint32_t> counts, used;
        if (!p.load(argv[2], why) || !p.materials || !prepare_character_materials(p, known, counts, why)) {
            fprintf(stderr, "materials rejected: %s\n", why.c_str()); return 1;
        }
        std::ifstream in(argv[3], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CharacterMaterialProfile source;
        source.code.resize(bytes.size() / 4);
        if (bytes.empty() || bytes.size() % 4) { fprintf(stderr, "fragment module unreadable\n"); return 1; }
        memcpy(source.code.data(), bytes.data(), bytes.size());
        for (int i = 4; i < argc; ++i) {
            CharacterMaterialRule r; uint32_t kind = 0;
            if (sscanf(argv[i], "%u:%u:%u:%u", &r.slot, &kind, &r.set, &r.binding) != 4) { fprintf(stderr, "rule unreadable\n"); return 1; }
            r.kind = static_cast<CharacterMaterialRule::Kind>(kind); source.rules.push_back(r);
        }
        std::vector<efmi::gpu::SourceRegister> registers;
        std::string unread;
        const bool readable = efmi::gpu::source_registers(source.code, registers, unread);
        std::vector<CharacterMaterial> plans;
        const bool prepared = prepare_character_profile(p, source, plans, used, why);
        bool covered = true;
        for (uint32_t count : used) covered &= counts.count(count) != 0;
        JsonWriter w; w.begin_object(); w.kb("readable", readable); w.kb("prepared", prepared); w.kv("reason", why); w.kb("counts_covered", covered);
        // The DX11 registers the rule gives the module's resources: [letter, register, set, binding].
        w.key("registers"); w.begin_array();
        for (const auto& r : registers) {
            w.begin_array(); w.str(std::string(1, r.letter)); w.num(r.index); w.num(r.set); w.num(r.binding); w.end_array();
        }
        w.end_array(); w.key("draws"); w.begin_array();
        for (const auto& plan : plans) {
            w.begin_object(); w.kb("ok", plan.supported); w.kv("reason", plan.reason);
            w.key("images"); w.begin_array();
            for (size_t j = 0; j < plan.patch.images.size(); ++j) {
                w.begin_array(); w.num(plan.assets[j]); w.num(plan.patch.images[j].set); w.num(plan.patch.images[j].binding); w.end_array();
            }
            w.end_array(); w.key("nulls"); w.begin_array();
            for (const auto& image : plan.patch.nulls) { w.begin_array(); w.num(image.set); w.num(image.binding); w.end_array(); }
            w.end_array(); w.end_object();
        }
        w.end_array(); w.end_object(); printf("%s\n", w.out.c_str()); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--view-formats")) {
        // D-170: the DXGI format the layer answers for a view of each Vulkan format, per aspect (1 colour,
        // 2 depth): "<VkFormat> <aspect> <DXGI_FORMAT>" for those it maps (scripts/check_texture_properties.py).
        for (uint32_t format = 1; format < 185; ++format)
            for (uint32_t aspect : {1u, 2u})
                if (const uint32_t dxgi = efvk::dxgi_view_format(VkFormat(format), VkImageAspectFlags(aspect))) printf("%u %u %u\n", format, aspect, dxgi);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--shader-hooks")) {
        // EFMITTL's hook facts (D-113, D-171) of SPIR-V modules: "<file> <lit vertex 0/1> <copy fragment 0/1>
        // <vertex group 0-3> <pixel groups known 0/1> <colour slot or -1> <narrow first 0/1>".
        for (int i = 2; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary | std::ios::ate);
            std::vector<uint32_t> code(size_t(in.tellg()) / 4); in.seekg(0); in.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
            const auto pixel = efvk::character_pixel_hook(code);
            printf("%s %d %d %u %d %d %d\n", argv[i], efvk::lit_character_vertex(code) ? 1 : 0, efvk::copy_fragment(code) ? 1 : 0,
                   efvk::character_vertex_hook(code), pixel.known ? 1 : 0, pixel.colour_slot, pixel.narrow_first ? 1 : 0);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--core-facts")) {
        // D-079 as the backend computes it, per vertex module: "<file> <slot> <rolling> <set>.<binding>".
        for (int i = 2; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary | std::ios::ate);
            std::vector<uint32_t> code(in ? size_t(in.tellg()) / 4 : 0);
            if (in) { in.seekg(0); in.read(reinterpret_cast<char*>(code.data()), code.size() * 4); }
            std::vector<efmi::gpu::SourceRegister> registers; std::string reason;
            if (!efmi::gpu::source_registers(code, registers, reason)) { printf("%s refused %s\n", argv[i], reason.c_str()); continue; }
            const auto facts = efmi::gpu::core_source_facts(registers);
            printf("%s %d %d %d.%d\n", argv[i], facts.vs_config_slot, facts.rolling_bones ? 1 : 0,
                   facts.config ? int(facts.config->set) : -1, facts.config ? int(facts.config->binding) : -1);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--srv-declarations")) {
        // D-124 as the backend answers it, per module: "<file> <t registers> <slot>:<type>.<dimension>.<stride>"
        // for each t register, "<slot>:-" where the SPIR-V does not decide it.
        for (int i = 2; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary | std::ios::ate);
            std::vector<uint32_t> code(in ? size_t(in.tellg()) / 4 : 0);
            if (in) { in.seekg(0); in.read(reinterpret_cast<char*>(code.data()), code.size() * 4); }
            std::vector<efmi::gpu::SourceRegister> registers; std::string reason;
            if (!efmi::gpu::source_registers(code, registers, reason)) { printf("%s refused %s\n", argv[i], reason.c_str()); continue; }
            uint32_t count = 0;
            for (const auto& r : registers) if (r.letter == 't') count = std::max(count, r.index + r.count);
            std::string line;
            for (uint32_t slot = 0; slot < count; ++slot) {
                uint32_t type = 0, dimension = 0, stride = 0; char item[48];
                if (srv_of(registers, slot, type, dimension, stride, reason) && type != efmi::frontend::kWordBufferDeclaration)
                    snprintf(item, sizeof(item), " %u:%u.%u.%u", slot, type, dimension, stride);
                else snprintf(item, sizeof(item), " %u:-", slot);
                line += item;
            }
            printf("%s %u%s\n", argv[i], count, line.c_str());
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--texture-roles")) {
        // D-082 as the backend reads a fragment module: "<file> t.set.binding.decoded.at ..." per sample.
        for (int i = 2; i < argc; ++i) {
            std::ifstream in(argv[i], std::ios::binary | std::ios::ate);
            std::vector<uint32_t> code(in ? size_t(in.tellg()) / 4 : 0);
            if (in) { in.seekg(0); in.read(reinterpret_cast<char*>(code.data()), code.size() * 4); }
            std::vector<efmi::gpu::RoleSample> samples; std::string reason;
            if (!efmi::gpu::role_samples(code, samples, reason)) { printf("%s refused %s\n", argv[i], reason.c_str()); continue; }
            std::string line;
            for (const auto& s : samples) {
                char item[64];
                snprintf(item, sizeof(item), "%s%u.%u.%u.%d.%u", line.empty() ? "" : " ", s.t, s.set, s.binding, s.decoded ? 1 : 0, s.at);
                line += item;
            }
            printf("%s %s\n", argv[i], line.c_str());
        }
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--plan-mods")) {
        // Offline dry run of the directory generation (D-078): every character Mod of the folder
        // read as the layer reads it, then the live plan of each of its sections in package order,
        // with the LOD the backend would pass. No GPU, so no owned runtime: the Mod's other INIs and
        // CustomShader sections are not part of these generations. What a replaced draw would bind.
        const std::filesystem::path mods = path_argument(2), core = path_argument(3), settings = path_argument(4);
        std::string reason;
        const auto vertex = common_vertex_registers();
        const auto inis = CharacterProgram::mods_characters(mods, reason);
        if (inis.empty()) { fprintf(stderr, "no character Mod: %s\n", reason.c_str()); return 1; }
        for (const auto& ini : inis) {
            CharacterProgram program;
            CharacterPackage package;
            CharacterProgram::Source source{std::filesystem::absolute(ini), std::filesystem::absolute(core), std::filesystem::absolute(settings),
                                            CharacterProgram::declared_namespace(ini), std::filesystem::absolute(mods), true};
            JsonWriter w; w.begin_object();
            w.kv("ini", ini.lexically_relative(mods).u8string());
            std::string why;
            const bool loaded = program.discover(source, package, why);
            w.kb("loaded", loaded);
            if (!loaded) {
                w.kv("reason", why);
                // What the generation discovered before it refused (a declaration that needs the GPU).
                w.key("sections"); w.begin_array();
                for (const auto& s : package.sections) {
                    char hash[16]; snprintf(hash, sizeof(hash), "%08x", s.hash);
                    w.begin_object(); w.kv("hash", hash); w.kn("count", s.match_count);
                    if (s.component != kCharacterNone) w.kn("component", s.component);
                    w.kb("planned", false); w.end_object();
                }
                w.end_array();
                w.end_object(); printf("%s\n", w.out.c_str()); continue;
            }
            w.kn("warnings", program.status().diagnostics);
            // The libraries this generation of one Mod called and those it left out, with why (D-112).
            w.key("library_notes"); w.begin_array(); for (const auto& note : program.library_notes()) w.str(note); w.end_array();
            uint64_t frame = 0;
            write_plans(w, program, program, package, vertex, frame);
            // The Mod's own enable state after the run (EFMI exports register at a Present and draw after).
            w.key("variables"); w.begin_object();
            for (const auto& v : program.status().variables)
                if (v.first.find("mod_enabled") != std::string::npos || v.first.find("object_detected") != std::string::npos ||
                    v.first.find("mod_id") != std::string::npos)
                    w.kv(v.first.c_str(), std::to_string(v.second));
            w.end_object();
            w.end_object();
            printf("%s\n", w.out.c_str());
        }
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--plan-folder-states"))
        return plan_folder_states(path_argument(2), path_argument(3), path_argument(4));
    if (argc == 5 && !strcmp(argv[1], "--plan-folder")) {
        // The same dry run through one generation for the whole Mods folder (D-131): every character
        // Mod in it, each planned through the shared generation with the draw naming its Mod.
        const std::filesystem::path mods = path_argument(2), core = path_argument(3), settings = path_argument(4);
        const auto vertex = common_vertex_registers();
        CharacterProgram::FolderSource source{std::filesystem::absolute(mods), std::filesystem::absolute(core), std::filesystem::absolute(settings)};
        source.declarations_only = true;
        std::vector<CharacterProgram::FolderMod> found;
        std::string reason;
        const bool loaded = CharacterProgram::discover_folder(source, found, reason);
        if (!loaded) fprintf(stderr, "folder generation refused: %s\n", reason.c_str());
        CharacterProgram* presenter = nullptr;
        for (auto& m : found) if (m.program) { presenter = m.program.get(); break; }
        uint64_t frame = 0;
        for (auto& m : found) {
            JsonWriter w; w.begin_object();
            w.kv("ini", m.ini.lexically_relative(mods).u8string());
            w.kb("loaded", loaded && m.program != nullptr);
            if (!loaded || !m.program) {
                w.kv("reason", loaded ? m.reason : reason);
                w.key("sections"); w.begin_array();
                for (const auto& s : m.package.sections) {
                    char hash[16]; snprintf(hash, sizeof(hash), "%08x", s.hash);
                    w.begin_object(); w.kv("hash", hash); w.kn("count", s.match_count);
                    if (s.component != kCharacterNone) w.kn("component", s.component);
                    w.kb("planned", false); w.end_object();
                }
                w.end_array();
                w.end_object(); printf("%s\n", w.out.c_str()); continue;
            }
            w.kn("warnings", m.program->status().diagnostics);
            // The libraries the Mod calls (their folders hold what the renderer compiles for them, D-113) and
            // those it was found not to call, with why.
            w.key("libraries"); w.begin_object();
            for (const auto& library : m.program->called_libraries()) w.kv(library.first.c_str(), library.second.u8string());
            w.end_object();
            w.key("library_notes"); w.begin_array(); for (const auto& note : m.program->library_notes()) w.str(note); w.end_array();
            write_plans(w, *m.program, *presenter, m.package, vertex, frame);
            w.key("variables"); w.begin_object();
            for (const auto& v : m.program->status().variables)
                if (v.first.find("mod_enabled") != std::string::npos || v.first.find("object_detected") != std::string::npos ||
                    v.first.find("mod_id") != std::string::npos)
                    w.kv(v.first.c_str(), std::to_string(v.second));
            w.end_object();
            w.end_object();
            printf("%s\n", w.out.c_str());
        }
        return loaded ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--inspect")) {
        if (!p.load(argv[2], why)) { fprintf(stderr, "package rejected: %s\n", why.c_str()); return 1; }
        printf("{\"bones\":%u,\"components\":%zu,\"streams\":%zu,\"sections\":%zu,\"draws\":%zu,\"payload_bytes\":%zu,\"required\":[",
               p.bones, p.components.size(), p.streams.size(), p.sections.size(), p.draws.size(), static_cast<size_t>(p.payload_bytes));
        for (size_t i = 0; i < p.lists.size(); ++i) printf("%s%llu", i ? "," : "", static_cast<unsigned long long>(p.lists[i].required));
        printf("],\"materials\":%s,\"images\":%zu,\"profiles\":%zu}\n", p.materials ? "true" : "false", p.images.size(), p.profiles.size());
        return 0;
    }
    {
        // D-079: the original Core's rules on a vertex module's registers.
        using efmi::gpu::SourceRegister;
        std::vector<SourceRegister> game = common_vertex_registers();
        auto facts = efmi::gpu::core_source_facts(game);
        check(facts.vs_config_slot == 2 && facts.rolling_bones && facts.config && facts.config->set == 2 && facts.bones->binding == 17,
              "the game's common vertex layout: the instance window at vs-cb2, the rolling bones at vs-t0");
        game[2].bytes = 448; game[1].bytes = 65536;
        facts = efmi::gpu::core_source_facts(game);
        check(facts.vs_config_slot == 1 && facts.rolling_bones, "Component_ReadConfig's order: vs-cb2, then vs-cb1, then vs-cb3");
        game[1].bytes = 3328;
        facts = efmi::gpu::core_source_facts(game);
        check(facts.vs_config_slot == -1 && !facts.rolling_bones, "no instance window: no facts, the Core leaves the mesh to the game");
        game[1].bytes = 65536; game[4].stride = 64;
        facts = efmi::gpu::core_source_facts(game);
        check(facts.vs_config_slot == 1 && !facts.rolling_bones, "vs-t0 of another element size is not the rolling bone buffer");
        game[4].stride = 16; game[4].kind = SourceRegister::ImageSrv;
        check(!efmi::gpu::core_source_facts(game).rolling_bones, "an image at vs-t0 is not the rolling bone buffer");
    }
    {
        // D-124: the DX11 declaration of a t register, as the original's ByteCodeReader reads it.
        using efmi::gpu::SourceRegister;
        auto t = [](uint32_t index, SourceRegister::Kind kind, uint32_t count = 1) {
            SourceRegister r; r.kind = kind; r.letter = 't'; r.index = index; r.count = count; return r;
        };
        std::vector<SourceRegister> shader = {t(0, SourceRegister::BufferSrv), t(1, SourceRegister::BufferSrv), t(2, SourceRegister::TexelSrv),
                                              t(3, SourceRegister::ImageSrv, 2), t(5, SourceRegister::ImageSrv), t(6, SourceRegister::ImageSrv),
                                              t(7, SourceRegister::ImageSrv), t(8, SourceRegister::ImageSrv)};
        shader[0].stride = 16;
        shader[1].stride = 4; shader[1].words = true;
        shader[3].dim = 1;
        shader[4].dim = 3; shader[4].arrayed = true;
        shader[5].dim = 1; shader[5].arrayed = true; shader[5].multisampled = true;
        shader[6].dim = 2;
        shader[7].dim = 2; shader[7].arrayed = true;
        auto is = [&](unsigned slot, uint32_t type, uint32_t dimension, uint32_t stride) {
            uint32_t a = 0, b = 0, c = 0; std::string why;
            return srv_of(shader, slot, a, b, c, why) && a == type && b == dimension && c == stride;
        };
        auto unknown = [&](unsigned slot) { uint32_t a = 0, b = 0, c = 0; std::string why; return !srv_of(shader, slot, a, b, c, why) && !why.empty(); };
        check(is(0, 2, 1, 16), "a read-only buffer of 16-byte elements is dcl_resource_structured with its stride (vs->srv_stride(0) == 16)");
        check(is(1, efmi::frontend::kWordBufferDeclaration, 1, 4), "a buffer of uint words is a byte address buffer or a structured buffer of uint: answered as one of the two");
        check(is(2, 1, 1, 0), "a texel buffer is a typed buffer view");
        check(is(3, 1, 4, 0) && is(4, 1, 4, 0), "each element of an image array declares its own typed 2D register");
        check(is(5, 1, 10, 0) && is(6, 1, 7, 0) && is(7, 1, 8, 0), "cube arrays, multisampled 2D arrays and 3D images by their view dimensions");
        check(unknown(8), "an arrayed 3D image has no DX11 view");
        check(is(9, 0, 0, 0) && is(59, 0, 0, 0), "a slot below t60 without a register is not declared, as the DX11 build declares none there");
        check(unknown(60) && unknown(127), "a slot from t60 up without a register is not known: the DX11 builds declare more than their SPIR-V there");
    }
    Model m;
    const auto valid = package(&m);
    check(p.decode(valid, why) && p.bones == 5 && p.components.size() == 2 && p.draws.size() == 2 && p.payload.size() == m.payload.size(),
          "valid package accepted");
    check(p.components[0].has_remap && p.components[0].remap_lod1 == std::vector<uint16_t>({1, 0}) && !p.components[1].has_remap,
          "LOD 1 remap read for the component that has one");
    check(p.lists[0].required == 3 && p.lists[2].required == 0 && p.lists[0].count == 2 && p.lists[2].count == 0,
          "a draw list requires exactly the components whose bones carry weight; a hidden component requires none");
    check(p.sections[0].match_first == 0 && p.sections[1].match_first == kCharacterNone && p.sections[1].lod == kCharacterNone,
          "sections keep an unspecified first index and an unassigned LOD");
    {
        // D-141: the original Core sizes its pools by the Mod's component count; 40 components load and a
        // list's bones of the 40th are a bit of its mask.
        CharacterPackage wide;
        check(wide.decode(package(nullptr, 38), why) && wide.components.size() == 40 && wide.lists[0].required == (1ull | 1ull << 39),
              "a character of 40 components loads, and its list requires the 40th component's bones");
    }
    const auto kept = p.payload;
    const size_t tables = 48, payload = 48 + m.tables.size();
    {
        auto hidden = valid;
        poke(hidden, tables + m.section_table + 12, kCharacterNone);
        poke(hidden, tables + m.section_table + 16, kCharacterNone);
        seal(hidden);
        CharacterPackage independent;
        check(independent.decode(hidden, why) && independent.sections[0].component == kCharacterNone,
              "exact IB-only hide does not need a fabricated bone component");
        poke(hidden, tables + m.section_table + 16, uint32_t(0)); seal(hidden);
        check(!independent.decode(hidden, why), "IB-only hide cannot change the character LOD");
    }
    auto reject = [&](std::vector<uint8_t> data, const char* text, bool reseal = true) {
        if (reseal) seal(data);
        std::string reason;
        check(!p.decode(data, reason) && !reason.empty() && p.payload == kept, text);
    };
    { auto d = valid; d[0] = 'X'; reject(d, "wrong magic refused", false); }
    { auto d = valid; d.back() ^= 1; reject(d, "checksum mismatch refused", false); }
    { auto d = valid; d.pop_back(); reject(d, "truncated file refused"); }
    { auto d = valid; d.push_back(0); reject(d, "trailing bytes refused"); }
    { auto d = valid; poke(d, 8, uint32_t(65536)); reject(d, "bone count past 16-bit indices refused"); }
    { auto d = valid; poke(d, tables + m.component_table + 24, uint32_t(0)); reject(d, "overlapping components refused"); }
    { auto d = valid; poke(d, tables + m.component_table + 24, uint32_t(5000)); reject(d, "a component past the skeleton refused"); }
    { auto d = valid; poke(d, tables + m.component_table + 24, uint32_t(3)); poke(d, tables + m.component_table + 28, uint32_t(2)); seal(d);
      CharacterPackage gap; std::string reason;
      check(gap.decode(d, reason) && gap.components.size() == 2 && gap.components[1].first == 3 && gap.components[1].count == 2,
            "bones between components stay as initialized, as the Core leaves them (BUG-048)"); }
    { auto d = valid; poke(d, tables + m.component_table + 12, uint32_t(9)); reject(d, "draw list index out of range refused"); }
    { auto d = valid; poke(d, tables + m.stream_table + 16, uint64_t(1) << 40); reject(d, "stream larger than the payload refused"); }
    { auto d = valid; poke(d, tables + m.stream_table + 32 + 4, uint32_t(3)); reject(d, "index stride other than 2 or 4 refused"); }
    { auto d = valid; poke(d, tables + m.stream_table + 4 * 32 + 24, uint32_t(12)); reject(d, "bone indices outside their element refused"); }
    { auto d = valid; poke(d, tables + m.stream_table + 4 * 32 + 28, uint32_t(4)); reject(d, "weights overlapping the indices refused"); }
    { auto d = valid; poke(d, tables + m.section_table + 12, uint32_t(2)); reject(d, "section naming a missing component refused"); }
    { auto d = valid; poke(d, tables + m.section_table + 16, uint32_t(2)); reject(d, "section with LOD 2 refused"); }
    { auto d = valid; poke(d, tables + m.list_table + 4, uint32_t(3)); reject(d, "draw list past the draws refused"); }
    { auto d = valid; poke(d, tables + m.draw_table + 4, uint32_t(4)); reject(d, "draw past its index stream refused"); }
    { auto d = valid; poke(d, tables + m.draw_table + 12, int32_t(1)); reject(d, "base vertex that leaves a vertex stream refused"); }
    { auto d = valid; poke(d, tables + m.draw_table + 12, int32_t(-1)); reject(d, "negative vertex index refused"); }
    { auto d = valid; poke(d, tables + m.draw_table + 16, uint32_t(1)); reject(d, "draw binding an index stream as vertices refused"); }
    { auto d = valid; poke(d, tables + m.edit_table + 8, uint32_t(VK_FORMAT_R32G32B32A32_UINT)); reject(d, "layout edit outside the rule refused"); }
    { auto d = valid; poke(d, payload + m.index_data + 8, uint32_t(3)); reject(d, "index past the vertex count refused"); }
    { auto d = valid; poke(d, payload + m.blend_a + 8, uint16_t(5)); reject(d, "weighted bone outside the skeleton refused"); }
    { auto d = valid; poke(d, payload + m.blend_a + 14, uint16_t(5)); reject(d, "unweighted bone outside the skeleton refused"); }
    { auto d = valid; poke(d, payload + m.blend_b, uint16_t(100)); reject(d, "weights that do not sum to one refused"); }
    { auto d = valid; poke(d, payload + m.remap_data, uint16_t(2000)); reject(d, "remap source outside the window refused"); }
    check(p.decode(valid, why), "valid package still accepted after the refusals");
    declaration_tests(p);
    payload_tests(valid, m);
    material_tests(valid);
    register_tests();
    texture_role_tests();
    check(!is_character_package("character-test-missing.bin"), "a missing file is not a character package");
    printf("character package: %d checks passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
