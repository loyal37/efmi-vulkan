#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace efmi::gpu {

// DX11 registers of a game shader's bound resources, read from its SPIR-V alone (D-076,
// docs/source-slots.md). The game builds each shader with FXC and with DXC from one HLSL source;
// both keep the declaration order, and the DX11 build puts read-only buffers ahead of textures.
// A t register is therefore the rank among the module's shader resource views, buffers first, then
// images, each group in declaration order; an array takes one register per element. Constant
// buffers (b), samplers (s) and UAVs (u) are numbered in declaration order; only t is verified
// against observed slots. No DX11 data and no capture of a character is involved.
struct SourceRegister {
    enum Kind : uint8_t { ImageSrv, BufferSrv, TexelSrv, ImageUav, BufferUav, TexelUav, Sampler, ConstantBuffer };
    Kind kind = ImageSrv;
    char letter = 't';
    uint32_t index = 0, count = 1, set = 0, binding = 0;
    uint32_t dim = 0;       // SPIR-V Dim of an image: 0 1D, 1 2D, 2 3D, 3 Cube, 5 Buffer
    bool arrayed = false, multisampled = false;
    uint32_t bytes = 0;     // a constant buffer: its block's size (last member's offset plus its size)
    uint32_t stride = 0;    // a buffer view: the array stride of its block's last member, 0 when none
    bool words = false;     // a buffer view whose elements are single 32-bit unsigned integers
};

// Every resource variable with a binding, numbered by the rule. False (output unchanged) for input
// this does not read: malformed words, a runtime-sized array, a resource of unknown type.
bool source_registers(const std::vector<uint32_t>& code, std::vector<SourceRegister>& out, std::string& reason);

// The resource that holds a register, or nullptr. A register inside an array returns the array;
// the caller decides whether it can address one element.
const SourceRegister* find_source_register(const std::vector<SourceRegister>& registers, char letter, uint32_t index);

// The DX11 declaration of t register `slot` as the original reads it off the shader's DXBC
// (ByteCodeReader: dcl_resource, dcl_resource_raw, dcl_resource_structured) for `vs->srv_type(n)`,
// `srv_stride` and `srv_dimension` (D-124), decided by the register above, in the original's values:
// type 1 typed, 2 structured, 3 raw (ShaderResourceType); dimension a D3D_SRV_DIMENSION (1 buffer,
// 2 1D, 3 1D array, 4 2D, 5 2D array, 6 2D MS, 7 2D MS array, 8 3D, 9 cube, 10 cube array); stride
// for a structured buffer, else 0. Checked against the DXBC of the paired catalogue
// (scripts/check_source_declarations.py). False where SPIR-V does not decide it: a slot without a
// register (the DX11 builds of the game's pixel shaders declare registers their SPIR-V lacks, t60 and
// up), a buffer of 32-bit words (a byte address buffer and a structured buffer of uint compile
// alike), an image of a kind DX11 has no view for.
// `type` 0: the slot is not declared (the original's ShaderResourceType::NONE).
// `words`: a read-only buffer of 32-bit unsigned words. DX11 declares it as a structured buffer of
// 4-byte elements or as a byte address buffer, and the SPIR-V is the same for both: the answer is one of
// the two, `stride` being the structured one's.
struct SourceSrvDeclaration { uint32_t type = 0, dimension = 0, stride = 0; bool words = false; };
// The first t register the game's DX11 pixel shaders assign explicitly; the Vulkan builds do not declare
// those textures, so a slot from here up without a SPIR-V register is not decided (docs/source-slots.md).
constexpr uint32_t kSourceExplicitRegisters = 60;
bool source_srv_declaration(const std::vector<SourceRegister>& registers, uint32_t slot, SourceSrvDeclaration& out, std::string& reason);

// What the original EFMIv1 Core reads off a draw's vertex shader before it touches a mesh (D-079),
// decided by its own rules on the registers above: Component_ReadConfig takes the first of vs-cb2,
// vs-cb1, vs-cb3 that holds 4096 vectors (the per-instance window), and
// MergedSkeleton_DetectBoneDataSource the rolling bone buffer when vs-t0 is a buffer of 16-byte
// elements and such a window exists. A size the Core reads from the bound view is the declared
// block here; the caller checks the block against what it binds.
// Without the rolling buffer, MergedSkeleton_DetectBoneDataSource takes a constant buffer whose declared size is
// 4096 vectors at vs-cb4, then at vs-cb3 (`vs->cb_size(4) == 4096`, "Sometimes used for FX shaders. Both bone data
// offsets and bone data entries are supplied via vs-cb4"): `constant_bones` and its slot (D-186).
struct CoreSourceFacts {
    int32_t vs_config_slot = -1;  // 1, 2 or 3; -1 none
    const SourceRegister* config = nullptr, *bones = nullptr;
    bool rolling_bones = false;
    const SourceRegister* constant_bones = nullptr;
    uint32_t constant_slot = 0;   // 3 or 4; 0 none
};
CoreSourceFacts core_source_facts(const std::vector<SourceRegister>& vertex_registers);

} // namespace efmi::gpu
