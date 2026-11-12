// What the shader hooks of a library stand for on Vulkan, read off the game's SPIR-V (D-113).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace efvk {

// EFMITTL's lit vertex shader: the outputs of the game's lit character vertex shader under DX11 (o1.xy,
// o2.xyz, o3.xyz, o4.xyzw, o5-o8.xyz, o9.x) at locations 0-8, a pipeline leaving out some it does not
// pass on, the UV at 0 and the instance at 8 kept, the instance index read, a storage and a uniform
// buffer bound. Its copy shader: one explicit-LOD sample of one 2D texture at the interpolated UV, its
// LOD from a uniform, stored as it is to the only output.
bool lit_character_vertex(const std::vector<uint32_t>& code);
bool copy_fragment(const std::vector<uint32_t>& code);

// The library's other vertex shader groups (D-171; its 0.5.3 and 2026-11-08 additions), checked on the paired
// catalogue against the groups' own patterns run on the DX11 listings (scripts/check_shader_hooks.py).
// The lit vertex shader of skin: DX11 outputs o1.xy, o2.xyz, o3.xyz, o4.xyzw, o5-o7.xyz, o8.xyzw, o9.xyz,
// o10.x at locations 0-9, read the same way as the lit one above.
bool lit_skin_vertex(const std::vector<uint32_t>& code);
// An outline vertex shader: the library finds the ramp `saturate(x * 4.583663)` that widens the outline.
// The DX11 compiler folded that constant from two the Vulkan build keeps apart, 114.59157 and 0.04; the
// first is in every outline vertex module of the catalogue and in no other.
bool outline_vertex(const std::vector<uint32_t>& code);
// The shadow vertex shaders of skinned characters (D-185, the library's ShadowVSPlain and ShadowVSFading groups; game
// 1.6.3: DX11 f11c7e1d and bbc56f5c, Vulkan d29292e6 and c8fa3abe on the paired catalogue): a skinned character's inputs
// (float3, float2, float3, float4, float4, int4 at locations 0, 1, 2, 4, 5, 6), the instance index, a storage buffer and
// uniform blocks; the plain one outputs the position alone, the fading one the position and the UV (float2 at location 0).
// 4 plain, 5 fading, 0 neither.
uint32_t shadow_vertex(const std::vector<uint32_t>& code);
// 0 none, 1 lit, 2 lit skin, 3 outline, 4 plain shadow, 5 fading shadow: the values of efmi::frontend::VertexHook.
uint32_t character_vertex_hook(const std::vector<uint32_t>& code);

// The game's character material block and its UV transform (D-185): the fading shadow vertex shader writes its UV output as
// `uv * t.xy + t.zw` with `t` the vec4 at byte 160 of the uniform block at set 1 binding 0 (Unity's _MainTex_ST; DX11's
// `mad o0.xy, v1.xyxx, cb3[10].xyxx, cb3[10].zwzz` in the plain one, whose Vulkan build leaves the output out). Facts of
// this game, as the instance entry's place is (review L-07).
constexpr uint32_t kMaterialSet = 1, kMaterialBinding = 0, kUvTransformOffset = 160;
// Adds that UV output at location 0 to a vertex module that has none, from its float2 input at location `uv_location` and
// the material block's transform. False with the reason, the code unchanged, when the module has not these.
bool add_uv_output(std::vector<uint32_t>& code, uint32_t uv_location, std::string& why);

// The library's pixel shader groups (D-171, its DiffuseAt and ClipFirst groups). Their patterns read the
// DX11 listing from its start: the first sample of a 2D texture below t24 at an interpolated input (a `v`
// register) whose colour is taken (three or four channels written) names the diffuse slot, and a first such
// sample of one or two channels is the hair outline's alpha clip. The same from the SPIR-V build: the
// samples of 2D textures at their DX11 registers (D-076) whose coordinate is an input as it is, with the
// channels of the result the module goes on to read, in the module's order, except that the DX11 compiler
// puts a sample after every discard that comes before its first use and does not depend on it (so a clip
// on another texture's alpha is listed first). `known` false for a module this does not read.
struct PixelHook {
    bool known = false;
    int32_t colour_slot = -1;  // the t register of the first sample of three or four channels, -1 for none
    bool narrow_first = false; // the first sample has one or two channels
};
PixelHook character_pixel_hook(const std::vector<uint32_t>& code);

} // namespace efvk
