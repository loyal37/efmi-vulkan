#include "draw_plan.h"
#include "layer_gap.h"
#include "resource_match.h"
#include <cstdarg>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <typeinfo>
#include <chrono>
#include <climits>
#include <array>
#include <map>
#include <filesystem>
#include "ResourceHash.h"
#include "Overlay.h"
#include "globals.h"

void FrontendRunCommandList(CommandList&,CommandListState&);
void (*g_layer_gap_sink)(const wchar_t* text) = nullptr;
void LayerGap(const wchar_t* format, ...)
{
    wchar_t text[2048]{}; va_list args; va_start(args, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args); va_end(args);
    LogOverlayW(LOG_WARNING, const_cast<wchar_t*>(L"%ls\n"), text);
    if (g_layer_gap_sink) g_layer_gap_sink(text);
}
bool FrontendSlotRange(SlotRangeCopyOperation&,CommandListState&,bool& bind,unsigned& first,std::vector<CustomResource*>&);

namespace efmi::frontend {
namespace {
// What ends a command (D-162). The original's unit of failure is the line: a command that cannot act does
// nothing, a warning says so, and its list goes on. `refuse` is that: the command does nothing, the layer's
// notice names it once, the list runs to its end and the draws it records are drawn. `refuse_draw` is for
// what no line of the original corresponds to: the host cannot draw this draw as the Mod's (a native
// command's contract with the renderer, a budget of the planner). The lists still run to their end, so
// the Mod's variables are what the original leaves, and the draw stays the game's.
struct Refused : std::exception {
    std::wstring reason;
    bool draw = false;
    explicit Refused(std::wstring text, bool whole = false) : reason(std::move(text)), draw(whole) {}
};
[[noreturn]] void refuse(const std::wstring& text) { throw Refused{text}; }
[[noreturn]] void refuse_draw(const std::wstring& text) { throw Refused{text, true}; }
std::wstring widen(const char* text) { const std::string s = text ? text : ""; return std::wstring(s.begin(), s.end()); }
constexpr size_t max_steps = 65536, max_items = 4096, max_draws = 256;
constexpr size_t max_active_depth = 64;
// declared: a replaced draw's plan answers a declaration operand (`ResourceX !== null`) from its own
// references (D-112).
struct QueryService {DrawPlanAdapter::ResourceQuery read,texture,plan_read,declared;DrawPlanAdapter::ShaderQuery shader,plan_shader;size_t count=0;bool general_matches=false;
    // Registers whose declaration is one of two (undecided_words), as the running command asked them, and
    // which of them take the second answer in this evaluation (bit i for the i-th).
    std::vector<std::pair<wchar_t,unsigned>> words;uint32_t words_second=0;bool words_over=false;
    DrawPlanAdapter::HostValue host;DrawPlanAdapter::RenderTargetSize rt;
    // inside: in a draw's scope, within a CustomShader (D-087); drawing: a draw's scope is open.
    std::function<bool()> gate,inside,drawing;bool open() const {return !gate || gate() || (inside && inside());}
    // What a replaced draw's state holds at a target (a slot, `this`): false for a target that is no slot of
    // the context, or outside an execution. Queries on a slot read what the lists bound there (D-161).
    std::function<bool(ResourceCopyTarget&,CommandListState&,SlotValue&,SlotId&)> held;
    // The line of the bridged command that runs, for an operand's notice.
    const std::wstring* line=nullptr;
    std::unordered_set<const void*> told;
    std::unordered_map<const CustomResource*,float> file_sizes; // a declared buffer file's size, -1 where it cannot be read
    // An operand nothing answers (P1-4, D-163): the table's value, said once per operand, and the
    // expression goes on. It refused the whole list before.
    float gap(const void* operand,OperandGap kind,const std::wstring& why){return gap(operand,kind,operand_value(kind),why);}
    bool strict=false; // an intercepted shader call: an operand nothing answers ends the call
    float gap(const void* operand,OperandGap kind,float value,const std::wstring& why)
    {
        if(strict)refuse(why.empty()?L"An operand of an intercepted shader call has no answer":why);
        if(told.size()<64 && told.insert(operand).second){
            const char* name="";
            for(const auto& row:operand_table())if(row.gap==kind)name=row.operand;
            LayerGap(L"The Vulkan layer cannot answer an operand and goes on with %g: %ls (%hs%ls%ls)",double(value),
                     line?line->c_str():L"a condition or value",name,why.empty()?L"":L": ",why.c_str());
        }
        return value;
    }
};
// What the owned work of a replaced draw may read or bind (D-087): the Mod's resources, IniParams,
// null and the compute stage's slots. The application's bindings are not reachable from it.
bool owned_side(const ResourceCopyTarget& t)
{
    switch(t.type){
    case ResourceCopyTargetType::EMPTY: case ResourceCopyTargetType::CUSTOM_RESOURCE: case ResourceCopyTargetType::INI_PARAMS: return true;
    case ResourceCopyTargetType::SHADER_RESOURCE: case ResourceCopyTargetType::CONSTANT_BUFFER: case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
        return t.shader_type==L'c';
    default: return false;
    }
}
class NativeShaderQuery final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListOperand> source;
    std::shared_ptr<QueryService> service;
    NativeShaderQuery(std::shared_ptr<CommandListOperand> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    float evaluate(CommandListState* state,HackerDevice* =nullptr) override {
        float value=0;std::wstring why;
        const auto& target=source->shader_target;
        const auto mode=target.evaluation_mode;
        try{
            if(!state || ++service->count>max_steps)return service->gap(this,OperandGap::Evaluation,L"shader query outside a command list or over the query budget");
            if(!service->shader || !service->open()){
                // A replaced draw's plan: what the host answers on the CPU (D-089).
                if(service->plan_shader && service->plan_shader(*source,*state,value,why))return value;
            }else if(service->shader(*source,*state,value,why))return value;
        }catch(const Refused& e){why=e.reason;} // a slot argument outside the shader's registers
        // Nothing answers (process_shader_filter's own values, D-163): a bound shader no ShaderOverride
        // names, a register that is not declared; a draw has no compute shader.
        if(mode==ShaderTargetEvaluationMode::SHADER)
            return service->gap(this,OperandGap::ShaderFilter,target.shader_type==L'c'?-0.0f:operand_value(OperandGap::ShaderFilter),why);
        return service->gap(this,mode==ShaderTargetEvaluationMode::DCL_CB_TYPE || mode==ShaderTargetEvaluationMode::DCL_SRV_TYPE?
                            OperandGap::ShaderDeclaredType:OperandGap::ShaderDeclaredSize,why);
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
class ShaderSlotGuard final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListEvaluatable> source;unsigned limit;
    ShaderSlotGuard(std::shared_ptr<CommandListEvaluatable> value,unsigned maximum):source(std::move(value)),limit(maximum){}
    float evaluate(CommandListState* state,HackerDevice* device=nullptr) override {
        const float value=source->evaluate(state,device);
        if(!std::isfinite(value) || value<0 || double(value)>=limit)refuse(L"Shader declaration slot is outside source bounds");return value;
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
class NativeResourceQuery final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListOperand> source;
    std::shared_ptr<QueryService> service;
    NativeResourceQuery(std::shared_ptr<CommandListOperand> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    // What the original's process_texture_filter gives where no resource is bound, per kind of question.
    static float nothing_bound(ResourceCopyTargetEvaluationMode mode){
        switch(mode){
        case ResourceCopyTargetEvaluationMode::RESOURCE:return -0.0f;
        case ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY:return 0.0f;
        case ResourceCopyTargetEvaluationMode::RESOURCE_SOURCE_STRIDE:return -1.0f;
        case ResourceCopyTargetEvaluationMode::POOL_INDEX:return -3.0f;
        case ResourceCopyTargetEvaluationMode::RESOURCE_BIND_FLAGS:return 0.0f;
        default:return -2.0f; // ResourcePropertyResult::RESOURCE_NOT_FOUND
        }
    }
    static OperandGap gap_of(ResourceCopyTargetEvaluationMode mode){
        switch(mode){
        case ResourceCopyTargetEvaluationMode::RESOURCE:return OperandGap::ResourceFilter;
        case ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY:return OperandGap::ResourceIdentity;
        case ResourceCopyTargetEvaluationMode::RESOURCE_REGION_HASH:case ResourceCopyTargetEvaluationMode::RESOURCE_SPATIAL_HASH:return OperandGap::ResourceHash;
        default:return OperandGap::ResourceProperty;
        }
    }
    // An identity for the application's binding of a slot: of the slot, since the layer does not hold the
    // game's resource objects (the original hashes the object's pointer).
    static float slot_identity(const SlotId& id){
        const uint64_t mixed=(uint64_t(DrawState::cell(id))+1)*0x9E3779B97F4A7C15ull;
        return EncodeFloat30(uint32_t(mixed^mixed>>32)|1u);
    }
    // A question about a resource of the Mod bound at a slot, as the original answers it for the custom
    // resource (GetResourceStride, GetResourceFormat read the declaration first). False: not known here.
    bool of_declaration(CustomResource* resource,ResourceCopyTargetEvaluationMode mode,float& result){
        switch(mode){
        case ResourceCopyTargetEvaluationMode::RESOURCE_SIZE:{
            // A buffer's ByteWidth as the original makes it: its `byte_width`, its `data`, or its file's size
            // (read once per declaration).
            if(resource->override_type!=CustomResourceType::BUFFER && resource->override_type!=CustomResourceType::STRUCTURED_BUFFER &&
               resource->override_type!=CustomResourceType::RAW_BUFFER)return false;
            if(resource->override_byte_width!=-1){result=float(resource->override_byte_width);return true;}
            if(resource->initial_data && resource->initial_data_size){result=float(resource->initial_data_size);return true;}
            if(resource->filename.empty())return false;
            auto found=service->file_sizes.find(resource);
            if(found==service->file_sizes.end()){
                std::error_code error;const auto size=std::filesystem::file_size(resource->filename,error);
                if(service->file_sizes.size()>=4096)service->file_sizes.clear();
                found=service->file_sizes.emplace(resource,error?-1.0f:float(size)).first;
            }
            if(found->second<0)return false;
            result=found->second;return true;}
        case ResourceCopyTargetEvaluationMode::RESOURCE:result=0.0f;return true; // bound; the original keeps no hash for what it made
        case ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY:result=EncodeFloat30(uint32_t(HashPointer(resource)));return true;
        case ResourceCopyTargetEvaluationMode::RESOURCE_STRIDE:
            if(resource->override_stride!=-1){result=float(resource->override_stride);return true;}
            if(resource->override_format!=DXGI_FORMAT(-1) && resource->override_format!=DXGI_FORMAT_UNKNOWN){result=float(dxgi_format_size(resource->override_format));return true;}
            return false;
        case ResourceCopyTargetEvaluationMode::RESOURCE_FORMAT:
            if(resource->override_format!=DXGI_FORMAT(-1) && resource->override_format!=DXGI_FORMAT_UNKNOWN){result=float(resource->override_format);return true;}
            return false;
        case ResourceCopyTargetEvaluationMode::RESOURCE_SOURCE_STRIDE:result=float(resource->source_stride);return true;
        case ResourceCopyTargetEvaluationMode::POOL_INDEX:result=float(resource->pool_index);return true;
        default:return false;
        }
    }
    // What the lists bound at the slot the question names (D-161): the original reads the context as it
    // stands when the line runs, so after `vb0 = ResourceX` a question about vb0 is one about ResourceX,
    // after `ps-t3 = null` one about nothing, after `ps-t1 = ref ps-t0` one about the game's texture of
    // slot 0. Given: answered. None: not answerable, with the reason. Host: the application's own binding
    // is there (or the target is no slot), and the host is asked as before. `matcher`: a pixel slot's
    // filter value, which the texture matcher answers for a Mod's resource and the game's alike.
    enum class Answer {Given,None,Host};
    Answer from_state(CommandListState* state,ResourceCopyTarget& target,bool matcher,float& result,std::wstring& reason){
        SlotValue held;SlotId slot;
        if(!service->held || !service->held(target,*state,held,slot))return Answer::Host;
        const auto mode=target.evaluation_mode;
        switch(held.kind){
        case SlotValue::Kind::Null:result=nothing_bound(mode);return Answer::Given;
        case SlotValue::Kind::Resource:case SlotValue::Kind::Owned:
            if(matcher)return Answer::Host;
            if(of_declaration(held.resource,mode,result))return Answer::Given;
            reason=L"a property of the Mod's resource bound at "+slot_name(slot)+L" that its declaration does not state";
            return Answer::None;
        case SlotValue::Kind::Foreign:
            if(mode==ResourceCopyTargetEvaluationMode::RESOURCE){result=0.0f;return Answer::Given;} // bound, and no section names it
            if(mode==ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY){result=slot_identity(slot);return Answer::Given;}
            reason=L"a resource the plan does not hold is bound at "+slot_name(slot);
            return Answer::None;
        default:break;
        }
        if(mode==ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY){result=slot_identity(held.source);return Answer::Given;}
        if(held.source==slot || matcher)return Answer::Host;
        // The application's binding of another slot: the question is that slot's.
        if(service->plan_read && !target.custom_resource_pool && target.type!=ResourceCopyTargetType::THIS_RESOURCE &&
           held.source.kind==slot.kind && held.source.stage==slot.stage){
            // The operand's own target names that slot for the length of the question.
            struct Named {ResourceCopyTarget& t;unsigned slot;std::unique_ptr<CommandListExpression> expression;
                ~Named(){t.slot=slot;t.slot_expression=std::move(expression);}} named{target,target.slot,std::move(target.slot_expression)};
            target.slot=held.source.slot;
            if(service->plan_read(target,*state,result,reason))return Answer::Given;
        }
        if(reason.empty())reason=L"the application's binding of "+slot_name(held.source)+L" is bound at "+slot_name(slot);
        return Answer::None;
    }
    bool answer(CommandListState* state,ResourceCopyTarget& target,float& result,std::wstring& reason,OperandGap& gap){
        // `null` (`X === null`): the original finds no resource, -0 as a resource, 0 as an identity.
        if(target.type==ResourceCopyTargetType::EMPTY && !target.custom_resource_pool){
            if(target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE){result=-0.0f;return true;}
            if(target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY){result=0.0f;return true;}
        }
        const bool drawing=service->drawing && service->drawing();
        // A declaration outside an owned scope, or in a generation without owned GPU work at all.
        if((!service->open() || !service->read) && service->declared && target.type==ResourceCopyTargetType::CUSTOM_RESOURCE && !target.custom_resource_pool &&
           (target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE || target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY)){
            if(service->declared(target,*state,result,reason))return true;
            if(reason.empty())reason=L"Declaration query refused";
            return false;
        }
        const bool pixel=target.type==ResourceCopyTargetType::THIS_RESOURCE ||
            (target.type==ResourceCopyTargetType::SHADER_RESOURCE && target.shader_type==L'p');
        const bool texture_match=(pixel || service->general_matches) && target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE && service->texture;
        if(!service->open() || (drawing && !owned_side(target))){
            // Outside an owned scope (a replaced draw's plan), or the application's bindings in a draw's
            // scope. First what the lists bound at the slot.
            switch(from_state(state,target,texture_match && pixel,result,reason)){
            case Answer::Given:return true;
            case Answer::None:return false;
            default:break;
            }
            // What the host answers on the CPU. A Mod resource in a draw's scope: what the owned work
            // holds (D-087).
            if(service->plan_read && service->plan_read(target,*state,result,reason))return true;
            if(!drawing || !owned_side(target) || !service->read){
                // `ps-t0 == 905031` where nothing else answers: the original's FindTextureOverrides on the texture
                // bound at that pixel slot, the filter_index of the section that matches it, from the host's
                // texture matcher (a Mod finding which slot holds one of its textures, BUG-064).
                if(texture_match && pixel){
                    reason.clear();
                    if(service->texture(target,*state,result,reason))return true;
                    if(reason.empty())reason=L"Texture query refused";
                    return false;
                }
                if(reason.empty())reason=L"nothing answers for the game's resource in a replaced draw's plan";
                return false;
            }
            reason.clear();
        }
        const auto& read=texture_match?service->texture:service->read;
        if(!read){
            // No owned GPU work in this generation (the offline dry run, a CPU generation): what the lists
            // bound at the slot, then what the host answers on the CPU, as a replaced draw's plan outside an
            // owned scope.
            switch(from_state(state,target,false,result,reason)){
            case Answer::Given:return true;
            case Answer::None:return false;
            default:break;
            }
            if(service->plan_read && service->plan_read(target,*state,result,reason))return true;
            if(reason.empty())reason=L"Resource query adapter is unavailable";
            return false;
        }
        if(read(target,*state,result,reason))return true;
        if(reason.empty())reason=L"Native resource query refused";
        return false;
    }
    float evaluate(CommandListState* state,HackerDevice* =nullptr) override {
        float result=0;std::wstring reason;
        auto& target=source->texture_filter_target;
        OperandGap gap=gap_of(target.evaluation_mode);
        if(!state || ++service->count>max_steps)return service->gap(this,OperandGap::Evaluation,L"resource query outside a command list or over the query budget");
        try{if(answer(state,target,result,reason,gap))return result;}
        catch(const Refused& e){reason=e.reason;}
        catch(const std::exception& e){reason=widen(e.what());}
        // Nothing answers (D-163). A filter value reads "bound, and no TextureOverride names it"; an
        // identity is one of the operand; a property is the original's own UNKNOWN.
        if(gap==OperandGap::ResourceIdentity)return service->gap(this,gap,EncodeFloat30(uint32_t(HashPointer(this))|1u),reason);
        return service->gap(this,gap,reason);
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
class PoolIndexGuard final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListEvaluatable> source;
    CustomResourcePool* pool;
    PoolIndexGuard(std::shared_ptr<CommandListEvaluatable> value,CustomResourcePool* owner):source(std::move(value)),pool(owner){}
    float evaluate(CommandListState* state,HackerDevice* device=nullptr) override {
        auto* resolved=pool->ResolvePool();
        try{validate_pool_runtime(*resolved);}
        catch(const std::exception& e){const std::string text=e.what();refuse(std::wstring(text.begin(),text.end()));}
        const float value=source->evaluate(state,device);
        if((resolved->index_type==PoolIndexType::RING || resolved->index_type==PoolIndexType::STATIC) &&
           (!std::isfinite(value) || double(value)<INT_MIN || double(value)>INT_MAX))refuse(L"Direct pool index outside signed integer range");
        return value;
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
class RangeIndexGuard final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListEvaluatable> source;std::shared_ptr<QueryService> service;
    RangeIndexGuard(std::shared_ptr<CommandListEvaluatable> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    float evaluate(CommandListState* state,HackerDevice* device=nullptr) override {
        // A bound outside the integers: not a number, which the original's ResolveRange finds invalid (its
        // warning, and the command does nothing) where its cast would be undefined.
        const auto value=source->evaluate(state,device);
        if(!std::isfinite(value) || double(value)<INT_MIN || double(value)>INT_MAX){
            if(service->strict)throw std::runtime_error("Resource range bound outside signed integer range");
            return std::numeric_limits<float>::quiet_NaN();
        }
        return value;
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
// The original's ProcessParamRTSize caches the size of o0 in the state once a 2D view is bound
// and asks again while the cache stays -1. The owned domain answers for o0; -2 marks "asked,
// nothing bound" for the window heuristic, so the reference context is never queried; -3 is the
// host's answer for a replaced draw whose pass binds no colour target (the original's -1 there).
// False with the reason where nothing answers: the host gave the state no size and no owned domain is open.
bool fill_render_target(CommandListState& state,QueryService& service,std::wstring& why)
{
    if(state.rt_width!=-1 && state.rt_width!=-2)return true;
    if(!service.rt || !service.open()){why=L"the host does not know the draw's target, and no owned GPU domain is open";return false;}
    bool bound=false;float width=-1,height=-1;
    if(!service.rt(state,bound,width,height,why)){if(why.empty())why=L"Render target size query refused";return false;}
    if(bound){state.rt_width=width;state.rt_height=height;}
    return true;
}
class RenderTargetQuery final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListOperand> source;
    std::shared_ptr<QueryService> service;
    RenderTargetQuery(std::shared_ptr<CommandListOperand> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    float evaluate(CommandListState* state,HackerDevice* =nullptr) override {
        // Where nothing answers: the original's -1, what it reads while no 2D target is bound (D-163).
        std::wstring why;
        if(!state)return service->gap(this,OperandGap::RenderTargetSize,L"outside a command list");
        if(!fill_render_target(*state,*service,why))return service->gap(this,OperandGap::RenderTargetSize,why);
        const float value=source->type==ParamOverrideType::RT_WIDTH?state->rt_width:state->rt_height;
        return value==-2 || value==-3?-1.0f:value;
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
// window_width, window_height and the relative cursor fall back to the bound render target
// when neither the window nor the resolution reaches 640. Outside the owned domain a hosted
// generation has no render target, so the heuristic continues as the original does with none
// bound.
class HostWindowQuery final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListEvaluatable> source;
    std::shared_ptr<QueryService> service;
    HostWindowQuery(std::shared_ptr<CommandListEvaluatable> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    float evaluate(CommandListState* state,HackerDevice* device=nullptr) override {
        if(state && (state->rt_width==-1 || state->rt_width==-2)){
            std::wstring why;
            if(service->rt && service->open())fill_render_target(*state,*service,why);
            if(state->rt_width==-1)state->rt_width=state->rt_height=-2;
        }
        return source->evaluate(state,device);
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
// effective_dpi and cursor_hotspot_x/y: the original asks Win32 directly (GetDpiForMonitor on
// the window's monitor, GetIconInfo on the cursor); the host answers from its sample.
class HostValueQuery final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListOperand> source;
    std::shared_ptr<QueryService> service;
    HostValueQuery(std::shared_ptr<CommandListOperand> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    float evaluate(CommandListState* state,HackerDevice* =nullptr) override {
        // Without a sample: the original's least DPI, and no hotspot (D-163).
        float value=0;
        if((state || source->type==ParamOverrideType::EFFECTIVE_DPI) && service->host && service->host(source->type,value) && std::isfinite(value))return value;
        return service->gap(this,OperandGap::HostSample,source->type==ParamOverrideType::EFFECTIVE_DPI?96.0f:0.0f,L"no host sample");
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
// scissor_left and its like: the original asks the context (RSGetScissorRects), which a hosted generation
// has not; the table's value, said once (D-163).
class UnansweredOperand final : public CommandListEvaluatable {
public:
    std::shared_ptr<QueryService> service;OperandGap kind;
    UnansweredOperand(std::shared_ptr<QueryService> owner,OperandGap gap):service(std::move(owner)),kind(gap){}
    float evaluate(CommandListState*,HackerDevice* =nullptr) override {return service->gap(this,kind,L"a hosted generation has no D3D11 context to ask");}
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
bool scissor_operand(ParamOverrideType type){
    return type==ParamOverrideType::SCISSOR_LEFT || type==ParamOverrideType::SCISSOR_TOP ||
           type==ParamOverrideType::SCISSOR_RIGHT || type==ParamOverrideType::SCISSOR_BOTTOM;
}
bool host_value(ParamOverrideType type){
    return type==ParamOverrideType::EFFECTIVE_DPI || type==ParamOverrideType::CURSOR_HOTSPOT_X || type==ParamOverrideType::CURSOR_HOTSPOT_Y;
}
bool window_heuristic(ParamOverrideType type){
    return type==ParamOverrideType::WINDOW_WIDTH || type==ParamOverrideType::WINDOW_HEIGHT ||
           type==ParamOverrideType::CURSOR_X || type==ParamOverrideType::CURSOR_Y;
}
class SlotIndexGuard final : public CommandListEvaluatable {
public:
    std::shared_ptr<CommandListEvaluatable> source;std::shared_ptr<QueryService> service;
    SlotIndexGuard(std::shared_ptr<CommandListEvaluatable> value,std::shared_ptr<QueryService> owner):source(std::move(value)),service(std::move(owner)){}
    float evaluate(CommandListState* state,HackerDevice* device=nullptr) override {
        // Not a number: below every slot, which the original's ResolveSlot reports as out of range (its
        // warning, and the command does nothing) where its cast would be undefined.
        const auto value=source->evaluate(state,device);
        if(!std::isfinite(value) && service->strict)refuse(L"Resource slot index is not finite");
        return std::isfinite(value)?value:-1.0f;
    }
    bool static_evaluate(float*,HackerDevice* =nullptr,bool=false) override {return false;}
    bool optimise(HackerDevice*,std::shared_ptr<CommandListEvaluatable>*) override {return false;}
};
bool cpu_expression(const std::shared_ptr<CommandListEvaluatable>&,size_t);
bool cpu_target(const ResourceCopyTarget& target,size_t depth=0)
{
    if(!(depth<=256 && (!target.custom_resource_pool || target.custom_resource_pool->GetPoolSize()<=max_steps) &&
        (!target.pool_dynamic_index_expression || cpu_expression(target.pool_dynamic_index_expression->evaluatable,depth+1)) &&
        (!target.slot_expression || cpu_expression(target.slot_expression->evaluatable,depth+1)) &&
        (!target.range_start || cpu_expression(target.range_start->evaluatable,depth+1)) &&
        (!target.range_end || cpu_expression(target.range_end->evaluatable,depth+1))))return false;
    for(const auto& arg:target.member_args)if(arg.expression && !cpu_expression(arg.expression->evaluatable,depth+1))return false;
    return true;
}

// Inspect the original expression tree without reimplementing its evaluator.
// Unadapted resource/window queries must not reach the reference D3D context.
bool cpu_expression(const std::shared_ptr<CommandListEvaluatable>& node, size_t depth = 0)
{
    if (!node || depth > 256) return false;
    if(const auto* query=dynamic_cast<NativeShaderQuery*>(node.get())){
        for(const auto& arg:query->source->shader_target.member_args)
            if(arg.expression && !cpu_expression(arg.expression->evaluatable,depth+1))return false;return true;
    }
    if(const auto* guard=dynamic_cast<ShaderSlotGuard*>(node.get()))return cpu_expression(guard->source,depth+1);
    if(const auto* query=dynamic_cast<NativeResourceQuery*>(node.get()))return cpu_target(query->source->texture_filter_target,depth+1);
    if(const auto* guard=dynamic_cast<PoolIndexGuard*>(node.get()))return cpu_expression(guard->source,depth+1);
    if(const auto* guard=dynamic_cast<RangeIndexGuard*>(node.get()))return cpu_expression(guard->source,depth+1);
    if(const auto* guard=dynamic_cast<SlotIndexGuard*>(node.get()))return cpu_expression(guard->source,depth+1);
    if(const auto* query=dynamic_cast<HostWindowQuery*>(node.get()))return dynamic_cast<CommandListOperand*>(query->source.get())!=nullptr;
    if(dynamic_cast<HostValueQuery*>(node.get()) || dynamic_cast<RenderTargetQuery*>(node.get()) || dynamic_cast<UnansweredOperand*>(node.get()))return true;
    if (const auto* operand = dynamic_cast<CommandListOperand*>(node.get())) {
        switch (operand->type) {
        case ParamOverrideType::VALUE: return true;
        case ParamOverrideType::INI_PARAM:
            return G && operand->param_idx >= 0 && size_t(operand->param_idx) < G->iniParams.size();
        case ParamOverrideType::VARIABLE: return operand->var_ftarget != nullptr;
        case ParamOverrideType::TEXTURE: {
            const auto& target=operand->texture_filter_target;
            // process_texture_filter has no case for ->region: it returns 0 without the resource.
            if(target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE_REGION)return true;
            if(!target.custom_resource_pool || !cpu_target(target,depth+1))return false;
            switch(target.evaluation_mode){
            case ResourceCopyTargetEvaluationMode::VARIABLE:
            case ResourceCopyTargetEvaluationMode::POOL_SIZE:
            case ResourceCopyTargetEvaluationMode::POOL_INDEX:
            case ResourceCopyTargetEvaluationMode::POOL_IDENTITY:return true;
            default:return false;
            }
        }
        case ParamOverrideType::VERTEX_COUNT: case ParamOverrideType::INDEX_COUNT:
        case ParamOverrideType::INSTANCE_COUNT: case ParamOverrideType::FIRST_VERTEX:
        case ParamOverrideType::FIRST_INDEX: case ParamOverrideType::FIRST_INSTANCE:
        case ParamOverrideType::DRAW_TYPE: case ParamOverrideType::FRAME_NUMBER: case ParamOverrideType::FRAME_TIME:
        case ParamOverrideType::DRAW_NUMBER: case ParamOverrideType::DISPATCH_NUMBER:
        case ParamOverrideType::TIME: case ParamOverrideType::FPS:
        case ParamOverrideType::THREAD_GROUP_COUNT_X: case ParamOverrideType::THREAD_GROUP_COUNT_Y:
        case ParamOverrideType::THREAD_GROUP_COUNT_Z: case ParamOverrideType::INDIRECT_OFFSET:
        // The host's window, cursor and swap chain sample answers these (RuntimeInput).
        case ParamOverrideType::RES_WIDTH: case ParamOverrideType::RES_HEIGHT:
        case ParamOverrideType::CURSOR_VISIBLE: case ParamOverrideType::CURSOR_SCREEN_X: case ParamOverrideType::CURSOR_SCREEN_Y:
        case ParamOverrideType::CURSOR_WINDOW_X: case ParamOverrideType::CURSOR_WINDOW_Y:
        // The generation's own globals and the original's constants: hunting and frame analysis are off in
        // a hosted generation, SLI and stereo read 0 in the original itself.
        case ParamOverrideType::HUNTING: case ParamOverrideType::FRAME_ANALYSIS: case ParamOverrideType::SLI:
        case ParamOverrideType::STEREO_ACTIVE: case ParamOverrideType::STEREO_AVAILABLE:
            return true;
        default: return false;
        }
    }
    if (const auto* operation = dynamic_cast<CommandListOperator*>(node.get()))
        return (!operation->lhs || cpu_expression(operation->lhs, depth + 1)) &&
               (!operation->rhs || cpu_expression(operation->rhs, depth + 1));
    return false;
}

void guard_expression(std::shared_ptr<CommandListEvaluatable>&,const std::shared_ptr<QueryService>&,size_t=0);
void guard_target(ResourceCopyTarget& target,const std::shared_ptr<QueryService>& queries,size_t depth=0)
{
    if(depth>256)refuse(L"Pool expression nesting budget exceeded");
    if(target.pool_dynamic_index_expression){
        auto& expression=target.pool_dynamic_index_expression->evaluatable;guard_expression(expression,queries,depth+1);
        if(target.custom_resource_pool && !dynamic_cast<PoolIndexGuard*>(expression.get()))
            expression=std::make_shared<PoolIndexGuard>(expression,target.custom_resource_pool);
    }
    if(target.slot_expression){auto& expression=target.slot_expression->evaluatable;guard_expression(expression,queries,depth+1);
        if(!dynamic_cast<SlotIndexGuard*>(expression.get()))expression=std::make_shared<SlotIndexGuard>(expression,queries);}
    for(auto* bound:{target.range_start.get(),target.range_end.get()})if(bound){
        guard_expression(bound->evaluatable,queries,depth+1);
        if(!dynamic_cast<RangeIndexGuard*>(bound->evaluatable.get()))bound->evaluatable=std::make_shared<RangeIndexGuard>(bound->evaluatable,queries);
    }
    for(auto& arg:target.member_args)if(arg.expression)guard_expression(arg.expression->evaluatable,queries,depth+1);
}
void guard_expression(std::shared_ptr<CommandListEvaluatable>& node,const std::shared_ptr<QueryService>& queries,size_t depth)
{
    if(depth>256)refuse(L"Pool expression nesting budget exceeded");
    if(auto operand=std::dynamic_pointer_cast<CommandListOperand>(node)){
        // Always: where no host answers for the game's shaders the query gives the table's value (D-163).
        if(operand->type==ParamOverrideType::SHADER){
            auto& target=operand->shader_target;
            const auto mode=target.evaluation_mode;
            const unsigned limit=mode==ShaderTargetEvaluationMode::DCL_CB_TYPE || mode==ShaderTargetEvaluationMode::DCL_CB_SIZE?14u:128u;
            for(auto& arg:target.member_args)if(arg.expression){guard_expression(arg.expression->evaluatable,queries,depth+1);
                if(!dynamic_cast<ShaderSlotGuard*>(arg.expression->evaluatable.get()))
                    arg.expression->evaluatable=std::make_shared<ShaderSlotGuard>(arg.expression->evaluatable,limit);}
            node=std::make_shared<NativeShaderQuery>(std::move(operand),queries);return;
        }
        if(window_heuristic(operand->type)){node=std::make_shared<HostWindowQuery>(std::move(operand),queries);return;}
        if(host_value(operand->type)){node=std::make_shared<HostValueQuery>(std::move(operand),queries);return;}
        if(scissor_operand(operand->type)){node=std::make_shared<UnansweredOperand>(queries,OperandGap::Scissor);return;}
        // Always: a replaced draw's state carries the host's answer (RuntimeDraw::target_width), with or
        // without an owned domain to ask; without either the query refuses with that reason.
        if(operand->type==ParamOverrideType::RT_WIDTH || operand->type==ParamOverrideType::RT_HEIGHT){
            node=std::make_shared<RenderTargetQuery>(std::move(operand),queries);return;
        }
        guard_target(operand->texture_filter_target,queries,depth+1);
        if(operand->type==ParamOverrideType::TEXTURE && !cpu_expression(operand,depth+1))
            node=std::make_shared<NativeResourceQuery>(std::move(operand),queries);
    }
    else if(auto* op=dynamic_cast<CommandListOperator*>(node.get())){
        if(op->lhs)guard_expression(op->lhs,queries,depth+1);if(op->rhs)guard_expression(op->rhs,queries,depth+1);
    }
}
int draw_argument_count(DrawCommandType type){
    switch(type){
    case DrawCommandType::DRAW:return 2;
    case DrawCommandType::DRAW_INDEXED:case DrawCommandType::DISPATCH:return 3;
    case DrawCommandType::DRAW_INSTANCED:return 4;
    case DrawCommandType::DRAW_INDEXED_INSTANCED:return 5;
    case DrawCommandType::DRAW_INDEXED_INSTANCED_INDIRECT:case DrawCommandType::DRAW_INSTANCED_INDIRECT:
    case DrawCommandType::DISPATCH_INDIRECT:return 1;
    default:return 0;
    }
}
bool indirect_command(DrawCommandType type){return type==DrawCommandType::DRAW_INDEXED_INSTANCED_INDIRECT ||
    type==DrawCommandType::DRAW_INSTANCED_INDIRECT || type==DrawCommandType::DISPATCH_INDIRECT;}
void guard_arguments(CommandListCommand& command,const std::shared_ptr<QueryService>& queries)
{
    if(auto* draw=dynamic_cast<DrawCommand*>(&command)){
        const int count=draw_argument_count(draw->type);
        for(int i=0;i<count;++i)guard_expression(draw->args[i].evaluatable,queries);
        if(indirect_command(draw->type))guard_target(draw->indirect_buffer,queries);
    }else if(auto* branch=dynamic_cast<IfCommand*>(&command))guard_expression(branch->expression.evaluatable,queries);
    else if(auto* assignment=dynamic_cast<AssignmentCommand*>(&command)){
        guard_expression(assignment->expression.evaluatable,queries);
        if(auto* pool=dynamic_cast<PoolVariableOperation*>(assignment))guard_target(pool->dst,queries);
    }else if(auto* binding=dynamic_cast<ResourceCopyOperation*>(&command)){guard_target(binding->src,queries);guard_target(binding->dst,queries);}
    else if(auto* range=dynamic_cast<SlotRangeCopyOperation*>(&command)){guard_target(range->src,queries);guard_target(range->dst,queries);}
    else if(auto* layout=dynamic_cast<LayoutElementOperation*>(&command)){
        if(layout->dst.member_args[1].expression)guard_expression(layout->dst.member_args[1].expression->evaluatable,queries);
    }else if(auto* clear=dynamic_cast<ClearViewCommand*>(&command))guard_target(clear->target,queries);
    else if(auto* store=dynamic_cast<StoreCommand*>(&command)){
        guard_target(store->src,queries);guard_target(store->pool_var,queries);
        if(store->offset_expression)guard_expression(store->offset_expression->evaluatable,queries);
    }else if(auto* batch=dynamic_cast<ShaderResourceBatch*>(&command)){
        for(auto& binding:batch->operations)if(binding)guard_arguments(*binding,queries);
    }
    else if(auto* check=dynamic_cast<CheckTextureOverrideCommand*>(&command))guard_target(check->target,queries);
}

bool cpu_arguments(CommandListCommand& command)
{
    if (auto* draw = dynamic_cast<DrawCommand*>(&command)) {
        const int count=draw_argument_count(draw->type);
        for (int i = 0; i < count; ++i) if (!cpu_expression(draw->args[i].evaluatable)) return false;
        if(indirect_command(draw->type) && !cpu_target(draw->indirect_buffer))return false;
    } else if (auto* branch = dynamic_cast<IfCommand*>(&command)) {
        return cpu_expression(branch->expression.evaluatable);
    } else if (auto* assignment = dynamic_cast<AssignmentCommand*>(&command)) {
        const auto* pool=dynamic_cast<PoolVariableOperation*>(assignment);
        return cpu_expression(assignment->expression.evaluatable) && (!pool || cpu_target(pool->dst));
    } else if (auto* binding = dynamic_cast<ResourceCopyOperation*>(&command)) {
        return cpu_target(binding->src) && cpu_target(binding->dst);
    } else if(auto* range=dynamic_cast<SlotRangeCopyOperation*>(&command)) {
        return cpu_target(range->src) && cpu_target(range->dst);
    } else if(auto* clear=dynamic_cast<ClearViewCommand*>(&command)) {
        return cpu_target(clear->target);
    } else if(auto* store=dynamic_cast<StoreCommand*>(&command)) {
        return cpu_target(store->src) && cpu_target(store->pool_var) && store->offset_expression &&
               cpu_expression(store->offset_expression->evaluatable);
    } else if (auto* layout = dynamic_cast<LayoutElementOperation*>(&command)) {
        return layout->dst.member_args[1].expression &&
               cpu_expression(layout->dst.member_args[1].expression->evaluatable);
    } else if(auto* check=dynamic_cast<CheckTextureOverrideCommand*>(&command)) {
        return cpu_target(check->target);
    }
    return true;
}

int integer(CommandListExpression& expression, CommandListState* state)
{
    const double value = expression.evaluate(state);
    if (!std::isfinite(value) || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max())
        refuse(L"Draw argument is outside the signed integer range");
    return static_cast<int>(value); // XXMI truncates towards zero.
}
unsigned positive(CommandListExpression& expression, CommandListState* state)
{
    const int value = integer(expression, state);
    if (value < 0) refuse(L"Negative unsigned draw argument");
    return static_cast<unsigned>(value);
}
}

void validate_pool_runtime(CustomResourcePool& input)
{
    auto* pool=input.ResolvePool();
    if(!pool->GetPoolSize() || pool->GetPoolSize()>max_steps)throw std::runtime_error("Pool size outside planner budget");
    if(pool->index_type!=PoolIndexType::RING && pool->index_type!=PoolIndexType::STATIC &&
       pool->index_type!=PoolIndexType::FIFO && pool->index_type!=PoolIndexType::SPATIAL)
        throw std::runtime_error("Original pool index type is invalid");
    if(pool->index_type==PoolIndexType::SPATIAL){
        if(!G || !std::isfinite(G->gFrameTime) || G->gFrameTime<0)throw std::runtime_error("Spatial pool frame duration is invalid");
        const float scaled=std::ceil(pool->spatial_radius*(G->gFrameTime*120.0f));
        if(!std::isfinite(scaled) || scaled<0 || double(scaled)>UINT32_MAX)
            throw std::runtime_error("Spatial pool radius outside unsigned integer range");
    }
}

#ifdef EFMI_COMMAND_TIMING
thread_local uint64_t g_command_children_ns = 0; // nanoseconds the children of the running command took
#endif
struct DrawPlanAdapter::Runtime {
    DrawPlan* active = nullptr;
    size_t steps = 0, draws = 0;
    // Microseconds and calls per command class over every execution (D-099): where a draw's INI time goes.
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> command_timing;
    // The plan an execution builds, kept between executions for its capacity (D-108); an execution
    // never nests (it refuses an active planner), so one suffices.
    DrawPlan candidate;
    std::unordered_map<const char*, std::pair<uint64_t, uint64_t>*> timing_slots; // note_timing's, by name pointer
    std::unordered_set<CommandList*> adapted;
    struct External {std::wstring name;DrawPlanAdapter::NativeCommand command;};
    std::unordered_map<ExplicitCommandListSection*,External> externals;
    std::unordered_map<ExplicitCommandListSection*,DrawPlanAdapter::NativeCommand> observers;
    // What a declaration holds, where the lists assigned it: a value of the draw state, with the execution
    // an application's or owned value was taken in (0 for a value that lasts).
    struct Held {SlotValue value;uint64_t execution=0;};
    std::unordered_map<CustomResource*,Held> references;
    std::shared_ptr<QueryService> queries=std::make_shared<QueryService>();
    DrawPlanAdapter::FreshState fresh;
    DrawPlanAdapter::PaletteSource palette_source;
    DrawPlanAdapter::ResourceCommand resource_command;
    DrawPlanAdapter::ParameterFlush parameter_flush;
    DrawPlanAdapter::DrawPrepare draw_prepare;
    DrawPlanAdapter::ShaderCommand shader_command;
    bool nested_failed=false;
    // In a Present, Constants or key list the original runs each command on its own: one that cannot act
    // does nothing and the list goes on, at every depth of called lists (D-155). With this set a command
    // the planner refuses, or that raises, is skipped and said once (skipped()).
    bool tolerant=false;
    void skipped(const CommandListCommand* command,const std::wstring& line,const std::wstring& why)
    {
        if(unknown_identity_logged.size()<64 && unknown_identity_logged.insert(command).second)
            LayerGap(L"The Vulkan layer does not take this command, which does nothing: %ls (%ls)",line.c_str(),why.c_str());
    }
    bool keep_original=false; // a native kept the application's draw (DrawPlanAdapter::keep_original)
    bool application_drawn=false; // the pre lists ended: the application's draw ran, or was skipped
    // An intercepted shader call (execute_intercepted): the owned domain's transaction, where a command
    // that cannot act and an operand nothing answers end the call, as before D-162.
    bool strict=false;
    const CommandList* root=nullptr; // the matched root whose commands run (DrawPlanAdapter::current_root)
    const std::wstring* calling_line=nullptr; // the line that runs a native command, during it (current_line)
    size_t active_depth=0;
    DrawPlanAdapter::TextureQuery texture_query;
    DrawPlanAdapter::ResourceMatchQuery resource_match;
    // The context the lists of an execution run against (D-161, draw_state.h): every slot they set. It is
    // the one structure in place of the six the plan kept before: the pixel bindings and what a reference of
    // a pixel slot held, the vertex and index bindings, the stand-ins of the application's textures and of
    // its other slots, and the changes of those.
    DrawState context;
    // The execution's serial. "The application's binding of slot S" is that of the draw the lists run for:
    // a declaration that took one holds it for the lists of that draw. In a later draw it holds a resource
    // the plan does not know (the original holds the object, which the game may have bound anywhere, or
    // released, since); so does a declaration that took what the draw's owned work wrote.
    uint64_t execution=0;
    std::function<PipelineFacts()> facts; // DrawPlanAdapter::pipeline
    std::function<bool(uint32_t&)> draw_index_hash;
    std::unordered_set<const void*> unknown_identity_logged;
    // The first reason the host cannot draw this draw as the Mod's (refuse_draw), kept while the lists run
    // to their end (D-162).
    std::wstring draw_refused;
    void whole(const std::wstring& why){if(draw_refused.empty())draw_refused=why.empty()?L"The draw cannot be the Mod's":why;}
    // What a command raised: a host failure leaves the draw to the game (in a list that draws nothing it
    // is one more skipped command); anything else is the command's own, which does nothing and is said once.
    void contained(const CommandListCommand* command,const std::wstring& line,const Refused& refused)
    {
        if(refused.draw && !tolerant)whole(refused.reason);
        else skipped(command,line,refused.reason);
    }
    // The pipeline slot a target names, with its number resolved as the original resolves it: Nothing for a
    // number out of range (the original's warning; the command then does nothing), Other for a target
    // that is no slot of the context.
    enum class Named {Slot,Nothing,Other};
    Named named(ResourceCopyTarget& target,CommandListState& state,SlotId& id)
    {
        switch(target.type){
        case ResourceCopyTargetType::VERTEX_BUFFER:case ResourceCopyTargetType::INDEX_BUFFER:case ResourceCopyTargetType::SHADER_RESOURCE:
        case ResourceCopyTargetType::CONSTANT_BUFFER:case ResourceCopyTargetType::RENDER_TARGET:case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
        case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:case ResourceCopyTargetType::STREAM_OUTPUT:break;
        default:return Named::Other;
        }
        const unsigned slot=target.ResolveSlot(&state);
        return slot!=UINT_MAX && slot_of(target,slot,id)?Named::Slot:Named::Nothing;
    }
    // What a declaration holds now.
    SlotValue held(CustomResource* resource) const
    {
        SlotValue value;
        if(!resource)return value;
        const auto found=references.find(resource);
        if(found!=references.end()){
            // The original pool engine marks only the affected resource null when
            // a slot changes type or a resource reset runs. Preserve references in
            // other slots and the independent resource half of a mixed element.
            if(resource->is_null)return value;
            value=found->second.value;
            if(found->second.execution && found->second.execution!=execution){value.kind=SlotValue::Kind::Foreign;value.owned.reset();}
            return value;
        }
        // An empty original Resource declaration has no object until assigned; nor has a pool element its
        // untyped template made that nothing assigned or wrote (D-143: the original binds it as null).
        // Preserve typed/generated resources for their own adapters.
        if(resource->filename.empty() && resource->override_type==CustomResourceType::INVALID &&
           resource->override_format==DXGI_FORMAT(-1) && resource->override_stride==-1 &&
           !resource->initial_data && !resource->resource && (!resource->pool || !owned_contents.count(resource)))return value;
        value.kind=SlotValue::Kind::Resource;value.resource=resource;
        return value;
    }
    // What a source of an assignment gives (ResourceCopyTarget::GetResource): a declaration's value, a
    // slot's, null, or a resource the plan does not hold. `by` is the command, for a later notice.
    SlotValue source_value(ResourceCopyTarget& source,CommandListState& state,const CommandListCommand* by)
    {
        SlotValue value;value.by=by;
        ResourceCopyTarget* target=&source;
        if(source.type==ResourceCopyTargetType::THIS_RESOURCE){
            // The original: "this is not valid in this context", and no resource.
            if(!state.this_target || state.this_target->type==ResourceCopyTargetType::THIS_RESOURCE)return value;
            target=state.this_target;
        }
        switch(target->type){
        case ResourceCopyTargetType::EMPTY:return value;
        case ResourceCopyTargetType::CUSTOM_RESOURCE:{
            auto* declaration=target->GetCustomResource(&state);
            if(!declaration)return value;
            direct_resource(declaration);
            value=held(declaration);value.by=by;
            break;}
        default:{
            SlotId id;
            switch(named(*target,state,id)){
            case Named::Slot:value=context.get(id);value.by=by;break;
            case Named::Nothing:return value; // a number D3D11 has no slot for: nothing is there
            default:value.kind=SlotValue::Kind::Foreign;break; // IniParams, the cursor, the back buffer
            }}
        }
        // A view of a region of the resource (`ref vs-cb0->Region(a, b)`) is another view than the plan binds.
        if(source.evaluation_mode!=ResourceCopyTargetEvaluationMode::RESOURCE && !value.null()){value.kind=SlotValue::Kind::Foreign;value.owned.reset();}
        return value;
    }
    // A declaration takes a value (ResourceCopyTarget::SetResource for a custom resource).
    void store(CustomResource* destination,SlotValue value)
    {
        if(references.size()>=max_steps && !references.count(destination))refuse(L"Declaration reference budget exceeded");
        Held entry;
        entry.execution=value.kind==SlotValue::Kind::Application || value.kind==SlotValue::Kind::Owned?execution:0;
        const bool written=value.kind==SlotValue::Kind::Resource && owned_contents.count(value.resource);
        if(!written)owned_contents.erase(destination);
        destination->is_null=value.null();
        entry.value=std::move(value);
        references[destination]=std::move(entry);
    }
    // One slot of the state as a draw sees it, by the table: what the plan's items must leave bound there
    // (the slot's value where honoured, else the application's own binding). A degraded slot is said once
    // per command that set it.
    Honour settle(const DrawState::Entry& entry,const PipelineFacts& pipeline,SlotValue& wanted)
    {
        const SlotId id=DrawState::slot(entry.cell);
        wanted=SlotValue::application(id);
        const Honour outcome=honour(id,entry.value,pipeline);
        if(outcome==Honour::Honoured)wanted=entry.value;
        else if(outcome==Honour::Degraded && entry.value.by && unknown_identity_logged.size()<64 && unknown_identity_logged.insert(entry.value.by).second)
            LayerGap(L"The Vulkan layer cannot bind this at %ls for a replaced draw; the game's binding stays there: %ls",
                     slot_name(id).c_str(),entry.value.by->ini_line.c_str());
        return outcome;
    }
    // A draw is recorded: it sees the context as it stands (D-161). The items written here are the
    // bindings it needs beyond what the items before it left bound. False: the draw would not reach the
    // game's target and is left out, alone. `application`: the draw is the game's own, after the pre lists.
    bool issue(bool application)
    {
        const PipelineFacts pipeline=facts?facts():PipelineFacts{};
        auto& entries=context.entries();
        auto& wanted=scratch_wanted;wanted.resize(entries.size());
        for(size_t i=0;i<entries.size();++i){
            const auto& entry=entries[i];
            const Honour outcome=settle(entry,pipeline,wanted[i]);
            if(outcome==Honour::NotDrawable){
                if(entry.value.by && unknown_identity_logged.size()<64 && unknown_identity_logged.insert(entry.value.by).second)
                    LayerGap(L"The Vulkan layer draws into the game's targets only; a draw with %ls changed is left out: %ls",
                             slot_name(DrawState::slot(entry.cell)).c_str(),entry.value.by->ini_line.c_str());
                return false;
            }
            // The game's own draw has the game's buffers whatever the lists put there; a draw of the
            // Mod's cannot be made of them.
            if(outcome==Honour::GameDraw && !application)
                refuse_draw(L"The Mod's draw needs a buffer at "+slot_name(DrawState::slot(entry.cell))+L" that the Vulkan layer cannot bind"+
                            (entry.value.by?L": "+entry.value.by->ini_line:std::wstring()));
        }
        for(size_t i=0;i<entries.size();++i){
            auto& entry=entries[i];
            if(entry.issued.same(wanted[i]))continue;
            const SlotId id=DrawState::slot(entry.cell);
            const SlotValue& value=wanted[i];
            // The plan has items for the buffers of the input assembler and the pixel textures; a slot of
            // another kind is honoured only as it stands (the table), and there is nothing to bind.
            if(id.kind!=SlotKind::Vertex && id.kind!=SlotKind::Index && !(id.kind==SlotKind::ShaderResource && id.stage==kPixelStage)){
                entry.issued=value;continue;
            }
            PlanItem item;item.slot=id.slot;
            item.operation=id.kind==SlotKind::Vertex?PlanOperation::BindVertex:id.kind==SlotKind::Index?PlanOperation::BindIndex:PlanOperation::BindPixelResource;
            switch(value.kind){
            case SlotValue::Kind::Application:item.application_slot=int(value.source.slot);break;
            case SlotValue::Kind::Resource:item.resource=value.resource;break;
            case SlotValue::Kind::Owned:item.owned=value.owned;break;
            default:break;
            }
            append(std::move(item));
            entry.issued=value;
        }
        return true;
    }
    std::vector<SlotValue> scratch_wanted;
    // The pre lists ran: where the application's draw stays it sees the context as they left it. Its
    // bindings stand before `pre_items`; changed targets hide it, as it would not reach them (the table).
    void application_draw()
    {
        if(active->skip_original && !keep_original)return;
        if(!issue(true)){active->skip_original=true;keep_original=false;}
    }
    // A slot range's single slot operations, one per slot of the range, kept with the command (D-143).
    std::unordered_map<const SlotRangeCopyOperation*,std::vector<std::shared_ptr<ResourceCopyOperation>>> range_ops;
    std::vector<CustomResource*> range_resources;
    std::unordered_set<CustomResource*> files;
    std::unordered_map<CustomResource*, unsigned> file_roles; // referenced_roles
    std::function<bool()> owned_gate;
    // The owned GPU callbacks apply unless a gate says the host has no owned scope open.
    bool owned() const {return !owned_gate || owned_gate();}
    // A replaced draw's scope for owned GPU work (D-087), the CustomShader nesting within it, the
    // host's owned vertex buffers and the Mod resources whose content owned work wrote.
    std::function<bool()> draw_gate;
    DrawPlanAdapter::OwnedBinding owned_binding;
    size_t shader_depth=0;
    std::unordered_set<CustomResource*> owned_contents;
    bool drawing() const {return draw_gate && draw_gate();}
    // The owned callbacks apply: the host's owned scope, or within a CustomShader in a draw's scope.
    bool gpu() const {return owned() || (shader_depth && drawing());}
    // A copy executed by owned work leaves the Mod resource it wrote with owned content.
    void mark_owned(CommandListCommand& command,CommandListState& state)
    {
        auto* copy=dynamic_cast<ResourceCopyOperation*>(&command);
        if(!copy || typeid(*copy)!=typeid(ResourceCopyOperation))return;
        auto& dst=effective_target(copy->dst,state);
        if(dst.type!=ResourceCopyTargetType::CUSTOM_RESOURCE || dst.custom_resource_pool || dst.evaluation_mode!=ResourceCopyTargetEvaluationMode::RESOURCE)return;
        auto* destination=dst.GetCustomResource(&state,true);if(!destination)return;
        if(copies(copy->options)){
            if(references.size()>=max_steps && !references.count(destination))refuse(L"Declaration reference budget exceeded");
            Held own;own.value.kind=SlotValue::Kind::Resource;own.value.resource=destination;
            references[destination]=std::move(own);owned_contents.insert(destination);
        }
    }
    // Owned work in a draw's scope stays on the owned side (D-087).
    void owned_only(CommandListCommand& command,CommandListState& state)
    {
        auto check=[&](const ResourceCopyTarget& t){
            if(!owned_side(t))refuse(L"Owned work in a replaced draw reaches the application's resources: "+command.ini_line);
            // A declaration holding the application's binding of a slot is the application's (D-152).
            if(t.type==ResourceCopyTargetType::CUSTOM_RESOURCE && !t.custom_resource_pool &&
               t.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE){
                const auto kind=held(const_cast<ResourceCopyTarget&>(t).GetCustomResource(&state)).kind;
                if(kind==SlotValue::Kind::Application || kind==SlotValue::Kind::Foreign)
                    refuse(L"Owned work in a replaced draw reaches the application's resources: "+command.ini_line);
            }
        };
        if(auto* copy=dynamic_cast<ResourceCopyOperation*>(&command)){check(copy->src);check(copy->dst);}
        else if(auto* range=dynamic_cast<SlotRangeCopyOperation*>(&command)){check(range->src);check(range->dst);}
        else if(auto* clear=dynamic_cast<ClearViewCommand*>(&command))check(clear->target);
        else if(auto* store=dynamic_cast<StoreCommand*>(&command))check(store->src);
        else if(auto* batch=dynamic_cast<ShaderResourceBatch*>(&command)){
            for(const auto& operation:batch->operations)if(operation){check(operation->src);check(operation->dst);}
        }
    }

    void flush(CommandListState& state,bool force=false){
        if(!parameter_flush || !gpu() || (!force && !state.update_params))return;
        std::wstring why;
        if(!parameter_flush(state,force,why))refuse(why.empty()?L"GPU parameter publication refused":why);
    }

    void collect_files(CommandListCommand& command)
    {
        if (auto* copy = dynamic_cast<ResourceCopyOperation*>(&command)) {
            if (copy->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !copy->src.custom_resource_pool) {
                auto* resource = copy->src.GetCustomResource(nullptr);
                if (resource && !resource->filename.empty()) {
                    files.insert(resource);
                    const auto type = copy->dst.type;
                    file_roles[resource] |= type == ResourceCopyTargetType::VERTEX_BUFFER ? 1u : type == ResourceCopyTargetType::INDEX_BUFFER ? 2u :
                                            type == ResourceCopyTargetType::SHADER_RESOURCE ? 4u : 0u;
                }
            }
        } else if (auto* batch = dynamic_cast<ShaderResourceBindBatch*>(&command)) {
            for (const auto& copy : batch->operations) if (copy) collect_files(*copy);
        }
    }

    ResourceCopyTarget& effective_target(ResourceCopyTarget& target,CommandListState& state)
    {
        if(target.type!=ResourceCopyTargetType::THIS_RESOURCE)return target;
        if(!state.this_target || state.this_target->type==ResourceCopyTargetType::THIS_RESOURCE)
            refuse(L"Texture this target is missing or cyclic");
        return *state.this_target;
    }
    // `this` as the original resolves it, null where it is not valid (the original logs that and takes or
    // sets nothing).
    static ResourceCopyTarget* resolved(ResourceCopyTarget& target,CommandListState& state)
    {
        if(target.type!=ResourceCopyTargetType::THIS_RESOURCE)return &target;
        if(!state.this_target || state.this_target->type==ResourceCopyTargetType::THIS_RESOURCE)return nullptr;
        return state.this_target;
    }
    // `copy`, `copy_desc` and the MSAA resolve make a resource; everything else binds or holds the source's.
    static bool copies(ResourceCopyOptions options){return (int(options)&int(ResourceCopyOptions::COPY_MASK))!=0;}
    // A copy in a draw's scope that the owned side can make (D-087): its source is the Mod's. One whose
    // source is the application's binding (`ResourceX = copy ps-t0`) is the plan's: the declaration holds
    // that binding for this draw.
    bool owned_reach(ResourceCopyOperation& copy,CommandListState& state)
    {
        if(!owned_side(copy.src) || !owned_side(copy.dst))return false;
        if(copy.src.type==ResourceCopyTargetType::CUSTOM_RESOURCE && !copy.src.custom_resource_pool &&
           copy.src.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE){
            const auto kind=held(copy.src.GetCustomResource(&state)).kind;
            if(kind==SlotValue::Kind::Application || kind==SlotValue::Kind::Foreign)return false;
        }
        return true;
    }
    // `ResourceX = copy ...` where no owned work makes the copy: what X then holds. A copy of the game's
    // resource holds, for the lists of this draw, what the game bound (nothing writes that between the
    // line and the draw). A copy of a file of the Mod, which nothing writes, shows what the file shows,
    // where X takes the source's description as it is (a declaration with keys of its own makes another
    // resource). Any other copy, and every copy of a description alone, is not made: X is null, as the
    // original leaves it for a null source, and the notice says so once (D-155).
    SlotValue copied(ResourceCopyOperation& op,const CustomResource& destination,SlotValue value,const CommandListCommand* by)
    {
        const bool description=(int(op.options)&int(ResourceCopyOptions::COPY_DESC))!=0;
        const bool plain=destination.filename.empty() && destination.override_type==CustomResourceType::INVALID &&
                         destination.override_format==DXGI_FORMAT(-1) && destination.override_stride==-1 && !destination.initial_data;
        switch(value.kind){
        case SlotValue::Kind::Null:return value;
        case SlotValue::Kind::Application:if(!description)return value;break;
        case SlotValue::Kind::Resource:
            if(!description && plain && !value.resource->filename.empty() && !owned_contents.count(value.resource))return value;
            break;
        default:break;
        }
        if(by && unknown_identity_logged.size()<64 && unknown_identity_logged.insert(by).second)
            LayerGap(L"The Vulkan layer does not make this copy; the resource stays null: %ls",by->ini_line.c_str());
        SlotValue none;none.by=by;return none;
    }
    // A resource assignment (ResourceCopyOperation::run): a total operation on the draw state and the
    // declarations (D-161). Whatever the source and the destination, it sets what the original's
    // SetResource sets and refuses nothing; what a draw can make of it is decided where the draw is
    // recorded (issue()).
    void assign(ResourceCopyOperation& op,CommandListState& state,const CommandListCommand* by)
    {
        SlotValue value=source_value(op.src,state,by);
        // "Copy source was NULL": the destination is set to null, unless unless_null keeps it.
        if(value.null() && (int(op.options)&int(ResourceCopyOptions::UNLESS_NULL)))return;
        ResourceCopyTarget* dst=resolved(op.dst,state);
        if(!dst)return;
        if(dst->type==ResourceCopyTargetType::POOL){
            // CopyResourceToPool: every element of the pool takes it.
            auto* pool=dst->custom_resource_pool;
            if(!pool)return;
            validate_pool_runtime(*pool);
            for(size_t i=0;i<pool->GetPoolSize();++i){
                auto* element=pool->GetResource(float(i),false,true,true);
                if(!element)continue;
                direct_resource(element);
                store(element,copies(op.options)?copied(op,*element,value,by):value);
            }
            return;
        }
        if(dst->type==ResourceCopyTargetType::CUSTOM_RESOURCE){
            auto* destination=dst->GetCustomResource(&state,true);
            if(!destination)return;
            direct_resource(destination);
            if(copies(op.options))value=copied(op,*destination,std::move(value),by);
            store(destination,std::move(value));
            return;
        }
        SlotId id;
        // A number D3D11 has no slot for, IniParams, the back buffer, the CPU: the original sets nothing.
        if(named(*dst,state,id)!=Named::Slot)return;
        if(value.kind==SlotValue::Kind::Resource && owned_contents.count(value.resource)){
            // What owned work wrote: at a vertex slot its buffer as the owned side holds it now (D-087);
            // elsewhere, or without the draw's owned scope, content the plan cannot bind.
            PlanItem::OwnedBuffer buffer;std::wstring why;
            if(id.kind==SlotKind::Vertex && owned_binding && drawing() && owned_binding(op.src,state,buffer,why)){
                value.kind=SlotValue::Kind::Owned;value.owned=std::make_shared<const PlanItem::OwnedBuffer>(std::move(buffer));
            }else value.kind=SlotValue::Kind::Foreign;
        }
        context.set(id,std::move(value));
    }
    void texture_matches(ResourceCopyTarget& target,CommandListState& state,bool& bound,TextureOverrideMatches& matches)
    {
        auto& resolved=effective_target(target,state);
        if(resolved.evaluation_mode!=ResourceCopyTargetEvaluationMode::RESOURCE || resolved.IsRange())
            refuse(L"Resource matching requires an adapted single resource target");
        matches.clear();bound=false;
        if(resource_match && gpu()){
            SourceResourceMatch source;std::wstring why;
            if(!resource_match(resolved,state,source,why))refuse(why.empty()?L"Original resource identity is unknown":why);
            // Native THIS delegates resource resolution but keeps its own type
            // when deciding whether to apply IA region hashing.
            bound=source.bound;source_resource_matches(source,target.type,state,matches);
        }else{
            if(!texture_query)refuse(L"Texture query requires an adapted single pixel slot");
            if(resolved.type!=ResourceCopyTargetType::SHADER_RESOURCE || resolved.shader_type!=L'p'){
                other_matches(resolved,state,bound,matches);
                if(matches.size()>max_items)refuse(L"Texture match count exceeds planner budget");
                for(auto* match:matches)if(!match || !adapted.count(match->command_list.ResolveCommandList()) ||
                                           !adapted.count(match->post_command_list.ResolveCommandList()))
                    refuse(L"Matched texture command lists were not adapted");
                return;
            }
            // What the slot holds (D-161): nothing, a resource of the Mod, or the application's texture
            // of a pixel slot, which the host names by the slot the game bound it at.
            SlotId id;
            if(named(resolved,state,id)!=Named::Slot)return; // no such slot: nothing is bound there
            const SlotValue value=context.get(id);
            DrawPlanAdapter::TextureIdentity identity;
            std::wstring why;
            switch(value.kind){
            case SlotValue::Kind::Null:return;
            case SlotValue::Kind::Resource:case SlotValue::Kind::Owned:
                if(!texture_query(id.slot,PixelBinding{false,value.resource},state,identity,why))refuse(why.empty()?L"Texture source identity is unknown":why);
                break;
            case SlotValue::Kind::Application:
                if(value.source.kind==SlotKind::ShaderResource && value.source.stage==kPixelStage){
                    if(!texture_query(value.source.slot,PixelBinding{},state,identity,why))refuse(why.empty()?L"Texture source identity is unknown":why);
                    break;
                }
                // The application's binding of another kind of slot, or a resource the plan does not hold:
                // bound, and no identity the layer knows.
            default:bound=true;return;
            }
            bound=identity.bound;if(!bound)return;
            find_texture_override_for_hash(identity.hash,&matches,state.call_info);
            // The original appends the match_* sections the resource's description meets after the hash's
            // (find_texture_overrides_for_resource); one the host cannot describe meets none (D-138).
            if(!G->mFuzzyTextureOverrides.empty() && identity.described)described_resource_matches(identity.description,state,matches);
        }
        if(matches.size()>max_items)refuse(L"Texture match count exceeds planner budget");
        for(auto* match:matches)if(!match || !adapted.count(match->command_list.ResolveCommandList()) ||
                                   !adapted.count(match->post_command_list.ResolveCommandList()))
            refuse(L"Matched texture command lists were not adapted");
    }
    // CheckTextureOverride of a target other than a pixel slot in a replaced draw's plan (D-144), as the
    // original's find_texture_overrides_for_resource answers it: a Mod's own resource (a declaration, or one
    // the plan bound) has no handle info and meets nothing; an empty slot is not bound; the matched draw's own
    // index buffer meets the sections of the hash it was matched by, through the original's region path. The
    // game's other buffers and the other stages' resources have no DX11 identity the layer knows: they meet
    // nothing, and the original's warning says so once per command.
    void other_matches(ResourceCopyTarget& target,CommandListState& state,bool& bound,TextureOverrideMatches& matches)
    {
        bound=false;matches.clear();
        auto unknown=[&](){
            bound=true;
            if(unknown_identity_logged.size()<64 && unknown_identity_logged.insert(&target).second)
                LayerGap(L"CheckTextureOverride of the game's resource that the Vulkan layer cannot identify meets no section");
        };
        switch(target.type){
        case ResourceCopyTargetType::CUSTOM_RESOURCE:
            bound=target.GetCustomResource(&state)!=nullptr;return;
        case ResourceCopyTargetType::EMPTY:
            return;
        default:{
            SlotId id;
            const auto slot=named(target,state,id);
            if(slot==Named::Nothing)return; // no such slot: nothing is bound there
            if(slot==Named::Other){unknown();return;}
            const SlotValue value=context.get(id);
            switch(value.kind){
            case SlotValue::Kind::Null:return;
            // A Mod's own buffer has no handle info and meets nothing.
            case SlotValue::Kind::Resource:case SlotValue::Kind::Owned:bound=true;return;
            case SlotValue::Kind::Application:{
                uint32_t hash=0;
                // The matched draw's own index buffer, where it is still bound.
                if(id.kind==SlotKind::Index && value.source==id && draw_index_hash && draw_index_hash(hash)){
                    bound=true;region_hash_matches(hash,state,matches);return;
                }
                unknown();return;}
            default:unknown();return;
            }}
        }
    }
    // A declaration reference executed by the owned backend is also what later replaced draws
    // resolve through the plan, exactly as the plan path itself would record it.
    void mirror_reference(CommandListCommand& command,CommandListState& state)
    {
        auto* copy=dynamic_cast<ResourceCopyOperation*>(&command);
        if(!copy || typeid(*copy)!=typeid(ResourceCopyOperation) || copies(copy->options))return;
        auto& dst=effective_target(copy->dst,state);
        if(dst.type!=ResourceCopyTargetType::CUSTOM_RESOURCE || dst.custom_resource_pool || dst.evaluation_mode!=ResourceCopyTargetEvaluationMode::RESOURCE ||
           copy->src.evaluation_mode!=ResourceCopyTargetEvaluationMode::RESOURCE || copy->src.custom_resource_pool)return;
        Held mirrored;
        if(copy->src.type==ResourceCopyTargetType::CUSTOM_RESOURCE){
            mirrored.value=held(copy->src.GetCustomResource(&state));
            if(mirrored.value.kind==SlotValue::Kind::Application || mirrored.value.kind==SlotValue::Kind::Owned)mirrored.execution=execution;
        }else if(copy->src.type!=ResourceCopyTargetType::EMPTY)return;
        auto* destination=dst.GetCustomResource(&state,true);if(!destination)return;
        if(references.size()>=max_steps && !references.count(destination))refuse(L"Declaration reference budget exceeded");
        references[destination]=std::move(mirrored);
    }
    void check_texture(CheckTextureOverrideCommand& check,CommandListState& state)
    {
        auto& target=effective_target(check.target,state);
        // Any resource, slot range or pool range, as the original checks them; a target other than a pixel
        // slot is answered by other_matches (D-144).
        if(!(resource_match && gpu()) && target.evaluation_mode!=ResourceCopyTargetEvaluationMode::RESOURCE &&
           target.evaluation_mode!=ResourceCopyTargetEvaluationMode::SLOT_RANGE && target.evaluation_mode!=ResourceCopyTargetEvaluationMode::POOL_RANGE)
            refuse(L"Texture override needs a resource, slot range or pool range target");
        int first=0;unsigned count=1;
        if(target.IsRange()){
            if(target.custom_resource_pool)validate_pool_runtime(*target.custom_resource_pool);
            if(!target.ResolveRange(&state,&first,&count))return; // Native finite invalid range is a no-op.
            if(count>max_items)refuse(L"Checked resource range exceeds transaction budget");
        }
        auto* saved_this=state.this_target;const auto saved_post=state.post;
        struct Restore {CommandListState& state;ResourceCopyTarget* target;bool post;~Restore(){state.this_target=target;state.post=post;}} restore{state,saved_this,saved_post};
        for(unsigned i=0;i<count;++i){
            if(++steps>max_steps)refuse(L"Checked resource range exceeds execution budget");
            ResourceCopyTarget local;ResourceCopyTarget& checked=target.IsRange()?local:target;
            if(target.evaluation_mode==ResourceCopyTargetEvaluationMode::POOL_RANGE){
                const int64_t index=int64_t(first)+i;
                if(!target.custom_resource_pool || index<INT_MIN || index>INT_MAX)refuse(L"Checked pool index exceeds signed bounds");
                local.type=ResourceCopyTargetType::CUSTOM_RESOURCE;
                local.SetCustomResource(target.custom_resource_pool->GetResource(float(index),false,true,false));
            }else if(target.IsRange()){
                local.type=target.type;local.shader_type=target.shader_type;local.slot=unsigned(first)+i;local.max_slot=target.max_slot;
            }
            state.this_target=&checked;bool bound=false;TextureOverrideMatches matches;texture_matches(checked,state,bound,matches);
            auto run=[&](bool post){state.post=post;for(auto* match:matches){
                auto& list=post?match->post_command_list:match->command_list;
                if(!adapted.count(list.ResolveCommandList()))refuse(L"Texture callback alias target was not adapted");
                FrontendRunCommandList(list,state);
            }};
            if(check.run_pre_and_post_together){run(false);run(true);}else run(saved_post);
        }
    }

    // A declaration operand in a replaced draw's plan (D-112), what it holds as the plan's references
    // say: -0 as a resource and 0 as an identity when it holds nothing, as the original. Holding a
    // resource of the generation it is 0 as a resource (the original keeps no hash for what it made
    // itself, so no TextureOverride matches it) and the holder's identity.
    void declared(ResourceCopyTarget& target,CommandListState& state,float& result)
    {
        auto* declaration=target.GetCustomResource(&state);direct_resource(declaration);
        const SlotValue value=held(declaration);
        const bool identity=target.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY;
        switch(value.kind){
        case SlotValue::Kind::Null:result=identity?0.0f:-0.0f;return;
        case SlotValue::Kind::Resource:case SlotValue::Kind::Owned:
            result=identity?EncodeFloat30(uint32_t(HashPointer(value.resource))):0.0f;return;
        case SlotValue::Kind::Application:
            // The application's binding of a slot, taken in this draw (D-161): the slot's identity; as a
            // filter value what the matcher says of the game's texture where it is a pixel slot's, else
            // bound and named by no section.
            if(identity){result=NativeResourceQuery::slot_identity(value.source);return;}
            result=0.0f;
            if(value.source.kind==SlotKind::ShaderResource && value.source.stage==kPixelStage && texture_query && !(resource_match && gpu())){
                DrawPlanAdapter::TextureIdentity texture;std::wstring why;
                if(!texture_query(value.source.slot,PixelBinding{},state,texture,why))refuse(why.empty()?L"Texture source identity is unknown":why);
                if(!texture.bound){result=-0.0f;return;}
                TextureOverrideMatches matches;
                find_texture_override_for_hash(texture.hash,&matches,state.call_info);
                if(!G->mFuzzyTextureOverrides.empty() && texture.described)described_resource_matches(texture.description,state,matches);
                result=filter_value(matches);
            }
            return;
        default:
            // A resource the plan does not hold: bound, named by no section, and itself.
            result=identity?EncodeFloat30(uint32_t(HashPointer(declaration))|1u):0.0f;return;
        }
    }
    // process_texture_filter for a bound resource: the filter_index of the last matching section that
    // has one, 1 for a match without, 0 for none.
    static float filter_value(const TextureOverrideMatches& matches)
    {
        if(matches.empty())return 0.0f;
        for(auto it=matches.rbegin();it!=matches.rend();++it)if((*it)->filter_index!=FLT_MAX)return (*it)->filter_index;
        return 1.0f;
    }
    // The Mod's resource a declaration holds, null where it holds none (nothing, or a binding of the
    // application's).
    CustomResource* referenced(CustomResource* resource) const
    {
        const SlotValue value=held(resource);
        return value.kind==SlotValue::Kind::Resource?value.resource:nullptr;
    }
    void direct_resource(const CustomResource* resource)
    {
        if(!resource)refuse(L"Missing resource declaration");
        if(resource->pool){
            const auto* pool=resource->pool->ResolvePool();
            if((pool->index_type!=PoolIndexType::RING && pool->index_type!=PoolIndexType::STATIC) ||
               pool->expiration_timeout_frames!=UINT32_MAX)
                refuse(L"Resource references require a nonexpiring direct-index pool");
        }
    }

    // A planned indexed draw takes the input layout edits and the native draw data of the moment.
    // A slot range (D-143) as the original runs one: a single slot operation per slot of the range (its
    // SlotOp), each through the plan as `ps-tN = ...` or `Pool[i] = ref ps-tN` would go. unless_null keeps a
    // bound slot whose source is none; a fetch takes the slot's binding as the plan holds it.
    void slot_range(SlotRangeCopyOperation& range,CommandListState& state)
    {
        bool bind=false;unsigned first=0;
        auto& resources=range_resources;
        if(!FrontendSlotRange(range,state,bind,first,resources))return;
        auto& ops=range_ops[&range];
        const auto& slots=bind?range.dst:range.src;
        for(unsigned i=0;i<resources.size();++i){
            if(bind && !resources[i] && (range.options&ResourceCopyOptions::UNLESS_NULL))continue;
            if(ops.size()<=i)ops.push_back(std::make_shared<ResourceCopyOperation>());
            auto& op=*ops[i];
            ResourceCopyTarget& slot=bind?op.dst:op.src;
            ResourceCopyTarget& resource=bind?op.src:op.dst;
            slot=ResourceCopyTarget();slot.type=slots.type;slot.shader_type=slots.shader_type;slot.max_slot=slots.max_slot;slot.slot=first+i;
            resource=ResourceCopyTarget();
            if(resources[i]){resource.type=ResourceCopyTargetType::CUSTOM_RESOURCE;resource.SetCustomResource(resources[i]);}
            else resource.type=ResourceCopyTargetType::EMPTY;
            op.options=ResourceCopyOptions(int(range.options)&~int(ResourceCopyOptions::UNLESS_NULL));op.ini_line=range.ini_line;
            run(std::shared_ptr<CommandListCommand>(ops[i]),&state);
        }
    }
    void indexed(PlanItem& item,CommandListState& state)
    {
        if (++draws > max_draws) refuse(L"Plan draw budget exceeded");
        for (const auto* layout : state.input_layout_overrides) {
            if (!layout || item.layout.size() >= 256) refuse(L"Invalid layout operation list");
            item.layout.push_back(*layout);
        }
        if(palette_source){
            // The draw's bone data is the renderer's: without it the Mod's mesh cannot be posed.
            std::wstring reason;
            if(!palette_source(state,item.palette,reason))refuse_draw(reason.empty()?L"Native draw palette unavailable":reason);
        }
        // The draw sees the context as the lists left it (D-161); one that would not reach the game's
        // target is left out, alone.
        if(!issue(false))return;
        append(std::move(item));
    }
    void append(PlanItem item)
    {
        if (!active) refuse(L"Planner command invoked outside a transaction");
        if (active->items.size() >= max_items) refuse(L"Plan operation budget exceeded");
        active->items.push_back(std::move(item));
    }

    // What a command is, as the dynamic_cast chain of run() decided it before; one word per command.
    enum Kind : uint32_t {
        K_AbortCommand = 1u << 0,
        K_CheckTextureOverrideCommand = 1u << 1,
        K_ClearViewCommand = 1u << 2,
        K_CopyCommandListCommand = 1u << 3,
        K_Draw3DMigotoOverlayCommand = 1u << 4,
        K_DrawCommand = 1u << 5,
        K_IfCommand = 1u << 6,
        K_LayoutElementOperation = 1u << 7,
        K_ParamOverride = 1u << 8,
        K_PoolCopyOperation = 1u << 9,
        K_PoolVariableOperation = 1u << 10,
        K_PresetCommand = 1u << 11,
        K_ResetPerFrameLimitsCommand = 1u << 12,
        K_ResourceCopyOperation = 1u << 13,
        K_RunCustomShaderCommand = 1u << 14,
        K_RunExplicitCommandList = 1u << 15,
        K_RunLinkedCommandList = 1u << 16,
        K_ShaderResourceBatch = 1u << 17,
        K_SkipCommand = 1u << 18,
        K_SlotRangeCopyOperation = 1u << 19,
        K_StoreCommand = 1u << 20,
        K_UpscalingFlipBBCommand = 1u << 21,
        K_VariableAssignmentExact = 1u << 22,
        K_ResourceCopyOperationExact = 1u << 23
    };
    static uint32_t classify(CommandListCommand& c)
    {
        uint32_t k = 0;
        if (dynamic_cast<AbortCommand*>(&c)) k |= K_AbortCommand;
        if (dynamic_cast<CheckTextureOverrideCommand*>(&c)) k |= K_CheckTextureOverrideCommand;
        if (dynamic_cast<ClearViewCommand*>(&c)) k |= K_ClearViewCommand;
        if (dynamic_cast<CopyCommandListCommand*>(&c)) k |= K_CopyCommandListCommand;
        if (dynamic_cast<Draw3DMigotoOverlayCommand*>(&c)) k |= K_Draw3DMigotoOverlayCommand;
        if (dynamic_cast<DrawCommand*>(&c)) k |= K_DrawCommand;
        if (dynamic_cast<IfCommand*>(&c)) k |= K_IfCommand;
        if (dynamic_cast<LayoutElementOperation*>(&c)) k |= K_LayoutElementOperation;
        if (dynamic_cast<ParamOverride*>(&c)) k |= K_ParamOverride;
        if (dynamic_cast<PoolCopyOperation*>(&c)) k |= K_PoolCopyOperation;
        if (dynamic_cast<PoolVariableOperation*>(&c)) k |= K_PoolVariableOperation;
        if (dynamic_cast<PresetCommand*>(&c)) k |= K_PresetCommand;
        if (dynamic_cast<ResetPerFrameLimitsCommand*>(&c)) k |= K_ResetPerFrameLimitsCommand;
        if (dynamic_cast<ResourceCopyOperation*>(&c)) k |= K_ResourceCopyOperation;
        if (dynamic_cast<RunCustomShaderCommand*>(&c)) k |= K_RunCustomShaderCommand;
        if (dynamic_cast<RunExplicitCommandList*>(&c)) k |= K_RunExplicitCommandList;
        if (dynamic_cast<RunLinkedCommandList*>(&c)) k |= K_RunLinkedCommandList;
        if (dynamic_cast<ShaderResourceBatch*>(&c)) k |= K_ShaderResourceBatch;
        if (dynamic_cast<SkipCommand*>(&c)) k |= K_SkipCommand;
        if (dynamic_cast<SlotRangeCopyOperation*>(&c)) k |= K_SlotRangeCopyOperation;
        if (dynamic_cast<StoreCommand*>(&c)) k |= K_StoreCommand;
        if (dynamic_cast<UpscalingFlipBBCommand*>(&c)) k |= K_UpscalingFlipBBCommand;
        if (typeid(c) == typeid(VariableAssignment)) k |= K_VariableAssignmentExact;
        if (typeid(c) == typeid(ResourceCopyOperation)) k |= K_ResourceCopyOperationExact;
        return k;
    }
    template <class X> static X* as(const std::shared_ptr<CommandListCommand>& o, uint32_t kinds, uint32_t bit)
    {
        return (kinds & bit) ? static_cast<X*>(o.get()) : nullptr;
    }
    void run(const std::shared_ptr<CommandListCommand>& original, CommandListState* state, std::pair<uint64_t, uint64_t>* timing_slot = nullptr,
             uint32_t kinds = 0)
    {
        // The command's kinds, decided once when the bridge command was made (a dynamic_cast chain per
        // command, a hundred commands per matched draw, cost a third of the plan).
        if (!kinds) kinds = classify(*original);
        // Command timing by class (D-099) is compiled in with EFMI_COMMAND_TIMING: the measurement of
        // the 1.6.8 session (about a hundred commands per draw) cost as much as the commands.
#ifdef EFMI_COMMAND_TIMING
        // Exclusive time per class: what a command's children (an if's branches, a called list) took
        // goes to their own classes, not to the parent's.
        // The slot is the bridge command's, found once at adaptation: the timer itself costs two clock reads.
        struct Timer {
            std::pair<uint64_t, uint64_t>& slot; std::chrono::steady_clock::time_point began; uint64_t children_before;
            Timer(std::pair<uint64_t, uint64_t>& s) : slot(s), began(std::chrono::steady_clock::now()), children_before(g_command_children_ns) { g_command_children_ns = 0; }
            ~Timer() {
                const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
                const uint64_t own = elapsed > g_command_children_ns ? elapsed - g_command_children_ns : 0;
                ++slot.first; slot.second += own / 1000;
                g_command_children_ns = children_before + elapsed;
            }
        } timer{timing_slot ? *timing_slot : command_timing[typeid(*original).name()]};
#endif
        if (!active) refuse(L"Planner command invoked outside a transaction");
        // The original stops a list at this depth with a warning (_RunCommandList), and its caller goes on.
        if (state->recursion >= MAX_COMMAND_LIST_RECURSION)
            refuse(L"Plan recursion budget exceeded");
        // The planner's own bounds, which the original has not: past them the draw is not the Mod's.
        if (++steps > max_steps) refuse_draw(L"Plan execution budget exceeded");
        if(active_depth>=max_active_depth)refuse_draw(L"Native command stack budget exceeded");
        ++active_depth;struct Depth {size_t& value;~Depth(){--value;}} depth{active_depth};
        if(auto* check=as<CheckTextureOverrideCommand>(original, kinds, K_CheckTextureOverrideCommand)){
            check_texture(*check,*state);return;
        }
        if(auto* shader=as<RunCustomShaderCommand>(original, kinds, K_RunCustomShaderCommand)){
            std::wstring why;
            if(!shader_command){
                // No owned GPU runtime in this generation (its tools not deployed, switched off, or the offline
                // dry run): as a CustomShader the original could not compile, whose runs do nothing.
                if(unknown_identity_logged.size()<64 && unknown_identity_logged.insert(original.get()).second)
                    LayerGap(L"A CustomShader does nothing without the owned GPU runtime: %ls",original->ini_line.c_str());
                return;
            }
            if(shader_command && !owned() && !drawing())refuse(L"CustomShader needs the host's owned GPU scope, which replaced draws do not open");
            // In a draw's scope everything the shader's lists run is owned work (D-087).
            const bool nested=!owned() && drawing();
            struct Nesting {size_t& depth;bool on;~Nesting(){if(on)--depth;}} nesting{shader_depth,nested};
            if(nested)++shader_depth;
            if(!shader_command || !shader_command(*shader,*state,why) || nested_failed)
                refuse(why.empty()?L"CustomShader requires a shader execution adapter":why);
            return;
        }
        if(as<DrawCommand>(original, kinds, K_DrawCommand)){
            if(state->call_info && state->call_info->hunting_skip)return;
            if(draw_prepare && gpu()){std::wstring why;if(!draw_prepare(*state,why))refuse(why.empty()?L"Source draw preparation refused":why);}
            // Original DrawCommand flushes even for a zero-count draw, before
            // evaluating arguments or entering the draw/dispatch implementation.
            flush(*state);
        }
        // A replaced draw's scope, outside CustomShaders: a copy into a Mod resource, or a reference to
        // one with owned content, is owned work; the draw's own bindings and draws keep the plan (D-087).
        if(resource_command && drawing() && !shader_depth && !owned()){
            // store and clear of the Mod's own resources are owned work as its copies are (D-147).
            if(as<StoreCommand>(original, kinds, K_StoreCommand) || as<ClearViewCommand>(original, kinds, K_ClearViewCommand)){
                owned_only(*original,*state);
                std::wstring why;
                if(!resource_command(*original,*state,why))refuse(why.empty()?L"GPU resource command refused":why);
                return;
            }
            if(auto* copy=as<ResourceCopyOperation>(original, kinds, K_ResourceCopyOperation); copy && (kinds & K_ResourceCopyOperationExact)){
                auto* dst=resolved(copy->dst,*state);
                bool owned_source=false;
                if(copy->src.type==ResourceCopyTargetType::CUSTOM_RESOURCE && !copy->src.custom_resource_pool &&
                   copy->src.evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE)
                    owned_source=owned_contents.count(referenced(copy->src.GetCustomResource(state)))!=0;
                if(dst && dst->type==ResourceCopyTargetType::CUSTOM_RESOURCE && !dst->custom_resource_pool &&
                   dst->evaluation_mode==ResourceCopyTargetEvaluationMode::RESOURCE &&
                   (copies(copy->options) || owned_source) && owned_reach(*copy,*state)){
                    std::wstring why;
                    if(!resource_command(*original,*state,why))refuse(why.empty()?L"GPU resource command refused":why);
                    mirror_reference(*original,*state);mark_owned(*original,*state);
                    return;
                }
            }
        }
        if(resource_command && gpu() && (as<ResourceCopyOperation>(original, kinds, K_ResourceCopyOperation) ||
           as<ClearViewCommand>(original, kinds, K_ClearViewCommand) || as<StoreCommand>(original, kinds, K_StoreCommand) ||
           as<DrawCommand>(original, kinds, K_DrawCommand) || as<SlotRangeCopyOperation>(original, kinds, K_SlotRangeCopyOperation))) {
            std::wstring why;
            if(!owned())owned_only(*original,*state);
            if(owned_gate || draw_gate){mirror_reference(*original,*state);mark_owned(*original,*state);}
            if(!resource_command(*original,*state,why))refuse(why.empty()?L"GPU resource command refused":why);
            return;
        }
        if (auto* range = as<SlotRangeCopyOperation>(original, kinds, K_SlotRangeCopyOperation)) {
            slot_range(*range, *state);
            return;
        }
        if (auto* draw = as<DrawCommand>(original, kinds, K_DrawCommand)) {
            if (state->call_info && state->call_info->hunting_skip) return;
            PlanItem item; item.operation = PlanOperation::DrawIndexed;
            const auto* info = state->call_info;
            // DrawCommand::run, case by case (D-142). A Draw's vertex count and first vertex take the
            // indexed fields with `indexed` false.
            switch (draw->type) {
            case DrawCommandType::DRAW_INDEXED:
                item.index_count = positive(draw->args[0], state);
                item.first_index = positive(draw->args[1], state);
                item.vertex_offset = integer(draw->args[2], state);
                break;
            case DrawCommandType::DRAW_INDEXED_INSTANCED:
                item.index_count = positive(draw->args[0], state);
                item.instance_count = positive(draw->args[1], state);
                item.first_index = positive(draw->args[2], state);
                item.vertex_offset = integer(draw->args[3], state);
                item.first_instance = positive(draw->args[4], state);
                break;
            case DrawCommandType::DRAW:
                item.indexed = false;
                item.index_count = positive(draw->args[0], state);
                item.first_index = positive(draw->args[1], state);
                break;
            case DrawCommandType::DRAW_INSTANCED:
                item.indexed = false;
                item.index_count = positive(draw->args[0], state);
                item.instance_count = positive(draw->args[1], state);
                item.first_index = positive(draw->args[2], state);
                item.first_instance = positive(draw->args[3], state);
                break;
            case DrawCommandType::FROM_CALLER:
                // The matched draw's own call again (no active draw: nothing, as the original logs).
                if (!info) return;
                if (info->type == DrawCall::DrawIndexedInstanced || info->type == DrawCall::DrawIndexed) {
                    item.index_count = info->IndexCount; item.first_index = info->FirstIndex; item.vertex_offset = INT(info->FirstVertex);
                    if (info->type == DrawCall::DrawIndexedInstanced) { item.instance_count = info->InstanceCount; item.first_instance = info->FirstInstance; }
                } else if (info->type == DrawCall::DrawInstanced || info->type == DrawCall::Draw) {
                    item.indexed = false; item.index_count = info->VertexCount; item.first_index = info->FirstVertex;
                    if (info->type == DrawCall::DrawInstanced) { item.instance_count = info->InstanceCount; item.first_instance = info->FirstInstance; }
                } else refuse(L"The matched draw's call is not a direct draw: " + original->ini_line);
                break;
            case DrawCommandType::AUTO_VERTEX_COUNT:
                item.indexed = false; item.automatic = true;
                break;
            case DrawCommandType::AUTO_INDEX_COUNT:
                item.automatic = true;
                break;
            case DrawCommandType::AUTO_INDEX_INSTANCE_COUNT:
                if (!info) return;
                item.automatic = true; item.instance_count = info->InstanceCount; item.first_instance = info->FirstInstance;
                break;
            default:
                // dispatch (the game's last compute shader, which a render pass cannot run), drawauto (stream
                // output) and the indirect draws (arguments in a GPU buffer): the layer does not issue them in a
                // replaced draw. The command does nothing, the original's warning says so once, and the draw's
                // other commands go on (D-146).
                if (unknown_identity_logged.size() < 64 && unknown_identity_logged.insert(draw).second)
                    LayerGap(L"The Vulkan layer does not issue this command in a replaced draw: %ls", original->ini_line.c_str());
                return;
            }
            if ((!item.index_count && !item.automatic) || !item.instance_count) return;
            indexed(item,*state);
            return;
        }
        if (auto* binding = as<ResourceCopyOperation>(original, kinds, K_ResourceCopyOperation)) {
            assign(*binding,*state,original.get());
            return;
        }
        if (auto* batch = as<ShaderResourceBatch>(original, kinds, K_ShaderResourceBatch)) {
            // Upstream batches adjacent slot operations after parsing; each is the single operation it was,
            // on its own: one that cannot act does nothing and the others of the batch go on.
            for (auto& operation : batch->operations) {
                if (!operation) continue;
                try {
                    if (!cpu_arguments(*operation)) refuse(L"Unadapted batched binding query");
                    run(operation, state);
                } catch (const Refused& refused) { contained(operation.get(), operation->ini_line, refused); }
            }
            return;
        }
        if (auto* call = as<RunExplicitCommandList>(original, kinds, K_RunExplicitCommandList)) {
            auto found = externals.find(call->command_list_section);
            if (found != externals.end()) {
                PlanItem item; item.operation = PlanOperation::External;
                item.phase_mask = call->run_pre_and_post_together ? 3 : state->post ? 2 : 1;
                if(found->second.command){
                    std::wstring reason;
                    struct Calling {const std::wstring*& slot;const std::wstring* before;~Calling(){slot=before;}} calling{calling_line,calling_line};
                    calling_line=&original->ini_line;
                    // A native command stands for the Core's list and the renderer's contract with it: where
                    // it cannot act the layer cannot draw this draw as the Mod's (D-162).
                    if(!found->second.command(*state,item.phase_mask,reason) || nested_failed)
                        refuse_draw(reason.empty()?L"Native command refused: "+found->second.name:reason);
                    return;
                }
                item.external = found->second.name; // copied only for an item that is kept (D-108)
                append(std::move(item));
                return;
            }
            if (!call->command_list_section ||
                !adapted.count(call->command_list_section->command_list.ResolveCommandList()) ||
                !adapted.count(call->command_list_section->post_command_list.ResolveCommandList()))
                refuse(L"Call target was not adapted: " + original->ini_line);
            original->run(state);
            auto observer=observers.find(call->command_list_section);
            if(observer!=observers.end()){
                std::wstring reason;const unsigned phases=call->run_pre_and_post_together?3:state->post?2:1;
                if(!observer->second(*state,phases,reason) || nested_failed)
                    refuse_draw(reason.empty()?L"Original command observer refused":reason);
            }
            return;
        }
        if (as<SkipCommand>(original, kinds, K_SkipCommand)) {
            // Do not mutate the live caller before GPU readiness. After the application's draw the flag
            // comes too late, as in the original (the context reads call_info.skip between its pre and
            // post lists only).
            if (!application_drawn) active->skip_original = true;
            return;
        }
        if (auto* param = as<ParamOverride>(original, kinds, K_ParamOverride)) {
            if (!G || param->param_idx < 0 || size_t(param->param_idx) >= G->iniParams.size())
                refuse(L"INI parameter assignment exceeds generation storage");
            original->run(state); return;
        }
        if(auto* call=as<RunLinkedCommandList>(original, kinds, K_RunLinkedCommandList)){
            if(!call->link || !adapted.count(call->link->ResolveCommandList()))refuse(L"Linked command target was not adapted");
            original->run(state);return;
        }
        if (as<IfCommand>(original, kinds, K_IfCommand) || (kinds & K_VariableAssignmentExact) ||
            as<PoolVariableOperation>(original, kinds, K_PoolVariableOperation) || as<AbortCommand>(original, kinds, K_AbortCommand) ||
            as<PresetCommand>(original, kinds, K_PresetCommand) || as<ResetPerFrameLimitsCommand>(original, kinds, K_ResetPerFrameLimitsCommand) ||
            as<UpscalingFlipBBCommand>(original, kinds, K_UpscalingFlipBBCommand)) {
            // The upscaling flip only clears this generation's bb_is_upscaling_bb flag.
            original->run(state);
            return;
        }
        // The original draws the 3DMigoto overlay only when the swap chain has one, and a
        // hosted generation has none; it must not reach the reference swap chain scaffolding.
        if (as<Draw3DMigotoOverlayCommand>(original, kinds, K_Draw3DMigotoOverlayCommand)) return;
        if(auto* pool=as<PoolCopyOperation>(original, kinds, K_PoolCopyOperation)){
            if(pool->dst.type!=ResourceCopyTargetType::POOL || !pool->dst.custom_resource_pool ||
               (pool->src.type!=ResourceCopyTargetType::POOL && pool->src.type!=ResourceCopyTargetType::EMPTY) ||
               (pool->src.type==ResourceCopyTargetType::POOL && !pool->src.custom_resource_pool) ||
               !cpu_target(pool->dst) || !cpu_target(pool->src))refuse(L"Unsupported pool operation: "+original->ini_line);
            auto* destination=pool->dst.custom_resource_pool;auto* source=pool->src.custom_resource_pool;
            const bool reference=source && !(pool->options & ResourceCopyOptions::COPY_MASK);
            if(reference && destination==source->ResolvePool())refuse(L"Cyclic pool reference");
            // Original operations own metadata copies, proxy detach and the
            // independent resource/variable/full resets. They do no GPU work.
            original->run(state);
            if(reference && destination->ResolvePool()!=source->ResolvePool())refuse(L"Pool reference did not resolve");
            return;
        }
        if(auto* copy=as<CopyCommandListCommand>(original, kinds, K_CopyCommandListCommand)){
            if(!copy->dst || (copy->src &&
               (!adapted.count(copy->src->command_list.ResolveCommandList()) || !adapted.count(copy->src->post_command_list.ResolveCommandList()))))
                refuse(L"Callback source was not adapted");
            if(copy->src && (&copy->dst->command_list==copy->src->command_list.ResolveCommandList() ||
                            &copy->dst->post_command_list==copy->src->post_command_list.ResolveCommandList()))refuse(L"Cyclic callback reference");
            original->run(state);return;
        }
        if (auto* layout = as<LayoutElementOperation>(original, kinds, K_LayoutElementOperation)) {
            if (state->input_layout_overrides.size() >= 256) refuse(L"Layout operation budget exceeded");
            layout->run(state);
            return;
        }
        // Never forward arbitrary commands to the reference's D3D scaffolding.
        // Without the owned GPU runtime (offline) a store keeps its variable, as the original's for a null
        // source, and a clear does nothing; the warning says so once (D-147).
        if(as<StoreCommand>(original, kinds, K_StoreCommand) || as<ClearViewCommand>(original, kinds, K_ClearViewCommand)){
            if(unknown_identity_logged.size()<64 && unknown_identity_logged.insert(original.get()).second)
                LayerGap(L"GPU work of a replaced draw without the owned GPU runtime does nothing: %ls",original->ini_line.c_str());
            return;
        }
        refuse(L"Unsupported frontend operation: " + original->ini_line);
    }
};

namespace {
class BridgeCommand final : public CommandListCommand {
public:
    std::shared_ptr<CommandListCommand> original;
    std::shared_ptr<DrawPlanAdapter::Runtime> runtime;
    bool arguments_supported;
    std::pair<uint64_t, uint64_t>* timing_slot = nullptr;
    uint32_t kinds = 0;
    BridgeCommand(std::shared_ptr<CommandListCommand> command,
                  std::shared_ptr<DrawPlanAdapter::Runtime> owner)
        : original(std::move(command)), runtime(std::move(owner)) {
        guard_arguments(*original,runtime->queries);arguments_supported=cpu_arguments(*original);ini_line=original->ini_line;
        kinds = DrawPlanAdapter::Runtime::classify(*original);
#ifdef EFMI_COMMAND_TIMING
        timing_slot = &runtime->command_timing[typeid(*original).name()];
#endif
    }
    // A command that asked a declaration the SPIR-V leaves between two answers (D-159). An `if` goes on
    // when its condition is the same with every combination of them, as it then is under the original
    // whichever the DX11 build declares; anything else that asked has a value that depends on it.
    void checked(CommandListState* state) {
        auto& q = *runtime->queries;
        q.words.clear(); q.words_second = 0; q.words_over = false;
        if (kinds & DrawPlanAdapter::Runtime::K_IfCommand) {
            auto& condition = static_cast<IfCommand*>(original.get())->expression;
            const bool first = condition.evaluate(state) != 0.0f;
            if (!q.words.empty() || q.words_over) {
                bool same = !q.words_over;
                const uint32_t combinations = 1u << q.words.size();
                for (uint32_t second = 1; same && second < combinations; ++second) {
                    q.words_second = second;
                    same = (condition.evaluate(state) != 0.0f) == first && !q.words_over;
                }
                q.words.clear(); q.words_second = 0; q.words_over = false;
                if (!same) refuse(L"The condition depends on whether a buffer of 32-bit words is structured or raw, which its SPIR-V does not tell: " + ini_line);
            }
            runtime->run(original, state, timing_slot, kinds);
            q.words.clear(); q.words_over = false;
            return;
        }
        runtime->run(original, state, timing_slot, kinds);
        if (!q.words.empty() || q.words_over) {
            q.words.clear(); q.words_over = false;
            refuse(L"A value depends on whether a buffer of 32-bit words is structured or raw, which its SPIR-V does not tell: " + ini_line);
        }
    }
    void run(CommandListState* state) override {
        auto& q = *runtime->queries;
        struct Line { const std::wstring*& slot; const std::wstring* before; ~Line() { slot = before; } } line{q.line, q.line};
        q.line = &ini_line;
        if (runtime->strict) {
            // An intercepted shader call is one transaction of the owned domain: it stands or falls whole.
            if (!arguments_supported) refuse(L"Unadapted expression query: " + ini_line);
            checked(state);
            return;
        }
        // Each command on its own, in a called list as in the root, in a replaced draw's lists as in the
        // Present's (D-155, D-162): what this one cannot do ends here, and the commands after it in its list
        // run. Before, in a replaced draw, it ended every list of the draw and the game's mesh stayed.
        try {
            if (!arguments_supported) refuse(L"Unadapted expression query: " + ini_line);
            checked(state);
        }
        catch (const Refused& refused) { runtime->contained(original.get(), ini_line, refused); }
        catch (const std::exception& e) { runtime->contained(original.get(), ini_line, Refused{widen(e.what())}); }
    }
};
class BridgeLinkedCommand final : public RunLinkedCommandList {
public:
    std::shared_ptr<RunLinkedCommandList> original;
    std::shared_ptr<DrawPlanAdapter::Runtime> runtime;
    BridgeLinkedCommand(std::shared_ptr<RunLinkedCommandList> command,std::shared_ptr<DrawPlanAdapter::Runtime> owner)
        :RunLinkedCommandList(command->link),original(std::move(command)),runtime(std::move(owner)){ini_line=original->ini_line;}
    void run(CommandListState* state) override {
        if(runtime->strict){runtime->run(original,state);return;}
        try{runtime->run(original,state);}
        catch(const Refused& refused){runtime->contained(original.get(),ini_line,refused);}
        catch(const std::exception& e){runtime->contained(original.get(),ini_line,Refused{widen(e.what())});}
    }
};
}

DrawPlanAdapter::DrawPlanAdapter() : runtime_(std::make_shared<Runtime>())
{
    std::weak_ptr<Runtime> weak=runtime_;
    runtime_->queries->held=[weak](ResourceCopyTarget& target,CommandListState& state,SlotValue& value,SlotId& slot){
        auto owner=weak.lock();
        if(!owner || !owner->active)return false;
        auto* named=Runtime::resolved(target,state);
        if(!named || named->custom_resource_pool || owner->named(*named,state,slot)!=Runtime::Named::Slot)return false;
        value=owner->context.get(slot);return true;
    };
    runtime_->queries->declared=[weak](ResourceCopyTarget& target,CommandListState& state,float& result,std::wstring& reason){
        auto owner=weak.lock();if(!owner || !owner->active){reason=L"Declaration query outside an active plan";return false;}
        owner->declared(target,state,result);return true;
    };
}
DrawPlanAdapter::~DrawPlanAdapter() = default;

void DrawPlanAdapter::external(ExplicitCommandListSection& section, const std::wstring& name)
{
    if(runtime_->active || !runtime_->adapted.empty())refuse(L"Register native boundaries before adaptation");
    runtime_->externals.emplace(&section, Runtime::External{name,{}});
    section.command_list.runtime_populated = section.post_command_list.runtime_populated = true;
}
void DrawPlanAdapter::native(ExplicitCommandListSection& section,const std::wstring& name,NativeCommand command)
{
    if(!command)refuse(L"Native command handler is empty");
    external(section,name);runtime_->externals.at(&section).command=std::move(command);
}
void DrawPlanAdapter::after(ExplicitCommandListSection& section,NativeCommand command)
{
    if(!command || runtime_->active || !runtime_->adapted.empty() || runtime_->externals.count(&section))
        refuse(L"Register an original command observer before adaptation");
    runtime_->observers[&section]=std::move(command);
    section.command_list.runtime_populated=section.post_command_list.runtime_populated=true;
}
bool DrawPlanAdapter::translucent(CommandListState& state,uint32_t count,uint32_t first,float alpha,CustomResource* mask,
                                  uint32_t channel,bool invert,std::wstring& why)
{
    why.clear();
    try{
        if(!runtime_->active)refuse(L"Translucent draw outside an active plan");
        if(!std::isfinite(alpha) || channel>3)refuse(L"Translucent draw parameters out of range");
        if(!count)return true;
        PlanItem item;item.operation=PlanOperation::Translucent;item.index_count=count;item.first_index=first;
        item.alpha=alpha;item.mask_channel=channel;item.mask_invert=invert;
        if(mask){
            runtime_->direct_resource(mask);
            const SlotValue held=runtime_->held(mask);
            if(held.kind==SlotValue::Kind::Application || held.kind==SlotValue::Kind::Foreign)refuse(L"A translucency mask from the application's textures");
            item.resource=held.kind==SlotValue::Kind::Resource?held.resource:nullptr;
        }
        runtime_->indexed(item,state);return true;
    }catch(const Refused& e){why=e.reason;}
    return false;
}
void DrawPlanAdapter::palette_source(PaletteSource source)
{
    if(runtime_->active || !runtime_->adapted.empty())refuse(L"Install native palette source before adaptation");
    runtime_->palette_source=std::move(source);
}
bool DrawPlanAdapter::invoke(ExplicitCommandListSection& section,CommandListState& state,unsigned phases,std::wstring& reason)
{
    reason.clear();const auto post=state.post;
    if(!runtime_->active || !phases || phases>3 || state.aborted ||
       state.recursion>=MAX_COMMAND_LIST_RECURSION-1 || ++runtime_->steps>max_steps){
        runtime_->nested_failed=true;reason=L"Native callback requires an adapted, bounded active transaction";return false;
    }
    ++state.recursion;
    try{
        auto run=[&](CommandList& list,bool post){
            if(state.aborted)return;
            auto* resolved=list.ResolveCommandList();
            if(!runtime_->adapted.count(resolved))refuse(L"Native callback target was not adapted");
            state.post=post;
            for(auto& command:resolved->commands){
                if(state.aborted)break;
                command->run(&state);
            }
        };
        if(phases&1)run(section.command_list,false);
        // Original callback aliases may change during their pre phase.
        if(phases&2)run(section.post_command_list,true);
    }catch(const Refused& e){reason=e.reason;}
     catch(const std::exception&){reason=L"Native callback raised a frontend exception";}
    --state.recursion;state.post=post;
    if(!reason.empty())runtime_->nested_failed=true;
    return reason.empty();
}

void DrawPlanAdapter::adapt(CommandList& root)
{
    adapt_tree(root, 0);
}
std::shared_ptr<RunLinkedCommandList> DrawPlanAdapter::linked(CommandList& target,const std::wstring& line){
    if(runtime_->active)refuse(L"Create linked command boundaries outside execution");
    adapt(target);CommandList holder;auto source=LinkCommandLists(&holder,&target,&line);
    return std::make_shared<BridgeLinkedCommand>(std::move(source),runtime_);
}
CustomResource* DrawPlanAdapter::referenced_resource(CustomResource* resource) const
{
    return runtime_->referenced(resource);
}
SlotValue DrawPlanAdapter::held(CustomResource* declaration) const { return runtime_->held(declaration); }
bool DrawPlanAdapter::held(ResourceCopyTarget& target, CommandListState& state, SlotValue& value, SlotId& slot) const
{
    return runtime_->queries->held(target, state, value, slot);
}
void DrawPlanAdapter::pipeline(std::function<PipelineFacts()> facts)
{
    if(runtime_->active || !runtime_->adapted.empty() || !facts)refuse(L"Install the pipeline's facts before adapting command lists");
    runtime_->facts=std::move(facts);
}
bool DrawPlanAdapter::has_reference(const CustomResource* resource) const
{
    return runtime_->references.count(const_cast<CustomResource*>(resource))!=0;
}
void DrawPlanAdapter::resource_queries(ResourceQuery query)
{
    if(runtime_->active || !runtime_->adapted.empty())refuse(L"Install resource queries before adapting command lists");
    runtime_->queries->read=std::move(query);
}
void DrawPlanAdapter::plan_resource_queries(ResourceQuery query)
{
    if(runtime_->active || !runtime_->adapted.empty() || !query)refuse(L"Install plan resource queries before adapting command lists");
    runtime_->queries->plan_read=std::move(query);
}
const CommandList* DrawPlanAdapter::current_root() const { return runtime_->root; }
const std::wstring* DrawPlanAdapter::current_line() const { return runtime_->calling_line; }
void DrawPlanAdapter::keep_original()
{
    if(!runtime_->active)refuse(L"Keeping the original draw needs an active plan");
    runtime_->keep_original=true;
}
void DrawPlanAdapter::host_values(HostValue values){
    if(runtime_->active || !runtime_->adapted.empty() || !values)refuse(L"Install host values before adapting command lists");
    runtime_->queries->host=std::move(values);
}
void DrawPlanAdapter::fresh_state(FreshState initialize){
    if(runtime_->active || !runtime_->adapted.empty() || !initialize)refuse(L"Install the fresh state before adapting command lists");
    runtime_->fresh=std::move(initialize);
}
void DrawPlanAdapter::render_target_size(RenderTargetSize query){
    if(runtime_->active || !runtime_->adapted.empty() || !query)refuse(L"Install the render target size before adapting command lists");
    runtime_->queries->rt=std::move(query);
}
bool DrawPlanAdapter::undecided_words(wchar_t stage,unsigned slot){
    auto& q=*runtime_->queries;const auto key=std::make_pair(stage,slot);
    auto at=std::find(q.words.begin(),q.words.end(),key);
    if(at==q.words.end()){
        if(q.words.size()>=4){q.words_over=true;return false;}
        q.words.push_back(key);at=q.words.end()-1;
    }
    return (q.words_second>>(at-q.words.begin()))&1u;
}
void DrawPlanAdapter::plan_shader_queries(ShaderQuery query){
    if(runtime_->active || !runtime_->adapted.empty() || !query)refuse(L"Install plan shader queries before adapting command lists");
    runtime_->queries->plan_shader=std::move(query);
}
void DrawPlanAdapter::shader_queries(ShaderQuery query){
    if(runtime_->active || !runtime_->adapted.empty() || !query)refuse(L"Install shader queries before adapting command lists");
    runtime_->queries->shader=std::move(query);
}
void DrawPlanAdapter::resource_commands(ResourceCommand command)
{
    if(runtime_->active || !runtime_->adapted.empty() || !command)refuse(L"Install resource commands before adaptation");
    runtime_->resource_command=std::move(command);
}
void DrawPlanAdapter::parameter_flush(ParameterFlush callback)
{
    if(runtime_->active || !runtime_->adapted.empty() || !callback)refuse(L"Install parameter publication before adaptation");
    runtime_->parameter_flush=std::move(callback);
}
void DrawPlanAdapter::draw_prepare(DrawPrepare callback){
    if(runtime_->active || !runtime_->adapted.empty())refuse(L"Draw preparation must be installed before adaptation");
    runtime_->draw_prepare=std::move(callback);
}
void DrawPlanAdapter::owned_scope(std::function<bool()> gate){
    if(runtime_->active || !runtime_->adapted.empty() || !gate || runtime_->owned_gate)refuse(L"Install one owned scope gate before adaptation");
    runtime_->owned_gate=gate;runtime_->queries->gate=std::move(gate);
}
void DrawPlanAdapter::draw_scope(std::function<bool()> gate){
    if(runtime_->active || !runtime_->adapted.empty() || !gate || runtime_->draw_gate)refuse(L"Install one draw scope gate before adaptation");
    auto* runtime=runtime_.get();
    runtime_->queries->drawing=gate;runtime_->queries->inside=[runtime]{return runtime->shader_depth && runtime->drawing();};
    runtime_->draw_gate=std::move(gate);
}
void DrawPlanAdapter::owned_bindings(OwnedBinding binding){
    if(runtime_->active || !runtime_->adapted.empty() || !binding || runtime_->owned_binding)refuse(L"Install one owned binding provider before adaptation");
    runtime_->owned_binding=std::move(binding);
}
void DrawPlanAdapter::tolerate_refusals(bool on){runtime_->tolerant=on;}
void DrawPlanAdapter::shader_commands(ShaderCommand callback){
    if(runtime_->active || !runtime_->adapted.empty() || !callback)refuse(L"Install shader execution before adaptation");
    runtime_->shader_command=std::move(callback);
}
bool DrawPlanAdapter::invoke_shader(CustomShader& shader,CommandListState& state,std::wstring& reason){
    reason.clear();const auto post=state.post;
    if(!runtime_->active || state.recursion>=MAX_COMMAND_LIST_RECURSION || state.aborted){
        runtime_->nested_failed=true;reason=L"CustomShader callback requires a bounded active transaction";return false;
    }
    ++state.recursion;
    try{for(unsigned phase=0;phase<2 && !state.aborted;++phase){
        auto* list=(phase?shader.post_command_list:shader.command_list).ResolveCommandList();
        if(!runtime_->adapted.count(list))refuse(L"CustomShader command list was not adapted");state.post=phase!=0;
        for(auto& command:list->commands){if(state.aborted)break;command->run(&state);}
    }}catch(const Refused& e){reason=e.reason;}
      catch(const std::exception&){reason=L"CustomShader callback raised a frontend exception";}
    --state.recursion;state.post=post;if(!reason.empty())runtime_->nested_failed=true;return reason.empty();
}
bool DrawPlanAdapter::flush_parameters(CommandListState& state,bool force,std::wstring& reason)
{
    reason.clear();
    try{runtime_->flush(state,force);return true;}
    catch(const Refused& e){reason=e.reason;}
    catch(const std::exception&){reason=L"Frontend raised an exception publishing parameters";}
    return false;
}

void DrawPlanAdapter::texture_queries(TextureQuery query)
{
    if(runtime_->active || !runtime_->adapted.empty() || !query || runtime_->texture_query)refuse(L"Install one texture matcher before adapting command lists");
    runtime_->texture_query=std::move(query);std::weak_ptr<Runtime> weak=runtime_;
    runtime_->queries->texture=[weak](ResourceCopyTarget& target,CommandListState& state,float& result,std::wstring& reason){
        auto owner=weak.lock();if(!owner || !owner->active){reason=L"Texture query outside an active plan";return false;}
        bool bound=false;TextureOverrideMatches matches;owner->texture_matches(target,state,bound,matches);
        if(!bound){result=-0.f;return true;}
        if(matches.empty()){result=0.f;return true;}
        result=1.f;for(auto it=matches.rbegin();it!=matches.rend();++it)if((*it)->filter_index!=FLT_MAX){result=(*it)->filter_index;break;}
        return true;
    };
}
bool DrawPlanAdapter::has_texture_queries() const {return bool(runtime_->texture_query) || bool(runtime_->resource_match);}
void DrawPlanAdapter::index_region_hash(std::function<bool(uint32_t&)> query){runtime_->draw_index_hash=std::move(query);}
const CommandListCommand* DrawPlanAdapter::original(const CommandListCommand& command)
{
    const auto* bridge=dynamic_cast<const BridgeCommand*>(&command);return bridge?bridge->original.get():&command;
}
void DrawPlanAdapter::resource_matches(ResourceMatchQuery query){
    if(runtime_->active || !runtime_->adapted.empty() || !query || runtime_->resource_match)
        refuse(L"Install one source resource matcher before adapting command lists");
    runtime_->resource_match=std::move(query);runtime_->queries->general_matches=true;std::weak_ptr<Runtime> weak=runtime_;
    runtime_->queries->texture=[weak](ResourceCopyTarget& target,CommandListState& state,float& result,std::wstring& reason){
        auto owner=weak.lock();if(!owner || !owner->active){reason=L"Resource matching outside an active transaction";return false;}
        bool bound=false;TextureOverrideMatches matches;owner->texture_matches(target,state,bound,matches);
        if(!bound){result=-0.f;return true;}result=matches.empty()?0.f:1.f;
        for(auto it=matches.rbegin();it!=matches.rend();++it)if((*it)->filter_index!=FLT_MAX){result=(*it)->filter_index;break;}
        return true;
    };
}
std::vector<CustomResource*> DrawPlanAdapter::referenced_files() const
{
    return {runtime_->files.begin(), runtime_->files.end()};
}
unsigned DrawPlanAdapter::referenced_roles(CustomResource* resource) const
{
    const auto found = runtime_->file_roles.find(resource);
    return found == runtime_->file_roles.end() ? 0u : found->second;
}

void DrawPlanAdapter::adapt_tree(CommandList& root, size_t depth)
{
    if (depth > MAX_COMMAND_LIST_RECURSION || runtime_->adapted.size() >= max_steps)
        refuse(L"Command graph adaptation budget exceeded");
    if (!runtime_->adapted.insert(&root).second) return;
    for (auto& command : root.commands) {
        if(auto* linked=dynamic_cast<BridgeLinkedCommand*>(command.get())){
            if(linked->runtime!=runtime_ || !linked->link || !runtime_->adapted.count(linked->link->ResolveCommandList()))
                refuse(L"Linked command belongs to another planner");continue;
        }
        if (dynamic_cast<BridgeCommand*>(command.get()))
            refuse(L"A command list already belongs to another planner");
        runtime_->collect_files(*command);
        if (auto* branch = dynamic_cast<IfCommand*>(command.get())) {
            for (auto* list : {branch->true_commands_pre.get(), branch->true_commands_post.get(),
                               branch->false_commands_pre.get(), branch->false_commands_post.get()})
                if (list) adapt_tree(*list, depth + 1);
        } else if (auto* call = dynamic_cast<RunExplicitCommandList*>(command.get())) {
            if (call->command_list_section && !runtime_->externals.count(call->command_list_section)) {
                adapt_tree(call->command_list_section->command_list, depth + 1);
                adapt_tree(call->command_list_section->post_command_list, depth + 1);
            }
        } else if(auto* copy=dynamic_cast<CopyCommandListCommand*>(command.get())) {
            for(auto* target:{copy->src,copy->dst})if(target && !runtime_->externals.count(target)){
                adapt_tree(target->command_list,depth+1);adapt_tree(target->post_command_list,depth+1);
            }
        } else if(auto* call=dynamic_cast<RunCustomShaderCommand*>(command.get())) {
            if(call->custom_shader){adapt_tree(call->custom_shader->command_list,depth+1);adapt_tree(call->custom_shader->post_command_list,depth+1);}
        } else if(auto* call=dynamic_cast<RunLinkedCommandList*>(command.get())){
            if(call->link)adapt_tree(*call->link,depth+1);
        }
        // A native command (an if, a variable assignment, a pool variable, abort, preset, the limit
        // reset, the upscaling flip) whose arguments the CPU evaluates stays the original's own object
        // (D-106): the bridge only budgeted it and called it, a hundred commands per matched draw.
        {
            const uint32_t kinds = Runtime::classify(*command);
            constexpr uint32_t native = Runtime::K_IfCommand | Runtime::K_VariableAssignmentExact | Runtime::K_PoolVariableOperation |
                                        Runtime::K_AbortCommand | Runtime::K_PresetCommand | Runtime::K_ResetPerFrameLimitsCommand |
                                        Runtime::K_UpscalingFlipBBCommand;
            if ((kinds & native) && cpu_arguments(*command)) { guard_arguments(*command, runtime_->queries); continue; }
        }
        command = std::make_shared<BridgeCommand>(command, runtime_);
    }
}

bool DrawPlanAdapter::execute(CommandList& root, CommandListState& state,
                              DrawPlan& output, std::wstring& reason)
{
    return execute_impl({&root},nullptr,state,output,reason);
}
bool DrawPlanAdapter::execute_pair(CommandList& pre,CommandList& post,CommandListState& state,DrawPlan& output,std::wstring& reason,OriginalCall between)
{
    const std::vector<CommandList*> after={&post};return execute_impl({&pre},&after,state,output,reason,std::move(between));
}
std::vector<std::tuple<std::string, uint64_t, uint64_t>> DrawPlanAdapter::command_timing() const
{
    std::vector<std::tuple<std::string, uint64_t, uint64_t>> result;
    for (const auto& entry : runtime_->command_timing) result.emplace_back(entry.first, entry.second.first, entry.second.second);
    return result;
}
void DrawPlanAdapter::note_timing(const char* name, uint64_t microseconds)
{
    auto& slot = runtime_->timing_slots[name];
    if (!slot) slot = &runtime_->command_timing[name];
    ++slot->first; slot->second += microseconds;
}
bool DrawPlanAdapter::execute_matches(const std::vector<CommandList*>& pre,const std::vector<CommandList*>& post,CommandListState& state,DrawPlan& output,std::wstring& reason)
{
    return execute_impl(pre,&post,state,output,reason,{},true);
}
bool DrawPlanAdapter::execute_impl(const std::vector<CommandList*>& pre,const std::vector<CommandList*>* post,CommandListState& state,DrawPlan& output,std::wstring& reason,OriginalCall between,bool matched)
{
    reason.clear();
    auto valid=[&](const auto& lists){if(lists.size()>max_items)return false;for(auto* list:lists)
        if(!list || !runtime_->adapted.count(list->ResolveCommandList()))return false;return true;};
    if (runtime_->active || !valid(pre) || (post && !valid(*post)) || !state.mHackerContext ||
        state.aborted || state.recursion) {
        reason = L"Planner requires an adapted root and a fresh serial execution state";
        return false;
    }
#if defined(EFMI_COMMAND_TIMING) || defined(EFMI_PHASE_TIMING)
    auto phase_began = std::chrono::steady_clock::now();
    auto phase = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        note_timing(name, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - phase_began).count())); phase_began = now;
    };
#else
    auto phase = [](const char*) {};
#endif
    DrawPlan& candidate = runtime_->candidate;
    candidate.items.clear(); candidate.skip_original = false; candidate.pre_items = SIZE_MAX;
    auto saved_layout = state.input_layout_overrides;const auto saved_post=state.post;
    runtime_->active = &candidate; runtime_->steps = runtime_->draws = 0;runtime_->queries->count=0;runtime_->nested_failed=false;runtime_->active_depth=0;
    runtime_->keep_original=false;runtime_->shader_depth=0;
    // A fresh context: every slot holds the application's binding of the draw the lists run for (D-161).
    runtime_->context.reset();++runtime_->execution;runtime_->draw_refused.clear();runtime_->application_drawn=false;
    phase("exec setup");
    const bool listing=runtime_->tolerant; // a Present, Constants or key list: it draws nothing
    try {
        // A nested callback that raised past its native caller: in a list that draws nothing the execution
        // fails as before; a draw's lists run on and the draw stays the game's (D-162).
        auto nested=[&]{
            if(!runtime_->nested_failed)return;
            if(listing)refuse(L"Nested native callback failed");
            runtime_->whole(L"Nested native callback failed");
        };
        auto run=[&](const auto& lists,CommandListState& phase){for(auto* root:lists){
            if(phase.aborted)break;
            auto* resolved=root->ResolveCommandList();
            // An earlier source command can change a later list's alias.
            if(!runtime_->adapted.count(resolved))refuse(L"Matched root callback target was not adapted");
            runtime_->root=root;
            for(auto& command:resolved->commands){
                if(phase.aborted)break;
                // As the original: the command did nothing, the list goes on. A bridged command holds what it
                // raises itself; what reaches here is a command the bridge does not hold (the original's own
                // if or assignment, whose condition or value raised).
                const auto* source=original(*command);
                const std::wstring line=source?source->ini_line:std::wstring();
                try{command->run(&phase);}
                catch(const Refused& refused){runtime_->contained(source?source:command.get(),line,refused);}
                catch(const std::exception& e){runtime_->contained(source?source:command.get(),line,Refused{widen(e.what())});}
            }
            runtime_->root=nullptr;
            nested();
        }};
        if(post)state.post=false;
        run(pre,state);
        phase("exec pre lists");
        nested();
        runtime_->flush(state);
        phase("exec flush");
        // The application's draw, where it stays, sees the context as the pre lists left it; its bindings
        // stand before pre_items. A list that draws nothing has no draw to bind for.
        if(!listing)runtime_->application_draw();
        candidate.pre_items=candidate.items.size();
        runtime_->application_drawn=true;
        if(between){std::wstring why;if(!between(state,why))refuse(why.empty()?L"Root phase boundary refused":why);}
        // The original reaches a matched section's post list by checking again: the list that checked
        // (`CheckTextureOverride = ib` stands in both of its phases) looks the index buffer up afresh in its
        // post phase, CheckTextureOverrideCommand::run, and that is the buffer bound by then. One the pre
        // lists bound is the Mod's own, which the original keeps no hash for and which meets no section; with
        // none bound nothing is checked. The post lists of the sections then do not run (the judge's line
        // `post_draw`: a Mod that replaces `ib` and draws again in `post` draws once under the original).
        if(post && !(matched && !runtime_->context.in_place(SlotId{SlotKind::Index,0,0}))){
            // RunCommandListComplete creates a fresh state for the independent
            // post root. Applied resource/layout state persists in the backend;
            // pending edits, abort and cached queries do not cross this boundary.
            CommandListState after;after.mHackerDevice=state.mHackerDevice;after.mHackerContext=state.mHackerContext;
            after.mOrigDevice1=state.mOrigDevice1;after.mOrigContext1=state.mOrigContext1;after.call_info=state.call_info;
            after.this_target=state.this_target;after.resource=state.resource;after.view=state.view;after.post=true;
            if(runtime_->fresh)runtime_->fresh(after);
            run(*post,after);
            nested();
            runtime_->flush(after);
            phase("exec post lists+flush");
        }
        // What the lists leave bound after their last draw reaches no draw of the plan: the game binds its
        // own for its next draw, and nothing is issued for it.
        nested();
        if(runtime_->keep_original)candidate.skip_original=false;
        // The lists ran to their end (D-162): the Mod's variables are what the original leaves. Where the
        // host cannot draw the draw as the Mod's, the output stays and the reason goes back.
        if(!runtime_->draw_refused.empty())reason=runtime_->draw_refused;
        else std::swap(output, candidate); // both keep their capacity
        phase("exec output");
    } catch (const Refused& error) {
        reason = error.reason;
    } catch (const std::exception&) {
        reason = L"Frontend raised an exception while planning";
    }
    runtime_->active = nullptr; runtime_->root = nullptr;
    state.input_layout_overrides = std::move(saved_layout);state.post=saved_post;
    // The state is throwaway, but leave nesting counters usable for diagnostics.
    state.recursion = state.extra_indent = 0;state.aborted=false;
    return reason.empty();
}

bool DrawPlanAdapter::execute_intercepted(const std::vector<CommandList*>& pre,const std::vector<CommandList*>& post,
                                         CommandListState& state,OriginalCall original,std::wstring& reason){
    reason.clear();
    auto valid=[&](const auto& lists){if(lists.size()>6)return false;for(auto* list:lists)
        if(!list || !runtime_->adapted.count(list->ResolveCommandList()))return false;return true;};
    if(runtime_->active || !state.mHackerContext || !state.call_info || state.aborted || state.recursion || !original || !valid(pre) || !valid(post)){
        reason=L"Intercepted call requires adapted stage roots and fresh source state";return false;
    }
    DrawPlan candidate;candidate.skip_original=state.call_info->skip;
    runtime_->active=&candidate;runtime_->steps=runtime_->draws=runtime_->queries->count=0;
    runtime_->nested_failed=false;runtime_->active_depth=0;
    runtime_->context.reset();++runtime_->execution;runtime_->draw_refused.clear();runtime_->application_drawn=false;runtime_->keep_original=false;
    runtime_->shader_depth=0;
    // One transaction of the owned domain: a command that cannot act ends the call (Runtime::strict).
    struct Strict {Runtime& r;Strict(Runtime& owner):r(owner){r.strict=r.queries->strict=true;}~Strict(){r.strict=r.queries->strict=false;}} strict(*runtime_);
    try{
        auto fresh=[&](CommandListState& root,bool after){root.mHackerDevice=state.mHackerDevice;root.mHackerContext=state.mHackerContext;
            root.mOrigDevice1=state.mOrigDevice1;root.mOrigContext1=state.mOrigContext1;root.call_info=state.call_info;root.post=after;
            if(runtime_->fresh)runtime_->fresh(root);};
        auto run=[&](const auto& lists,bool after){for(auto* list:lists){CommandListState root;fresh(root,after);
            auto* resolved=list->ResolveCommandList();
            if(!runtime_->adapted.count(resolved))refuse(L"Shader root callback alias was not adapted");
            for(auto& command:resolved->commands){if(root.aborted)break;command->run(&root);}
            if(runtime_->nested_failed)refuse(L"Nested shader root callback failed");runtime_->flush(root);
            if(!candidate.items.empty())refuse(L"Intercepted shader calls require immediate resource/draw execution");
        }};
        run(pre,false);
        if(!candidate.skip_original && !state.call_info->hunting_skip){CommandListState caller;fresh(caller,false);
            std::wstring why;if(!original(caller,why))refuse(why.empty()?L"Original call callback refused":why);
            if(runtime_->nested_failed || !candidate.items.empty())refuse(L"Original call callback left failed or deferred work");}
        run(post,true);
    }catch(const Refused& e){reason=e.reason;}
     catch(const std::exception& e){const std::string text=e.what();reason.assign(text.begin(),text.end());}
    runtime_->active=nullptr;return reason.empty();
}

} // namespace efmi::frontend
