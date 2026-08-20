#pragma once
#include "core/LightScene.hpp"
#include "core/SceneQueries.hpp"
#include <array>
#include <vector>

namespace neomdl {

struct LensFlareSettings {
    bool enabled{true};             // renderlensflares, engine default 1
    bool hitCheck{true};            // hitcheckflare, default 1
    bool cameraToLight{true};       // reverseflarehit, default 1
    float visibilitySpeed{3.0f};   // flarespeed
    float sizeScale{0.1f};         // lensflaresize
};
struct LensFlareCamera {
    model::Vec3 position{};
    model::Quaternion orientation{}; // camera-to-world, not its view matrix
};
struct LensFlareQuad {
    std::uint64_t light{};
    std::size_t instanceIndex{},element{};
    std::string texture;
    model::Vec2 center{}; // eye-plane center, z is exactly -2
    float halfSize{};
    std::array<std::uint8_t,4> color{{0,0,0,255}};
};
struct LensFlareFrame {
    std::vector<LensFlareQuad> quads;
    std::vector<std::string> diagnostics;
    std::size_t queried{},occluded{},outsideRadius{};
};




LensFlareFrame updateEngineLensFlares(LightScene& lights,const SceneQueries& queries,
    const LensFlareCamera& camera,float elapsedSeconds,const LensFlareSettings& settings = {});
void buildLensFlareQuad(const LensFlareQuad& flare,model::Mesh& destination);

} // namespace neomdl
