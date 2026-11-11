// The DX11 resource hash of a game image (D-077, docs/texture-identity.md), as the original computes it
// for the same texture: crc32c of the 44-byte D3D11_TEXTURE2D_DESC, seeded with the data hash of the
// first subresource's initial data (zero for an image created without data). The data hash comes from
// the tracker's first upload of mip 0, slice 0 (ImageInfo::dx11_data_crc).
#pragma once
#include "resources.h"

namespace efvk {

// The DXGI format the game's DX11 build creates for this Vulkan format; 0 when not known.
uint32_t dxgi_format(VkFormat format);

// The DXGI format of the shader resource view the game's DX11 build binds where its Vulkan build binds a
// view of this format and aspect (D-170): what the original's `ps-tN->Format` reads. A colour view has the
// texture's own format on both sides. A depth texture is typeless under DX11 and its view names the depth
// bits: D16_UNORM is R16_UNORM, D32_SFLOAT_S8_UINT is R32_FLOAT_X8X24_TYPELESS, the two the paired catalogue
// shows (scripts/check_texture_properties.py). 0 when not known, the other depth formats included.
uint32_t dxgi_view_format(VkFormat format, VkImageAspectFlags aspect);

// The D3D11_TEXTURE2D_DESC the game's DX11 build creates for this image, field by field (Width, Height,
// MipLevels, ArraySize, Format, SampleDesc.Count, SampleDesc.Quality, Usage, BindFlags, CPUAccessFlags,
// MiscFlags): what the original's hash is made of and its match_* sections compare (D-138). False for a
// 1D or 3D image and an unknown format.
bool dx11_texture_desc(const ImageInfo& image, uint32_t desc[11]);

// False when the hash cannot be computed: image content tracking off, a 3D or 1D image, an unknown
// format, or an upload the tracker could not read whole. An image never uploaded from a buffer has
// the hash of a texture without data.
bool dx11_texture_hash(const ImageInfo& image, uint32_t& hash);

} // namespace efvk
