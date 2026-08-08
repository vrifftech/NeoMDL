#pragma once

#include "core/WorldAnimation.hpp"
#include "core/ResourceContext.hpp"
#include "core/SceneQueries.hpp"

namespace neogff {class GffStruct;}
namespace neomdl {

struct TrackModelDefinition {
    std::string model;
    bool rotating{true}; // Load's BYTE default is one, in all four executables.
};
struct TrackFollowerDefinition {
    TrackFollowerKind kind{TrackFollowerKind::Enemy};
    std::size_t sourceIndex{};
    std::vector<TrackModelDefinition> models;
    std::string track, camera;
    bool cameraRotating{};
    std::int32_t loops{1};
    model::Vec3 startOffset{};
};
struct MiniGameDefinition {
    std::uint32_t type{};
    // Player first, then successfully admitted Enemies in GFF list order.
    std::vector<TrackFollowerDefinition> followers;
};
std::optional<MiniGameDefinition> readTrackMiniGame(const neogff::GffStruct& area,
    const neoshared::bif::JobControl& job=resourceLoadJob());

struct TrackModelAttachment {
    std::size_t instance{};
    std::uint8_t mode{}; // Attach=0, Translate=1, Rotate=2
    model::Vec3 offset{};
    // Translate's constructor does not initialize this engine memory. A host
    // supplying a nonzero Control offset must also supply its captured flag.
    std::optional<bool> worldSpaceOffset;
};
struct TrackFollowerInstance {
    std::size_t definition{}, track{}, hook{};
    std::vector<TrackModelAttachment> models;
    std::optional<TrackModelAttachment> camera;
    model::Vec3 initialHook{};
};
// Attachment Control consumes a post-physics offset. This function does not
// invent input, acceleration, collision, inertia, or tunnel state.
HitPartTransform trackAttachmentFrame(HitPartTransform hook,const TrackModelAttachment&);

} // namespace neomdl
