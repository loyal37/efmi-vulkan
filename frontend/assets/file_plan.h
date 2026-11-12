#pragma once
#include "resource_cache.h"
#include <cstdint>
namespace efmi::core {struct PaletteSnapshot;}

namespace efmi::assets {
enum class FileOperation { BindVertex, BindIndex, BindPixelResource, DrawIndexed, External, Translucent };

// Owned frontend data. The byte uploader does not infer stride/index format
// from a filename. Neither this data nor its loader executes native Core calls.
struct FileResource {
    std::wstring name;
    std::filesystem::path path;
    VkBufferUsageFlags usage=0; // Zero means an image file (a .dds, or any WIC container).
    // An image's (D-140): the original's loader flags from its declaration's colour space (png_force_srgb,
    // png_ignore_srgb; the DDS loader takes the first), the view format its binding asks for (0: the
    // texture's own), and whether its bind flags allow a shader resource view.
    uint32_t loader_flags=0,view_format=0;
    bool sampled=true;
    uint32_t stride=0;
    VkIndexType index_type=VK_INDEX_TYPE_MAX_ENUM;
};
struct LayoutEdit {
    struct Match {
        std::string semantic_name;
        uint32_t semantic_index=UINT32_MAX,input_slot=UINT32_MAX,format=0;
        uint32_t format_byte_size=UINT32_MAX,aligned_byte_offset=UINT32_MAX;
    } match;
    struct Replace {
        std::string semantic_name;
        uint32_t semantic_index=UINT32_MAX,format=0,aligned_byte_offset=UINT32_MAX;
    } replace;
};
// A buffer the owned GPU work of a replaced draw wrote (P2, D-087): a Vulkan handle on the
// application's device, bound as it is; `keep` holds the allocation while an item lives.
struct OwnedBuffer {uint64_t buffer=0,offset=0,size=0;uint32_t stride=0;std::shared_ptr<void> keep;};
struct FilePlanItem {
    FileOperation operation=FileOperation::External;
    uint32_t slot=0;
    std::shared_ptr<const FileResource> resource; // Null is an explicit unbind.
    int32_t application_slot=-1; // the application's texture at this DX11 pixel slot of the draw (PlanItem)
    std::shared_ptr<const OwnedBuffer> owned; // BindVertex of an owned buffer; resource is null then
    uint32_t index_count=0,first_index=0,instance_count=1,first_instance=0;
    int32_t vertex_offset=0;
    bool indexed=true,automatic=false; // PlanItem's (D-142)
    std::vector<LayoutEdit> layout;
    std::wstring external;
    uint32_t phase_mask=0;
    std::shared_ptr<const core::PaletteSnapshot> palette;
    // Translucent (D-113, PlanItem): `resource` is the opacity mask image, null without one.
    float alpha=1;
    uint32_t mask_channel=0;
    bool mask_invert=false;
    bool shadow=false; // PlanItem's (D-185): in a shadow pass, drawn in place by the opacity
};
struct FilePlan {
    bool skip_original=false;
    std::vector<FilePlanItem> items;
    size_t pre_items=SIZE_MAX; // items before the original draw (DrawPlan::pre_items)
};
enum class FileReadiness { Pending, Ready, Failed };
class PreparedFilePlan {
public:
    const FilePlan& plan() const {return plan_;}
    const std::vector<ResourceHandle>& requests() const {return requests_;}
private:
    friend bool prepare_file_plan(const FilePlan&,ResourceCache&,PreparedFilePlan&,std::string&);
    friend FileReadiness request_file_plan(const PreparedFilePlan&,ResourceCache&,std::vector<ResourceSnapshot>&,std::string&);
    FilePlan plan_;
    // One slot per operation. Only nonnull file bindings have requests.
    std::vector<ResourceHandle> requests_;
};

// Setup only: resolves cache handles with no file IO or demand. Output remains
// unchanged on refusal; earlier declarations may remain dormant in the cache.
bool prepare_file_plan(const FilePlan&,ResourceCache&,PreparedFilePlan&,std::string& reason);
// Demand and collect immutable Ready owners. Pending/Failed preserves output.
// A Ready result means assets only: External items still require their native
// adapters before the complete draw transaction may execute.
FileReadiness request_file_plan(const PreparedFilePlan&,ResourceCache&,
                               std::vector<ResourceSnapshot>&,std::string& reason);
}
