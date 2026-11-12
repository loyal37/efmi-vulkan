#include "resource_plan.h"
#include "layer_gap.h"
#include "../assets/png.h"
#include "util.h"
#include <stdexcept>

namespace efmi::frontend {
namespace {
std::shared_ptr<const assets::FileResource> describe(const CustomResource& r,unsigned role,std::wstring& reason)
{
    auto refuse=[&](const wchar_t* text)->std::shared_ptr<const assets::FileResource>{reason=text;return {};};
    if(r.filename.empty())return refuse(L"Binding needs a file-backed resource adapter");
    if(r.resource || r.view || r.device || r.substantiated || r.pool || r.pool_index>=0 || r.offset ||
       r.initial_data || r.initial_data_size)return refuse(L"Mutable, pooled or initialized resource requires its native adapter");
    const std::filesystem::path path(r.filename);
    if(!path.is_absolute() || r.filename.size()>32760 || r.filename.find(L'\0')!=std::wstring::npos || r.name.size()>4096)
        return refuse(L"Resource path or name outside owned declaration limits");
    auto file=std::make_shared<assets::FileResource>();file->name=r.name;file->path=path.lexically_normal();
    if(role==2){
        // A .dds by the DDS loader, any other file by WIC, as the original's LoadFromFile (D-139). The loaders
        // take the declaration's colour space and bind flags and nothing of its description (size, mips,
        // array, misc, stride: DirectXTK makes the texture from the file); the binding's view takes its
        // format, which a typed texture must have (CreateCompatibleView), D-140.
        if(r.override_type!=CustomResourceType::INVALID && r.override_type!=CustomResourceType::TEXTURE2D)
            return refuse(L"Pixel file binding requires a texture declaration");
        file->loader_flags=r.override_color_space==CustomColorSpace::SRGB?assets::png_force_srgb:
                           r.override_color_space==CustomColorSpace::LINEAR?assets::png_ignore_srgb:0u;
        file->view_format=r.override_format==DXGI_FORMAT(-1)?0u:UINT(r.override_format);
        file->sampled=r.override_bind_flags==CustomResourceBindFlags::INVALID ||
                      (UINT(r.override_bind_flags)&D3D11_BIND_SHADER_RESOURCE)!=0;
        return file;
    }
    if(r.override_misc_flags!=ResourceMiscFlags::INVALID || r.misc_flags ||
       r.override_width!=-1 || r.override_height!=-1 || r.override_depth!=-1 || r.override_mips!=-1 ||
       r.override_array!=-1 || r.override_msaa!=-1 || r.override_msaa_quality!=-1 ||
       r.override_byte_width!=-1 || r.width_multiply!=1.0f || r.height_multiply!=1.0f)
        return refuse(L"Resource transformation is not supported by immutable file upload");
    if(r.override_type!=CustomResourceType::BUFFER)return refuse(L"Mesh file binding requires a plain Buffer declaration");
    const auto flags=static_cast<unsigned>(r.override_bind_flags);
    const auto required=role==0?D3D11_BIND_VERTEX_BUFFER:D3D11_BIND_INDEX_BUFFER;
    if(flags && ((flags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER)) || !(flags&required)))
        return refuse(L"Explicit buffer bind flags do not support this binding");
    if(role==0){
        // The original binds it with IASetVertexBuffers and the buffer's StructureByteStride: `stride`, or the
        // format's size without one. The format is otherwise a view's; the input layout decides how the
        // vertex fetch reads the elements.
        int stride=r.override_stride;
        if(stride==-1 && r.override_format!=DXGI_FORMAT(-1) && r.override_format!=DXGI_FORMAT_UNKNOWN)
            stride=static_cast<int>(dxgi_format_size(r.override_format));
        if(stride<=0 || stride>2048)return refuse(L"Vertex stride is not a supported explicit byte stride");
        file->usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;file->stride=static_cast<uint32_t>(stride);return file;
    }
    if(r.override_format==DXGI_FORMAT_R32_UINT){file->index_type=VK_INDEX_TYPE_UINT32;file->stride=4;}
    else if(r.override_format==DXGI_FORMAT_R16_UINT){file->index_type=VK_INDEX_TYPE_UINT16;file->stride=2;}
    else return refuse(L"Index binding requires R16_UINT or R32_UINT");
    // A stride beside the format (some packagers write 6, a triangle of 16-bit indices) is what the original
    // puts into the buffer description's StructureByteStride; IASetIndexBuffer takes the format alone.
    file->usage=VK_BUFFER_USAGE_INDEX_BUFFER_BIT;return file;
}
assets::LayoutEdit copy_layout(const InputLayoutElementOverride& source)
{
    assets::LayoutEdit out;
    out.match={source.match.semantic_name,source.match.semantic_index,source.match.input_slot,
               static_cast<uint32_t>(source.match.format),source.match.format_byte_size,source.match.aligned_byte_offset};
    out.replace={source.replace.semantic_name,source.replace.semantic_index,static_cast<uint32_t>(source.replace.format),source.replace.aligned_byte_offset};
    return out;
}
}
FilePlanCompiler::FilePlanCompiler(const CustomResources& resources)
{
    if(resources.size()>4096)throw std::invalid_argument("Frontend resource declaration budget exceeded");
    for(const auto& pair:resources){
        Declaration d;
        for(unsigned role=0;role<3;++role)d.bindings[role]=describe(pair.second,role,d.reasons[role]);
        declarations_.emplace(&pair.second,std::move(d));
    }
}
std::shared_ptr<const assets::FileResource> FilePlanCompiler::binding(const CustomResource* resource,PlanOperation operation) const
{
    const auto found=declarations_.find(resource);
    if(found==declarations_.end())return {};
    return found->second.bindings[operation==PlanOperation::BindVertex?0:operation==PlanOperation::BindIndex?1:2];
}
bool FilePlanCompiler::compile(const DrawPlan& source,assets::FilePlan& output,std::wstring& reason) const
{
    reason.clear();
    auto refuse=[&](const wchar_t* text){reason=text;return false;};
    if(source.items.size()>4096)return refuse(L"Resource plan operation budget exceeded");
    assets::FilePlan& next=next_;next.items.clear();next.skip_original=source.skip_original;next.pre_items=source.pre_items;size_t draws=0;
    for(const auto& item:source.items){
        assets::FilePlanItem own;own.slot=item.slot;own.application_slot=item.application_slot;
        if(item.owned){
            if(item.operation!=PlanOperation::BindVertex || item.resource)return refuse(L"Owned buffer outside a vertex binding");
            const auto& o=*item.owned;
            own.owned=std::make_shared<const assets::OwnedBuffer>(assets::OwnedBuffer{o.buffer,o.offset,o.size,o.stride,o.keep});
        }
        switch(item.operation){
        case PlanOperation::BindVertex: own.operation=assets::FileOperation::BindVertex;break;
        case PlanOperation::BindIndex: own.operation=assets::FileOperation::BindIndex;break;
        case PlanOperation::BindPixelResource: own.operation=assets::FileOperation::BindPixelResource;break;
        case PlanOperation::DrawIndexed:
            if((!item.index_count && !item.automatic) || !item.instance_count || ++draws>256 || item.layout.size()>256)
                return refuse(L"Invalid indexed draw in resource plan");
            own.operation=assets::FileOperation::DrawIndexed;
            own.index_count=item.index_count;own.first_index=item.first_index;own.vertex_offset=item.vertex_offset;
            own.instance_count=item.instance_count;own.first_instance=item.first_instance;
            own.indexed=item.indexed;own.automatic=item.automatic;
            own.palette=item.palette;
            for(const auto& layout:item.layout)own.layout.push_back(copy_layout(layout));
            next.items.push_back(std::move(own));continue;
        case PlanOperation::Translucent:
            if(!item.index_count || ++draws>256 || item.layout.size()>256)return refuse(L"Invalid translucent draw in resource plan");
            own.operation=assets::FileOperation::Translucent;
            own.index_count=item.index_count;own.first_index=item.first_index;own.vertex_offset=item.vertex_offset;
            own.palette=item.palette;own.alpha=item.alpha;own.mask_channel=item.mask_channel;own.mask_invert=item.mask_invert;own.shadow=item.shadow;
            for(const auto& layout:item.layout)own.layout.push_back(copy_layout(layout));
            if(item.resource){
                auto found=declarations_.find(item.resource);
                if(found==declarations_.end())return refuse(L"Translucency mask belongs to another frontend generation");
                own.resource=found->second.bindings[2];
                if(!own.resource){reason=found->second.reasons[2];return false;}
            }
            next.items.push_back(std::move(own));continue;
        case PlanOperation::External:
            if(item.external.empty() || item.external.size()>4096 || !item.phase_mask || item.phase_mask>3)
                return refuse(L"Invalid native boundary in resource plan");
            own.operation=assets::FileOperation::External;own.external=item.external;own.phase_mask=item.phase_mask;
            next.items.push_back(std::move(own));continue;
        default: return refuse(L"Unknown resource plan operation");
        }
        const unsigned role=item.operation==PlanOperation::BindVertex?0:item.operation==PlanOperation::BindIndex?1:2;
        if(item.slot>=(role==0?32u:role==1?1u:128u))return refuse(L"Resource binding slot out of range");
        if(item.resource){
            auto found=declarations_.find(item.resource);
            if(found==declarations_.end())return refuse(L"Resource belongs to another frontend generation");
            own.resource=found->second.bindings[role];
            if(!own.resource){
                // A buffer the Mod's mesh needs and the layer cannot make of the declaration: the draw
                // cannot be the Mod's.
                if(role!=2){reason=found->second.reasons[role];return false;}
                // At a pixel slot (the draw state's table, draw_state.h): a resource of the Mod that is no
                // image file (a buffer, a target of its owned work) is not what the layer binds for the
                // game's fragment shader. The application's texture stays there, the Mod's mesh is drawn,
                // and the notice says so once per resource. It refused the whole draw before.
                own.application_slot=int32_t(item.slot);
                if(told_.size()<64 && told_.insert(item.resource).second)
                    LayerGap(L"The Vulkan layer cannot bind %ls at a pixel slot of a replaced draw (%ls); the game's texture stays there",
                             item.resource->name.c_str(),found->second.reasons[2].c_str());
            }
        }
        next.items.push_back(std::move(own));
    }
    std::swap(output,next);return true;
}
}
