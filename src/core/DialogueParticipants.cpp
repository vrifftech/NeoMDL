#include "core/DialogueParticipants.hpp"
#include "core/Engine2DA.hpp"
#include <cmath>

namespace neomdl {
DialogueHeadTracking dialogueHeadTracking(const neo2da::TwoDAFile& table,int row,
        const std::string& overrideBone) {
    DialogueHeadTracking result;int enabled;
    result.enabled=engine2da::getInt(table,row,"HEADTRACK",enabled)&&enabled!=0;
    std::string bone;float arc;
    if(!overrideBone.empty())result.bone=overrideBone;
    else if(engine2da::getString(table,row,"HEADBONE",bone))result.bone=std::move(bone);
    if(engine2da::getFloat(table,row,"HEAD_ARC_H",arc))result.horizontal=arc;
    if(engine2da::getFloat(table,row,"HEAD_ARC_V",arc))result.vertical=arc;
    return result;
}
DialogueLookAtResult dialogueLookAt(DialogueParticipantState& state,
        const DialogueParticipantInputs& inputs,const DialogueHeadTracking& head,
        bool sourceModelPresent,std::optional<HitOwnerId> target,bool targetModelPresent,
        model::Vec3 source,model::Vec3 destination,float distance,model::GameVersion) {
    if(inputs.lookAtSuppressed||state.lookTarget==target||inputs.objectLookAtFlag||!sourceModelPresent)
        return {};
    if(!head.enabled||!target) {
        state.lookTarget.reset();return {DialogueLookAtOperation::End,false};
    }
    if(!targetModelPresent)return {};
    const float x=destination.x-source.x,y=destination.y-source.y,z=destination.z-source.z;
    const float squared=(x*x+y*y)+z*z;
    if(squared>distance*distance)return {};
    state.lookTarget=target;
    // BeginLookAtAnimate is void: unresolved bones do not change this success.
    return {DialogueLookAtOperation::Begin,true};
}
namespace {
model::Vec3 selectedDirection(model::Vec3 current,model::Vec3 source,
        model::Vec3 destination,float horizontal,bool followed,model::GameVersion game) {
    const auto desired=engineNormalize({destination.x-source.x,destination.y-source.y,destination.z-source.z});
    if(!followed)return desired;
    const auto forward=engineNormalize(current);
    float dot=desired.x*forward.x+desired.y*forward.y+desired.z*forward.z;
    if(dot>1.0f)dot=1.0f;else if(dot<-1.0f)dot=-1.0f;
    const float radians=game==model::GameVersion::Kotor2
        ?static_cast<float>(std::acos(double(dot))):std::acos(dot);
    float angle=radians*57.2957763671875f;
    if(!std::isfinite(angle))angle=0.0f;
    if(angle>horizontal) {
        float excess=angle-(horizontal+1.0f);
        const model::Vec3 cross{forward.y*1.0f-forward.z*0.0f,
            forward.z*0.0f-forward.x*1.0f,forward.x*0.0f-forward.y*0.0f};
        const float side=desired.x*cross.x+desired.y*cross.y+desired.z*cross.z;
        if(side>0.0f)excess=-excess;
        const float half=(excess*0.01745329238474369f)*0.5f;
        return engineRotate({0,0,std::sin(half),std::cos(half)},forward);
    }
    return current;
}
model::Quaternion orientation(model::Vec3 direction) {
    float yaw=0.0f;
    if(direction.x!=0.0f||direction.y!=0.0f)
        yaw=static_cast<float>(std::atan2(double(-direction.x),double(direction.y)))*57.2957763671875f;
    const float half=(yaw*0.01745329238474369f)*0.5f;
    return {0,0,std::sin(half),std::cos(half)};
}
}
DialogueParticipantFacing dialogueParticipantFacing(model::Vec3 current,model::Vec3 source,
        model::Vec3 target,float horizontal,bool followed,model::GameVersion game) {
    const auto selected=selectedDirection(current,source,target,horizontal,followed,game);
    return {engineNormalize(selected),orientation(selected)};
}
} // namespace neomdl
