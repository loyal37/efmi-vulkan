#pragma once
#include "CommandList.h"
#include <filesystem>
#include <string>
#include <unordered_map>

namespace efmi::frontend {
// `ps-t3->Format`, `->Width`, `->Height`, `->Mips` and `->Array` in a replaced draw's plan (D-170). The
// original answers them from the D3D11 resource behind the target (CommandList.cpp,
// ResourceCopyTarget::GetResourceFormat, GetResourceWidth, GetResourceHeight, GetResourceArray,
// GetResourceMips); a plan has no D3D11 resource, so whoever knows the texture describes it with this
// record: the host for the game's texture at a pixel slot, the Mod's file for its own.
struct TextureProperties {
    // What the resource is; UNKNOWN when that is not known here, and every query on it refuses.
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    // The format as the original finds it: the bound view's, and the texture's own where the view has
    // none. With `format_known` an UNKNOWN is the original's own (the view of a structured buffer) and
    // reads -1; without it the format is not known here and `->Format` refuses.
    bool format_known = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // The resource's description, not the view's (a view of one mip level still reads the whole chain).
    UINT width = 0, height = 0, mips = 0, array = 0;
};
inline bool texture_property_mode(ResourceCopyTargetEvaluationMode mode)
{
    return mode == ResourceCopyTargetEvaluationMode::RESOURCE_FORMAT || mode == ResourceCopyTargetEvaluationMode::RESOURCE_WIDTH ||
           mode == ResourceCopyTargetEvaluationMode::RESOURCE_HEIGHT || mode == ResourceCopyTargetEvaluationMode::RESOURCE_ARRAY ||
           mode == ResourceCopyTargetEvaluationMode::RESOURCE_MIPS;
}
// The getters' result for a target that resolved to this resource (`bound` false: to none). The values
// are the original's, its sentinels included (ResourcePropertyResult): -1 no format, -2 no resource,
// -4 not a texture. False with `why` when the layer cannot tell what the original would read.
inline bool texture_property(ResourceCopyTargetEvaluationMode mode, bool bound, const TextureProperties& p, float& value, std::wstring& why)
{
    if (!texture_property_mode(mode)) { why = L"Not a texture property"; return false; }
    if (!bound) { value = -2.0f; return true; }
    if (p.dimension == D3D11_RESOURCE_DIMENSION_UNKNOWN) { why = L"The description of the resource at this slot is not known to the Vulkan layer"; return false; }
    const bool buffer = p.dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
    switch (mode) {
    case ResourceCopyTargetEvaluationMode::RESOURCE_FORMAT:
        if (!p.format_known) { why = L"The format of the resource at this slot is not known to the Vulkan layer"; return false; }
        value = p.format == DXGI_FORMAT_UNKNOWN ? -1.0f : float(p.format); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_WIDTH:
        value = buffer ? -4.0f : float(p.width); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_HEIGHT:
        // GetResourceExtent: a 1D texture's height is 1.
        value = buffer ? -4.0f : p.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1.0f : float(p.height); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_ARRAY:
        // GetResourceArray has no case for a 3D texture: NOT_A_TEXTURE, as for a buffer.
        value = buffer || p.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? -4.0f : float(p.array); return true;
    default:
        value = buffer ? -4.0f : float(p.mips); return true;
    }
}
// What a declaration answers by itself, before the getters look for a resource: its `format`, `width`,
// `height`, `array` and `mips` keys (override_*), whatever it holds.
inline bool declared_texture_property(ResourceCopyTargetEvaluationMode mode, const CustomResource& declaration, float& value)
{
    switch (mode) {
    case ResourceCopyTargetEvaluationMode::RESOURCE_FORMAT:
        if (declaration.override_format == DXGI_FORMAT(-1) || declaration.override_format == DXGI_FORMAT_UNKNOWN) return false;
        value = float(declaration.override_format); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_WIDTH: if (declaration.override_width == -1) return false; value = float(declaration.override_width); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_HEIGHT: if (declaration.override_height == -1) return false; value = float(declaration.override_height); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_ARRAY: if (declaration.override_array == -1) return false; value = float(declaration.override_array); return true;
    case ResourceCopyTargetEvaluationMode::RESOURCE_MIPS: if (declaration.override_mips == -1) return false; value = float(declaration.override_mips); return true;
    default: return false;
    }
}
// The texture the original's loader makes of a Mod's image file (CustomResource::LoadFromFile: a .dds by
// DirectXTK's DDS loader, any other file by its WIC loader), read off the file's header and the
// declaration's colour space. The format is the file's as loaded, at a slot and through a reference alike;
// a declaration's own `format` key is declared_texture_property's. One read per declaration of a generation.
class ModTextures {
public:
    // `bound` false: the loader makes no texture of the file (missing, not an image), which the original
    // holds as its null resource. False with `why`: not a file-backed texture, whose description the plan
    // of a replaced draw does not know.
    bool describe(const CustomResource& resource, TextureProperties& properties, bool& bound, std::wstring& why);
private:
    struct Entry { bool loads = false; TextureProperties properties; };
    std::unordered_map<const CustomResource*, Entry> known_;
};
}
