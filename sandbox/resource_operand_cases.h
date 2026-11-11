// Original operands the owned domain answers: rt_width/rt_height from o0 (ProcessParamRTSize)
// and ->hashregion/->spatialhash on owned resources (GetResourceRegionHash/SpatialHash).
static void original_owned_operands(Device& d,const std::filesystem::path& root){
    ShaderFixture f(d,root,R"ini(namespace = compute
[Constants]
global $none_w = 0
global $window_none = 0
global $wide_w = 0
global $wide_h = 0
global $cached_w = 0
global $array_w = 0
global $window_w = 0
global $region = 0
global $spatial = 0
global $image_region = 0
global $empty_region = 0
global $cb_region = 0
[ResourceWide]
type = Texture2D
width = 700
height = 2
array = 1
mips = 1
msaa = 1
format = R8G8B8A8_UNORM
bind_flags = render_target shader_resource
[ResourceArray]
type = Texture2D
width = 4
height = 4
array = 2
mips = 1
msaa = 1
format = R8G8B8A8_UNORM
bind_flags = render_target shader_resource
[ResourceBuffer]
type = Buffer
format = R32_FLOAT
data = R32_FLOAT 1 2 3 4
[ResourceEmpty]
[ResourceConstants]
type = Buffer
bind_flags = constant_buffer
data = R32_FLOAT 1 2 3 4
[Present]
$none_w = rt_width
$window_none = window_width
o0 = ResourceWide
$wide_w = rt_width
$wide_h = rt_height
o0 = ResourceArray
$cached_w = rt_width
$region = ResourceBuffer->hashregion(0, 8)
$spatial = ResourceBuffer->spatialhash(0, 1, 2, 1.5)
$image_region = ResourceWide->hashregion(0, 4)
$empty_region = ResourceEmpty->hashregion(0, 4)
vs-cb3 = ResourceConstants
$cb_region = vs-cb3->hashregion(0, 16)
post $array_w = rt_width
post o0 = ResourceWide
post $window_w = window_width
post o0 = null
)ini");
    f.present();
    check(f.variable("none_w")==-1 && f.variable("window_none")==1920,"Without a render target rt_width is the original -1 and window_width reaches its fallback");
    check(f.variable("wide_w")==700 && f.variable("wide_h")==2,"rt_width and rt_height are asked again once o0 is bound");
    check(f.variable("cached_w")==700,"The original keeps the first bound size for the rest of the command list");
    check(f.variable("array_w")==-1,"A Texture2DArray view on o0 reports no size, as OMGetRenderTargets does");
    check(f.variable("window_w")==700,"window_width falls back to the size of o0");
    check(f.variable("region")==-1 && f.variable("spatial")==-1 && f.variable("cb_region")==-1,
          "An owned buffer has no original data cache, so its hashes stay UNKNOWN");
    check(f.variable("image_region")==-3 && f.variable("empty_region")==-2,"Hash queries on an image and on a null resource keep the original sentinels");
    check(f.backend->resources().binding(L'\0',ResourceCopyTargetType::RENDER_TARGET,0).image==nullptr,"The fixture leaves o0 unbound");
    f.release();
    {   // GetVertexBufferRegionOffset reads the draw call, which a Present list does not have.
        ShaderFixture vertex(d,root/"vertex",R"ini(namespace = compute
[Constants]
global $value = 0
[ResourceBuffer]
type = Buffer
stride = 4
bind_flags = vertex_buffer
data = R32_FLOAT 1 2 3 4
[Present]
vb0 = ResourceBuffer
$value = vb0->hashregion(0, 4)
)ini");
        // The command that asks does nothing and the Present goes on, said once (it ended the Present before).
        vertex.present();
        bool told=false;
        for(const auto& gap:vertex.ini->status().gaps)told=told || gap.find("requires a draw context")!=std::string::npos;
        // D-163: the operand reads the table's value (the original's UNKNOWN, -1), said once, and the assignment runs.
        check(told && vertex.variable("value")==-1,"A vertex buffer hash outside a draw reads the original's unknown, where the original dereferences no draw call");
        vertex.release();
    }
}
