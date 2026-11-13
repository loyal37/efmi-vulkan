#pragma once
#include "variant_cache.h"

namespace efvk {
// Loaded on the folder-generation worker, published with its INIs. No per-draw filesystem reads.
std::shared_ptr<const NativeShaderSet> load_shader_fixes(Device*, const std::shared_ptr<const NativeShaderSet>& previous,
                                                       const std::map<uint32_t, int>& depth_filters = {});
}
