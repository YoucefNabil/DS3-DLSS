#pragma once
// The dlaa-core modules under the names the game code uses (ds3::Log,
// ds3::shaders, ...). DS3 keeps its own capture (dlss/capture.h): the F9
// capture feeds tools/capture_replay through DS3_DLSS_ReplayFrame.

#include "dlaa_core/config.h"
#include "dlaa_core/debug_overlay.h"
#include "dlaa_core/log.h"
#include "dlaa_core/mip_bias.h"
#include "dlaa_core/pointer_set.h"
#include "dlaa_core/shader_registry.h"

namespace ds3 {

using dlaa_core::BytecodeContains;
using dlaa_core::Log;
using dlaa_core::ModuleDir;
template <size_t kSize>
using PointerSet = dlaa_core::PointerSet<kSize>;

namespace debug_overlay = dlaa_core::debug_overlay;
namespace mip_bias = dlaa_core::mip_bias;
namespace shaders = dlaa_core::shaders;

}  // namespace ds3
