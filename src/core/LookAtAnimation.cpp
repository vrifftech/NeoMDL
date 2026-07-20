#include "core/LookAtAnimation.hpp"
#include <algorithm>
#include <cmath>

namespace neomdl {
namespace {
namespace m=model;
m::Quaternion product(m::Quaternion a,m::Quaternion b) noexcept {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
float angle(float y,float x) noexcept {
    return float(std::atan2(double(y),double(x)));
}
float yaw(m::Quaternion q) noexcept {
    const float y=-2.0f*(q.x*q.y-q.w*q.z),x=1.0f-2.0f*(q.x*q.x+q.z*q.z);
    return y==0.0f&&x==0.0f?0.0f:angle(y,x)*57.2957763671875f;
}
float pitch(m::Quaternion q) noexcept {
    const float a=2.0f*(q.x*q.y-q.w*q.z),b=1.0f-2.0f*(q.x*q.x+q.z*q.z);
    return angle(2.0f*(q.y*q.z+q.w*q.x),std::sqrt(a*a+b*b))*57.2957763671875f;
}
m::Quaternion axisRadians(m::Vec3 axis,float radians) noexcept {
    if(radians==0.0f)return {};
    const float half=radians*0.5f,s=std::sin(half);return {axis.x*s,axis.y*s,axis.z*s,std::cos(half)};
}
m::Quaternion yawPitchRoll(float horizontal,float vertical,float roll) noexcept {
    constexpr float radians=0.01745329238474369f;
    return product(product(axisRadians({0,0,1},horizontal*radians),axisRadians({1,0,0},vertical*radians)),
        axisRadians({0,1,0},roll*radians));
}
float absolute(float value) noexcept {return value<0.0f?-value:value;}
float arc(float value,bool k1) noexcept {const auto a=absolute(value);return k1?(a<89.0f?a:89.0f):(a>89.0f?89.0f:a);}
float clamp(float value,float limit,bool k1) noexcept {
    // Engine SSE MAXSS/MINSS return their second operand for unordered.
    if(k1) {value=value>-limit?value:-limit;return value<limit?value:limit;}
    value=value<-limit?-limit:value;return value>limit?limit:value;
}
std::string lower(std::string value) {for(auto& c:value)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return value;}
}
bool LookAtAnimation::begin(model::AnimationRuntime& runtime,const model::Model& model,
    const std::string& sourceBone,LookAtTargetResolver target,float horizontal,float vertical,
    float transition,float maximumDistance) {
    const auto name=lower(sourceBone);
    const auto node=std::find_if(model.nodes.begin(),model.nodes.end(),[&](const auto& n){return lower(n.name)==name;});
    if(node==model.nodes.end()||!target()) {end();return false;}
    sourceNode_=std::size_t(node-model.nodes.begin());target_=std::move(target);
    const bool k1=model.game==model::GameVersion::Kotor1;
    horizontal_=arc(horizontal,k1);vertical_=arc(vertical,k1);distance_=absolute(maximumDistance);
    deactivated_=false;ending_=false;
    runtime.beginLookAtAnimation(sourceNode_,absolute(transition));return true;
}
void LookAtAnimation::control(model::AnimationRuntime& runtime,const model::Model& model,
    const model::EngineFrame& owner) {
    if(!target_)return;
    if(!runtime.lookAtAnimationActive()) {clear();return;}
    const auto target=target_();
    if(!target)deactivated_=true;
    else {
        const float x=owner.position.x-target->ownerPosition.x,y=owner.position.y-target->ownerPosition.y,
            z=owner.position.z-target->ownerPosition.z;
        const float distance=std::sqrt(x*x+y*y+z*z);
        if(distance>distance_)deactivated_=true;
    }
    const auto current=runtime.pose().orientations[sourceNode_];
    if(deactivated_) {
        if(!ending_) {
            ending_=true;
            runtime.controlLookAtAnimation(current,model.game==model::GameVersion::Kotor1?current:model::Quaternion{},true);
        }
        return;
    }
    const auto source=m::enginePartWorld(model,runtime.pose(),owner,sourceNode_);
    const auto forward=engineRotate(source.orientation,{0,1,0});
    auto direction=model::Vec3{target->part.position.x-source.position.x,
        target->part.position.y-source.position.y,target->part.position.z-source.position.z};
    const auto horizontalRotation=engineRotationArc({forward.x,forward.y,0},{direction.x,direction.y,0});
    const float horizontal=clamp(yaw(product(horizontalRotation,current)),horizontal_,model.game==model::GameVersion::Kotor1);
    direction=engineNormalize(direction);
    const float vertical=clamp(pitch(axisRadians({1,0,0},std::asin(direction.z))),vertical_,model.game==model::GameVersion::Kotor1);
    const auto desired=product(yawPitchRoll(horizontal,0,0),yawPitchRoll(0,vertical,0));
    runtime.controlLookAtAnimation(current,desired,false);
}
} // namespace neomdl
