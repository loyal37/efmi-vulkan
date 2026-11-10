#include "gpu_resources.h"
#include "image_formats.h"
#include "globals.h"
#include "util.h"
#include "ResourceHash.h"
#include "Overlay.h"
#include "layer_gap.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <typeinfo>

void FrontendBufferViewElements(UINT,UINT,DXGI_FORMAT,UINT,UINT&,UINT&);

namespace efmi::frontend {
namespace {
using Target=ResourceCopyTargetType;
using Mode=ResourceCopyTargetEvaluationMode;
constexpr uint64_t max_bytes=128ull<<20;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::wstring widen(const char* text){std::string s(text);return {s.begin(),s.end()};}
bool swap_chain(Target t){return t==Target::SWAP_CHAIN || t==Target::REAL_SWAP_CHAIN || t==Target::FAKE_SWAP_CHAIN;}
uint32_t uint_value(float v){require(std::isfinite(v) && double(v)>=0 && double(v)<=UINT32_MAX,"GPU resource index outside unsigned range");return uint32_t(v);}
void validate_desc(const D3D11_BUFFER_DESC& d) {
    require(d.ByteWidth && d.ByteWidth<=max_bytes,"GPU buffer description exceeds size budget");
    require(!(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || (d.StructureByteStride && d.StructureByteStride%4==0 &&
            d.StructureByteStride<=D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES && d.ByteWidth%d.StructureByteStride==0 &&
            !(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)),"GPU structured buffer size/stride invalid");
    require(!(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || d.ByteWidth%4==0,"GPU raw buffer size invalid");
    require(!(d.BindFlags&D3D11_BIND_CONSTANT_BUFFER) || (d.ByteWidth%16==0 && d.ByteWidth<=65536),"GPU constant buffer size invalid");
}
VkBufferUsageFlags usage(const D3D11_BUFFER_DESC& d) {
    VkBufferUsageFlags result=0;
    if(d.BindFlags&D3D11_BIND_VERTEX_BUFFER)result|=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if(d.BindFlags&D3D11_BIND_INDEX_BUFFER)result|=VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if(d.BindFlags&D3D11_BIND_CONSTANT_BUFFER)result|=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if(d.BindFlags&D3D11_BIND_SHADER_RESOURCE)result|=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    if(d.BindFlags&D3D11_BIND_UNORDERED_ACCESS)result|=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if(d.MiscFlags&D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)result|=VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    // DX11 without its debug layer draws from a buffer referenced into a vertex or index slot it was
    // not created for (a shape-key Mod's compute output, `cs-u5 = copy ...` then `vb0 = ref`); Vulkan
    // needs the usage at creation, where an extra usage costs nothing.
    if(result&(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))
        result|=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    require(!(d.BindFlags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS)),"GPU buffer bind flags need another adapter");
    return result;
}
uint32_t bind_flag(Target t) {
    switch(t){
    case Target::VERTEX_BUFFER:return D3D11_BIND_VERTEX_BUFFER;
    case Target::INDEX_BUFFER:return D3D11_BIND_INDEX_BUFFER;
    case Target::CONSTANT_BUFFER:return D3D11_BIND_CONSTANT_BUFFER;
    case Target::SHADER_RESOURCE:return D3D11_BIND_SHADER_RESOURCE;
    case Target::UNORDERED_ACCESS_VIEW:return D3D11_BIND_UNORDERED_ACCESS;
    case Target::RENDER_TARGET:return D3D11_BIND_RENDER_TARGET;
    case Target::DEPTH_STENCIL_TARGET:return D3D11_BIND_DEPTH_STENCIL;
    default:throw std::runtime_error("GPU target needs a non-buffer adapter");
    }
}
void fill_buffer_info(GpuResourceBinding& b,Target type=Target::CUSTOM_RESOURCE){
    // CPU mirror of the resource/view facts consumed by original FillInMissingInfo.
    b.visible_size=b.visible_size?std::min(b.visible_size,b.description.ByteWidth):b.description.ByteWidth;
    if(!b.stride)b.stride=b.description.StructureByteStride;
    if(b.view!=GpuResourceBinding::View::None && (type==Target::SHADER_RESOURCE || type==Target::UNORDERED_ACCESS_VIEW)){
        if(b.format==DXGI_FORMAT_UNKNOWN)b.format=b.view_format;
        if(!b.stride)b.stride=dxgi_format_size(b.format);
        if(!b.offset)b.offset=b.view_offset;
    }
    if(!b.stride)b.stride=dxgi_format_size(b.format);
    if(!b.stride)b.stride=b.visible_size;
}
void select_buffer_view(GpuResourceBinding& b,GpuResourceBinding::View kind,bool force_raw=false,bool complete_info=true){
    if(!b.buffer || b.view==kind)return;
    if(kind==GpuResourceBinding::View::None){b.view=kind;return;}
    require(kind==GpuResourceBinding::View::ShaderResource || kind==GpuResourceBinding::View::UnorderedAccess,"GPU buffer target view needs an additional adapter");
    if(complete_info)fill_buffer_info(b);auto format=b.format;auto stride=b.stride;
    // _CreateCompatibleView: a description D3D11 refuses (a raw view without the raw flag, a typeless or
    // missing format, a typed view of a structured buffer, no element in range) makes no view, and the
    // original binds, clears or copies through nothing (D-151).
    const bool raw=force_raw || ((b.description.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) &&
        (format==DXGI_FORMAT_UNKNOWN || format==DXGI_FORMAT_R32_TYPELESS));
    if(raw){format=DXGI_FORMAT_R32_TYPELESS;stride=4;}
    const auto width=format!=DXGI_FORMAT_UNKNOWN && format!=DXGI_FORMAT(-1)?dxgi_format_size(format):b.description.StructureByteStride;
    if((raw && !(b.description.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)) || (format==DXGI_FORMAT_R32_TYPELESS && !raw) ||
       ((b.description.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && format!=DXGI_FORMAT_UNKNOWN) || !width || b.offset>b.visible_size){
        b={};return;
    }
    UINT first=0,count=0;FrontendBufferViewElements(stride,b.offset,format,b.visible_size,first,count);
    const uint64_t offset=uint64_t(first)*width,size=uint64_t(count)*width;
    if(!size || offset>=b.buffer->size() || size>b.buffer->size()-offset){b={};return;}
    b.view=kind;b.view_format=format;b.view_offset=UINT(offset);b.view_size=UINT(size);
}
GpuResourceBinding::View view_kind(Target t){return t==Target::SHADER_RESOURCE?GpuResourceBinding::View::ShaderResource:
    t==Target::UNORDERED_ACCESS_VIEW?GpuResourceBinding::View::UnorderedAccess:
    t==Target::RENDER_TARGET?GpuResourceBinding::View::RenderTarget:
    t==Target::DEPTH_STENCIL_TARGET?GpuResourceBinding::View::DepthStencil:GpuResourceBinding::View::None;}
}
void GpuResourceRuntime::fill_info(GpuResourceBinding& b,Target type){
    if(!b.image){fill_buffer_info(b,type);return;}
    if(b.image_view){
        if(b.format==DXGI_FORMAT_UNKNOWN && (type==Target::SHADER_RESOURCE || type==Target::UNORDERED_ACCESS_VIEW ||
           type==Target::RENDER_TARGET || type==Target::DEPTH_STENCIL_TARGET))b.format=b.view_format;
    }else if(b.format==DXGI_FORMAT_UNKNOWN)b.format=b.image_description.texture.Format;
    if(!b.stride)b.stride=dxgi_format_size(b.format);
    if(!b.stride)b.stride=b.visible_size;
}
void GpuResourceRuntime::select_view(GpuResourceBinding& b,GpuResourceBinding::View kind,bool raw,bool complete_info){
    if(b.image){image_view(b,kind,complete_info);return;}
    select_buffer_view(b,kind,raw,complete_info);
}
GpuResourceRuntime::GpuResourceRuntime(gpu::ResourceDevice d,gpu::ResourceBudgets budgets):commands_(d,std::move(budgets)){}
void GpuResourceRuntime::backbuffer(gpu::Image image,DXGI_FORMAT format,UINT bind_flags){
    if(!image){backbuffer_={};return;}
    const auto& vk=image->description();
    require(vk.type==VK_IMAGE_TYPE_2D && vk.mips==1 && vk.layers==1 && vk.samples==VK_SAMPLE_COUNT_1_BIT && vk.extent.depth==1,
            "Host back buffer must be one single-sample 2D image");
    require(source_image_format(format).format==vk.format,"Host back buffer format does not match its image");
    require((bind_flags&D3D11_BIND_RENDER_TARGET) && !(bind_flags&~(D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS)),
            "Host back buffer bind flags are invalid");
    GpuResourceBinding b;b.image=std::move(image);auto& s=b.image_description;s.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;s.depth=1;
    auto& d=s.texture;d.Width=vk.extent.width;d.Height=vk.extent.height;d.MipLevels=1;d.ArraySize=1;d.Format=format;
    d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind_flags;
    backbuffer_=std::move(b);
}
std::vector<std::wstring> GpuResourceRuntime::install(DrawPlanAdapter& adapter) {
    require(!installed_,"GPU resource runtime cannot belong to two INI generations");installed_=true;
    adapter.resource_commands([this](auto& command,auto& state,auto& why){return run(command,state,why);});
    adapter.resource_queries([this](auto& target,auto& state,float& value,auto& why){return query(target,state,value,why);});
    adapter.render_target_size([this](auto& state,bool& bound,float& width,float& height,auto& why){return render_target(state,bound,width,height,why);});
    adapter.resource_matches([this](auto& target,auto& state,auto& value,auto& why){return match_source(target,state,value,why);});
    adapter.parameter_flush([this](auto& state,bool force,auto& why){return flush_parameters(state,force,why);});
    adapter.owned_bindings([this](auto& target,auto& state,auto& out,auto& why){return owned_vertex(target,state,out,why);});
    return {};
}
void GpuResourceRuntime::draw_commands(DrawExecutor executor){
    require(!installed_ && bool(executor),"Install GPU draw executor before generation adaptation");draw_executor_=std::move(executor);
}
void GpuResourceRuntime::begin(VkQueue queue,ExecutionSite site) {
    require(!active_ && ((queue && site==ExecutionSite::OutsideRenderPass) || (!queue && (site==ExecutionSite::Deferred || site==ExecutionSite::Draw))),
            "GPU resource execution requires an explicit outside-render-pass scope");
    require(!queue || !queue_ || queue_==queue,"GPU resource scope cannot change queues");if(queue)queue_=queue;active_=true;site_=site;recorded_=false;
    backbuffer_targeted_=false;failed_=false;
}
std::shared_ptr<gpu::ResourceSubmission> GpuResourceRuntime::finish(const gpu::ResourceCommands::SubmitSync& sync,VkQueue queue) {
    active();require(!failed_,"Cannot submit a failed GPU resource scope");
    require(!queue || !queue_ || queue_==queue,"GPU resource scope cannot change queues");if(queue)queue_=queue;
    require(queue_!=VK_NULL_HANDLE,"A deferred GPU resource scope needs its queue to submit");
    auto result=commands_.submit(queue_,sync);active_=false;return result;
}
// A failed scope that is suspended drops what it and the scopes pending before it recorded: their work
// is one batch, and a later scope starts clean.
void GpuResourceRuntime::suspend(){active();active_=false;if(failed_){commands_.discard();failed_=false;}}
void GpuResourceRuntime::discard(){commands_.discard();active_=false;failed_=false;}
void GpuResourceRuntime::active() const {require(active_,"GPU resource operation has no outside-render-pass execution scope");}
ResourceCopyTarget& GpuResourceRuntime::effective(ResourceCopyTarget& t,CommandListState& state) {
    if(t.type!=Target::THIS_RESOURCE)return t;
    require(state.this_target && state.this_target->type!=Target::THIS_RESOURCE,"GPU this target missing or cyclic");return *state.this_target;
}
GpuResourceRuntime::Slot GpuResourceRuntime::slot(ResourceCopyTarget& t,CommandListState& state) {
    unsigned limit=0;bool stage=false;
    switch(t.type){
    case Target::VERTEX_BUFFER:limit=32;break;
    case Target::INDEX_BUFFER:limit=1;break;
    case Target::CONSTANT_BUFFER:limit=14;stage=true;break;
    case Target::SHADER_RESOURCE:limit=128;stage=true;break;
    case Target::UNORDERED_ACCESS_VIEW:limit=64;stage=true;require(t.shader_type==L'p' || t.shader_type==L'c',"GPU UAV stage invalid");break;
    case Target::RENDER_TARGET:limit=8;break;
    case Target::DEPTH_STENCIL_TARGET:limit=1;break;
    default:throw std::runtime_error("GPU target has no buffer slot adapter");
    }
    require(!stage || (t.shader_type && std::wstring(L"vhdgpc").find(t.shader_type)!=std::wstring::npos),"GPU shader stage invalid");
    const auto index=t.ResolveSlot(&state);require(index<limit,"GPU binding slot out of range");return {stage?t.shader_type:L'\0',t.type,index};
}
CustomResource* GpuResourceRuntime::declaration(ResourceCopyTarget& t,CommandListState& state,bool writing) {
    if(t.custom_resource_pool)validate_pool_runtime(*t.custom_resource_pool);
    auto* r=t.GetCustomResource(&state,writing);
    if(r && r->pool)validate_pool_runtime(*r->pool);
    return r;
}
GpuResourceBinding GpuResourceRuntime::load(CustomResource& r,ResourceCopyTarget* destination,CommandListState* state) {
    require(substantiated_.size()<65536 || substantiated_.count(&r),"GPU resource declaration budget exceeded");
    const bool first=substantiated_.insert(&r).second;
    const auto found=resources_.find(&r);
    if(found!=resources_.end()){
        if(r.is_null)return {};
        auto value=found->second;value.stride=r.stride;value.offset=r.offset;value.format=r.format;
        value.visible_size=r.buf_size;value.source_stride=r.source_stride;return value;
    }
    // CustomResource::Substantiate, once per resource and unless something was assigned to it first: the
    // bind flags of the target it is first fetched for (GetResource's dst, the destination of a reference
    // or a slot range's slots) join the declaration's, covering what parsing could not deduce (D-151).
    if(first && destination){
        D3D11_RESOURCE_MISC_FLAG misc=D3D11_RESOURCE_MISC_FLAG(0);
        const auto flags=destination->BindFlags(state,&misc);
        r.bind_flags=D3D11_BIND_FLAG(r.bind_flags|flags);r.misc_flags=D3D11_RESOURCE_MISC_FLAG(r.misc_flags|misc);
    }
    require(resources_.size()<65536,"GPU resource declaration budget exceeded");
    if(r.filename.empty() && r.override_type==CustomResourceType::INVALID)
        return {};
    if(r.override_type==CustomResourceType::TEXTURE1D || r.override_type==CustomResourceType::TEXTURE2D ||
       r.override_type==CustomResourceType::TEXTURE3D || r.override_type==CustomResourceType::CUBE ||
       (!r.filename.empty() && r.override_type==CustomResourceType::INVALID))return load_image(r);
    require(r.override_type==CustomResourceType::BUFFER || r.override_type==CustomResourceType::STRUCTURED_BUFFER ||
            r.override_type==CustomResourceType::RAW_BUFFER || r.override_type==CustomResourceType::INVALID,"GPU image declaration requires image commands");
    GpuResourceBinding b;std::vector<uint8_t> bytes;
    if(!r.filename.empty()) {
        require(r.override_type!=CustomResourceType::INVALID,"GPU untyped file requires image/resource type adaptation");
        // LoadBufferFromFile: a file it cannot open or read (or an empty one, which no buffer is made of) is null.
        std::ifstream in(std::filesystem::path(r.filename),std::ios::binary|std::ios::ate);
        if(!in || in.tellg()<=0)return unloaded(r,L"buffer","the file cannot be read");
        require(uint64_t(in.tellg())<=max_bytes,"GPU resource file over budget");
        bytes.resize(size_t(in.tellg()));in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        if(!in)return unloaded(r,L"buffer","the file cannot be read");
        b.visible_size=uint32_t(bytes.size()); // Original LoadBufferFromFile keeps file size separately.
    }else if(r.initial_data_size){require(r.initial_data && r.initial_data_size<=max_bytes,"GPU initial data exceeds budget");
        const auto* p=static_cast<const uint8_t*>(r.initial_data);bytes.assign(p,p+r.initial_data_size);}
    b.description.ByteWidth=UINT(bytes.size());b.description.BindFlags=r.bind_flags;b.description.MiscFlags=r.misc_flags;
    if(r.override_type==CustomResourceType::STRUCTURED_BUFFER)b.description.StructureByteStride=UINT(bytes.size());
    // Original description rules, including format-derived stride and array.
    r.OverrideBufferDesc(&b.description);
    // SubstantiateBuffer: a description D3D11 does not create leaves the resource null (D-150).
    try{validate_desc(b.description);}catch(const std::exception& e){return unloaded(r,L"buffer",e.what());}
    r.OverrideOutOfBandInfo(&b.format,&b.stride);b.source_stride=r.source_stride;
    b.buffer=commands_.create(b.description.ByteWidth,usage(b.description),bytes.empty()?nullptr:bytes.data(),std::min<size_t>(bytes.size(),b.description.ByteWidth),true);
    r.is_null=false;r.stride=b.stride;r.format=b.format;r.buf_size=b.visible_size;resources_[&r]=b;return b;
}
GpuResourceBinding GpuResourceRuntime::unloaded(CustomResource& r,const wchar_t* kind,const std::string& why){
    // CustomResource::LoadFromFile and LoadBufferFromFile: the overlay's warning, and the resource stays null,
    // so every assignment of it binds nothing and the rest goes on.
    LogOverlayW(LOG_WARNING,L"Failed to load custom %ls resource %ls: %hs\n",kind,r.filename.c_str(),why.c_str());
    r.is_null=true;resources_[&r]={};return {};
}
GpuResourceBinding GpuResourceRuntime::resolve(ResourceCopyTarget& input,CommandListState& state){active();return get(input,state);}
// The owned buffer a replaced draw binds as a vertex stream (D-087): what the resource holds now,
// with the stride the original would bind it with.
bool GpuResourceRuntime::owned_vertex(ResourceCopyTarget& target,CommandListState& state,PlanItem::OwnedBuffer& out,std::wstring& why){
    try{
        active();
        auto b=get(target,state);
        require(b.buffer && !b.image,"Only an owned buffer binds as a replaced draw's vertex stream");
        require(b.buffer->usage()&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,"The owned buffer was not made for vertex input");
        require(b.stride && b.offset<b.buffer->size() && b.buffer->size()-b.offset>=b.stride,"The owned vertex buffer has no whole vertex");
        out.buffer=reinterpret_cast<uint64_t>(b.buffer->buffer());out.offset=b.offset;out.size=b.buffer->size()-b.offset;
        out.stride=b.stride;out.keep=b.buffer;return true;
    }catch(const std::exception& e){why=widen(e.what());return false;}
}
namespace {
std::shared_ptr<void> resource_owner(const GpuResourceBinding& binding){
    return binding.image?std::static_pointer_cast<void>(binding.image):std::static_pointer_cast<void>(binding.buffer);
}
}
void GpuResourceRuntime::identify_resource(const GpuResourceBinding& binding,SourceResourceIdentity identity){
    require(bool(binding),"Resource identity requires a live owned allocation");
    require(identity.hash_known || !identity.hash,"Unknown original resource hash has an asserted value");
    require(identity.tracking==SourceResourceIdentity::Tracking::Tracked || (!identity.hash_known && !identity.region_hash),
            "Untracked resource has asserted source hash facts");
    const std::weak_ptr<void> key=resource_owner(binding);
    if(identity.tracking==SourceResourceIdentity::Tracking::Untracked){source_identities_.erase(key);return;}
    if(!source_identities_.count(key) && source_identities_.size()>=65536){
        for(auto it=source_identities_.begin();it!=source_identities_.end();)if(it->first.expired())it=source_identities_.erase(it);else ++it;
        require(source_identities_.size()<65536,"Original resource identity budget exceeded");
    }
    source_identities_[key]=std::move(identity);
}
bool GpuResourceRuntime::match_source(ResourceCopyTarget& target,CommandListState& state,SourceResourceMatch& result,std::wstring& reason){
    const uint64_t mark=commands_.recorded();
    try{active();
        result={};ResourceCopyTarget single;ResourceCopyTarget* resolved=&target;
        if(target.slot_expression){const auto slot=target.ResolveSlot(&state);if(slot==UINT_MAX)return true;
            single.type=target.type;single.shader_type=target.shader_type;single.slot=slot;single.max_slot=target.max_slot;resolved=&single;}
        auto binding=get(*resolved,state);result.bound=bool(binding);if(!binding)return true;
        const auto found=source_identities_.find(std::weak_ptr<void>(resource_owner(binding)));
        if(found==source_identities_.end())result.identity.tracking=SourceResourceIdentity::Tracking::Untracked;
        else result.identity=found->second;
        result.stride=binding.stride;result.offset=binding.offset;result.format=binding.format;
        if(!binding.image)result.description=binding.description;
        else{const auto& s=binding.image_description;const auto& d=s.texture;
            if(s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D)
                result.description=D3D11_TEXTURE1D_DESC{d.Width,d.MipLevels,d.ArraySize,d.Format,d.Usage,d.BindFlags,d.CPUAccessFlags,d.MiscFlags};
            else if(s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D)result.description=d;
            else if(s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE3D)
                result.description=D3D11_TEXTURE3D_DESC{d.Width,d.Height,s.depth,d.MipLevels,d.Format,d.Usage,d.BindFlags,d.CPUAccessFlags,d.MiscFlags};
            else throw std::runtime_error("Original image dimension is unknown");
        }
        return true;
    }catch(const std::exception& e){if(active_ && commands_.recorded()!=mark)failed_=true;reason=widen(e.what());return false;}
}
GpuResourceBinding GpuResourceRuntime::get(ResourceCopyTarget& input,CommandListState& state,ResourceCopyTarget* destination) {
    auto& t=effective(input,state);GpuResourceBinding b;
    require(!t.IsRange(),"GPU range command requires the slot-range adapter");
    if(t.type==Target::EMPTY)return {};
    if(t.type==Target::INI_PARAMS){
        require(parameters_initialized_,"GPU parameters used before initialization");b=parameters_;
        // The original special target returns its image/view without supplying
        // any out-of-band format or buffer metadata. Reading never flushes it.
        b.format=DXGI_FORMAT_UNKNOWN;b.stride=b.source_stride=b.offset=b.visible_size=0;
    }else if(t.type==Target::CUSTOM_RESOURCE){if(auto* r=declaration(t,state))b=load(*r,destination,&state);}
    else if(swap_chain(t.type)){
        // GetBuffer(0) returns the resource alone: no view and no out-of-band format or size.
        b=backbuffer_;b.drop_view();b.format=DXGI_FORMAT_UNKNOWN;b.stride=b.source_stride=b.offset=b.visible_size=0;
    }
    else {const auto key=slot(t,state);const auto found=slots_.find(key);if(found!=slots_.end())b=found->second;
        // XXGetShaderResources/UnorderedAccessViews return resource and view,
        // not the custom declaration's out-of-band stride/offset/size/format.
        b.source_stride=0;
        if(t.type==Target::SHADER_RESOURCE || t.type==Target::UNORDERED_ACCESS_VIEW || t.type==Target::RENDER_TARGET ||
           t.type==Target::DEPTH_STENCIL_TARGET){b.stride=0;b.offset=0;b.visible_size=0;b.format=DXGI_FORMAT_UNKNOWN;b.region=false;}
        else if(t.type==Target::CONSTANT_BUFFER){b.stride=0;b.format=DXGI_FORMAT_UNKNOWN;}
        else if(t.type==Target::INDEX_BUFFER){b.stride=dxgi_format_size(b.format);b.visible_size=0;}
        else if(t.type==Target::VERTEX_BUFFER){b.format=DXGI_FORMAT_UNKNOWN;b.visible_size=0;}}
    if(b && input.evaluation_mode==Mode::RESOURCE_REGION) {
        require(input.member_args[0].expression && input.member_args[1].expression,"GPU region requires offset and size");
        b.offset=uint_value(input.member_args[0].GetValue(&state));b.visible_size=uint_value(input.member_args[1].GetValue(&state));
        b.region=true;
        // A reference of a region outside the buffer into a slot: CreateCompatibleView gets no element in
        // range and makes no view, so the slot takes the null view (D-151).
        if(b.buffer && !(b.offset<b.buffer->size() && b.visible_size<=b.buffer->size())){
            if(destination && destination->type!=Target::CUSTOM_RESOURCE && destination->type!=Target::POOL)return {};
            // Anything else reads no byte past the buffer: past it, the region ends where it starts (D-153).
            b.visible_size=b.offset<b.buffer->size()?UINT(b.buffer->size()):b.offset;
        }
    }
    return b;
}
void GpuResourceRuntime::set(ResourceCopyTarget& input,CommandListState& state,const GpuResourceBinding& b) {
    auto& t=effective(input,state);
    require(t.evaluation_mode==Mode::RESOURCE && !t.IsRange(),"GPU assignment needs a single resource target");
    if(b.image && backbuffer_ && b.image==backbuffer_.image && (t.type==Target::RENDER_TARGET || t.type==Target::UNORDERED_ACCESS_VIEW))backbuffer_targeted_=true;
    if(t.type==Target::CUSTOM_RESOURCE){auto* r=declaration(t,state,true);require(r!=nullptr,"GPU resource destination missing");
        require(resources_.size()<65536 || resources_.count(r),"GPU resource declaration budget exceeded");
        auto& cached=resources_[r];
        // Original SetResource(null) retains the old resource/view as a cache.
        // source_stride is changed by copy instructions, not SetResource itself.
        if(b)cached=b;
        cached.stride=b.stride;cached.offset=b.offset;cached.visible_size=b.visible_size;cached.format=b.format;cached.source_stride=r->source_stride;
        r->is_null=!b;r->stride=b.stride;r->offset=b.offset;r->buf_size=b.visible_size;r->format=b.format;
    }else {
        auto key=slot(t,state);
        // CreateCompatibleView makes no view of a resource without the slot's bind flag, nor of a description
        // D3D11 refuses: the original binds the null view and the list goes on (D-151).
        auto bound=b;
        if(bound && !(bound.bind_flags()&bind_flag(t.type)))bound={};
        select_view(bound,view_kind(t.type));
        if(t.type==Target::CONSTANT_BUFFER && bound.region)
            require(bound.offset%16==0 && bound.visible_size%16==0 && bound.visible_size<=bound.description.ByteWidth-bound.offset,
                    "GPU constant-buffer reference region is not aligned or outside allocation");
        assign_binding(key,std::move(bound));
    }
}
void GpuResourceRuntime::copy(ResourceCopyOperation& op,CommandListState& state) {
    require(typeid(op)==typeid(ResourceCopyOperation),"GPU staging command requires staging adaptation");
    require(op.src.evaluation_mode==Mode::RESOURCE || op.src.evaluation_mode==Mode::RESOURCE_REGION,"GPU copy source is not a resource");
    constexpr auto allowed=uint32_t(ResourceCopyOptions::COPY)|uint32_t(ResourceCopyOptions::COPY_DESC)|uint32_t(ResourceCopyOptions::REFERENCE)|
        uint32_t(ResourceCopyOptions::UNLESS_NULL)|uint32_t(ResourceCopyOptions::NO_VIEW_CACHE)|uint32_t(ResourceCopyOptions::RAW_VIEW)|
        uint32_t(ResourceCopyOptions::MONO)|uint32_t(ResourceCopyOptions::SET_VIEWPORT)|uint32_t(ResourceCopyOptions::RESOLVE_MSAA);
    require(!(uint32_t(op.options)&~allowed),"GPU copy option requires an additional adapter");
    auto& dst=effective(op.dst,state);
    if(op.src.type==Target::EMPTY){set(dst,state,{});return;} // Explicit null ignores unless_null upstream.
    // A reference substantiates its source with the destination's bind flags; a copy does not (D-151).
    auto source=get(op.src,state,(op.options&ResourceCopyOptions::REFERENCE)?&op.dst:nullptr);
    if(dst.type!=Target::POOL){copy_to(op,dst,state,std::move(source));return;}
    require(dst.custom_resource_pool && (dst.evaluation_mode==Mode::RESOURCE || dst.evaluation_mode==Mode::POOL_FULL_RANGE_RESOURCE),
            "GPU whole-pool assignment needs a resource pool target");
    auto* pool=dst.custom_resource_pool;validate_pool_runtime(*pool);
    const auto count=pool->GetPoolSize();require(count<=65536,"GPU whole-pool assignment budget exceeded");
    // Retain the resolved source resource/view once, before changing any pool
    // element. A local target avoids mutating the original command on failure.
    ResourceCopyTarget target;target.type=Target::CUSTOM_RESOURCE;
    for(size_t i=0;i<count;++i){
        auto* destination=pool->GetResource(float(i),false,true,true);
        require(destination!=nullptr,"GPU whole-pool destination missing");target.SetCustomResource(destination);
        copy_to(op,target,state,source);
    }
}
void GpuResourceRuntime::copy_to(ResourceCopyOperation& op,ResourceCopyTarget& dst,CommandListState& state,GpuResourceBinding source) {
    if(!source){if(!(op.options&ResourceCopyOptions::UNLESS_NULL))set(dst,state,{});return;}
    if(swap_chain(dst.type))backbuffer_targeted_=true;
    auto* custom=dst.type==Target::CUSTOM_RESOURCE?declaration(dst,state,true):nullptr;
    const bool explicit_region=op.src.evaluation_mode==Mode::RESOURCE_REGION;
    const auto region_size=source.visible_size;
    if(custom && custom->max_copies_per_frame) {
        if(custom->frame_no!=G->frame_no){custom->frame_no=G->frame_no;custom->copies_this_frame=1;}
        else if(custom->copies_this_frame++>=custom->max_copies_per_frame)return;
    }
    if(custom)custom->OverrideOutOfBandInfo(&source.format,&source.stride);
    source.source_stride=op.src.type==Target::CUSTOM_RESOURCE && source.source_stride?source.source_stride:source.stride;
    if(custom){
        // Original CopyResourceToResource reads source_stride from the current
        // declaration per destination; the retained GPU source itself is fixed.
        if(op.src.type==Target::CUSTOM_RESOURCE){auto* origin=declaration(op.src,state);
            source.source_stride=origin && origin->source_stride?origin->source_stride:source.stride;}
        custom->source_stride=source.source_stride;
    }
    fill_info(source,op.src.type);
    require(!custom || resources_.size()<65536 || resources_.count(custom),"GPU resource declaration budget exceeded");
    auto& cache=custom?resources_[custom]:copies_[&op];
    const bool no_view_cache=bool(op.options&ResourceCopyOptions::NO_VIEW_CACHE) || op.src.forbid_view_cache;
    const bool raw_view=bool(op.options&ResourceCopyOptions::RAW_VIEW);
    auto reuse_view=[&](GpuResourceBinding& value){
        if(cache.buffer==value.buffer && cache.image==value.image){value.view=cache.view;value.view_format=cache.view_format;
            value.view_offset=cache.view_offset;value.view_size=cache.view_size;value.image_view=cache.image_view;}
    };
    if(!(op.options&ResourceCopyOptions::COPY_MASK)){
        // UsableRefView retains a source view on a custom resource. If the
        // source has none, the original may reuse its old compatible view.
        if(custom && source.view==GpuResourceBinding::View::None)reuse_view(source);
        if(!custom && source.view!=view_kind(dst.type)){
            source.drop_view();reuse_view(source);
            select_view(source,view_kind(dst.type),raw_view,false);cache=source;
        }
        source.visible_size=explicit_region?region_size:0;source.region=explicit_region;
        // The original clears a constant-slot offset unless ->Region was used.
        if(dst.type==Target::CONSTANT_BUFFER && op.src.evaluation_mode!=Mode::RESOURCE_REGION){source.offset=0;source.visible_size=0;source.region=false;}
        set(dst,state,source);
        if(source.image && (op.options&ResourceCopyOptions::SET_VIEWPORT))viewport_={0,0,float(source.image_description.texture.Width),
            source.image_description.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D?0.0f:float(source.image_description.texture.Height),0,1};
        if(no_view_cache)cache.drop_view();return;
    }
    if(source.image){image_copy(op,dst,state,std::move(source),custom,cache);return;}
    GpuResourceBinding out=source;out.description.BindFlags=custom?custom->bind_flags:bind_flag(dst.type);
    if(custom)out.description.MiscFlags|=custom->misc_flags;
    if(!(out.description.BindFlags&(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS)))out.description.MiscFlags&=~D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    UINT source_end=source.visible_size,destination_size=0;
    const bool source_view=source.view!=GpuResourceBinding::View::None;
    // RecreateCompatibleBuffer: visible CB windows, CB size constraints,
    // view-less structure conversion, then the original declaration overrides.
    if((out.description.BindFlags&D3D11_BIND_CONSTANT_BUFFER) || op.src.type==Target::CONSTANT_BUFFER){
        if(op.src.type==Target::CONSTANT_BUFFER && source_end){destination_size=source_end;
            source_end=out.description.ByteWidth;out.description.ByteWidth=destination_size;}
        if(out.description.BindFlags&D3D11_BIND_CONSTANT_BUFFER){
            const auto rounded=std::min((out.description.ByteWidth+15)&~15u,65536u);
            out.description.MiscFlags&=~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;out.description.StructureByteStride=0;
            if(source.offset || rounded!=out.description.ByteWidth){destination_size=rounded;out.description.ByteWidth=rounded;}
        }
    }else if(!source_view && source.format==DXGI_FORMAT_UNKNOWN && source.stride &&
             (out.description.BindFlags&(D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS))){
        out.description.MiscFlags|=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;out.description.StructureByteStride=source.stride;
        const auto rounded=out.description.ByteWidth/source.stride*source.stride;
        if(source.offset || rounded!=out.description.ByteWidth){destination_size=rounded;out.description.ByteWidth=rounded;}
    }else if(!source_view && source.offset){
        destination_size=out.description.ByteWidth;
    }
    if(custom){custom->OverrideBufferDesc(&out.description);custom->OverrideOutOfBandInfo(&out.format,&out.stride);}
    out.view=GpuResourceBinding::View::None;
    validate_desc(out.description);
    auto& pool=custom?resource_copy_pools_[custom]:command_copy_pools_[&op];
    const std::array<UINT,4> key={out.description.ByteWidth,out.description.BindFlags,out.description.MiscFlags,out.description.StructureByteStride};
    auto found=pool.find(key);
    if(found!=pool.end())out.buffer=found->second;
    else{out.buffer=commands_.create(out.description.ByteWidth,usage(out.description),nullptr,0,true);pool.emplace(key,out.buffer);}
    reuse_view(out);
    // The original resolve helper returns for buffers. It still creates/reuses
    // the destination, but must not replace existing bytes with a normal copy.
    // MONO has no implementation upstream; buffer SET_VIEWPORT returns as well.
    if(!(op.options&ResourceCopyOptions::COPY_DESC) && !(op.options&ResourceCopyOptions::RESOLVE_MSAA)){
        if(destination_size){
            // SpecialCopyBufferRegion treats source size as an end boundary.
            const uint64_t end=std::min<uint64_t>(source_end,uint64_t(source.offset)+destination_size);
            // A box that ends before it starts (an end boundary below the offset, a region past the buffer) is
            // undefined in D3D11: the original's device stops answering on the test machine. The layer copies
            // nothing; the destination is made as for any copy and reads zero (D-153).
            auto bytes=end>source.offset?end-source.offset:0;if(source.stride)bytes=bytes/source.stride*source.stride;
            require(bytes<=out.description.ByteWidth,"GPU copy override truncates the original region");
            if(bytes)commands_.copy(source.buffer,source.offset,out.buffer,0,bytes);
            out.offset=0;
        }else{
            require(out.description.ByteWidth==source.description.ByteWidth,"GPU whole copy has incompatible allocation sizes");
            commands_.copy(source.buffer,0,out.buffer,0,source.description.ByteWidth);
        }
    }
    out.visible_size=source_end;
    if(!custom)select_view(out,view_kind(dst.type),raw_view,false);
    out.visible_size=destination_size;out.region=dst.type==Target::CONSTANT_BUFFER && (out.offset || out.visible_size);
    cache=out;set(dst,state,out);if(no_view_cache)cache.drop_view();
}
void GpuResourceRuntime::clear(ClearViewCommand& op,CommandListState& state) {
    auto b=get(op.target,state);if(!b)return;
    if(b.image){image_clear(std::move(b),op);return;}
    // ClearViewCommand queries the existing view first. An SRV has no clear
    // operation, even when its underlying buffer also has the UAV bind flag.
    if(b.view==GpuResourceBinding::View::ShaderResource)return;
    // Without the UAV bind flag the original makes no view to clear through and clears nothing (D-150).
    if(!(b.description.BindFlags&D3D11_BIND_UNORDERED_ACCESS))return;
    select_view(b,GpuResourceBinding::View::UnorderedAccess);
    if(!b)return; // no view to clear through (D-151)
    b.format=b.view_format;b.offset=b.view_offset;b.visible_size=b.view_size;
    // `clear = Resource depth` or `stencil`: the original asks for a depth-stencil view (create_best_view),
    // which D3D11 makes of no buffer, and clears nothing.
    if(op.clear_depth || op.clear_stencil)return;
    require(b.description.BindFlags&D3D11_BIND_UNORDERED_ACCESS,"GPU buffer clear needs a UAV-compatible resource");
    // Raw/structured UAV clears use only the first uint, including when the
    // INI token looked floating point. Typed 32-bit views use all components.
    unsigned channels=1;bool floating=false;
    if(!(b.description.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && b.format!=DXGI_FORMAT_R32_TYPELESS){
        switch(b.format){
        case DXGI_FORMAT_R32_FLOAT:channels=1;floating=true;break;
        case DXGI_FORMAT_R32G32_FLOAT:channels=2;floating=true;break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:channels=4;floating=true;break;
        case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_R32_SINT:channels=1;break;
        case DXGI_FORMAT_R32G32_UINT:case DXGI_FORMAT_R32G32_SINT:channels=2;break;
        case DXGI_FORMAT_R32G32B32A32_UINT:case DXGI_FORMAT_R32G32B32A32_SINT:channels=4;break;
        default:throw std::runtime_error("GPU typed clear format needs a conversion adapter");
        }
    }
    const auto count=b.visible_size?b.visible_size:b.description.ByteWidth-b.offset;
    require(b.offset%(channels*4)==0 && count%(channels*4)==0,"GPU clear view range alignment invalid");
    uint32_t words[4]{};
    for(unsigned i=0;i<channels;++i){if(floating && !op.clear_uav_uint)memcpy(&words[i],&op.fval[i],4);else words[i]=op.uval[i];}
    bool repeated=true;for(unsigned i=1;i<channels;++i)repeated&=words[i]==words[0];
    if(repeated)commands_.fill(b.buffer,b.offset,count,words[0]);
    else {std::vector<uint32_t> data(count/4);for(size_t i=0;i<data.size();++i)data[i]=words[i%channels];commands_.write(b.buffer,b.offset,data.data(),count);}
}
void GpuResourceRuntime::ranges(SlotRangeCopyOperation& op,CommandListState& state) {
    const bool bind=op.dst.evaluation_mode==Mode::SLOT_RANGE;
    auto& slots=bind?op.dst:op.src;auto& other=bind?op.src:op.dst;
    require(slots.evaluation_mode==Mode::SLOT_RANGE && slots.range_start && slots.range_end,"GPU slot range bounds missing");
    int first=0,pool_first=0;unsigned count=0,pool_count=0;
    require(slots.ResolveRange(&state,&first,&count) && count && count<=128,"GPU slot range is invalid");
    auto* pool=other.type==Target::POOL?other.custom_resource_pool:nullptr;
    if(pool){
        validate_pool_runtime(*pool);
        require(other.range_start && other.range_end && other.ResolveRange(&state,&pool_first,&pool_count) && pool_count==count,
                "GPU slot and pool ranges differ");
    }else require(bind && (other.type==Target::CUSTOM_RESOURCE || other.type==Target::EMPTY),"GPU range requires a pool or broadcast resource");
    const int plain_options=int(ResourceCopyOptions::REFERENCE)|int(ResourceCopyOptions::UNLESS_NULL)|int(ResourceCopyOptions::NO_VIEW_CACHE);
    const bool per_slot=(int(op.options)&~plain_options) && op.src.type!=Target::EMPTY;
    require(range_operations_.size()<4096 || range_operations_.count(&op),"GPU range command budget exceeded");
    auto& operations=range_operations_[&op];auto& views=range_views_[&op];
    if(operations.size()<count)operations.resize(count);if(views.size()<count)views.resize(count);
    auto operation=[&](unsigned i)->ResourceCopyOperation&{
        if(!operations[i])operations[i]=std::make_unique<ResourceCopyOperation>();
        auto& one=*operations[i];one.ini_line=op.ini_line;one.options=op.options;
        auto& target=bind?one.dst:one.src;target.type=slots.type;target.shader_type=slots.shader_type;
        target.slot=unsigned(first)+i;target.max_slot=slots.max_slot;
        auto& resource=bind?one.src:one.dst;resource.type=Target::CUSTOM_RESOURCE;return one;
    };
    // Fetch the entire source range before writes to a pool. This also retains
    // owners independently of aliases and element switches during assignment.
    std::vector<GpuResourceBinding> fetched;
    if(!bind)for(unsigned i=0;i<count;++i){auto& one=operation(i);fetched.push_back(get(one.src,state));}
    Bindings pending;
    if(bind)for(unsigned i=0;i<count;++i){auto& one=operation(i);auto key=slot(one.dst,state);
        pending.emplace(key,binding(std::get<0>(key),std::get<1>(key),std::get<2>(key)));}
    require(!deferred_bindings_,"Nested GPU binding publication is invalid");
    struct DeferredScope {Bindings*& target;~DeferredScope(){target=nullptr;}} deferred{deferred_bindings_};
    if(bind)deferred_bindings_=&pending;
    CustomResource* broadcast=bind && other.type==Target::CUSTOM_RESOURCE?declaration(other,state):nullptr;
    for(unsigned i=0;i<count;++i){auto& one=operation(i);
        if(bind){
            auto* source=pool?pool->GetResource(float(int64_t(pool_first)+i),false,true,false):broadcast;
            one.src.SetCustomResource(source);
            if(per_slot){copy(one,state);continue;}
            // BindSlotRef fetches the element with the slots as GetResource's dst (D-151).
            auto value=source?load(*source,&slots,&state):GpuResourceBinding{};
            // Unlike scalar `null`, range null with unless_null preserves slots.
            if(!value && (op.options&ResourceCopyOptions::UNLESS_NULL))continue;
            // ViewForSlot: no view of a resource without the slots' bind flag; the slot takes the null view.
            if(value && !(value.bind_flags()&bind_flag(slots.type)))value={};
            if(value){
                const auto kind=view_kind(slots.type);
                if(kind==GpuResourceBinding::View::None)value.drop_view();
                else if(value.view!=kind){
                    if(views[i].buffer==value.buffer && views[i].image==value.image && views[i].view==kind){value.view=kind;value.view_format=views[i].view_format;
                        value.view_offset=views[i].view_offset;value.view_size=views[i].view_size;value.image_view=views[i].image_view;}
                    else if(value.image){value.format=DXGI_FORMAT_UNKNOWN;select_view(value,kind,false,false);views[i]=value;}
                    else if(!(value.description.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)){
                        // ViewForSlot calls its view helper with zero stride, offset and size and an UNKNOWN
                        // format: a typed buffer has no format to view it with, a raw one gets no element,
                        // and D3D11 makes no view: the slot takes the null view (D-151).
                        value={};
                    }else {
                        // That call makes one structured element of a structured buffer, not a full view.
                        UINT start=0,n=0;FrontendBufferViewElements(0,0,DXGI_FORMAT_UNKNOWN,0,start,n);
                        value.view=kind;value.view_format=DXGI_FORMAT_UNKNOWN;
                        value.view_offset=start*value.description.StructureByteStride;value.view_size=n*value.description.StructureByteStride;
                        views[i]=value;
                    }
                }
                // Plain CB ranges bind the complete buffer; no resource-region
                // metadata is forwarded by the original fast path.
                value.stride=0;value.offset=0;value.visible_size=0;value.format=DXGI_FORMAT_UNKNOWN;value.region=false;
            }
            pending[slot(one.dst,state)]=std::move(value);
        }else{
            const auto& source=fetched[i];
            if(!source && (op.options&ResourceCopyOptions::UNLESS_NULL))continue;
            auto* destination=pool->GetResource(float(int64_t(pool_first)+i),false,true,true);
            require(destination!=nullptr,"GPU range pool destination missing");one.dst.SetCustomResource(destination);
            if(per_slot){copy(one,state);continue;}
            // Plain range fetch is SetResource directly: no per-frame copy
            // counter and no destination stride/format override is applied.
            auto value=source;value.stride=0;value.format=DXGI_FORMAT_UNKNOWN;value.source_stride=0;
            set(one.dst,state,value);
        }
    }
    deferred_bindings_=nullptr;
    if(bind)publish_bindings(std::move(pending));
    if(op.options&ResourceCopyOptions::NO_VIEW_CACHE)for(auto& cached:views)cached={};
}
void GpuResourceRuntime::store(StoreCommand& op,CommandListState& state) {
    auto source=get(op.src,state);if(!source)return; // Original null keeps the previous variable value.
    require(!source.image,"Original store requires a buffer source");
    require(op.offset_expression!=nullptr,"GPU store offset missing");
    const uint64_t offset=uint64_t(uint_value(op.offset_expression->evaluate(&state)))*sizeof(float);
    UINT64 from=0,size=0;UINT value_offset=0;float value=0;
    // The original treats the offset as a 4-byte element index even for
    // structured buffers, and returns zero for an out-of-range read.
    if(op.CalculateCopyRegion(source.description,sizeof(float),offset,&from,&size,&value_offset) &&
       from<=source.description.ByteWidth && size<=source.description.ByteWidth-from){
        if(site_==ExecutionSite::Draw){
            // A replaced draw's scope cannot wait for the GPU (the game's thread records it): the value this
            // store read at an earlier execution, once that submission completed, and a read for a later
            // one. Before the first, the variable keeps its value, as the original's for a null source (D-147).
            auto& reads=feedback_[&op];std::vector<uint8_t> bytes;
            for(size_t i=reads.size();i-->0;){
                if(!commands_.feedback_ready(reads[i].first,bytes))continue;
                memcpy(&value,bytes.data()+reads[i].second,sizeof(value));op.SetOutputValue(&state,value);
                reads.erase(reads.begin(),reads.begin()+ptrdiff_t(i)+1);break;
            }
            if(reads.size()<4)reads.push_back({commands_.feedback(source.buffer,from,size),value_offset});
            return;
        }
        require(queue_!=VK_NULL_HANDLE,"Original store needs immediate GPU feedback, which a scope without its queue cannot give");
        auto bytes=commands_.readback(queue_,source.buffer,from,size);memcpy(&value,bytes.data()+value_offset,sizeof(value));
    }
    op.SetOutputValue(&state,value);
}
// A command that cannot act does nothing, as the original's (D-137): one that fails before it recorded
// any GPU work leaves its scope as it was, and the list goes on. One that fails after recording part
// of its work leaves the scope's batch in a state no INI command describes: that scope's work is
// dropped (finish refuses it, the host discards), what follows in the scope does nothing, the next scope
// starts clean, and the command is not run again in this generation. Before, either failure ended the
// generation's owned work for good, which in a folder generation is every Mod's.
bool GpuResourceRuntime::run(CommandListCommand& command,CommandListState& state,std::wstring& reason) {
    const uint64_t mark=commands_.recorded();
    auto dropped=[&](const std::wstring& why){
        if(!active_ || commands_.recorded()==mark)return;
        failed_=true;
        if(disabled_.size()<4096 && disabled_.insert(&command).second)
            LayerGap(L"A command failed after recording GPU work: this presentation's owned work is dropped and the command is not run again before a reload: %ls (%ls)",
                     command.ini_line.c_str(),why.c_str());
    };
    try{active();
        if(failed_ || disabled_.count(&command))return true;
        recorded_=true;
        if(auto* c=dynamic_cast<ResourceCopyOperation*>(&command))copy(*c,state);
        else if(auto* c=dynamic_cast<SlotRangeCopyOperation*>(&command))ranges(*c,state);
        else if(auto* c=dynamic_cast<ClearViewCommand*>(&command))clear(*c,state);
        else if(auto* c=dynamic_cast<StoreCommand*>(&command))store(*c,state);
        else if(auto* c=dynamic_cast<DrawCommand*>(&command);c && draw_executor_){
            if(!draw_executor_(*c,state,reason)){dropped(reason);return false;}
        }
        else throw std::runtime_error("GPU draw/dispatch needs a shader execution adapter");
        return true;
    }catch(const std::exception& e){reason=widen(e.what())+L": "+command.ini_line;dropped(widen(e.what()));return false;}
}
bool GpuResourceRuntime::query(ResourceCopyTarget& input,CommandListState& state,float& result,std::wstring& reason) {
    const uint64_t mark=commands_.recorded();
    try{const auto mode=input.evaluation_mode;
        require(mode==Mode::RESOURCE_STRIDE || mode==Mode::RESOURCE_SIZE || mode==Mode::RESOURCE_SOURCE_STRIDE ||
                mode==Mode::RESOURCE_FORMAT || mode==Mode::RESOURCE_WIDTH || mode==Mode::RESOURCE_HEIGHT ||
                mode==Mode::RESOURCE_ARRAY || mode==Mode::RESOURCE_MIPS || mode==Mode::RESOURCE_BIND_FLAGS ||
                mode==Mode::RESOURCE_OFFSET || mode==Mode::RESOURCE_IDENTITY || mode==Mode::RESOURCE_REGION_HASH ||
                mode==Mode::RESOURCE_SPATIAL_HASH,"GPU resource property requires source-semantic adaptation");
        auto* custom=input.type==Target::CUSTOM_RESOURCE?declaration(input,state):nullptr;
        // These original getters return declaration metadata before attempting
        // resource creation. They also work in Constants without a GPU scope.
        if(mode==Mode::RESOURCE_SOURCE_STRIDE){result=custom?float(custom->source_stride):-1.0f;return true;}
        if(mode==Mode::RESOURCE_STRIDE && input.type==Target::CUSTOM_RESOURCE){result=input.GetResourceStride(&state);return true;}
        if(mode==Mode::RESOURCE_STRIDE && input.type==Target::CONSTANT_BUFFER){result=16;return true;}
        if(custom){
            if(mode==Mode::RESOURCE_BIND_FLAGS){result=float(custom->bind_flags);return true;}
            if(mode==Mode::RESOURCE_SIZE && custom->buf_size){result=float(custom->buf_size);return true;}
            if(mode==Mode::RESOURCE_FORMAT){
                if(custom->override_format!=DXGI_FORMAT(-1) && custom->override_format!=DXGI_FORMAT_UNKNOWN){result=float(custom->override_format);return true;}
                if(custom->format!=DXGI_FORMAT_UNKNOWN){result=float(custom->format);return true;}
            }
            const int declared=mode==Mode::RESOURCE_WIDTH?custom->override_width:mode==Mode::RESOURCE_HEIGHT?custom->override_height:
                mode==Mode::RESOURCE_ARRAY?custom->override_array:mode==Mode::RESOURCE_MIPS?custom->override_mips:-1;
            if(declared!=-1){result=float(declared);return true;}
        }
        active();auto b=get(input,state);
        // Sentinels from the pinned CommandList.cpp ResourcePropertyResult.
        if(!b){result=mode==Mode::RESOURCE_IDENTITY || mode==Mode::RESOURCE_BIND_FLAGS?0.0f:-2.0f;return true;}
        if(mode==Mode::RESOURCE_REGION_HASH || mode==Mode::RESOURCE_SPATIAL_HASH){
            if(b.image){result=-3.0f;return true;}
            // GetResourceRegionHash/GetResourceSpatialHash: the region offset of a vertex or index
            // buffer reads the draw call, then the arguments are evaluated.
            require(state.call_info || (input.type!=Target::VERTEX_BUFFER && input.type!=Target::INDEX_BUFFER),
                    "GPU buffer hash query requires a draw context");
            const size_t arguments=mode==Mode::RESOURCE_REGION_HASH?2:4;
            for(size_t i=0;i<arguments;++i){
                require(input.member_args[i].expression!=nullptr,"GPU buffer hash query arguments incomplete");
                input.member_args[i].GetValue(&state);
            }
            // The original hashes its CPU copy of application buffers (cache_resource_data) and of
            // resources copied from them. An owned buffer holds Mod data, which the original never
            // caches, so the hash is zero and the result stays UNKNOWN.
            result=-1.0f;return true;
        }
        switch(mode){
        case Mode::RESOURCE_SIZE:result=b.visible_size?float(b.visible_size):b.image?-3.0f:float(b.description.ByteWidth);break;
        case Mode::RESOURCE_STRIDE:
            // This getter does not request a format, so IAGetIndexBuffer does
            // not derive stride from it. The resource description is used.
            if(effective(input,state).type==Target::INDEX_BUFFER)b.stride=0;
            result=b.stride?float(b.stride):b.image?-3.0f:b.description.StructureByteStride?float(b.description.StructureByteStride):-1.0f;break;
        case Mode::RESOURCE_IDENTITY:result=EncodeFloat30(uint32_t(HashPointer(b.image?static_cast<void*>(b.image.get()):static_cast<void*>(b.buffer.get()))));break;
        case Mode::RESOURCE_BIND_FLAGS:result=float(b.bind_flags());break;
        case Mode::RESOURCE_FORMAT:{
            auto format=b.format;
            if(format==DXGI_FORMAT_UNKNOWN && (input.type==Target::SHADER_RESOURCE || input.type==Target::UNORDERED_ACCESS_VIEW ||
               input.type==Target::RENDER_TARGET || input.type==Target::DEPTH_STENCIL_TARGET))format=b.view_format;
            if(format==DXGI_FORMAT_UNKNOWN && b.image)format=b.image_description.texture.Format;
            result=format==DXGI_FORMAT_UNKNOWN?-1.0f:float(format);break;
        }
        case Mode::RESOURCE_OFFSET:
            if(b.image){result=-3.0f;break;}
            result=-1.0f;
            if(input.type==Target::CONSTANT_BUFFER)result=float(b.offset);
            else if(input.type==Target::VERTEX_BUFFER || input.type==Target::INDEX_BUFFER){
                require(state.call_info!=nullptr,"GPU buffer offset query requires a draw context");
                result=float(input.type==Target::VERTEX_BUFFER?GetVertexBufferRegionOffset(b.stride,state.call_info,b.offset):
                             GetIndexBufferRegionOffset(b.format,state.call_info,b.offset));
            }break;
        case Mode::RESOURCE_WIDTH:result=b.image?float(b.image_description.texture.Width):-4.0f;break;
        case Mode::RESOURCE_HEIGHT:result=b.image?float(b.image_description.texture.Height):-4.0f;break;
        case Mode::RESOURCE_ARRAY:result=b.image && b.image_description.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE3D?float(b.image_description.texture.ArraySize):-4.0f;break;
        case Mode::RESOURCE_MIPS:result=b.image?float(b.image_description.texture.MipLevels):-4.0f;break;
        default:throw std::runtime_error("GPU resource property requires source-semantic adaptation");
        }
        return true;
    }catch(const std::exception& e){if(active_ && commands_.recorded()!=mark)failed_=true;reason=widen(e.what());return false;}
}
bool GpuResourceRuntime::render_target(CommandListState&,bool& bound,float& width,float& height,std::wstring& reason) {
    try{active();bound=false;
        const auto found=slots_.find({L'\0',Target::RENDER_TARGET,0u});
        if(found==slots_.end() || !found->second.image)return true;
        // ProcessParamRTSize: OMGetRenderTargets(1), then the Texture2D of a 2D or 2D multisample view.
        const auto& b=found->second;
        const auto view=original_image_view(b.image_description,Target::RENDER_TARGET,b.view_format).rtv.ViewDimension;
        if(view!=D3D11_RTV_DIMENSION_TEXTURE2D && view!=D3D11_RTV_DIMENSION_TEXTURE2DMS)return true;
        bound=true;width=float(b.image_description.texture.Width);height=float(b.image_description.texture.Height);return true;
    }catch(const std::exception& e){reason=widen(e.what());return false;}
}
GpuResourceBinding GpuResourceRuntime::binding(wchar_t stage,Target type,unsigned index) const {
    const auto found=slots_.find({stage,type,index});return found==slots_.end()?GpuResourceBinding{}:found->second;
}
}
