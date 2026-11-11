#pragma once
#include "draw_plan.h"
#include "../assets/file_plan.h"

namespace efmi::frontend {
// Snapshot declarations once per retained frontend generation. Plans no longer
// borrow CustomResource pointers after compilation. Unsupported declarations
// remain dormant until a callback actually binds them; no files are opened.
class FilePlanCompiler {
public:
    explicit FilePlanCompiler(const CustomResources&);
    bool compile(const DrawPlan&,assets::FilePlan&,std::wstring& reason) const;
    // What a declaration is in a binding's role (a vertex buffer, the index buffer, a pixel texture), null
    // where the layer makes none of it there. For a survey of the generation's files: it says nothing.
    std::shared_ptr<const assets::FileResource> binding(const CustomResource*,PlanOperation) const;
private:
    struct Declaration {
        std::array<std::shared_ptr<const assets::FileResource>,3> bindings;
        std::array<std::wstring,3> reasons;
    };
    std::unordered_map<const CustomResource*,Declaration> declarations_;
    mutable assets::FilePlan next_; // kept for its capacity (D-108)
    mutable std::unordered_set<const CustomResource*> told_; // resources a pixel slot could not take, each said once
};
}
