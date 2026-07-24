#pragma once
#include "core/SceneQueries.hpp"
#include <string>

namespace neo2da { class TwoDAFile; }
namespace neomdl {
struct DialogueHeadTracking {
    bool enabled{};
    std::string bone{"hturn_g"};
    float horizontal{40.0f},vertical{30.0f};
};
DialogueHeadTracking dialogueHeadTracking(const neo2da::TwoDAFile&,int appearance,
                                         const std::string& boneOverride={});
struct DialogueParticipantInputs {
    float walkRate{}; // live client movement rate; not appearance WALKDIST
    bool preventOrientation{},preventHeadFollow{}; // GUI exclusion lists
    bool lookAtSuppressed{},objectLookAtFlag{}; // creature +140/object +54 on i386
};
struct DialogueParticipantState {
    std::optional<HitOwnerId> lookTarget;
    // CSWCObject retains a full logical direction, whereas its Gob receives yaw.
    std::optional<model::Vec3> direction;
};
enum class DialogueLookAtOperation {None,End,Begin};
struct DialogueLookAtResult {
    DialogueLookAtOperation operation{DialogueLookAtOperation::None};
    bool success{};
};
DialogueLookAtResult dialogueLookAt(DialogueParticipantState&,
    const DialogueParticipantInputs&,const DialogueHeadTracking&,
    bool sourceModelPresent,std::optional<HitOwnerId> target,bool targetModelPresent,
    model::Vec3 sourcePosition,model::Vec3 targetPosition,float maximumDistance,
    model::GameVersion);
// UpdateShotCamera's limited body turn is used only when LookAt succeeds.
struct DialogueParticipantFacing {model::Vec3 direction;model::Quaternion orientation;};
DialogueParticipantFacing dialogueParticipantFacing(model::Vec3 current,model::Vec3 sourcePosition,
    model::Vec3 targetPosition,float horizontalArc,bool headFollowSucceeded,
    model::GameVersion);
} // namespace neomdl
