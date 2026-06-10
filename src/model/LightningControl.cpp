#include <model/LightningControl.hpp>
#include <model/Emitter.hpp>
#include <model/EngineArithmetic.hpp>
#include <cmath>
#include <cstdint>

namespace neomdl::model {
namespace {
Vec3 add(Vec3 a,Vec3 b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,float b) {return {a.x*b,a.y*b,a.z*b};}
float magnitude(Vec3 a) {return std::sqrt((a.x*a.x+a.y*a.y)+a.z*a.z);}

Quaternion axisAngle(Vec3 axis,float angle) {
    if(angle==0) return {};
    const float half=angle*0.5f;
    const float cosine=std::cos(half);
    const float sine=std::sin(half);
    return {axis.x*sine,axis.y*sine,axis.z*sine,cosine};
}
Vec3 rotate(Vec3 p,Quaternion q) {
    return engineQuaternionRotate(q,p);
}
Vec3 cubic(Vec3 a,Vec3 b,Vec3 c,Vec3 d,float t) {
    // MINSS has its second operand's unordered behavior, unlike fmin.
    t=1.0f<t ? 1.0f:t;
    const float u=1.0f-t;
    const float aWeight=(u*u)*u;
    const float tripleT=t*3.0f;
    const float bWeight=(tripleT*u)*u;
    const float cWeight=(t*tripleT)*u;
    const float dWeight=(t*t)*t;
    return add(add(add(mul(a,aWeight),mul(b,bWeight)),mul(c,cWeight)),mul(d,dWeight));
}
Vec3 derivative(Vec3 a,Vec3 b,Vec3 c,Vec3 d,float t) {
    t=1.0f<t ? 1.0f:t;
    const float u=1.0f-t, tripleT=t*3.0f;
    return add(add(add(mul(a,(-3.0f*u)*u),mul(b,(3.0f*u)*(1.0f-tripleT))),
        mul(c,(2.0f-tripleT)*tripleT)),mul(d,t*tripleT));
}
std::size_t countFor(Vec3 source,Vec3 target,const NodeEmitterState& state) {
    return static_cast<std::size_t>(engineIntAdd(engineFloatToInt(
        magnitude(sub(target,source))*state.controlPointCount+0.5f),2));
}
void sizeArrays(EngineLightningControl& control,std::size_t count) {
    control.base.resize(count);control.baseTangents.resize(count);
    control.previous.resize(count);control.current.resize(count);
    control.previousTangents.resize(count);control.currentTangents.resize(count);
}
void baseCurve(EngineLightningControl& control,Vec3 source,Vec3 target,
    Vec3 localSourceForward,Vec3 targetForward,Vec3 tangentDelta,float tangentLength) {
    control.base.front()=source;control.base.back()=target;
    control.baseTangents.front()=mul(localSourceForward,tangentLength);
    control.baseTangents.back()=mul(engineLightningNormalize(tangentDelta),tangentLength);
    const auto b=add(source,mul(localSourceForward,tangentLength));
    const auto c=add(target,mul(targetForward,tangentLength));
    for(std::size_t i=1;i+1<control.base.size();++i) {
        const float t=static_cast<float>(i)/static_cast<float>(control.base.size()-1);
        control.base[i]=cubic(source,b,c,target,t);
        control.baseTangents[i]=mul(engineLightningNormalize(derivative(source,b,c,target,t)),tangentLength);
    }
}
float signedHundredths(std::int32_t divisor,ParticleRandom& random) {
    const auto remainder=engineSignedRemainder(static_cast<std::int32_t>(random.next()),divisor);
    const float value=static_cast<float>(remainder)*0.01f;
    return random.next()&1u ? -value:value;
}
}

Vec3 engineLightningNormalize(Vec3 value) noexcept {


    const float length=magnitude(value);
    if(static_cast<double>(length)<1.0e-9) return {1,0,0};
    return mul(value,1.0f/length);
}
Vec3 engineQuaternionAxis(Quaternion q,unsigned axis) noexcept {
    // K1Desktop xdir 0x3ed2ce, ydir 0x3de358, zdir 0x3de2e8;
    // the other three executables inline these same raw column expressions.
    if(axis==0) return {1.0f-2.0f*(q.y*q.y+q.z*q.z),
        2.0f*(q.x*q.y+q.w*q.z),2.0f*(q.x*q.z-q.w*q.y)};
    if(axis==1) return {2.0f*(q.x*q.y-q.w*q.z),
        1.0f-2.0f*(q.x*q.x+q.z*q.z),2.0f*(q.y*q.z+q.w*q.x)};
    return {2.0f*(q.x*q.z+q.w*q.y),2.0f*(q.y*q.z-q.w*q.x),
        1.0f-2.0f*(q.x*q.x+q.y*q.y)};
}
Quaternion engineQuaternionAxisAngle(Vec3 axis,float radians) noexcept {


    if(radians==0.0f) return {};
    return axisAngle(engineLightningNormalize(axis),radians);
}
Quaternion engineQuaternionRotationArc(Vec3 from,Vec3 to) noexcept {
    from=engineLightningNormalize(from);
    to=engineLightningNormalize(to);
    const float dot=(from.x*to.x+from.y*to.y)+from.z*to.z;
    if(dot<=-1.0f) return {1,0,0,0};
    const float sum=dot+1.0f;
    const float scale=std::sqrt(sum+sum);
    return {(from.y*to.z-from.z*to.y)/scale,
        (from.z*to.x-from.x*to.z)/scale,(from.x*to.y-from.y*to.x)/scale,scale*0.5f};
}
Vec3 engineLightningYawPitchRollRotate(Vec3 value,float yawDegrees,
    float pitchDegrees,float rollDegrees) noexcept {
    // YawPitchRoll multiplies Z(yaw), X(pitch), then Y(roll).
    constexpr float degrees=0.01745329238474369f;
    const auto yaw=axisAngle({0,0,1},yawDegrees*degrees);
    const auto pitch=axisAngle({1,0,0},pitchDegrees*degrees);
    const auto roll=axisAngle({0,1,0},rollDegrees*degrees);
    return rotate(value,engineQuaternionProduct(engineQuaternionProduct(yaw,pitch),roll));
}
Vec3 engineLightningAxisRotate(Vec3 value,Vec3 axis,float radians) noexcept {
    // Quaternion(Vector,float), K2Desktop 317276: zero angle returns identity
    // before evaluating/normalizing the axis.
    return rotate(value,engineQuaternionAxisAngle(axis,radians));
}
Vec3 engineLightningArcRotate(Vec3 value,Vec3 from,Vec3 to) noexcept {
    // RotationArc, K2Desktop 314e16. Its antiparallel case is the fixed X axis.
    return rotate(value,engineQuaternionRotationArc(from,to));
}
void engineInitializeLightningControl(EngineLightningControl& control,
    Vec3 source,Vec3 target,Vec3 localSourceForward,Vec3 targetForward,
    const NodeEmitterState& state) {
    // AnimateParticles initializes current/base, leaving previous arrays zero.
    control={};sizeArrays(control,countFor(source,target,state));
    baseCurve(control,source,target,localSourceForward,targetForward,sub(target,source),state.tangentLength);
    control.current=control.base;control.currentTangents=control.baseTangents;
    auto direction=sub(target,source);
    const float distance=magnitude(direction);
    if(distance!=0) direction=mul(direction,1.0f/distance);
    control.radial={-direction.y,direction.z,-direction.x};
}
void engineRefreshLightningControl(EngineLightningControl& control,
    Vec3 source,Vec3 target,Vec3 localSourceForward,Vec3 worldSourceForward,
    Vec3 targetForward,const NodeEmitterState& state,ParticleRandom& random,
    bool child) {


    const auto count=countFor(source,target,state);
    if(control.base.size()!=count) {
        const auto baseSource=control.base.front(),baseTarget=control.base.back();
        sizeArrays(control,count);
        baseCurve(control,baseSource,baseTarget,localSourceForward,targetForward,sub(target,source),state.tangentLength);
    }
    const auto radius=engineFloatToInt(state.controlPointRadius*100.0f);
    const auto spread=engineFloatToInt(state.tangentSpread*100.0f);
    for(std::size_t i=0;i<count;++i) {
        control.previous[i]=control.current[i];
        control.previousTangents[i]=control.currentTangents[i];
        if(i==0 || i+1==count) {
            control.current[i]=control.base[i];
            control.currentTangents[i]=mul(i==0 ? worldSourceForward:
                engineLightningNormalize(sub(target,source)),state.tangentLength);
        } else {
            if(radius!=0) {
                const float offset=signedHundredths(radius,random);
                const float angle=static_cast<float>(random.next()%360u);
                control.current[i]=add(control.base[i],engineLightningYawPitchRollRotate(
                    mul(control.radial,offset),0,angle,0));
            } else control.current[i]=control.base[i];
            if(spread!=0) {
                const float yaw=signedHundredths(spread,random);
                const float pitch=signedHundredths(spread,random);
                control.currentTangents[i]=engineLightningYawPitchRollRotate(
                    control.baseTangents[i],yaw,pitch,0);
            } else control.currentTangents[i]=control.baseTangents[i];
        }
        // Branch controls do not interpolate their positions; the tangent
        // previous array retains the prior value (K2Desktop 121eab).
        if(child) control.previous[i]=control.current[i];
    }
}
Vec3 engineEvaluateLightningControl(const EngineLightningControl& control,
    std::size_t particle,std::size_t particleCount,float blend,bool previousOnly) {
    // Match the incremental segment cursor, including the engine final-segment
    // boundary correction; multiplying t by the segment count rounds differently.
    const float step=1.0f/static_cast<float>(control.previous.size()-1);
    const float position=static_cast<float>(particle)/static_cast<float>(particleCount-1);
    float lower=0,upper=step;
    std::size_t end=1;
    while(position>upper) {
        lower=upper;upper+=step;++end;
        if(previousOnly && end>=control.previous.size()) {lower-=step;upper-=step;--end;break;}
    }
    const float t=(position-lower)/(upper-lower);
    const auto sample=[&](const std::vector<Vec3>& previous,const std::vector<Vec3>& current,std::size_t i) {
        return previousOnly ? previous[i]:add(mul(previous[i],1.0f-blend),mul(current[i],blend));
    };
    const auto a=sample(control.previous,control.current,end-1);
    const auto d=sample(control.previous,control.current,end);
    const auto b=previousOnly ? add(a,control.previousTangents[end-1]):
        add(add(a,mul(control.previousTangents[end-1],1.0f-blend)),
            mul(control.currentTangents[end-1],blend));
    const auto td=sample(control.previousTangents,control.currentTangents,end);
    return cubic(a,b,sub(d,td),d,t);
}
Vec3 engineEvaluateLightningLastControl(const EngineLightningControl& control,
    std::size_t particle,std::size_t particleCount,float blend) {
    // Target-only endpoint refresh, K2Desktop 123cf4: this interval uses one
    // division for its lower edge, rather than the ordinary incremental cursor.
    const auto end=control.previous.size()-1;
    const float lower=static_cast<float>(end-1)/static_cast<float>(end);
    const float position=static_cast<float>(particle)/static_cast<float>(particleCount-1);
    const float t=(position-lower)/(1.0f-lower);
    const auto sample=[&](const std::vector<Vec3>& previous,const std::vector<Vec3>& current,std::size_t i) {
        return add(mul(previous[i],1.0f-blend),mul(current[i],blend));
    };
    const auto a=sample(control.previous,control.current,end-1);
    const auto d=sample(control.previous,control.current,end);
    const auto b=add(add(a,mul(control.previousTangents[end-1],1.0f-blend)),
        mul(control.currentTangents[end-1],blend));
    const auto td=sample(control.previousTangents,control.currentTangents,end);
    return cubic(a,b,sub(d,td),d,t);
}
}
