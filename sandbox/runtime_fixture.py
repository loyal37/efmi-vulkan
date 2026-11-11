"""Original INI commands over the existing independently pixel-checked scene."""
import json
from pathlib import Path
import struct
from character_test import fixture_package, material_package, NONE
from efmi_vk.hashes import crc32c

CORE = Path("D:/XXMI-Launcher-Portable-v2.1.6/EFMI/Core/EFMI")
# The scene's application texture at the fixture shader's t1: a 64x64 R8G8B8A8_UNORM image that is
# sampled and never uploaded, so its original hash is the crc32c of its D3D11_TEXTURE2D_DESC alone
# (D-077). The same description over uploaded data has another hash, which must not match.
APP_TEXTURE_DESC = struct.pack("<11I", 64, 64, 1, 1, 28, 1, 0, 0, 8, 0, 0)
APP_TEXTURE_HASH = crc32c(APP_TEXTURE_DESC)
OTHER_TEXTURE_HASH = crc32c(APP_TEXTURE_DESC, 1)


COMMON = r"""
[CommandList_Component_DrawInstances]
handling = skip
$\EFMIv1\component_count = $component_count
$\EFMIv1\bones_count = $bones_count
$\EFMIv1\instance_count = $max_instance_count
run = CommandList\EFMIv1\Object_ReadConfig
$\EFMIv1\lod_level = $lod_level
$\EFMIv1\custom_mesh_scale = 1.00
run = CommandList\EFMIv1\Component_ReadConfig
Pool\EFMIv1\Input_ObjectSpatialIdentity = ref Pool_ObjectSpatialIdentity
run = CommandList\EFMIv1\SpatialIdentity_IdentifyComponentInstances
CommandList\EFMIv1\Callback_MergedSkeleton_ConnectComponent = ref CommandList_MergedSkeleton_ConnectComponent
run = CommandList\EFMIv1\Component_DrawInstances
[CommandList_MergedSkeleton_ConnectComponent]
if !$merged_skeleton_initialized
 $merged_skeleton_initialized = 1
 run = CommandListInitializeMergedSkeleton
endif
Pool\EFMIv1\Input_MergedSkeleton_Component_VertexGroupOffsets = ref Pool_MergedSkeleton_Component_VertexGroupOffsets
Pool\EFMIv1\Input_MergedSkeleton_Component_VertexGroupCounts = ref Pool_MergedSkeleton_Component_VertexGroupCounts
Pool\EFMIv1\Input_MergedSkeleton_Component_LodRemaps = ref Pool_MergedSkeleton_Component_LodRemaps
Pool\EFMIv1\Input_MergedSkeleton_Instance_UpdateFrame = ref Pool_MergedSkeleton_Instance_UpdateFrame
Pool\EFMIv1\Input_MergedSkeleton_Instance_LodLevel = ref Pool_MergedSkeleton_Instance_LodLevel
Resource\EFMIv1\Output_MergedSkeleton = ref ResourceMergedSkeletonDataRW
run = CommandList\EFMIv1\MergedSkeleton_AttachComponent
vb2->ElementFormat(BLENDINDICES, 0) = R16G16B16A16_UINT
[CommandListInitializeMergedSkeleton]
Resource\EFMIv1\OutputMergedSkeleton_Template = ref ResourceMergedSkeletonDataRW
run = CommandList\EFMIv1\InitializeMergedSkeleton
local $levels = $\EFMIv1\cfg_ms_max_lod_level_count
"""


