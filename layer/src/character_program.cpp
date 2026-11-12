#include "character_program.h"
#include "../../frontend/xxmi/character_core.h"
#include "../../frontend/assets/png.h"
#include "json.h"
#include <chrono>
#include <fstream>
#include <optional>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <string_view>

namespace efvk {
namespace {
// The folder of the module this code runs in (the layer DLL; a test program offline). Its configuration,
// settings, caches and log are never Mod content, also when a player unpacked the release into the Mods
// folder (EFMI\Mods\EFMI-Vulkan): its settings file is an INI the scans would otherwise load as a Mod's.
const std::filesystem::path& layer_folder()
{
    static const std::filesystem::path folder = [] {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&layer_folder), &module))
            return std::filesystem::path{};
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return std::filesystem::path{};
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }();
    return folder;
}
bool layer_folder_entry(const std::filesystem::directory_entry& entry)
{
    std::error_code error;
    const auto& own = layer_folder();
    return !own.empty() && !_wcsicmp(entry.path().filename().c_str(), own.filename().c_str()) && entry.is_directory(error) &&
           std::filesystem::equivalent(entry.path(), own, error);
}
// The enabled INI files under a folder in the original's order (IniHandler.cpp ParseIniFilesRecursive):
// in each folder its files named `.ini` (the four characters as they are: a `.INI` is not one to the
// original) sorted without regard to case, then its folders so sorted, each walked the same way; names
// starting with DISABLED and desktop.ini left out, as the installation's d3dx.ini excludes them
// (exclude_recursive, matched without regard to case), and the layer's own folder. `skip`, when given, is
// a folder left out with what is under it (the generation's settings, wherever they are kept).
void enabled_inis(const std::filesystem::path& folder, std::vector<std::filesystem::path>& out, const std::filesystem::path& skip = {},
                  unsigned depth = 0)
{
    if (depth > 64) return;
    const auto less = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
    std::set<std::wstring, decltype(less)> files(less), folders(less);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const auto name = entry.path().filename().wstring();
        if (!_wcsnicmp(name.c_str(), L"DISABLED", 8) || !_wcsicmp(name.c_str(), L"desktop.ini") || layer_folder_entry(entry)) continue;
        if (entry.is_directory(error)) {
            if (skip.empty() || !std::filesystem::equivalent(entry.path(), skip, error)) folders.insert(name);
        } else if (name.size() >= 4 && !wcscmp(name.c_str() + name.size() - 4, L".ini")) {
            files.insert(name);
        }
    }
    for (const auto& name : files) out.push_back(folder / name);
    for (const auto& name : folders) enabled_inis(folder / name, out, skip, depth + 1);
}
std::vector<std::filesystem::path> enabled_inis(const std::filesystem::path& folder, const std::filesystem::path& skip = {})
{
    std::vector<std::filesystem::path> out;
    enabled_inis(folder, out, skip);
    return out;
}
std::wstring key(const std::filesystem::path& path)
{
    auto result = path.lexically_normal().wstring();
    std::transform(result.begin(), result.end(), result.begin(), ::towlower); return result;
}
// A mesh stream by its file and how the generation declares it: the original makes a buffer per
// [Resource] declaration, so one file declared twice (another stride, or as index and vertex data) is
// two buffers, and here two streams (BUG-077).
std::wstring stream_key(const std::filesystem::path& path, bool index, uint32_t stride)
{
    return key(path) + (index ? L"|index|" : L"|vertex|") + std::to_wstring(stride);
}
// An image by its file and the loader flags of its declaration's colour space: the original makes a texture
// per declaration, and an sRGB one is another texture (D-140).
std::wstring image_key(const std::filesystem::path& path, uint32_t loader_flags)
{
    return loader_flags ? key(path) + L"|" + std::to_wstring(loader_flags) : key(path);
}
std::vector<uint8_t> read(const std::filesystem::path& path, uint64_t limit)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0 || uint64_t(in.tellg()) > limit) throw std::runtime_error("Runtime resource missing or over budget: " + path.u8string());
    std::vector<uint8_t> bytes(size_t(in.tellg())); in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!in) throw std::runtime_error("Runtime resource read failed");
    return bytes;
}
// Whether the original's loader makes a texture of this file (CustomResource::LoadFromFile, D-139): a .dds
// by its header (its levels inside the file), any other file by the system's WIC decoder (its frame 0,
// without decoding the pixels); within the size the layer's loader reads.
bool image_loads(const std::filesystem::path& path, std::string& why, uint32_t loader_flags, VkFormat& format)
{
    try {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in || in.tellg() <= 0) { why = "the file cannot be read"; return false; }
        const uint64_t size = uint64_t(in.tellg());
        if (size > (128ull << 20)) { why = "the file is larger than 128 MiB"; return false; }
        if (!_wcsicmp(path.extension().c_str(), L".dds")) {
            uint8_t header[efmi::assets::kDdsHeaderBytes]{};
            const size_t available = size_t(std::min<uint64_t>(size, sizeof(header)));
            in.seekg(0); in.read(reinterpret_cast<char*>(header), std::streamsize(available));
            if (!in) { why = "the file cannot be read"; return false; }
            efmi::assets::DdsImage image;
            if (!efmi::assets::decode_dds_header(header, available, size, image, why)) return false;
            format = loader_flags & efmi::assets::png_force_srgb ? efmi::assets::srgb_format(image.format) : image.format;
            return true;
        }
        const auto bytes = read(path, 128ull << 20);
        efmi::assets::PngPixels image;
        if (!efmi::assets::decode_wic(bytes.data(), bytes.size(), image, why, {}, loader_flags, false)) return false;
        format = image.metadata.format;
        return true;
    } catch (const std::exception& e) { why = e.what(); return false; }
}
// An INI's lines without comments, trimmed and in lower case (D-112).
std::string ini_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string line, text;
    while (std::getline(in, line)) {
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == ';') continue;
        text.append(line, start, std::string::npos); text += '\n';
    }
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
    return text;
}
// Whether the text has a section of this name (lower case), as the original names a section: the
// line up to its first `]`, without the spaces around.
bool has_section(const std::string& text, const char* wanted, bool prefix = false)
{
    const size_t length = strlen(wanted);
    for (size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        const auto line = std::string_view(text).substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (line.empty() || line[0] != '[') continue;
        auto name = line.substr(1, line.find(']') == std::string_view::npos ? std::string_view::npos : line.find(']') - 1);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.remove_suffix(1);
        if (prefix ? name.substr(0, length) == wanted : name == wanted) return true;
    }
    return false;
}
template<class T> void put(std::vector<uint8_t>& out, T value)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&value); out.insert(out.end(), p, p + sizeof(T));
}
// Route refreshed source bytes back through the existing complete package
// validator. It checks bone indices, weights, remaps, bounds and partitions.
bool validate_refreshed(CharacterPackage& package, std::string& reason)
{
    if (package.components.empty()) {
        // A package without a skeleton (D-174), which the package format does not hold: the checks decode
        // makes of streams and sections, and nothing that refers to bones.
        const uint64_t bytes = package.payload.size();
        if (package.bones || !package.lists.empty() || !package.draws.empty() || package.sections.empty() ||
            package.sections.size() > kCharacterSections || package.streams.size() > kCharacterStreams) {
            reason = "package without a skeleton outside its limits"; return false;
        }
        for (const auto& s : package.streams) {
            const bool stride_ok = s.kind == CharacterStream::Vertex ? (s.stride >= 1 && s.stride <= 256) :
                                   s.kind == CharacterStream::Index ? (s.stride == 2 || s.stride == 4) : false;
            if (!s.size || s.offset > bytes || s.size > bytes - s.offset || !stride_ok || s.size % s.stride ||
                (s.kind == CharacterStream::Index && s.offset % 4) || s.bone_index_offset != kCharacterNone || s.bone_weight_offset != kCharacterNone) {
                reason = "stream outside the payload, or of an unsupported stride"; return false;
            }
        }
        for (const auto& s : package.sections)
            if (!s.match_count || s.component != kCharacterNone || s.lod != kCharacterNone) { reason = "section out of range"; return false; }
        package.payload_bytes = bytes;
        return true;
    }
    std::vector<uint8_t> bytes{'E','F','M','I','C','H','0','1'};
    for (auto value : {package.bones, uint32_t(package.components.size()), uint32_t(package.streams.size()), uint32_t(package.sections.size()),
                      uint32_t(package.lists.size()), uint32_t(package.draws.size()), uint32_t(package.edits.size()), 0u}) put(bytes, value);
    put(bytes, uint64_t(package.payload.size()));
    for (const auto& c : package.components)
        for (auto value : {c.first, c.count, c.remap_stream, c.lists[0], c.lists[1], c.remap_stream0 == kCharacterNone ? 0u : c.remap_stream0 + 1})
            put(bytes, value);
    for (const auto& s : package.streams) {
        put(bytes, s.kind); put(bytes, s.stride); put(bytes, s.offset); put(bytes, s.size); put(bytes, s.bone_index_offset); put(bytes, s.bone_weight_offset);
    }
    for (const auto& s : package.sections)
        for (auto value : {s.hash, s.match_count, s.match_first, s.component, s.lod}) put(bytes, value);
    for (const auto& l : package.lists) { put(bytes, l.first); put(bytes, l.count); }
    for (const auto& d : package.draws) {
        put(bytes, d.index_stream); put(bytes, d.count); put(bytes, d.first); put(bytes, d.base);
        for (auto value : d.vb) put(bytes, value);
    }
    for (const auto& e : package.edits) { put(bytes, e.binding); put(bytes, uint32_t(e.source)); put(bytes, uint32_t(e.format)); }
    bytes.insert(bytes.end(), package.payload.begin(), package.payload.end());
    const auto checksum = crc32c(0, bytes.data() + 48, bytes.size() - 48); std::memcpy(bytes.data() + 36, &checksum, 4);
    CharacterPackage validated;
    if (!validated.decode(bytes, reason)) return false;
    for (size_t i = 0; i < validated.sections.size(); ++i) {
        validated.sections[i].stride_slot = package.sections[i].stride_slot; validated.sections[i].stride = package.sections[i].stride;
    }
    validated.materials = package.materials; validated.material_vendor = package.material_vendor; validated.material_device = package.material_device;
    validated.images = std::move(package.images); validated.profiles = std::move(package.profiles); validated.pixels = std::move(package.pixels);
    package = std::move(validated); return true;
}
}
bool CharacterProgram::configured(const std::string& path)
{
    return std::filesystem::exists(std::filesystem::u8path(path + ".runtime.json"));
}
bool CharacterProgram::init(const std::string& path, CharacterPackage& package, std::string& reason, const GpuFactory& factory)
{
    try {
        const auto manifest = std::filesystem::u8path(path + ".runtime.json");
        const auto contents = read(manifest, 1 << 20);
        JsonRequest config;
        if (!config.parse(std::string(contents.begin(), contents.end())) || config.get("schema") != "1") throw std::runtime_error("Invalid runtime sidecar schema");
        const auto ini = std::filesystem::u8path(config.get("ini"));
        const auto core = std::filesystem::u8path(config.get("core"));
        if (!ini.is_absolute() || !core.is_absolute()) throw std::runtime_error("Runtime INI/Core paths must be absolute");
        std::vector<uint8_t> payload;
        std::vector<CharacterSource> files;
        std::map<std::wstring, uint32_t> remaps;
        for (uint32_t i = 0; i < package.streams.size(); ++i) {
            auto& s = package.streams[i];
            const auto source = std::filesystem::u8path(config.get(("stream_" + std::to_string(i)).c_str()));
            files.push_back({source.u8string(), 0, 0});
            if (!source.is_absolute()) throw std::runtime_error("Runtime stream catalogue is incomplete");
            auto data = read(source, 128ull << 20);
            if (data.empty() || data.size() % s.stride || payload.size() + data.size() > (512ull << 20)) throw std::runtime_error("Runtime mesh size or stride invalid");
            while (payload.size() % 16) payload.push_back(0);
            s.offset = payload.size(); s.size = data.size(); payload.insert(payload.end(), data.begin(), data.end());
            if (s.kind != CharacterStream::Remap && !streams_.emplace(stream_key(source, s.kind == CharacterStream::Index, s.stride), i).second)
                throw std::runtime_error("Duplicate runtime stream catalogue entry");
            if (s.kind == CharacterStream::Remap) remaps.emplace(key(source), i);
        }
        package.payload = std::move(payload);
        if (!validate_refreshed(package, reason)) return false;
        if (!config.get("rabbit_textures").empty())
            throw std::runtime_error("RabbitFX texture-role inspection is not a complete runtime shader contract");
        auto facts = std::make_shared<efmi::frontend::CharacterDiscovery>();
        if (!load(ini, core, manifest.parent_path() / "settings", config.get("settings", "mod"),
                  efmi::frontend::character_core(config.get("namespace"), package, facts), factory, reason)) return false;
        identify_components_ = facts->identify_components;
        element_offsets_ = facts->element_offsets;
        efmittl_ = facts->efmittl;
        gaps_ = facts->gaps; // before the catalogue adds its own
        catalogue(package, remaps);
        // Validated and catalogued: the bytes are read again from the files when needed (ISSUE-011).
        return package.release_payload(std::move(files), reason);
    } catch (const std::exception& e) { reason = e.what(); return false; }
}
bool CharacterProgram::load(const std::filesystem::path& ini, const std::filesystem::path& core, const std::filesystem::path& settings_folder,
                            const std::string& settings_name, std::shared_ptr<efmi::frontend::RuntimeExtension> extension,
                            const GpuFactory& factory, std::string& reason, const std::filesystem::path& mods, bool declarations_only)
{
    // The original's root, from which an INI without a namespace line is named: the Mods folder's parent;
    // a Mod given without its Mods folder is named by its file, from its own folder.
    efmi::frontend::RuntimeConfig options{mods.empty() ? ini.parent_path() : mods.parent_path(),
                                         {core / "API.ini", core / "MergedSkeleton.ini", core / "SpatialIdentification.ini", core / "Helpers.ini", ini},
                                         settings_folder, settings_name};
    if (!mods.empty()) options.native_library_files = native_library_files(mods);
    // The installation's d3dx.ini sits beside the Mods folder; without one, above the Core.
    options.installation = mods.empty() ? core.parent_path().parent_path() : mods.parent_path();
    if (factory) gpu_ = factory(core.parent_path().parent_path(), gpu_reason_);
    if (gpu_ || declarations_only) {
        // The original loads every enabled INI of the Mod folder, in its order; the generation's own
        // settings are never a Mod input, wherever they are kept. Offline, with the declarations only,
        // the same files (their CustomShader sections declared, not compiled), so a Mod alone and in
        // the folder dry run declare the same images (D-155).
        std::error_code error;
        for (const auto& other : enabled_inis(ini.parent_path(), options.settings_directory))
            if (!std::filesystem::equivalent(other, ini, error)) options.inputs.push_back(other);
        // The libraries the Mod calls by namespace from elsewhere in the Mods folder (D-112), as the
        // original, which loads the whole folder into one space, finds them.
        library_notes_.clear(); libraries_.clear();
        if (!mods.empty()) options.libraries = libraries(mods, options.inputs, efmi::frontend::character_core_libraries(), library_notes_);
        for (const auto& library : options.libraries) libraries_[declared_namespace(library)] = library;
    }
    efmi::frontend::character_core_version(*extension, core_version(core));
    return start(options, std::move(extension), declarations_only, reason);
}
bool CharacterProgram::start(efmi::frontend::RuntimeConfig& options, std::shared_ptr<efmi::frontend::RuntimeExtension> extension,
                             bool declarations_only, std::string& reason)
{
    // The layer answers texture queries of replaced draws (D-077).
    options.texture_identity = true;
    if (gpu_) {
        extension = gpu_->combine(std::move(extension));
        gpu_->begin_deferred();
    } else if (declarations_only) {
        extension = efmi::frontend::declarations_only(std::move(extension));
    }
    runtime_ = efmi::frontend::IniRuntime::load(options, extension, reason);
    if (gpu_) {
        if (!runtime_ && gpu_->scope_active()) gpu_->discard();
        // Recorded work stays pending; replaced draws keep the plan until the first Present.
        if (runtime_ && gpu_->scope_active()) gpu_->suspend();
    }
    return bool(runtime_);
}
bool CharacterProgram::owns(const std::filesystem::path& file) const
{
    if (folders_.empty()) return true;
    const auto name = key(file);
    const auto under = [&](const std::vector<std::wstring>& folders) {
        return std::any_of(folders.begin(), folders.end(), [&](const std::wstring& folder) { return !name.compare(0, folder.size(), folder); });
    };
    return under(folders_) && !under(foreign_);
}
bool CharacterProgram::image_checked(const std::filesystem::path& path, uint32_t loader_flags)
{
    if (unloadable_.count(key(path))) return false;
    std::string why;
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (image_loads(path, why, loader_flags, format)) { image_formats_[image_key(path, loader_flags)] = format; return true; }
    // A file the original's loader makes no texture of stays its null resource as well (D-139).
    unloadable_.insert(key(path));
    if (gaps_.size() < 16)
        gaps_.push_back("texture file " + path.filename().u8string() + " does not load (" + why +
                        "): what binds it binds nothing, as the original's null resource");
    return false;
}
bool CharacterProgram::image_unloadable(const std::string& path) const
{
    return unloadable_.count(key(std::filesystem::u8path(path))) != 0;
}
void CharacterProgram::catalogue(CharacterPackage& package, const std::map<std::wstring, uint32_t>& remaps)
{
    {
        for (uint32_t i = 0; i < package.images.size(); ++i) {
            const auto path = std::filesystem::u8path(package.images[i]);
            const uint32_t flags = i < package.image_flags.size() ? package.image_flags[i] : 0u;
            images_.emplace(image_key(path, flags), i);
            image_checked(path, flags);
        }
        for (const auto& file : runtime_->resources()) {
            if (!owns(file->path)) continue; // another Mod's of a folder generation
            if (file->usage) {
                const auto stream_name = stream_key(file->path, file->usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT, file->stride);
                const auto found = streams_.find(stream_name);
                if (found == streams_.end()) {
                    if (remaps.count(key(file->path))) continue; // Validated by the native Core palette reader.
                    if (absent_.count(stream_name)) continue;     // binds nothing (BUG-077)
                    // A mesh of sections the renderer has no part for (another INI of the Mod folder,
                    // a LOD beyond the packager's scope) stays with the game's draws, which are not
                    // planned; a replaced draw that binds it refuses in plan().
                    ++outside_meshes_; continue;
                }
                const auto& stream = package.streams[found->second];
                if (file->stride != stream.stride || (file->usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != (stream.kind == CharacterStream::Index))
                    throw std::runtime_error("Original file declaration differs from the renderer stream profile");
            } else if (std::error_code error; !std::filesystem::exists(file->path, error)) {
                continue; // The original's load of it fails and it binds nothing (image_index, BUG-076).
            } else if (!images_.count(image_key(file->path, file->loader_flags)) && !unloadable_.count(key(file->path))) {
                if (!image_checked(file->path, file->loader_flags)) continue;
                if (package.images.size() >= kCharacterImages) throw std::runtime_error("Runtime image catalogue exceeds budget");
                package.image_flags.resize(package.images.size(), 0u);
                images_.emplace(image_key(file->path, file->loader_flags), uint32_t(package.images.size()));
                package.images.push_back(file->path.u8string()); package.image_flags.push_back(file->loader_flags);
            }
        }
        index_bounds_.resize(package.streams.size());
        index_blocks_.assign(package.streams.size(), {});
        exact_ranges_.clear();
        for (size_t s = 0; s < package.streams.size(); ++s) {
            const auto& stream = package.streams[s]; if (stream.kind != CharacterStream::Index) continue;
            uint32_t low = UINT32_MAX, high = 0;
            auto& blocks = index_blocks_[s];
            uint64_t n = 0;
            for (uint64_t at = 0; at < stream.size; at += stream.stride, ++n) {
                uint32_t v = 0; std::memcpy(&v, package.payload.data() + stream.offset + at, stream.stride);
                low = std::min(low, v); high = std::max(high, v);
                if (n % kIndexBlock == 0) blocks.push_back({v, v});
                else { blocks.back().first = std::min(blocks.back().first, v); blocks.back().second = std::max(blocks.back().second, v); }
            }
            index_bounds_[s] = {low, high};
        }
    }
}
std::string CharacterProgram::settings_name(const std::filesystem::path& ini, const std::string& declared)
{
    // The settings file is the namespace's and the Mod folder's: two Mods of one character declare
    // the same namespace (`namespace = yvonne`), and the toggles persisted by the one must not come
    // up in the other when it takes its place (BUG-039). The original keeps one d3dx_user.ini by
    // namespace alone and inherits them.
    std::string name = declared;
    if (name.empty()) {
        const auto file = ini.filename().wstring();
        for (wchar_t c : file) name += c < 0x80 ? static_cast<char>(towlower(c)) : '_';
    }
    std::string tag;
    for (wchar_t c : ini.parent_path().filename().wstring())
        tag += c < 0x80 && (isalnum(static_cast<int>(c)) || c == L'_' || c == L'-') ? static_cast<char>(c) : '_';
    if (!tag.empty()) name += "-" + tag;
    for (char& c : name) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    if (name.size() > 80) name.resize(80);
    return name;
}
std::string CharacterProgram::declared_namespace(const std::filesystem::path& ini)
{
    const auto bytes = read(ini, 32u << 20);
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line.erase(0, 3);
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == ';') continue;
        if (line[start] == '[') return {}; // the namespace line precedes every section
        const auto equals = line.find('=', start);
        if (equals == std::string::npos) continue;
        std::string name = line.substr(start, equals - start);
        name.erase(name.find_last_not_of(" \t") + 1);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name != "namespace") continue;
        std::string value = line.substr(equals + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return value;
    }
    return {};
}
// The version the installed Core declares: `global $version = N` of its main.ini, which the generation
// does not load (its GUI); 0 when the file or the line is not there.
float CharacterProgram::core_version(const std::filesystem::path& core)
{
    const std::string text = ini_text(core / "main.ini");
    for (size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (line.compare(0, 6, "global")) continue;
        const auto name = line.find("$version");
        const auto equals = line.find('=');
        if (name == std::string::npos || equals == std::string::npos || name > equals) continue;
        if (line.find_first_not_of(" \t", name + 8) != equals) continue; // `$version_x`, `$version2`
        char* stop = nullptr;
        const float value = strtof(line.c_str() + equals + 1, &stop);
        if (stop != line.c_str() + equals + 1 && std::isfinite(value) && value > 0) return value;
    }
    return 0;
}
std::vector<std::filesystem::path> CharacterProgram::mods_characters(const std::filesystem::path& mods, std::string& reason)
{
    // In the original's include order, which is the order of the packages and of their sections' candidates.
    std::vector<std::filesystem::path> found;
    std::error_code error;
    if (!std::filesystem::is_directory(mods, error)) { reason = "cannot read the Mods folder " + mods.u8string() + ": " + error.message(); return found; }
    for (const auto& ini : enabled_inis(mods))
        if (has_section(ini_text(ini), "commandlistinitializemergedskeleton")) found.push_back(ini);
    return found;
}
// The installed files of the libraries the runtime provides natively (RabbitFX, D-082, D-133): enabled
// INIs of the Mods folder that declare one of their namespaces.
std::vector<std::filesystem::path> CharacterProgram::native_library_files(const std::filesystem::path& mods)
{
    const auto native = efmi::frontend::character_core_libraries();
    std::vector<std::filesystem::path> found;
    for (const auto& ini : enabled_inis(mods)) {
        const auto name = declared_namespace(ini);
        if (!name.empty() && std::find(native.begin(), native.end(), name) != native.end()) found.push_back(ini);
    }
    return found;
}
std::vector<std::filesystem::path> CharacterProgram::libraries(const std::filesystem::path& mods, const std::vector<std::filesystem::path>& inputs,
                                                              const std::vector<std::string>& known, std::vector<std::string>& notes,
                                                              bool one_generation)
{
    std::set<std::string> declared(known.begin(), known.end());
    std::string calls;
    for (const auto& input : inputs) {
        calls += ini_text(input);
        const auto name = declared_namespace(input);
        if (!name.empty()) declared.insert(name);
    }
    // Most generations call nothing beyond their own INIs and the Core: the Mods folder is read only
    // when a reference names a namespace no input declares (by its first part; `CommandList\NS\Name`).
    std::set<std::string> heads;
    for (const auto& name : declared) heads.insert(name.substr(0, name.find('\\')));
    bool unknown = false;
    for (const char* prefix : {"commandlist\\", "customshader\\", "resource\\", "$\\"}) {
        const std::string p = prefix;
        for (size_t at = calls.find(p); !unknown && at != std::string::npos; at = calls.find(p, at + 1)) {
            const auto from = at + p.size(), end = calls.find_first_of("\\\n", from);
            if (end != std::string::npos && calls[end] == '\\' && !heads.count(calls.substr(from, end - from))) unknown = true;
        }
    }
    if (!unknown) return {};
    // Every enabled INI of the folder: those of character Mods mark their folders, whose INIs belong to
    // that Mod's generation; the others are libraries by their declared namespace.
    struct Candidate { std::filesystem::path path; std::string name, text; bool done = false; };
    std::vector<Candidate> candidates;
    std::vector<std::filesystem::path> characters;
    std::error_code error;
    if (!std::filesystem::is_directory(mods, error)) { notes.push_back("libraries: cannot read the Mods folder " + mods.u8string() + ": " + error.message()); return {}; }
    for (const auto& ini : enabled_inis(mods)) {
        auto text = ini_text(ini);
        if (has_section(text, "commandlistinitializemergedskeleton")) { characters.push_back(ini.parent_path()); continue; }
        auto name = declared_namespace(ini);
        if (!name.empty() && !declared.count(name)) candidates.push_back({ini, std::move(name), std::move(text)});
    }
    const auto inside = [](const std::filesystem::path& path, const std::filesystem::path& folder) {
        const auto relative = path.lexically_relative(folder);
        return !relative.empty() && *relative.begin() != "..";
    };
    std::vector<std::filesystem::path> taken;
    for (bool grew = true; grew;) {
        grew = false;
        for (auto& c : candidates) {
            if (c.done || calls.find("\\" + c.name + "\\") == std::string::npos) continue;
            c.done = true;
            if (std::any_of(characters.begin(), characters.end(), [&](const auto& folder) { return inside(c.path, folder); })) {
                notes.push_back("library not called: " + c.path.u8string() + " (namespace " + c.name + ") belongs to another character Mod");
                continue;
            }
            // A section acting on the game's resources by hash would act once for every generation that
            // loads the library, where the original acts once. In the folder's one generation the library is
            // one of its inputs and acts once, as in the original (D-155): the Mod calls it all the same.
            if (!one_generation && has_section(c.text, "textureoverride", true)) {
                notes.push_back("library not called: " + c.path.u8string() + " (namespace " + c.name + ") acts on game resources by hash");
                continue;
            }
            if (taken.size() >= 16) { notes.push_back("library not called: " + c.path.u8string() + ": more than 16 libraries"); continue; }
            notes.push_back("library called: " + c.path.u8string() + " (namespace " + c.name + ")");
            taken.push_back(c.path);
            calls += c.text;
            grew = true;
        }
    }
    return taken;
}
bool CharacterProgram::discover(const Source& source, CharacterPackage& package, std::string& reason, const GpuFactory& factory)
{
    try {
        if (!source.ini.is_absolute() || !source.core.is_absolute() || !source.settings.is_absolute())
            throw std::runtime_error("Directory generation paths must be absolute");
        // Without a namespace line the parser names the INI by its path from the original's root (the
        // Mods folder's parent, where d3dx.ini is), in any script, in lower case as it lowers names; a
        // Mod given without its Mods folder by its file name. The settings file gets a name of plain
        // characters.
        std::string mod_namespace = source.mod_namespace;
        if (mod_namespace.empty()) {
            auto name = (source.mods.empty() ? source.ini.filename() : source.ini.lexically_relative(source.mods.parent_path())).wstring();
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            mod_namespace = std::filesystem::path(name).u8string();
        }
        const std::string settings_name = CharacterProgram::settings_name(source.ini, mod_namespace);
        auto found = std::make_shared<efmi::frontend::CharacterDiscovery>();
        if (!load(source.ini, source.core, source.settings, settings_name,
                  efmi::frontend::character_discovery(mod_namespace, found), factory, reason, source.mods, source.declarations_only)) {
            // Offline the sections are known before a declaration that needs the GPU refuses: kept for
            // the dry run's listing (the capture replay builds its draws from them).
            if (source.declarations_only) { package = {}; package.sections = found->declared; }
            return false;
        }
        return adopt(*found, package, reason);
    } catch (const std::exception& e) { reason = e.what(); return false; }
}
bool CharacterProgram::adopt(efmi::frontend::CharacterDiscovery& found, CharacterPackage& package, std::string& reason)
{
    try {
        CharacterPackage made = found.contract;
        made.lists.clear();
        for (auto& c : made.components) {
            c.lists[0] = uint32_t(made.lists.size()); made.lists.push_back({});
            c.lists[1] = uint32_t(made.lists.size()); made.lists.push_back({});
        }
        // The blend-index edit is the one the live plans accept (CharacterProgram::plan).
        made.edits.push_back({2, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16G16B16A16_UINT});
        std::map<std::wstring, uint32_t> remaps, meshes;
        std::vector<uint8_t> payload;
        std::vector<CharacterSource> files(made.streams.size());
        // A file the original's buffer could not be made from (not there, or empty) gives no stream: a draw
        // that binds it binds nothing, as the original's null resource (BUG-077).
        auto add = [&](const std::filesystem::path& path, uint32_t kind, uint32_t stride) -> uint32_t {
            std::error_code error;
            if (!stride || !std::filesystem::is_regular_file(path, error)) return kCharacterNone;
            auto data = read(path, 128ull << 20);
            // The original's buffer holds the whole file; a last element the file holds only part of is fetched out
            // of bounds and reads zero: the stream takes the whole elements from the file and zero for that one.
            const uint64_t whole = data.size() / stride * stride;
            const uint64_t elements = (data.size() + stride - 1) / stride * stride;
            data.resize(whole);
            data.resize(elements, 0);
            if (data.empty()) return kCharacterNone;
            if (payload.size() + data.size() > (512ull << 20)) throw std::runtime_error("Mesh size or stride invalid: " + path.u8string());
            if (made.streams.size() >= kCharacterStreams) throw std::runtime_error("More mesh files than one character holds");
            while (payload.size() % 16) payload.push_back(0);
            CharacterStream s;
            s.kind = kind; s.stride = stride; s.offset = payload.size(); s.size = data.size();
            payload.insert(payload.end(), data.begin(), data.end());
            made.streams.push_back(s);
            files.push_back({path.u8string(), 0, 0, whole});
            return uint32_t(made.streams.size() - 1);
        };
        const auto remap_stream = [&](const std::filesystem::path& file) {
            const auto k = key(file);
            auto it = remaps.find(k);
            if (it == remaps.end()) it = remaps.emplace(k, add(file, CharacterStream::Remap, 2)).first;
            if (it->second == kCharacterNone) throw std::runtime_error("LOD remap file unreadable: " + file.u8string());
            return it->second;
        };
        for (size_t i = 0; i < made.components.size(); ++i) {
            if (i < found.remap_files.size() && !found.remap_files[i].empty()) made.components[i].remap_stream = remap_stream(found.remap_files[i]);
            if (i < found.remap_files0.size() && !found.remap_files0[i].empty()) made.components[i].remap_stream0 = remap_stream(found.remap_files0[i]);
        }
        // Every mesh file the generation declares (a folder Mod: under its folders); those of sections
        // the renderer has no part for are never bound by a replaced draw. In the order of their files,
        // whatever order the generation keeps its resources in: the same files make the same mesh, which
        // a successor generation keeps on the device (BUG-061).
        struct Declared { std::filesystem::path path; uint32_t kind, stride; };
        std::map<std::wstring, Declared> declared;
        for (const auto& file : runtime_->resources()) {
            if (!file->usage || remaps.count(key(file->path))) continue;
            const bool index = file->usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            // A file that is not there binds nothing whoever declares it; the original resolves it to its own
            // folder when the Mod's has none (IniHandler.cpp), outside every Mod's (BUG-076, BUG-077).
            if (std::error_code error; !std::filesystem::is_regular_file(file->path, error)) {
                if (absent_.insert(stream_key(file->path, index, file->stride)).second)
                    library_notes_.push_back("INI mesh file missing; a draw that binds it binds nothing, as the original's null buffer: " +
                                             file->path.u8string());
                continue;
            }
            if (!owns(file->path)) continue;
            declared.emplace(stream_key(file->path, index, file->stride),
                             Declared{file->path, index ? uint32_t(CharacterStream::Index) : uint32_t(CharacterStream::Vertex), file->stride});
        }
        for (const auto& file : declared) {
            const uint32_t stream = add(file.second.path, file.second.kind, file.second.stride);
            meshes.emplace(file.first, stream);
            if (stream == kCharacterNone)
                library_notes_.push_back("INI mesh file empty; a draw that binds it binds nothing, as the original's null buffer: " +
                                         file.second.path.u8string());
        }
        made.payload = std::move(payload);
        made.materials = true;
        if (!validate_refreshed(made, reason)) return false;
        for (const auto& mesh : meshes) {
            if (mesh.second != kCharacterNone) streams_.emplace(mesh.first, mesh.second);
            else absent_.insert(mesh.first);
        }
        gaps_ = found.gaps; // before the catalogue adds its own
        catalogue(made, remaps);
        // Validated and catalogued: as the original frees a buffer's file bytes once the resource holds
        // them, the package keeps where they are and reads them again when it needs them (ISSUE-011).
        if (!made.release_payload(std::move(files), reason)) return false;
        package = std::move(made);
        left_to_game_ = found.left_to_game;
        identify_components_ = found.identify_components;
        element_offsets_ = found.element_offsets;
        efmittl_ = found.efmittl;
        return true;
    } catch (const std::exception& e) { reason = e.what(); return false; }
}
namespace {
std::wstring folder_key(const std::filesystem::path& folder)
{
    auto name = key(folder);
    if (!name.empty() && name.back() != L'\\') name += L'\\';
    return name;
}
}
bool CharacterProgram::discover_folder(const FolderSource& source, std::vector<FolderMod>& out, std::string& reason, const GpuFactory& factory)
{
    try {
        out.clear();
        if (!source.mods.is_absolute() || !source.core.is_absolute() || !source.settings.is_absolute())
            throw std::runtime_error("Folder generation paths must be absolute");
        // The original's root: d3dx.ini's folder, the Mods folder's parent; every INI is named by its path from it.
        const auto root = source.mods.parent_path();
        // None is a folder as any other (D-174): its INIs load, and those with mesh sections have packages.
        reason.clear();
        const auto inis = mods_characters(source.mods, reason);
        if (!reason.empty()) return false;
        // Per character Mod: its namespace (declared, else the original's implicit one, the INI's path from
        // the root), its INIs and the libraries it calls.
        std::vector<std::vector<std::filesystem::path>> mod_inputs(inis.size());
        std::vector<std::map<std::string, std::filesystem::path>> called(inis.size());
        std::vector<std::vector<std::string>> notes(inis.size());
        const bool gpu = bool(factory);
        std::vector<std::wstring> names(inis.size());
        for (size_t i = 0; i < inis.size(); ++i) {
            FolderMod m; m.ini = inis[i]; m.mod_namespace = declared_namespace(inis[i]);
            // The namespace as the parser names the INI's sections: the declared one (UTF-8), else the
            // INI's path from the root, as the original's include names it; in lower case as it lowers them.
            std::wstring name;
            if (!m.mod_namespace.empty()) {
                const int n = MultiByteToWideChar(CP_UTF8, 0, m.mod_namespace.data(), int(m.mod_namespace.size()), nullptr, 0);
                name.resize(size_t(std::max(n, 0)));
                if (n) MultiByteToWideChar(CP_UTF8, 0, m.mod_namespace.data(), int(m.mod_namespace.size()), name.data(), n);
            } else {
                name = inis[i].lexically_relative(root).wstring();
                m.mod_namespace = inis[i].lexically_relative(root).u8string();
            }
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            names[i] = std::move(name);
            mod_inputs[i].push_back(inis[i]);
            // The libraries the Mod calls by namespace (D-112) from any INI of its folder: a fact of its text,
            // whatever runtime runs it. Every enabled INI is an input of the folder's generation (D-155), so the
            // library is loaded there in any case; what is recorded here is the Mod's use of it (the folder its
            // shaders and files are read from, BUG-088's fourth cause).
            std::error_code error;
            for (const auto& other : enabled_inis(inis[i].parent_path(), source.settings))
                if (!std::filesystem::equivalent(other, inis[i], error)) mod_inputs[i].push_back(other);
            for (const auto& library : libraries(source.mods, mod_inputs[i], efmi::frontend::character_core_libraries(), notes[i], true))
                called[i][declared_namespace(library)] = library;
            out.push_back(std::move(m));
        }
        // Two enabled INIs under one namespace (a second Mod for one character): the original parses both in
        // its include order, keeps the first file's sections and warns that the second's duplicate them. The
        // second is left out and told. Before, each loaded alone, the folder refused, and no Mod loaded.
        std::vector<bool> left_out(inis.size(), false);
        for (size_t i = 0; i < inis.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (!left_out[j] && names[j] == names[i]) {
                    left_out[i] = true;
                    out[i].reason = "its namespace is the one " + inis[j].lexically_relative(source.mods).u8string() +
                                    " declares before it: the original keeps the first file's sections and warns about this one's";
                    break;
                }
        // Inputs that are no character Mod's and that the folder does not load with, by their key.
        std::map<std::wstring, std::pair<std::filesystem::path, std::string>> left_plain;
        struct Attempt {
            std::unique_ptr<CharacterProgram> loader;
            std::vector<std::pair<std::wstring, std::shared_ptr<efmi::frontend::CharacterDiscovery>>> mods;
            std::vector<size_t> members;
            std::vector<std::filesystem::path> plain; // the inputs outside every kept character Mod's folder
            std::vector<std::filesystem::path> inputs; // every Mods folder input, in the original's order
            // The mesh sections of INIs outside every character Mod's folder (D-174).
            std::shared_ptr<std::vector<efmi::frontend::BesideSection>> rest = std::make_shared<std::vector<efmi::frontend::BesideSection>>();
            std::string reason;
            bool loaded = false;
        };
        const float version = core_version(source.core);
        // One load of the folder without the Mods marked in `without` and the plain inputs in `without_plain`.
        // Every enabled INI of the Mods folder is an input, in the original's include order, as its
        // include_recursive takes them (D-155); not the files of the native libraries (their key and
        // ShaderRegex sections join through native_library_files, the rest is the interface's), and nothing
        // of a character Mod left out (its INI; its folder's other INIs unless a kept Mod shares the folder).
        auto attempt = [&](const std::vector<bool>& without, const std::map<std::wstring, std::pair<std::filesystem::path, std::string>>& without_plain) {
            Attempt a;
            std::set<std::wstring> excluded, kept_folders;
            std::vector<std::wstring> excluded_folders, character_folders;
            for (size_t i = 0; i < inis.size(); ++i)
                if (!without[i]) kept_folders.insert(folder_key(inis[i].parent_path()));
            for (size_t i = 0; i < inis.size(); ++i) {
                const auto folder = folder_key(inis[i].parent_path());
                if (without[i]) {
                    if (kept_folders.count(folder)) excluded.insert(key(inis[i]));
                    else excluded_folders.push_back(folder);
                    continue;
                }
                a.members.push_back(i);
                a.mods.push_back({names[i], std::make_shared<efmi::frontend::CharacterDiscovery>()});
                character_folders.push_back(folder);
            }
            efmi::frontend::RuntimeConfig options{root, {source.core / "API.ini", source.core / "MergedSkeleton.ini", source.core / "SpatialIdentification.ini",
                                                         source.core / "Helpers.ini"}, source.settings, source.settings_name};
            options.native_library_files = native_library_files(source.mods);
            for (const auto& native : options.native_library_files) excluded.insert(key(native));
            options.installation = root;
            const auto under = [](const std::wstring& name, const std::vector<std::wstring>& folders) {
                return std::any_of(folders.begin(), folders.end(), [&](const std::wstring& folder) { return !name.compare(0, folder.size(), folder); });
            };
            for (const auto& ini : enabled_inis(source.mods, source.settings)) {
                const auto name = key(ini);
                if (excluded.count(name) || without_plain.count(name) || under(name, excluded_folders)) continue;
                options.inputs.push_back(ini);
                a.inputs.push_back(ini);
                if (!under(name, character_folders)) a.plain.push_back(ini);
            }
            a.loader = std::make_unique<CharacterProgram>();
            if (gpu) a.loader->gpu_ = factory(source.core.parent_path().parent_path(), a.loader->gpu_reason_);
            auto extension = efmi::frontend::character_folder(a.mods, a.rest);
            efmi::frontend::character_core_version(*extension, version);
            a.loaded = a.loader->start(options, std::move(extension), source.declarations_only, a.reason);
            return a;
        };
        // A failure stays with its unit (docs/architecture-review-2.1.md, R-10): the Mods the generation
        // names itself are left out and the folder loads again; then those that refuse in a generation of
        // their own; then, when every Mod loads alone and the folder still does not, the input the folder
        // does not load with is searched for by halving (plain inputs first, then the Mods).
        size_t loads = 0;
        std::string folder_reason;
        bool tried_alone = false;
        for (;;) {
            Attempt a = attempt(left_out, left_plain); ++loads;
            if (a.loaded) {
                for (size_t k = 0; k < a.members.size(); ++k) {
                    const size_t i = a.members[k];
                    auto program = std::make_unique<CharacterProgram>();
                    program->runtime_ = a.loader->runtime_; program->gpu_ = a.loader->gpu_; program->gpu_reason_ = a.loader->gpu_reason_;
                    program->folder_index_ = uint32_t(k);
                    program->libraries_ = called[i]; program->library_notes_ = notes[i];
                    program->folders_ = {folder_key(out[i].ini.parent_path()), folder_key(source.core)};
                    for (const auto& library : called[i]) program->folders_.push_back(folder_key(library.second.parent_path()));
                    out[i].reason.clear();
                    if (program->adopt(*a.mods[k].second, out[i].package, out[i].reason)) out[i].program = std::move(program);
                }
                // The packages without a skeleton (D-174): the mesh sections outside every character Mod's
                // folder, by the folder of their INI, in the order of the folders' first INIs. A package's
                // files are those under its folder that are no other package's.
                std::map<std::wstring, std::vector<CharacterSection>> sections;
                for (const auto& s : *a.rest) sections[folder_key(root / std::filesystem::u8path(s.folder))].push_back(s.section);
                std::vector<std::wstring> every;
                for (size_t i : a.members) every.push_back(folder_key(inis[i].parent_path()));
                for (const auto& unit : sections) every.push_back(unit.first);
                std::set<std::wstring> made;
                for (const auto& ini : a.inputs) {
                    const auto folder = folder_key(ini.parent_path());
                    const auto unit = sections.find(folder);
                    if (unit == sections.end() || !made.insert(folder).second) continue;
                    FolderMod m; m.plain = true; m.ini = ini;
                    m.mod_namespace = ini.parent_path().lexically_relative(root).u8string();
                    auto program = std::make_unique<CharacterProgram>();
                    program->runtime_ = a.loader->runtime_; program->gpu_ = a.loader->gpu_; program->gpu_reason_ = a.loader->gpu_reason_;
                    std::vector<std::filesystem::path> unit_inputs;
                    for (const auto& other : a.inputs) if (folder_key(other.parent_path()) == folder) unit_inputs.push_back(other);
                    for (const auto& library : libraries(source.mods, unit_inputs, efmi::frontend::character_core_libraries(), program->library_notes_, true))
                        program->libraries_[declared_namespace(library)] = library;
                    program->folders_ = {folder, folder_key(source.core)};
                    for (const auto& library : program->libraries_) program->folders_.push_back(folder_key(library.second.parent_path()));
                    for (const auto& other : every)
                        if (other.size() > folder.size() && !other.compare(0, folder.size(), folder)) program->foreign_.push_back(other);
                    efmi::frontend::CharacterDiscovery found;
                    found.contract.sections = unit->second;
                    if (program->adopt(found, m.package, m.reason)) m.program = std::move(program);
                    out.push_back(std::move(m));
                }
                if (source.left_inputs)
                    for (const auto& item : left_plain) source.left_inputs->push_back(item.second);
                if (source.generation) *source.generation = std::move(a.loader);
                return true;
            }
            if (folder_reason.empty()) folder_reason = a.reason;
            // The Core itself lacks what the layer binds by name: no Mod of the folder can load, and loading
            // each alone would say nothing more.
            if (a.reason.find("EFMI Core interface") != std::string::npos) { reason = a.reason; return false; }
            bool any = false;
            for (size_t k = 0; k < a.members.size(); ++k) {
                const auto& found = *a.mods[k].second;
                if (found.refused.empty()) continue;
                const size_t i = a.members[k];
                left_out[i] = true; any = true;
                out[i].reason = found.refused;
                out[i].package = {}; out[i].package.sections = found.declared; // the sections, for the dry run's listing
            }
            if (any) continue;
            if (!tried_alone) {
                tried_alone = true;
                for (size_t i : a.members) {
                    CharacterProgram alone; CharacterPackage package; std::string why;
                    Source own{inis[i], source.core, source.settings, declared_namespace(inis[i]), source.mods, source.declarations_only};
                    ++loads;
                    if (alone.discover(own, package, why, factory)) continue;
                    left_out[i] = true; any = true;
                    out[i].reason = why; out[i].package = std::move(package); // the sections, for the dry run's listing
                }
                if (any) continue;
            }
            // Nothing refuses by itself. What the folder loads without: tried by halves, at most two dozen loads.
            const size_t limit = loads + 24;
            const auto loads_without = [&](const std::vector<std::filesystem::path>& plain, const std::vector<size_t>& members) {
                auto trial_out = left_out; auto trial_plain = left_plain;
                for (const auto& p : plain) trial_plain[key(p)] = {p, {}};
                for (size_t i : members) trial_out[i] = true;
                ++loads; return attempt(trial_out, trial_plain).loaded;
            };
            const auto halves = [](const auto& all, auto& first, auto& second) {
                first.assign(all.begin(), all.begin() + all.size() / 2); second.assign(all.begin() + all.size() / 2, all.end());
            };
            const std::string with = "the folder does not load with it: " + folder_reason;
            if (!a.plain.empty() && loads_without(a.plain, {})) {
                auto suspects = a.plain;
                while (suspects.size() > 1 && loads < limit) {
                    std::vector<std::filesystem::path> first, second; halves(suspects, first, second);
                    if (loads_without(first, {})) suspects = first;
                    else if (loads_without(second, {})) suspects = second;
                    else break;
                }
                for (const auto& p : suspects) left_plain[key(p)] = {p, with};
                continue;
            }
            if (a.members.empty()) { reason = folder_reason; return false; }
            auto suspects = a.members; bool narrowed = false;
            while (suspects.size() > 1 && loads < limit) {
                std::vector<size_t> first, second; halves(suspects, first, second);
                if (loads_without({}, first)) { suspects = first; narrowed = true; }
                else if (loads_without({}, second)) { suspects = second; narrowed = true; }
                else break;
            }
            if (!narrowed) { reason = folder_reason; return false; }
            for (size_t i : suspects) { left_out[i] = true; out[i].reason = with; }
        }
    } catch (const std::exception& e) { reason = e.what(); return false; }
}
bool CharacterProgram::folder_has_inputs(const std::filesystem::path& mods)
{
    std::error_code error;
    return std::filesystem::is_directory(mods, error) && !enabled_inis(mods).empty();
}
uint32_t CharacterProgram::stream_index(const std::shared_ptr<const efmi::assets::FileResource>& resource)
{
    const auto cached = stream_cache_.find(resource.get());
    if (cached != stream_cache_.end()) return cached->second;
    const auto name = stream_key(resource->path, resource->usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT, resource->stride);
    const auto found = streams_.find(name);
    uint32_t index = found == streams_.end() ? kCharacterNone : found->second;
    // A file that gave no buffer binds nothing, as the original's null resource (BUG-077).
    if (found == streams_.end() && !absent_.count(name)) throw std::runtime_error("INI mesh not in prepared stream profile");
    if (stream_cache_.size() < 4096) stream_cache_.emplace(resource.get(), index);
    return index;
}
bool CharacterProgram::index_range(const CharacterPackage& package, uint32_t stream, uint32_t first, uint32_t count, bool exact,
                                   std::pair<uint32_t, uint32_t>& range)
{
    if (!count) { range = {0, 0}; return true; }
    const auto& blocks = index_blocks_[stream];
    if (!exact) {
        range = {UINT32_MAX, 0};
        for (uint64_t b = first / kIndexBlock; b <= (uint64_t(first) + count - 1) / kIndexBlock && b < blocks.size(); ++b) {
            range.first = std::min(range.first, blocks[b].first); range.second = std::max(range.second, blocks[b].second);
        }
        return true;
    }
    const auto key = std::make_tuple(stream, first, count);
    if (const auto found = exact_ranges_.find(key); found != exact_ranges_.end()) { range = found->second; return true; }
    std::vector<uint8_t> bytes; std::string why;
    if (!package.read_stream(stream, bytes, why)) return false;
    const uint32_t stride = package.streams[stream].stride;
    range = {UINT32_MAX, 0};
    for (uint64_t i = first; i < uint64_t(first) + count && (i + 1) * stride <= bytes.size(); ++i) {
        uint32_t v = 0; std::memcpy(&v, bytes.data() + i * stride, stride);
        range.first = std::min(range.first, v); range.second = std::max(range.second, v);
    }
    if (exact_ranges_.size() < 4096) exact_ranges_.emplace(key, range);
    return true;
}
uint32_t CharacterProgram::image_index(const std::shared_ptr<const efmi::assets::FileResource>& resource)
{
    const auto cached = image_cache_.find(resource.get());
    if (cached != image_cache_.end()) return cached->second;
    const auto image = image_key(resource->path, resource->loader_flags);
    const auto found = images_.find(image);
    uint32_t index = found == images_.end() ? kCharacterNone : found->second;
    // The original loads a texture file at its first use; where that fails it warns once and the
    // resource stays null, so every assignment of it binds nothing and the draw goes on
    // (CustomResource::LoadFromFile). A file that is not there is such an explicit null binding, in a
    // Mod's own generation and in the folder's alike (BUG-076: tangtang Ramune binds a normal map its
    // author never shipped; the folder generation refused the draw, the Mod's own one bound the file).
    std::error_code error;
    if (!std::filesystem::exists(resource->path, error)) {
        index = kCharacterNone;
        logf("INI texture file missing, bound as nothing as the original does: %s", resource->path.u8string().c_str());
    } else if (unloadable_.count(key(resource->path))) {
        index = kCharacterNone; // its load fails as the original's does (D-139)
    } else if (found == images_.end()) {
        throw std::runtime_error("INI image not in prepared resource catalogue");
    } else if (!resource->sampled) {
        // Its bind flags leave out the shader resource: the original's view of it is not made (D-140).
        index = kCharacterNone;
        logf("INI texture declared without shader_resource binds nothing, as the original's view fails: %s", resource->path.u8string().c_str());
    } else if (resource->view_format) {
        // The view takes the declaration's format, which the loaded (typed) texture must have, or
        // CreateShaderResourceView fails and the slot binds nothing (CreateCompatibleView, D-140).
        const auto loaded = image_formats_.find(image);
        if (loaded != image_formats_.end() && efmi::assets::image_vk_format(resource->view_format) != loaded->second) {
            index = kCharacterNone;
            logf("INI texture view format %u is not the format its file loads as, so it binds nothing as the original's view: %s",
                 resource->view_format, resource->path.u8string().c_str());
        }
    }
    if (image_cache_.size() < 4096) image_cache_.emplace(resource.get(), index);
    return index;
}
// The plans of distinct binding states are kept by their signature; a Mod whose keys reach more of them
// than are kept starts the cache again instead of refusing the next one (the original keeps nothing of
// the kind). Its caller holds what reads the package's draws (the generation's plan lock) and rebuilds what
// it keeps by draw index when told a draw was added.
bool CharacterProgram::forget_plans_when_full(CharacterPackage& package, size_t adding)
{
    if (lists_.size() < 1024 && package.draws.size() + adding <= 16384) return false;
    lists_.clear(); package.draws.clear(); package.pixels.clear();
    ++plans_forgotten_;
    return true;
}
bool CharacterProgram::plan(const efmi::frontend::RuntimeDraw& request, CharacterPackage& package, CharacterList& output,
                            bool& skip, bool& added, std::string& reason, const VertexInputRecipe* vertex, bool* textures)
{
    try {
        added = false; efmi::assets::FilePlan& plan = file_plan_;
        if (textures) *textures = false;
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
        struct Phase { CharacterProgram* p; std::chrono::steady_clock::time_point began;
                       ~Phase() { if (p) p->note_timing("phase list", uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count())); } };
        Phase listing{nullptr, {}};
#endif
        if (!runtime_->draw(request, plan, reason)) return false;
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
        listing.p = this; listing.began = std::chrono::steady_clock::now();
#endif
        skip = plan.skip_original;
        if (!skip) {
            output = {};
            if (!textures) return true;
            // The application's draw stays, with the textures the pre phase bound for it: under DX11
            // they are bound when the game's draw runs. Geometry or draws of the Mod make it no
            // texture-only plan; post commands come after the draw.
            auto& pixels = scratch_pixels_; pixels.clear();
            for (size_t i = 0; i < plan.items.size() && i < plan.pre_items; ++i) {
                const auto& item = plan.items[i];
                using Op = efmi::assets::FileOperation;
                if (item.operation == Op::BindPixelResource) {
                    if (item.application_slot >= 0) {
                        if (uint32_t(item.application_slot) != item.slot) throw std::runtime_error("An application texture moved to another pixel slot");
                        pixels.erase(item.slot); continue;
                    }
                    if (!item.resource) { pixels[item.slot] = kCharacterNone; continue; }
                    pixels[item.slot] = image_index(item.resource);
                } else if (item.operation != Op::External) return true;
            }
            if (pixels.empty()) return true;
            auto& signature = scratch_signature_; signature.clear(); signature.push_back('T');
            for (const auto& pixel : pixels) { put(signature, pixel.first); put(signature, pixel.second); }
            auto& id = scratch_id_; id.assign(signature.begin(), signature.end());
            const auto found = lists_.find(id);
            if (found == lists_.end()) {
                forget_plans_when_full(package, 1); // a texture-only plan reports an added draw anyway
                CharacterDraw draw;
                draw.index_stream = kCharacterNone; draw.texture_only = true;
                std::vector<CharacterPixel> material;
                for (const auto& pixel : pixels) material.push_back({pixel.first, pixel.second});
                const CharacterList result{uint32_t(package.draws.size()), 1, 0};
                package.draws.push_back(draw); package.pixels.push_back(std::move(material));
                lists_.emplace(id, result); added = true; output = result;
            } else output = found->second;
            *textures = true;
            return true;
        }
        CharacterDraw bound;
        bound.index_stream = kCharacterNone;
        bool index_null = false; // bound to nothing explicitly (null, or a file that gave no buffer), not left unbound
        uint32_t vb_null = 0;
        auto& pixels = scratch_pixels_; pixels.clear();
        auto& draws = scratch_draws_; draws.clear();
        auto& material = scratch_material_; size_t material_count = 0;
        uint64_t required = 0;
        // A draw without a component (D-176) draws under the game's own vertex stage: its blend stream's bones
        // are the game's of that draw, not a merged skeleton's, and are neither checked against one nor required.
        const bool own_bones = request.component == UINT32_MAX;
        auto& signature = scratch_signature_; signature.clear();
        signature.push_back(own_bones ? 'P' : 'C');
        for (const auto& item : plan.items) {
            using Op = efmi::assets::FileOperation;
            if (item.operation == Op::BindPixelResource) {
                if (item.application_slot >= 0) {
                    // The application's own texture at its own slot: what the draw binds anyway.
                    if (uint32_t(item.application_slot) != item.slot) throw std::runtime_error("An application texture moved to another pixel slot");
                    pixels.erase(item.slot);
                } else if (item.resource) {
                    pixels[item.slot] = image_index(item.resource);
                } else pixels[item.slot] = kCharacterNone;
            } else if (item.operation == Op::BindIndex || item.operation == Op::BindVertex) {
                if (item.operation == Op::BindVertex && item.slot < 4)
                    bound.owned.erase(std::remove_if(bound.owned.begin(), bound.owned.end(), [&](const auto& o) { return o.slot == item.slot; }),
                                      bound.owned.end());
                if (item.application_slot >= 0) {
                    // The application's own buffer back in its place (the draw state put the slot back): what
                    // the draw binds where the Mod binds nothing.
                    if (item.operation == Op::BindIndex) { bound.index_stream = kCharacterNone; index_null = false; }
                    else if (item.slot < 4) { bound.vb[item.slot] = kCharacterNone; vb_null &= ~(1u << item.slot); }
                    continue;
                }
                // A vertex slot the pipeline's vertex input does not read is nothing to the draw, as under
                // D3D11 (R-08). The frontend leaves those out where the host told it the pipeline's slots;
                // here for a caller that gave the vertex input alone.
                if (item.operation == Op::BindVertex && item.slot >= 4 && vertex &&
                    std::none_of(vertex->bindings.begin(), vertex->bindings.end(), [&](const auto& b) { return b.binding == item.slot; }))
                    continue;
                if (item.owned) {
                    // A buffer the owned GPU work of this draw wrote (P2, D-087).
                    if (item.operation != Op::BindVertex || item.slot >= 4) throw std::runtime_error("Owned GPU buffer outside a profiled vertex slot");
                    const auto& o = *item.owned;
                    if (!o.buffer || !o.stride || o.size < o.stride) throw std::runtime_error("Owned vertex buffer without a whole vertex");
                    bound.vb[item.slot] = kCharacterOwned; vb_null &= ~(1u << item.slot);
                    bound.owned.push_back({item.slot, o.buffer, o.offset, o.size, o.stride, o.keep});
                    continue;
                }
                if (!item.resource) {
                    if (item.operation == Op::BindIndex) { bound.index_stream = kCharacterNone; index_null = true; }
                    else if (item.slot < 4) { bound.vb[item.slot] = kCharacterNone; vb_null |= 1u << item.slot; }
                    else throw std::runtime_error("Runtime vertex slot outside profile");
                    continue;
                }
                const uint32_t found_stream = stream_index(item.resource);
                if (found_stream == kCharacterNone) {
                    // The original's null buffer (BUG-077), as an explicit null binding.
                    if (item.operation == Op::BindIndex) { bound.index_stream = kCharacterNone; index_null = true; }
                    else if (item.slot < 4) { bound.vb[item.slot] = kCharacterNone; vb_null |= 1u << item.slot; }
                    continue;
                }
                if (item.operation == Op::BindIndex) index_null = false;
                else if (item.slot < 4) vb_null &= ~(1u << item.slot);
                const auto& stream = package.streams[found_stream];
                if (item.resource->stride != stream.stride) throw std::runtime_error("Runtime binding stride mismatch");
                if (item.operation == Op::BindIndex) {
                    if (stream.kind != CharacterStream::Index) throw std::runtime_error("Runtime index kind mismatch");
                    bound.index_stream = found_stream;
                } else {
                    if (item.slot >= 4 || stream.kind != CharacterStream::Vertex) throw std::runtime_error("Runtime vertex binding outside profile");
                    bound.vb[item.slot] = found_stream;
                }
            } else if (item.operation == Op::DrawIndexed || item.operation == Op::Translucent) {
                // Without an index buffer, or with a null position buffer, the original's draw reads index 0 or
                // position zero for every vertex: its triangles have no area and nothing shows. That draw is left
                // out; the list's other draws go on (BUG-077).
                if ((item.indexed && index_null) || (vb_null & 1u)) continue;
                if (draws.size() >= 1024 || !item.instance_count || (item.indexed && bound.index_stream == kCharacterNone))
                    throw std::runtime_error("Runtime draw outside the package's index streams");
                // The blend-index widening EFMI exports use: BLENDINDICES of slot 2 as R16G16B16A16_UINT
                // (DXGI 12) or R32G32B32A32_UINT (DXGI 3); the shader reads four unsigned integers either way.
                // Besides it, element offset edits whose game offset the Mod's lists name (D-085).
                const efmi::assets::LayoutEdit* blend = nullptr;
                bound.offsets.clear();
                bool understood = true;
                auto same = [](const efmi::assets::LayoutEdit& a, const efmi::assets::LayoutEdit& b) {
                    const auto &x = a.match, &y = b.match; const auto &p = a.replace, &q = b.replace;
                    return x.semantic_name == y.semantic_name && x.semantic_index == y.semantic_index && x.input_slot == y.input_slot &&
                           x.format == y.format && x.format_byte_size == y.format_byte_size && x.aligned_byte_offset == y.aligned_byte_offset &&
                           p.semantic_name == q.semantic_name && p.semantic_index == q.semantic_index && p.format == q.format &&
                           p.aligned_byte_offset == q.aligned_byte_offset;
                };
                for (size_t e = 0; e < item.layout.size(); ++e) {
                    const auto& edit = item.layout[e];
                    const auto& m = edit.match; const auto& r = edit.replace;
                    const bool plain_match = !m.format && m.format_byte_size == UINT32_MAX && m.aligned_byte_offset == UINT32_MAX &&
                                             r.semantic_name.empty() && r.semantic_index == UINT32_MAX;
                    // The original applies every override in order to each element it matches; a plain one
                    // (by slot and semantic, keeping the semantic) given again later changes nothing more.
                    if (plain_match && std::any_of(item.layout.begin() + e + 1, item.layout.end(), [&](const auto& later) { return same(later, edit); }))
                        continue;
                    if (!blend && plain_match && m.input_slot == 2 && m.semantic_name == "BLENDINDICES" && (r.format == 12 || r.format == 3) &&
                        r.aligned_byte_offset == UINT32_MAX) { blend = &edit; continue; }
                    if (plain_match && !r.format && r.aligned_byte_offset != UINT32_MAX && m.input_slot < 4 && m.semantic_index != UINT32_MAX) {
                        const auto known = element_offsets_.find({m.input_slot, m.semantic_name, m.semantic_index});
                        if (known != element_offsets_.end() && known->second.size() == 2 && known->second.count(r.aligned_byte_offset)) {
                            const uint32_t game = *known->second.begin() == r.aligned_byte_offset ? *known->second.rbegin() : *known->second.begin();
                            if (game != r.aligned_byte_offset) { bound.offsets.push_back({m.input_slot, game, r.aligned_byte_offset}); continue; }
                        }
                    }
                    understood = false;
                }
                // An edit beyond those: the layer has the pipeline's Vulkan locations, not the game's DX11
                // semantics, so it cannot tell which element the edit names, and the Mod's streams would be
                // read wrongly. The draw stays the game's.
                if (!understood) {
                    std::string edits;
                    for (const auto& edit : item.layout)
                        edits += " slot " + std::to_string(edit.match.input_slot) + " " + edit.match.semantic_name + " to " +
                                 (edit.replace.aligned_byte_offset != UINT32_MAX ? "offset " + std::to_string(edit.replace.aligned_byte_offset)
                                                                                : "format " + std::to_string(edit.replace.format)) + ";";
                    throw std::runtime_error("Runtime input-layout edits differ from the validated blend-index contract (" +
                                             std::to_string(item.layout.size()) + " edits:" + edits + ")");
                }
                // The blend-index widening is the Mod's to make (R-08): a list without it has its blend
                // stream read by the pipeline's own BLENDINDICES element, of the width the game declares
                // (8-bit mostly). The width is then the pipeline's; without the pipeline (offline) it is not
                // known and the stream's bones are not adopted.
                uint32_t index_bytes = 0;
                bound.pipeline_blend = !blend;
                if (blend) {
                    bound.blend = blend->replace.format == 3 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R16G16B16A16_UINT;
                    index_bytes = bound.blend == VK_FORMAT_R32G32B32A32_UINT ? 4 : 2;
                } else {
                    bound.blend = VK_FORMAT_UNDEFINED;
                    if (vertex)
                        for (const auto& a : vertex->attributes)
                            if (a.binding == 2 && blend_index_format(a.format))
                                index_bytes = a.format == VK_FORMAT_R8G8B8A8_UINT ? 1 : a.format == VK_FORMAT_R16G16B16A16_UINT ? 2 : 4;
                }
                bound.count = item.index_count; bound.first = item.first_index; bound.base = item.indexed ? item.vertex_offset : 0;
                bound.instances = item.instance_count; bound.first_instance = item.first_instance; bound.indexed = item.indexed;
                if (item.automatic) {
                    // The original's get_index_count_from_current_ib / get_vertex_count_from_current_vb: the bound
                    // buffer's whole elements from its start, no first element or base; none, and nothing is drawn.
                    const uint32_t stream = item.indexed ? bound.index_stream : bound.vb[0];
                    uint64_t elements = 0;
                    if (stream == kCharacterOwned) {
                        const auto* o = bound.owned_stream(0);
                        elements = o && o->stride ? o->size / o->stride : 0;
                    } else if (stream != kCharacterNone) {
                        const auto& s = package.streams[stream];
                        const uint64_t file = stream < package.sources.size() ? std::min(s.size, package.sources[stream].file_bytes) : s.size;
                        elements = s.stride ? file / s.stride : 0;
                    }
                    bound.count = uint32_t(std::min<uint64_t>(elements, UINT32_MAX)); bound.first = 0; bound.base = 0;
                    if (!bound.count) continue;
                }
                // The stream under the blend-index edit carries the bone indices where the pipeline reads
                // the element the edit widens (R8G8B8A8_UINT in binding 2, then 16-bit indices), and
                // 16-bit weights where it reads R16G16B16A16_UNORM in that binding. Without such an
                // element (offline, or a pipeline that does not read the stream) the packager's rule:
                // indices in the last 8 bytes of an 8- or 16-byte element, weights before them. A stream
                // first seen there is checked once, as decode checks a package's; one that fits neither
                // keeps no bone mask.
                if (!own_bones && bound.vb[2] == kCharacterOwned) throw std::runtime_error("An owned blend stream has no known bone indices");
                if (!own_bones && index_bytes && bound.vb[2] != kCharacterNone && package.streams[bound.vb[2]].bone_index_offset != kCharacterNone &&
                    package.streams[bound.vb[2]].bone_index_bytes != index_bytes)
                    throw std::runtime_error("One blend stream under two blend-index widths");
                if (!own_bones && index_bytes && bound.vb[2] != kCharacterNone && package.streams[bound.vb[2]].bone_index_offset == kCharacterNone) {
                    const uint32_t stride = package.streams[bound.vb[2]].stride;
                    uint32_t indices = kCharacterNone, weights = kCharacterNone, weight_bytes = 2;
                    if (vertex) {
                        // BLENDINDICES is the pipeline's unsigned four-component element of binding 2, of
                        // any width (BUG-038: 32-bit in Typhoeus' clothes pipeline), BLENDWEIGHT its
                        // normalized or float one.
                        for (const auto& a : vertex->attributes) {
                            if (a.binding != 2) continue;
                            if (blend_index_format(a.format)) indices = a.offset;
                            else if (const uint32_t bytes = blend_weight_bytes(a.format)) { weights = a.offset; weight_bytes = bytes; }
                        }
                    }
                    if (indices == kCharacterNone && index_bytes == 2 && (stride == 8 || stride == 16)) {
                        indices = stride - 8; weights = stride == 16 ? 0 : kCharacterNone; weight_bytes = 2;
                    }
                    std::string why;
                    if (indices != kCharacterNone && !package.adopt_bones(bound.vb[2], indices, weights, why, index_bytes, weight_bytes))
                        throw std::runtime_error("Blend stream refused: " + why);
                }
                // A non-indexed draw reads the vertices from `first` on (D-142).
                const bool indexed = bound.indexed;
                if (indexed) {
                    // Past the index file the original's input assembler reads index 0 (D3D11's out-of-bounds
                    // rule): those triangles are one vertex three times and show nothing. The draw keeps its
                    // whole triangles inside the file (D-148).
                    const auto& index = package.streams[bound.index_stream];
                    const uint64_t elements = index.size / index.stride;
                    if (bound.first >= elements) continue;
                    if (uint64_t(bound.first) + bound.count > elements) {
                        bound.count = uint32_t((elements - bound.first) / 3 * 3);
                        if (!bound.count) continue;
                    }
                }
                std::pair<uint32_t, uint32_t> extrema{bound.first, bound.first + bound.count - 1};
                if (indexed && !index_range(package, bound.index_stream, bound.first, bound.count, false, extrema))
                    throw std::runtime_error("Runtime index range unknown");
                auto past = [&](const std::pair<uint32_t, uint32_t>& range) {
                    for (uint32_t slot = 0; slot < 4; ++slot) {
                        const uint32_t stream = bound.vb[slot];
                        if (stream == kCharacterNone) continue;
                        uint64_t vertices = 0;
                        if (stream == kCharacterOwned) {
                            const auto* o = bound.owned_stream(slot);
                            vertices = o ? o->size / o->stride : 0;
                        } else vertices = package.streams[stream].size / package.streams[stream].stride;
                        if (int64_t(range.first) + bound.base < 0 || int64_t(range.second) + bound.base >= int64_t(vertices)) return true;
                    }
                    return false;
                };
                // The bound of the draw's index blocks first; the file read exactly only where that reaches past
                // a vertex stream. A draw that does read past one shows vertices of zeros in the original; the
                // layer leaves that draw out and the list's others go on (D-148).
                if (past(extrema) && (!indexed || !index_range(package, bound.index_stream, bound.first, bound.count, true, extrema) || past(extrema))) {
                    if (past_logged_.insert(bound.index_stream).second && past_logged_.size() < 64)
                        logf("INI draw reads vertices past its files (the original reads zeros there); that draw is left out");
                    continue;
                }
                for (uint32_t slot = 0; slot < 4 && !own_bones; ++slot) {
                    const uint32_t stream = bound.vb[slot];
                    if (stream != kCharacterNone && stream != kCharacterOwned) required |= package.streams[stream].spans;
                }
                // A translucent draw (D-113): captured into a layer at the pass end with its opacity and mask.
                bound.translucent = item.operation == Op::Translucent;
                bound.alpha = bound.translucent ? item.alpha : 1.0f;
                bound.mask_image = bound.translucent && item.resource ? image_index(item.resource) : kCharacterNone;
                bound.mask_channel = bound.translucent ? item.mask_channel : 0;
                bound.mask_invert = bound.translucent && item.mask_invert;
                draws.push_back(bound);
                if (material_count == material.size()) material.emplace_back();
                auto& slots = material[material_count++]; slots.clear();
                for (const auto& pixel : pixels) slots.push_back({pixel.first, pixel.second});
                put(signature, bound.index_stream); put(signature, bound.count); put(signature, bound.first); put(signature, bound.base);
                put(signature, bound.instances); put(signature, bound.first_instance); put(signature, uint32_t(bound.indexed));
                for (auto stream : bound.vb) put(signature, stream);
                put(signature, uint32_t(bound.owned.size()));
                for (const auto& o : bound.owned) { put(signature, o.slot); put(signature, o.buffer); put(signature, o.offset); put(signature, o.size); put(signature, o.stride); }
                put(signature, uint32_t(bound.blend)); put(signature, uint8_t(bound.pipeline_blend));
                put(signature, uint32_t(bound.offsets.size()));
                for (const auto& o : bound.offsets) { put(signature, o.binding); put(signature, o.from); put(signature, o.to); }
                put(signature, uint32_t(pixels.size()));
                for (const auto& pixel : pixels) { put(signature, pixel.first); put(signature, pixel.second); }
                put(signature, uint8_t(bound.translucent));
                if (bound.translucent) { put(signature, bound.alpha); put(signature, bound.mask_image); put(signature, bound.mask_channel); put(signature, uint8_t(bound.mask_invert)); }
            } else throw std::runtime_error("Unresolved runtime native operation");
        }
        auto& id = scratch_id_; id.assign(signature.begin(), signature.end());
        auto found = lists_.find(id);
        if (found != lists_.end()) { output = found->second; return true; }
        const bool forgot = forget_plans_when_full(package, draws.size());
        CharacterList result{uint32_t(package.draws.size()), uint32_t(draws.size()), required};
        package.draws.insert(package.draws.end(), draws.begin(), draws.end());
        package.pixels.insert(package.pixels.end(), material.begin(), material.begin() + material_count);
        // After the plans were forgotten the caller rebuilds what it keeps by draw (materials, variants).
        lists_.emplace(id, result); added = !draws.empty() || forgot; output = result; return true;
    } catch (const std::exception& e) { reason = e.what(); return false; }
}
bool CharacterProgram::present(const efmi::frontend::RuntimeInput& input, uint64_t frame, float seconds, std::string& reason, bool keep_replay_state)
{
    return runtime_->present(input, frame, seconds, reason, keep_replay_state);
}

bool CharacterProgram::replay(const efmi::frontend::RuntimeInput& input, uint64_t frame, float seconds, std::string& reason)
{
    return runtime_->replay(input, frame, seconds, reason);
}
}
