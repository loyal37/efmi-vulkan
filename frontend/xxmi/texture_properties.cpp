#include "texture_properties.h"
#include "PlatformHelpers.h"
#include <wrl/client.h>
#include <wincodec.h>
#include "LoaderHelpers.h"
#include "../assets/png.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace efmi::frontend {
namespace {
// The description DirectXTK's DDS loader gives the texture it makes of this file, read off the header alone
// with the pinned loader's own structures and pixel format decoding (as the owned runtime's read_dds does
// for the whole file, gpu_images.cpp). False for what that loader makes no texture of.
bool dds_header(std::ifstream& in, uint64_t size, bool force_srgb, TextureProperties& p)
{
    using namespace DirectX; using namespace DirectX::LoaderHelpers;
    uint8_t bytes[4 + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10)]{};
    const size_t available = size_t(std::min<uint64_t>(size, sizeof(bytes)));
    in.seekg(0); in.read(reinterpret_cast<char*>(bytes), std::streamsize(available));
    if (!in || available < 4 + sizeof(DDS_HEADER)) return false;
    UINT magic = 0; DDS_HEADER header{};
    memcpy(&magic, bytes, 4); memcpy(&header, bytes + 4, sizeof(header));
    if (magic != DDS_MAGIC || header.size != sizeof(header) || header.ddspf.size != sizeof(DDS_PIXELFORMAT)) return false;
    p.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    p.width = header.width; p.height = header.height; p.array = 1; p.mips = header.mipMapCount ? header.mipMapCount : 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if ((header.ddspf.flags & DDS_FOURCC) && header.ddspf.fourCC == MAKEFOURCC('D', 'X', '1', '0')) {
        if (available < sizeof(bytes)) return false;
        DDS_HEADER_DXT10 extra{};
        memcpy(&extra, bytes + 4 + sizeof(header), sizeof(extra));
        format = extra.dxgiFormat; p.array = extra.arraySize;
        if (!p.array || p.array > 2048) return false;
        p.dimension = D3D11_RESOURCE_DIMENSION(extra.resourceDimension);
        if (p.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) p.height = 1;
        else if (p.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { if (extra.miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE) p.array *= 6; }
        else if (p.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D) return false;
    } else {
        format = GetDXGIFormat(header.ddspf);
        if (header.flags & DDS_HEADER_FLAGS_VOLUME) p.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
        else if (header.caps2 & DDS_CUBEMAP) {
            if ((header.caps2 & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES) return false;
            p.array = 6;
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN || !p.width || p.width > 16384 || !p.height || p.height > 16384 || p.mips > D3D11_REQ_MIP_LEVELS) return false;
    // CreateDDSTextureFromFileEx's forceSRGB, which a declaration's `color_space = srgb` asks for (D-140).
    p.format = force_srgb ? MakeSRGB(format) : format;
    p.format_known = true;
    return true;
}
// The DXGI format of a texture the WIC decoder made with this Vulkan format (frontend/assets/png.cpp keeps
// the pinned WIC loader's choices: grey, 16-bit grey, RGBA 16, BGRA and RGBA 8 with their sRGB twins).
DXGI_FORMAT wic_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case VK_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case VK_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
bool file_texture(const std::filesystem::path& path, uint32_t loader_flags, TextureProperties& p)
{
    try {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in || in.tellg() <= 0) return false;
        const uint64_t size = uint64_t(in.tellg());
        if (size > (128ull << 20)) return false;
        if (!_wcsicmp(path.extension().c_str(), L".dds")) return dds_header(in, size, (loader_flags & assets::png_force_srgb) != 0, p);
        // Any other file by WIC, as the original's LoadFromFile (D-139): frame 0, one level, one layer.
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!in) return false;
        assets::PngPixels decoded; std::string why;
        if (!assets::decode_wic(bytes.data(), bytes.size(), decoded, why, {}, loader_flags, false)) return false;
        p.format = wic_format(decoded.metadata.format);
        if (p.format == DXGI_FORMAT_UNKNOWN) return false;
        p.format_known = true; p.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        p.width = decoded.metadata.width; p.height = decoded.metadata.height; p.mips = 1; p.array = 1;
        return true;
    } catch (const std::exception&) { return false; }
}
}
bool ModTextures::describe(const CustomResource& r, TextureProperties& properties, bool& bound, std::wstring& why)
{
    auto known = known_.find(&r);
    if (known == known_.end()) {
        // Only what the loader alone decides: a declaration with data, a pool element, a resource other work
        // wrote or a buffer has no file to read here.
        if (r.filename.empty() || r.pool || r.initial_data || r.resource ||
            (r.override_type != CustomResourceType::INVALID && r.override_type != CustomResourceType::TEXTURE2D)) {
            why = L"The description of this resource of the Mod is not known in a replaced draw's plan: " + r.name;
            return false;
        }
        Entry entry;
        const uint32_t flags = r.override_color_space == CustomColorSpace::SRGB ? assets::png_force_srgb :
                               r.override_color_space == CustomColorSpace::LINEAR ? assets::png_ignore_srgb : 0u;
        // The file's own format, whatever `format` the declaration names: the original reads 28 at a slot that
        // holds an R8G8B8A8_UNORM file declared as its sRGB twin, and through a declaration that holds it by
        // reference (the judge's `properties` case); only the declaration itself answers its key.
        entry.loads = file_texture(std::filesystem::path(r.filename), flags, entry.properties);
        if (!entry.loads) entry.properties = {};
        if (known_.size() >= 65536) known_.clear();
        known = known_.emplace(&r, entry).first;
    }
    bound = known->second.loads;
    properties = known->second.properties;
    return true;
}
}
