#pragma once

#include <model/Model.hpp>
#include <neoshared/bwm/BwmFile.hpp>

namespace neomdl {

// CSWRoomSurfaceMesh constructor: the first 32 surfacemat rows whose
// WalkCheck value equals 1 form the room-membership query material mask.
std::uint32_t engineWalkableMaterialMask(const std::vector<std::uint8_t>& surfaceTable);

// CSWRoomSurfaceMeshHitInfo. ClearOutput deliberately retains face, mask and
// direction: SceneHitCheck reuses this record across its ordered room list.
struct RoomSurfaceHitInfo {
    std::uint32_t surfaceMask{0xffffffffu},directionMask{};
    bool hit{};
    std::int32_t face{-1};
    model::Vec3 position{};
    std::uint32_t surface{};
    void clearOutput() noexcept;
};

// CheckAABBAll: authored world-space WOK triangles, engine edge retry and
// endpoint output. The all-surface mask is the constructor's 32-bit sum.
bool engineRoomHitAll(const neoshared::bwm::BwmFile& mesh,
                      model::Vec3 start,model::Vec3 end,RoomSurfaceHitInfo& info);

// CSWSArea::GetRoom/ComputeHeight use this walkable query with a retained
// hit record and take the first room whose engine query succeeds.
bool engineRoomHitWalkable(const neoshared::bwm::BwmFile& mesh,
    model::Vec3 start,model::Vec3 end,RoomSurfaceHitInfo&,std::uint32_t walkableMaterials);
float engineRoomFaceHeight(const neoshared::bwm::BwmFile&,std::int32_t face,model::Vec3 point);

// CSWSArea::GetRoomIndex's per-room query. Caller visits rooms in LYT order
// and selects the first true result. Coordinates are engine world coordinates,
// not translated MDL vertices or an approximate XY room containment test.
bool engineRoomContainsPoint(const neoshared::bwm::BwmFile& mesh,
                             model::Vec3 position,std::uint32_t walkableMaterials);

} // namespace neomdl
