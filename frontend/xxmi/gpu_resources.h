#pragma once
#include "runtime_host.h"
#include "image_rules.h"
#include "resource_match.h"
#include "../gpu/resource_commands.h"
#include <map>
#include <set>
#include <tuple>

namespace efmi::frontend {
struct GpuResourceBinding {
    enum class View {None,ShaderResource,UnorderedAccess,RenderTarget,DepthStencil};
    gpu::Buffer buffer;
    gpu::Image image;
    gpu::ImageView image_view;
    ImageSourceDescription image_description;
    D3D11_BUFFER_DESC description{};
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint32_t stride=0,source_stride=0,offset=0,visible_size=0;
    bool region=false;
    View view=View::None;
    DXGI_FORMAT view_format=DXGI_FORMAT_UNKNOWN;
    uint32_t view_offset=0,view_size=0;
    explicit operator bool() const {return bool(buffer) || bool(image);}
    UINT bind_flags() const {return image?image_description.texture.BindFlags:description.BindFlags;}
    void drop_view(){view=View::None;image_view.reset();}
};
// D3D11 hazards operate on source subresources, not Vulkan byte intervals.
bool source_bindings_overlap(const GpuResourceBinding&,const GpuResourceBinding&) noexcept;
// Original INI adapter for owned mutable resources. Stage slots are logical
// source-API bindings; graphics/compute descriptor emission is a separate host
// responsibility. They must not be mistaken for changed application bindings.
// One instance per frontend generation, sharing budget ledgers across reloads.
class GpuResourceRuntime final : public RuntimeExtension {
public:
    // Deferred: recording without a queue yet, on a thread that must not touch it (a generation
    // loading in the background); finish() names the queue. Store refuses there.
    // Draw: recording without a queue during a replaced draw's plan (P2, D-087); the host submits
    // the work before the application's submission. Store there gives the value its previous execution
    // read, once that submission completed (D-147).
    enum class ExecutionSite { OutsideRenderPass, InsideRenderPass, Deferred, Draw };
    GpuResourceRuntime(gpu::ResourceDevice,gpu::ResourceBudgets={});
    std::vector<std::wstring> install(DrawPlanAdapter&) override;
    bool initialize_parameters(CommandListState&,std::wstring&) override;
    using DrawExecutor=std::function<bool(DrawCommand&,CommandListState&,std::wstring&)>;
    // A shader host may execute draws in this same explicit queue domain. The
    // original parameter flush runs before this callback, including count zero.
    void draw_commands(DrawExecutor);
    // In a replaced draw's scope a refusal before any owned work recorded leaves the runtime usable.
    void execution_failed() noexcept override {if(active_ && (site_!=ExecutionSite::Draw || recorded_))failed_=true;}
    // The host owns this queue's synchronization for the complete scope. Store
    // may submit and wait for GPU feedback inside it. A draw hook cannot opt in
    // by silently treating an active render pass as OutsideRenderPass.
    void begin(VkQueue,ExecutionSite);
    bool scope_active() const {return active_;}
    // Whether the active scope's recorded work is to be dropped: a command failed after it had recorded
    // part of its work, or the execution around the scope failed. finish() refuses such a scope;
    // discard() or suspend() drops its work and the next scope starts clean. The failure is the scope's:
    // it ended the whole generation's owned work before, which in a folder generation is every Mod's.
    bool failed() const {return failed_;}
    ExecutionSite site() const {return site_;}
    VkQueue queue() const {return queue_;}
    // Another queue of the family once the host waited for every earlier submission (D-090).
    void rebind(VkQueue queue){
        if(active_ || !queue)throw std::runtime_error("GPU resource scope rebinds only while closed");
        queue_=queue;commands_.rebind(queue);
    }
    bool pending() const {return commands_.pending();}
    // Closes the scope without submitting: recorded work stays pending for the next scope's
    // finish(). A generation loaded in the background is suspended until its first Present.
    void suspend();
    std::shared_ptr<gpu::ResourceSubmission> finish(const gpu::ResourceCommands::SubmitSync& sync={},VkQueue queue=VK_NULL_HANDLE);
    // Discards unsubmitted work. Like the original interpreter, earlier CPU
    // assignments and already-submitted store feedback are not rolled back.
    void discard();
    GpuResourceBinding binding(wchar_t stage,ResourceCopyTargetType,unsigned slot) const;
    // The host's current back buffer, which bb, r_bb and f_bb return: without upscaling the
    // original hands out the swap chain's current buffer for all three. The host sets it before
    // each Present it executes; without one they read as null, as with no swap chain.
    void backbuffer(gpu::Image,DXGI_FORMAT,UINT bind_flags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
    // Whether the scope since the last begin() bound the back buffer as a render target or an
    // unordered access view, or copied into it: what the host may have to draw again for a
    // presentation of the same frame (BUG-036).
    bool backbuffer_targeted() const {return backbuffer_targeted_;}
    // Source-semantic target resolution for an adapted draw in an active scope.
    GpuResourceBinding resolve(ResourceCopyTarget&,CommandListState&);
    // Attach proven native tracking facts to an owned allocation. Reference
    // aliases share them; newly allocated byte copies remain untracked. The
    // host updates the snapshot whenever its source hash policy changes it.
    // This neither imports application resources nor computes a DX11 hash.
    void identify_resource(const GpuResourceBinding&,SourceResourceIdentity);
    const D3D11_VIEWPORT& viewport() const {return viewport_;}
    const GpuResourceBinding& ini_parameters() const {return parameters_;}
    // Recorded uploads, not completed GPU execution or application binding.
    uint64_t parameter_uploads() const {return parameter_uploads_;}
    gpu::ResourceCommands& commands(){return commands_;}
    struct OutputState {
        D3D11_VIEWPORT viewport{};
        std::map<std::tuple<wchar_t,ResourceCopyTargetType,unsigned>,GpuResourceBinding> bindings;
    };
    OutputState save_outputs() const;
    void restore_outputs(OutputState&&) noexcept;
private:
    using Slot=std::tuple<wchar_t,ResourceCopyTargetType,unsigned>;
    GpuResourceBinding backbuffer_;
    bool backbuffer_targeted_=false;
    bool run(CommandListCommand&,CommandListState&,std::wstring&);
    bool query(ResourceCopyTarget&,CommandListState&,float&,std::wstring&);
    bool owned_vertex(ResourceCopyTarget&,CommandListState&,PlanItem::OwnedBuffer&,std::wstring&);
    bool render_target(CommandListState&,bool& bound,float& width,float& height,std::wstring&);
    bool match_source(ResourceCopyTarget&,CommandListState&,SourceResourceMatch&,std::wstring&);
    ResourceCopyTarget& effective(ResourceCopyTarget&,CommandListState&);
    Slot slot(ResourceCopyTarget&,CommandListState&);
    CustomResource* declaration(ResourceCopyTarget&,CommandListState&,bool writing=false);
    GpuResourceBinding get(ResourceCopyTarget&,CommandListState&,ResourceCopyTarget* destination=nullptr);
    GpuResourceBinding load(CustomResource&,ResourceCopyTarget* destination=nullptr,CommandListState* state=nullptr);
    GpuResourceBinding load_image(CustomResource&);
    // A file the original's loader makes no resource of: null from then on, with its warning (D-139).
    GpuResourceBinding unloaded(CustomResource&,const wchar_t* kind,const std::string& why);
    gpu::Image allocate_image(ImageSourceDescription&);
    void select_view(GpuResourceBinding&,GpuResourceBinding::View,bool raw=false,bool complete_info=true);
    void fill_info(GpuResourceBinding&,ResourceCopyTargetType type=ResourceCopyTargetType::CUSTOM_RESOURCE);
    void image_view(GpuResourceBinding&,GpuResourceBinding::View,bool complete_info);
    void image_copy(ResourceCopyOperation&,ResourceCopyTarget&,CommandListState&,GpuResourceBinding,CustomResource*,GpuResourceBinding&);
    void image_clear(GpuResourceBinding,ClearViewCommand&);
    void set(ResourceCopyTarget&,CommandListState&,const GpuResourceBinding&);
    using Bindings=std::map<Slot,GpuResourceBinding>;
    void assign_binding(Slot,GpuResourceBinding);
    void publish_bindings(Bindings);
    void copy(ResourceCopyOperation&,CommandListState&);
    void copy_to(ResourceCopyOperation&,ResourceCopyTarget&,CommandListState&,GpuResourceBinding);
    void ranges(SlotRangeCopyOperation&,CommandListState&);
    void clear(ClearViewCommand&,CommandListState&);
    void store(StoreCommand&,CommandListState&);
    void active() const;
    bool flush_parameters(CommandListState&,bool,std::wstring&);
    void publish_parameters();
    gpu::ResourceCommands commands_;
    GpuResourceBinding parameters_;
    DrawExecutor draw_executor_;
    uint64_t parameter_uploads_=0;
    bool parameters_initialized_=false;
    std::map<CustomResource*,GpuResourceBinding> resources_;
    // CustomResource::Substantiate runs once per resource: the declarations already substantiated (D-151).
    std::set<CustomResource*> substantiated_;
    std::map<std::weak_ptr<void>,SourceResourceIdentity,std::owner_less<std::weak_ptr<void>>> source_identities_;
    std::map<Slot,GpuResourceBinding> slots_;
    // A source slot-range command materializes every view before one API bind.
    Bindings* deferred_bindings_=nullptr;
    // Copies to a stage slot cache per instruction, as in the original engine.
    std::map<ResourceCopyOperation*,GpuResourceBinding> copies_;
    // A current alias is not a writable copy-cache allocation. Original
    // ResourcePool retains owned allocations for every description it creates.
    using CopyPool=std::map<std::array<UINT,4>,gpu::Buffer>;
    std::map<CustomResource*,CopyPool> resource_copy_pools_;
    std::map<ResourceCopyOperation*,CopyPool> command_copy_pools_;
    using ImageKey=std::array<UINT,13>;
    using ImageCopyPool=std::map<ImageKey,gpu::Image>;
    std::map<CustomResource*,ImageCopyPool> resource_image_pools_;
    std::map<ResourceCopyOperation*,ImageCopyPool> command_image_pools_;
    std::map<std::array<UINT,10>,gpu::Image> clear_images_;
    std::map<SlotRangeCopyOperation*,std::vector<std::unique_ptr<ResourceCopyOperation>>> range_operations_;
    std::map<SlotRangeCopyOperation*,std::vector<GpuResourceBinding>> range_views_;
    VkQueue queue_=VK_NULL_HANDLE;
    D3D11_VIEWPORT viewport_{};
    ExecutionSite site_=ExecutionSite::OutsideRenderPass;
    // A store's reads in flight in replaced draws' scopes, with the value's offset in each (D-147).
    std::unordered_map<const StoreCommand*,std::vector<std::pair<gpu::ResourceCommands::Feedback,UINT>>> feedback_;
    bool recorded_=false; // owned work recorded in the current Draw scope
    // Commands that failed after recording part of their work: not run again in this generation.
    std::unordered_set<const CommandListCommand*> disabled_;
    bool active_=false,installed_=false,failed_=false;
};
}
