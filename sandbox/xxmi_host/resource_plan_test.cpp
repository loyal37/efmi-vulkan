#include "resource_plan.h"
#include <cstdio>

using namespace efmi::frontend;
namespace assets=efmi::assets;
int HostResourcePlanTests()
{
    int passed=0,failed=0;
    auto check=[&](bool ok,const char* text){if(ok)++passed;else{++failed;printf("FAIL: %s\n",text);}};
    CustomResources resources;
    auto define=[&](const wchar_t* name,const wchar_t* file,CustomResourceType type)->CustomResource*{
        auto& r=resources[name];r.name=name;r.filename=(std::filesystem::current_path()/file).wstring();
        r.override_type=type;r.override_color_space=CustomColorSpace::DEFAULT;return &r;
    };
    auto* vertex=define(L"vertex",L"nonexistent-owned.buf",CustomResourceType::BUFFER);vertex->override_stride=20;
    auto* index=define(L"index",L"nonexistent-index.buf",CustomResourceType::BUFFER);index->override_format=DXGI_FORMAT_R16_UINT;
    auto* image=define(L"image",L"nonexistent-image.dds",CustomResourceType::INVALID);
    auto* unused=define(L"unused",L"missing.png",CustomResourceType::TEXTURE3D);
    auto* changed=define(L"transformed",L"changed.buf",CustomResourceType::BUFFER);changed->override_byte_width=128;
    auto* pool=define(L"pool",L"pool.buf",CustomResourceType::BUFFER);pool->pool_index=0;
    auto* relative=define(L"relative",L"relative.buf",CustomResourceType::BUFFER);relative->filename=L"relative.buf";
    auto* srgb=define(L"srgb",L"color.dds",CustomResourceType::INVALID);srgb->override_color_space=CustomColorSpace::SRGB;
    FilePlanCompiler compiler(resources);
    DrawPlan plan;plan.skip_original=true;
    auto bind=[&](PlanOperation op,unsigned slot,CustomResource* r){PlanItem item;item.operation=op;item.slot=slot;item.resource=r;plan.items.push_back(item);};
    PlanItem external;external.operation=PlanOperation::External;external.external=L"native/materials";external.phase_mask=3;plan.items.push_back(external);
    bind(PlanOperation::BindVertex,5,vertex);bind(PlanOperation::BindIndex,0,index);
    bind(PlanOperation::BindPixelResource,17,image);bind(PlanOperation::BindPixelResource,18,nullptr);
    PlanItem draw;draw.operation=PlanOperation::DrawIndexed;draw.index_count=6;draw.first_index=4;draw.vertex_offset=-2;
    draw.instance_count=2;draw.first_instance=7;
    InputLayoutElementOverride edit;edit.match.semantic_name="BLENDINDICES";edit.match.semantic_index=0;edit.match.input_slot=5;
    edit.replace.format=DXGI_FORMAT_R16G16B16A16_UINT;draw.layout.push_back(edit);plan.items.push_back(draw);
    assets::FilePlan owned;std::wstring reason;
    check(compiler.compile(plan,owned,reason),"file declarations compile without opening nonexistent files");
    check(owned.skip_original && owned.items.size()==6 && owned.items[0].external==L"native/materials" && owned.items[0].phase_mask==3,
          "native boundary and original skip survive resource ownership conversion");
    check(owned.items[1].resource && owned.items[1].resource->stride==20 && owned.items[1].resource->usage==VK_BUFFER_USAGE_VERTEX_BUFFER_BIT &&
          owned.items[2].resource && owned.items[2].resource->index_type==VK_INDEX_TYPE_UINT16 && owned.items[2].resource->stride==2,
          "vertex byte stride and index element format are independent owned contracts");
    check(owned.items[3].resource && !owned.items[3].resource->usage && !owned.items[4].resource && image->is_null,
          "unloaded file declarations are distinct from explicit null bindings");
    check(owned.items[5].index_count==6 && owned.items[5].first_index==4 && owned.items[5].vertex_offset==-2 &&
          owned.items[5].instance_count==2 && owned.items[5].first_instance==7 && owned.items[5].layout[0].replace.format==DXGI_FORMAT_R16G16B16A16_UINT,
          "all indexed arguments and layout edits retain execution order and values");
    const auto previous=owned.items[1].resource;
    vertex->filename=L"Z:/edited.buf";vertex->override_stride=4;
    check(compiler.compile(plan,owned,reason) && owned.items[1].resource==previous && owned.items[1].resource->stride==20,
          "catalog snapshot is immutable across later frontend declaration edits");
    auto bad=plan;
    for(auto* resource:{unused,changed,pool,relative}){
        // The draw state's table (draw_state.h): a resource of the Mod that is no image the layer makes at a
        // pixel slot leaves the application's texture there (it refused the whole plan before); as a vertex
        // buffer the Mod's geometry cannot be made of it, and the plan is refused whole.
        bad.items.back()={};bad.items.back().operation=PlanOperation::BindPixelResource;bad.items.back().slot=9;bad.items.back().resource=resource;
        check(compiler.compile(bad,owned,reason) && owned.items.size()==6 && !owned.items.back().resource && owned.items.back().application_slot==9 &&
              owned.items[1].resource==previous,
              "a resource the layer cannot bind at a pixel slot leaves the application's texture there");
        check(compiler.compile(plan,owned,reason),"the valid plan compiles again");
        bad.items.back().operation=PlanOperation::BindVertex;bad.items.back().slot=1;
        check(!compiler.compile(bad,owned,reason) && !reason.empty() && owned.items.size()==6 && owned.items[1].resource==previous,
              "late unsupported vertex buffer refuses atomically while dormant declarations are harmless");
    }
    // D-140: a texture's colour space is the loaders' flag (and its identity), as LoadFromFile passes it.
    bad=plan;bad.items.back()={};bad.items.back().operation=PlanOperation::BindPixelResource;bad.items.back().resource=srgb;
    check(compiler.compile(bad,owned,reason) && owned.items.back().resource && owned.items.back().resource->loader_flags==1u &&
          !owned.items.back().resource->view_format && owned.items.back().resource->sampled,
          "a texture's colour space reaches the loader as its flag, the view keeps the texture's format");
    CustomResource foreign;bad=plan;bad.items[1].resource=&foreign;
    check(!compiler.compile(bad,owned,reason),"foreign frontend resource is refused without dereferencing it");
    bad=plan;bad.items[1].slot=32;check(!compiler.compile(bad,owned,reason),"out of range owned binding refuses");
    bad=plan;bad.items[0].phase_mask=0;check(!compiler.compile(bad,owned,reason),"invalid native phase cannot vanish into a no-op");
    // An index-format file bound as a vertex buffer: the original's FillInMissingInfo fills the missing
    // stride from the format's size (CopyResourceToResource, before both its copy and reference paths).
    bad=plan;bad.items[2].operation=PlanOperation::BindVertex;
    check(compiler.compile(bad,owned,reason) && owned.items[2].resource && owned.items[2].resource->stride==2 &&
          owned.items[2].resource->usage==VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,"an index format bound as a vertex buffer strides by its size, as the original fills it");
    check(compiler.compile(plan,owned,reason),"the valid plan compiles again after the vertex binding");
    bad=plan;bad.items[2].slot=1;check(!compiler.compile(bad,owned,reason),"index slot is bounded independently of vertex slots");
    bad=plan;bad.items.back().instance_count=0;check(!compiler.compile(bad,owned,reason),"malformed zero draw refuses before resource preparation");
    resources.clear();plan.items.clear();
    check(owned.items[1].resource==previous && previous->stride==20 && previous->path.filename()==L"nonexistent-owned.buf" &&
          owned.items.back().layout[0].match.semantic_name=="BLENDINDICES",
          "owned file plan and layout survive destruction of the entire frontend declaration map");
    check(compiler.compile({},owned,reason) && owned.items.empty(),"empty plan remains a valid empty resource transaction");
    {
        // An index buffer declared with a stride besides its format (stride = 6, a triangle of 16-bit
        // indices, as some packagers write it): the original binds it with IASetIndexBuffer(format) and
        // leaves the stride to the buffer description's StructureByteStride, so the format decides.
        CustomResources declared;
        auto& r=declared[L"triangles"];r.name=L"triangles";r.filename=(std::filesystem::current_path()/L"triangles.buf").wstring();
        r.override_type=CustomResourceType::BUFFER;r.override_color_space=CustomColorSpace::DEFAULT;
        r.override_format=DXGI_FORMAT_R16_UINT;r.override_stride=6;
        FilePlanCompiler strided(declared);DrawPlan bound;PlanItem item;item.operation=PlanOperation::BindIndex;item.slot=0;item.resource=&r;
        bound.items.push_back(item);assets::FilePlan out;
        check(strided.compile(bound,out,reason) && out.items.size()==1 && out.items[0].resource &&
              out.items[0].resource->index_type==VK_INDEX_TYPE_UINT16 && out.items[0].resource->stride==2,
              "an index buffer's stride does not change its format's index size, as IASetIndexBuffer takes the format");
        // A vertex buffer with a typed format: the original binds it with its StructureByteStride (`stride`,
        // else the format's size); the format is a view's, not the vertex fetch's.
        auto vertex_declaration=[&](const wchar_t* name,int stride)->CustomResource&{
            auto& v=declared[name];v.name=name;v.filename=(std::filesystem::current_path()/L"typed.buf").wstring();
            v.override_type=CustomResourceType::BUFFER;v.override_color_space=CustomColorSpace::DEFAULT;
            v.override_format=DXGI_FORMAT_R16G16B16A16_UNORM;v.override_stride=stride;return v;
        };
        auto& typed=vertex_declaration(L"typed",16);
        auto& format_only=vertex_declaration(L"format_only",-1);
        FilePlanCompiler vertices(declared);
        for(auto* resource:{&typed,&format_only}){
            DrawPlan vertex;PlanItem bind;bind.operation=PlanOperation::BindVertex;bind.slot=1;bind.resource=resource;vertex.items.push_back(bind);
            const uint32_t expected=resource==&typed?16:8;
            check(vertices.compile(vertex,out,reason) && out.items.size()==1 && out.items[0].resource &&
                  out.items[0].resource->stride==expected && out.items[0].resource->usage==VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  "a typed vertex buffer binds with its stride, or its format's size without one");
        }
    }
    printf("XXMI resource planner: %d checks passed, %d failed\n",passed,failed);return failed;
}
