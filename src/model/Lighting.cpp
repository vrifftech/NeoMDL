#include <model/Lighting.hpp>

#include <algorithm>
#include <cmath>

namespace neomdl::model {
namespace {
bool finite(Vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
}
bool validFog(const FogSettings& f) noexcept {
    // Scene::SetFogRange (K1 Desktop 0x3f8dd0 / K2 Desktop 0xca53e) retains both
    // endpoints verbatim. Equal/reversed intervals reach DoFog unchanged.
    return static_cast<unsigned>(f.mode)<=static_cast<unsigned>(FogMode::ExponentialSquared) &&
        finite(f.color) && f.color.x>=0 && f.color.y>=0 && f.color.z>=0 &&
        std::isfinite(f.start) && std::isfinite(f.end) && std::isfinite(f.density) &&
        f.start>=0 && f.end>=0 && f.density>=0;
}
FogSettings fogForModel(const FogSettings& scene,bool modelFog) noexcept {
    auto result=scene;
    if(result.respectModelFlag && !modelFog) result.mode=FogMode::Disabled;
    return result;
}
} // namespace neomdl::model
