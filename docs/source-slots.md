# DX11 registers from the game's SPIR-V (D-076)

Mods address the game's resources by DX11 register: `ps-t16 = ResourceDiffuse` in a matched draw,
`checktextureoverride = ps-t17`, `vs-cb1` windows. Under Vulkan the same draw binds those resources
at a set and binding. The correspondence is derived from the Vulkan shader alone, so it holds for
every character and Mod, including shaders nobody captured (D-075).

## The rule

The game compiles each shader twice from one HLSL source: FXC for DX11 and DXC for Vulkan
(SPIR-V generator 14). Neither keeps names (no RDEF chunk, no OpName), but both keep the source's
declaration order: DXC emits the module's resource variables in that order, FXC numbers registers
in that order. The DX11 build additionally places read-only buffers ahead of textures (FXC alone
does not; checked with an own shader), so:

- the t register of a resource is its rank among the module's shader resource views, read-only
  buffers first (StructuredBuffer and ByteAddressBuffer as NonWritable storage buffers, typed
  Buffer as texel buffers), then images, each group in declaration order; an array takes as many
  registers as it has elements;
- constant buffers: cb registers in the declaration order of uniform blocks. The Vulkan block is
  as large as the DX11 one or larger (layout padding) in nearly all cases, so this is checked by
  size only;
- samplers (s) and UAVs (u) are numbered in declaration order without ground truth yet.

`server/efmi_vk/source_slots.py` implements it (`declarations`, `registers`, `binding_of`).

## Evidence

`docs/source-slot-truth.json` holds, per game shader of paired scenes, the resource declarations
read from its SPIR-V (no byte code) and every slot the catalogue placed in one binding:
258 shaders from the 2026-10-19 catalogue (16 characters) and the 2026-10-12/13 arcane frames.
`scripts/check_source_slots.py` (suite step) applies the rule: all 1187 observed t registers of
pixel and vertex shaders, textures and buffers, come out as observed. 1121 constant buffer windows
agree; 5 differ, where the catalogue paired windows by size alone. `--refresh` rebuilds the truth
file from catalogues and their capture bundles.

Unit tests compile an own HLSL fixture with DXC and check kinds, order, array counts and register
numbers of the parser.

## Declarations (D-124)

`vs->srv_type(n)`, `srv_stride(n)` and `srv_dimension(n)` read the declaration of register tN as
the original's ByteCodeReader takes it off the shader's DXBC. The register above gives it:

- an image: typed (1), its view dimension from Dim, Arrayed and MS (2D 4, 2D array 5, 3D 8, cube 9,
  cube array 10, ...);
- a texel buffer: typed (1), dimension buffer (1);
- a read-only storage buffer: structured (2), dimension buffer (1), stride its array stride; except
  an array of 32-bit unsigned words, which DXC makes of a ByteAddressBuffer and of a
  StructuredBuffer<uint> alike: not decided;
- a slot without a register, below t60: not declared (type 0; `srv_stride` and `srv_dimension` read -1), as the
  original answers a slot its DXBC does not declare (D-159);
- a slot without a register, from t60 up: not decided.

`frontend/gpu/source_registers.cpp` (`source_srv_declaration`); `efmi_vk_character_test
--srv-declarations` prints it per module. `scripts/check_source_declarations.py` (suite step)
compares every answer with the DXBC of the paired catalogue (2026-10-19, 180 shader pairs: 59
vertex, 121 pixel): all 1555 answers equal the DX11 declaration. Of the 146 undecided word
buffers DX11 declares 74 structured and 72 raw, so neither answer would be right. The DX11 builds
of the pixel shaders declare 554 registers their SPIR-V lacks, in 87 pairs (from t60 upward in
84, from t3 in 3). From t60 up a slot without a register is therefore not taken as undeclared. Below t60 it
is (D-159, 2026-11-10): of the 9094 slots below t60 that a SPIR-V of the pairs has no register for, the
DX11 build declares 3 (one pixel register, t3, in the pairs of two shaders) and none of the other 9091;
the check lists them and fails when there are more. 5 registers only
the SPIR-V build has (a vertex t0 in two pairs of one DX11 shader, a pixel t12 or t13 in three) read
as declared where DX11 would answer none.

Constant buffer declarations are not answered: FXC declares the highest vector a shader reads
(smaller than the SPIR-V block for 641 of 968 paired buffers, larger for 10), and the cb numbering
is checked by size only.

## Limits

- A shader whose DX11 build declares a resource the SPIR-V does not (or the other way round)
  would shift the following registers. The DXBC comparison above found such registers only after
  the SPIR-V's last one or where DX11 leaves a gap, never shifting a later register.
- The DX11 builds use explicit `register()` assignments for some textures (t60 and up, the gaps at
  t12 and t13); the SPIR-V builds do not declare those textures.
- Constant buffer member layouts differ (the Vulkan blocks are padded), so byte offsets inside a
  window still need translation where a Mod reads game constants.