def fixture(folder, salt=0, settings="fixture", name="character.bin", namespace="runtime_fixture", texture_properties=False):
    package = folder / name
    fixture_package(package, salt)
    data = package.read_bytes()
    bones, nc, ns, nt, nl, nd, ne, crc, payload_size = struct.unpack_from("<8IQ", data, 8)
    source_data = data[-payload_size:]
    config = dict(schema=1, ini=str(folder / "mod.ini"), core=str(CORE), namespace=namespace, settings=settings)
    declarations = []
    for i in range(ns):
        kind, stride, start, size, bone_index, bone_weight = struct.unpack_from("<2I2Q2I", data, 48 + nc * 24 + i * 32)
        buf = folder / f"stream-{i}.buf"
        buf.write_bytes(source_data[start:start + size])
        config[f"stream_{i}"] = str(buf)
        declarations += [f"[ResourceS{i}]", "type = Buffer", f"filename = {buf.name}"]
        declarations += [f"format = R{stride * 8}_UINT"] if kind else [f"stride = {stride}"]
    sections = [struct.unpack_from("<5I", data, 48 + nc * 24 + ns * 32 + i * 20) for i in range(nt)]
    material_package(package)
    Path(str(package) + ".runtime.json").write_text(json.dumps(config, indent=2), encoding="ascii")
    text = "namespace = " + namespace + """
[Constants]
global persist $tint = 0
global $active = 0
global $component_count = 3
global $bones_count = 5
global $max_instance_count = 8
global $lod_level = 0
global $merged_skeleton_initialized = 0
global $texture_seen = 0
global $texture_other = 0
global $texture_format = -9
global $texture_width = -9
global $texture_height = -9
global $texture_mips = -9
global $texture_array = -9
global $texture_absent = -9
global $stored = 0
[KeyTint]
key = ctrl 5
condition = $active == 1
type = cycle
$tint = 0,1,
[Present]
post $active = 0
"""
    text += f"\n[TextureOverride_AppTexture]\nhash = {APP_TEXTURE_HASH:08x}\n$texture_seen = 1\n"
    text += f"\n[TextureOverride_OtherTexture]\nhash = {OTHER_TEXTURE_HASH:08x}\n$texture_other = 1\n"
    for i, (hashed, count, first, component, lod) in enumerate(sections):
        text += f"\n[TextureOverride_EntryPoint_{i}]\nhash = {hashed:08x}\nmatch_index_count = {count}\n"
        if first != NONE:
            text += f"match_first_index = {first}\n"
        if lod != NONE:
            text += f"$lod_level = {lod}\n"
        # What the lists read of the application's texture at t1 before they bind their own (D-170), and of a
        # slot the fragment module has no register for. Only where the layer is the host: the offline planner's
        # stand-in does not describe the game's textures.
        if texture_properties:
            text += """$texture_format = ps-t1->Format
$texture_width = ps-t1->Width
$texture_height = ps-t1->Height
$texture_mips = ps-t1->Mips
$texture_array = ps-t1->Array
$texture_absent = ps-t9->Width
"""
        text += fr"""$\EFMIv1\component_id = {component}
$\EFMIv1\gpu_posed = 1
CommandList\EFMIv1\Callback_Component_DrawCustom = ref CommandListDraw{component}
run = CommandList_Component_DrawInstances
store = $stored, ResourceStoreSource, 0
$active = 1
"""
    text += COMMON
    for i, (first, count) in enumerate(((0, 2), (2, 2), (4, 1))):
        text += f"""$Pool_MergedSkeleton_Component_VertexGroupOffsets[{i}] = {first}
$Pool_MergedSkeleton_Component_VertexGroupCounts[{i}] = {count}
Pool_MergedSkeleton_Component_LodRemaps[{i}*$levels+0] = null
Pool_MergedSkeleton_Component_LodRemaps[{i}*$levels+1] = {('ref ResourceS0' if i == 0 else 'null')}
"""
    for name, size in (("ObjectSpatialIdentity", "$max_instance_count"),
                       ("MergedSkeleton_Component_VertexGroupOffsets", "$component_count"),
                       ("MergedSkeleton_Component_VertexGroupCounts", "$component_count"),
                       ("MergedSkeleton_Component_LodRemaps", "$component_count * $\\EFMIv1\\cfg_ms_max_lod_level_count"),
                       ("MergedSkeleton_Instance_UpdateFrame", "$component_count * $max_instance_count"),
                       ("MergedSkeleton_Instance_LodLevel", "$component_count * $max_instance_count")):
        text += f"\n[Pool_{name}]\npool_size = {size}\n"
    # A store in a replaced draw (D-147) reads this Mod buffer.
    text += "\n[ResourceStoreSource]\ntype = Buffer\nformat = R32_FLOAT\ndata = 7\n"
    text += """
[ResourceMergedSkeletonDataRW]
type = RWBuffer
bind_flags = shader_resource unordered_access
format = R32G32B32A32_FLOAT
array = (512 + 2 * $bones_count * $max_instance_count) * 3
[ResourceRed]
filename = red.dds
[ResourceGreen]
filename = green.dds
[ResourceUnused]
filename = missing-unused.dds
"""
    text += "\n".join(declarations) + "\n"

    # `vb3 = vb0` takes the buffer bound at vb0 by reference (the original's default between two vertex
    # slots), as Tangtang's lists do; the picture is the same as with vb3 named directly.
    def draw(position, blend, copy=False):
        vb3 = "vb0" if copy else f"ResourceS{position}"
        return f"vb0 = ResourceS{position}\nvb1 = ResourceS2\nvb2 = ResourceS{blend}\nvb3 = {vb3}\ndrawindexed = 3,0,0\n"

    text += r"""
[CommandListDraw0]
run = CommandList\EFMIv1\OverrideTextures
ib = ResourceS1
if $tint == 0
 ps-t1 = ResourceRed
else
 ps-t1 = ResourceGreen
endif
ps-t18 = ResourceGreen
vb2->ElementFormat(BLENDINDICES, 0) = R16G16B16A16_UINT
""" + draw(3, 4)
    text += r"""
[CommandListDraw1]
run = CommandList\EFMIv1\OverrideTextures
ib = ResourceS1
ps-t1 = ResourceGreen
""" + draw(5, 6) + "ps-t1 = null\n" + draw(7, 8, copy=True)
    text += "\n[CommandListDraw2]\nrun = CommandList\\EFMIv1\\OverrideTextures\n"
    (folder / "mod.ini").write_text(text, encoding="ascii")
    (folder / "keys.txt").write_text("1\n", encoding="ascii")
    return package
