#include <model/Emitter.hpp>
#include <model/EngineArithmetic.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace neomdl::model {
namespace {
constexpr float pi=3.14159265358979323846f, tau=2*pi;
std::string lower(std::string s) {
    for (auto& c:s) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return s;
}
bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
bool finite(const Mat4& m) {
    return std::all_of(m.values.begin(),m.values.end(),[](float v){return std::isfinite(v);});
}
float bound(float v,float lo,float hi,float fallback=0) {
    return std::isfinite(v) ? std::clamp(v,lo,hi) : fallback;
}
Vec3 add(Vec3 a,Vec3 b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,float s) {return {a.x*s,a.y*s,a.z*s};}
float dot(Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float length(Vec3 a) {return std::sqrt(dot(a,a));}
Vec3 unit(Vec3 v,Vec3 fallback={0,0,1}) {
    const float n=length(v);return std::isfinite(n)&&n>1e-10f ? mul(v,1/n) : fallback;
}
Vec3 position(const Mat4& m) {return {m.values[12],m.values[13],m.values[14]};}
Vec3 axis(const Mat4& m,unsigned n) {return unit({m.values[n*4],m.values[n*4+1],m.values[n*4+2]},n==0?Vec3{1,0,0}:n==1?Vec3{0,1,0}:Vec3{0,0,1});}
Vec3 rotate(Vec3 v,Vec3 a,float angle) {
    const float c=std::cos(angle),s=std::sin(angle);
    return add(add(mul(v,c),mul(cross(a,v),s)),mul(a,dot(a,v)*(1-c)));
}
void particleOrientationAxes(EmitterParticle& particle) {
    particle.right=engineQuaternionAxis(particle.orientation,0);
    particle.up=engineQuaternionAxis(particle.orientation,1);
}
bool hasParticleTarget(const ParticleEnvironment* environment,bool engine) {
    return environment && (engine?environment->engineTarget.has_value():environment->target.has_value());
}
Vec3 particleTargetPosition(const ParticleEnvironment& environment,bool engine) {
    return engine?environment.engineTarget->position:position(*environment.target);
}
Vec3 particleTargetForward(const ParticleEnvironment& environment,bool engine) {
    return engine?engineQuaternionAxis(environment.engineTarget->orientation,2):axis(*environment.target,2);
}
void applyDeadSpace(const EmitterNodeData& data,EmitterParticle& p,
    const ParticleEnvironment* environment,bool engine) {
    if(data.deadSpace==0 || !environment || !environment->cameraPosition ||
       (!engine && (!std::isfinite(data.deadSpace) || !finite(*environment->cameraPosition)))) return;
    // K1 Desktop 0x2ea225 / K2 Desktop 0x1128f4 use the rotated particle Y axis.
    // The correction is rebuilt from the original orientation each iteration;
    // the comparison uses cos(double(1.57079632)-deadSpace), while the rotation
    // angle uses float32 pi/2 and the dot product itself, not acos(dot).
    const auto cameraDelta=sub(p.position,*environment->cameraPosition);
    const Vec3 cameraDirection=engine?engineLightningNormalize(cameraDelta)
                                      :unit(cameraDelta,{});
    const Vec3 initialRight=p.right,initialUp=p.up,initialDirection=p.direction;
    const Quaternion initialOrientation=p.orientation;
    Vec3 turnAxis;
    if(!engine) turnAxis=unit(cross(cameraDirection,initialUp),{});
    float current=dot(cameraDirection,initialUp);
    const double threshold=std::cos(1.57079632-static_cast<double>(data.deadSpace));
    bool correct=threshold>std::fabs(static_cast<double>(current));
    while(correct) {
        const float angle=current<0 ? data.deadSpace+pi*0.5f-current
            : (engine?data.deadSpace+(current-pi*0.5f):data.deadSpace+current-pi*0.5f);
        if(engine) {
            // Both the correction axis and multiplied orientation use the
            // original particle quaternion. Quaternion(Vector,float) supplies
            // the engine axis normalization, including its short-axis rule.
            const auto engineAxis=cross(cameraDirection,initialUp);
            p.orientation=engineQuaternionProduct(engineQuaternionAxisAngle(engineAxis,angle),
                initialOrientation);
            particleOrientationAxes(p);
        } else {
            p.right=rotate(initialRight,turnAxis,angle);
            p.up=rotate(initialUp,turnAxis,angle);
            p.direction=rotate(initialDirection,turnAxis,angle);
        }
        current=std::fabs(dot(cameraDirection,p.up));
        correct=threshold>static_cast<double>(current);
    }
}
Vec3 rotationDelta(Vec3 v,const Mat4& old,const Mat4& now) {
    return add(add(mul(axis(now,0),dot(v,axis(old,0))),mul(axis(now,1),dot(v,axis(old,1)))),
               mul(axis(now,2),dot(v,axis(old,2))));
}
int integer(float v) {return engineFloatToInt(v);}
float interpolate(float a,float b,float t) {return a*(1-t)+b*t;}
struct LifetimeSegment {unsigned first{},second{};float weight{};};
LifetimeSegment segment(const NodeEmitterState& s,const EmitterParticle& p,
    std::optional<float> normalizedPosition={}) {
    if(!(s.lifeExpectancy>=0)) return {0,0,0};
    const float t=normalizedPosition ? *normalizedPosition:
        p.age*(s.lifeExpectancy>0 ? 1.0f/s.lifeExpectancy : 0.01f);
    if(s.percentStart==255.0f) return {0,2,t};
    if(t>=s.percentEnd) return {2,2,0};
    if(t>=s.percentMid) {
        const float d=s.percentEnd-s.percentMid;
        return {1,2,(t-s.percentMid)/d};
    }
    if(t>=s.percentStart) {
        const float d=s.percentMid-s.percentStart;
        return {0,1,(t-s.percentStart)/d};
    }
    return {0,0,0};
}
float evaluate(const std::array<float,3>& v,LifetimeSegment s) {
    return s.first==s.second ? v[s.first] : interpolate(v[s.first],v[s.second],s.weight);
}
void initialFrame(const EmitterNodeData& data,const NodeEmitterState& s,EmitterParticle& p,
    ParticleRandom& rng) {
    p.frame=integer(s.frameStart);
    if((data.rawWords.back()&emitter_flags::RandomFrame)!=0) {
        // The original applies abs AFTER adding one, even for descending ranges.
        const int span=engineIntAbs(engineIntAdd(integer(s.frameEnd-s.frameStart),1));
        if(span!=0) p.frame=integer(static_cast<float>(engineSignedRemainder(
            static_cast<std::int32_t>(rng.next()),span))+s.frameStart);
    }
}
Vec3 randomParticlePosition(const NodeEmitterState& s,const Mat4& world,float scale,
    ParticleRandom& rng,const EngineFrame* engineFrame=nullptr) {
    Vec3 result=position(world);
    const int width=integer(s.xSize*100*scale);
    const int height=integer(s.ySize*100*scale);
    if(width || height) {
        const auto offset=[&](int amount) {
            if(amount<=0) return 0;
            const auto a=rng.next(),b=rng.next();
            const int divisor=integer(static_cast<float>(amount)*0.5f);
            // Both Desktop executables use 32-bit IMUL followed by signed IDIV.
            // K1 loaded rand values can overflow the signed product.
            const auto product=static_cast<std::int32_t>(a*b);
            return engineSignedRemainder(product,divisor);
        };
        int x=offset(width),y=offset(height);
        if(rng.next()%2u) x=-x;
        if(rng.next()%2u) y=-y;
        const auto right=engineFrame?engineQuaternionAxis(engineFrame->orientation,0):axis(world,0);
        const auto up=engineFrame?engineQuaternionAxis(engineFrame->orientation,1):axis(world,1);
        result=add(add(result,mul(right,static_cast<float>(x)*0.0001f)),
                                  mul(up,static_cast<float>(y)*0.0001f));
    }
    return result;
}
EmitterParticle initializeParticle(const EmitterNodeData& data,const NodeEmitterState& s,
    const Mat4& world,const Mat4& previous,float dt,float scale,ParticleRandom& rng,
    const ParticleEnvironment* environment,bool engine,
    const EngineFrame* engineFrame=nullptr,const EngineFrame* previousFrame=nullptr) {
    EmitterParticle p;p.lifetime=s.lifeExpectancy;p.sourceScale=scale;
    if(engine) {
        p.orientation=engineFrame->orientation;
        particleOrientationAxes(p);
        p.direction=engineQuaternionAxis(p.orientation,2);
    } else {p.right=axis(world,0);p.up=axis(world,1);p.direction=axis(world,2);}
    p.position=position(world);
    float dv=0;
    const float rv=s.randomVelocity;
    if(static_cast<double>(rv)>0.01) {
        const int count=integer(rv*100);
        int value=static_cast<int>(rng.next()%static_cast<unsigned>(count));
        if(rng.next()%2u) value=-value;
        dv=static_cast<float>(value)*0.01f;
    }
    p.position=randomParticlePosition(s,world,scale,rng,engineFrame);
    p.tail=p.position;
    if(data.rawWords.back()&emitter_flags::InheritVelocity) {
        const float inv=dt>0 ? 1.0f/dt : 0.01f;
        const float radius=length(sub(position(world),p.position));
        const auto turn=engine ? sub(engineQuaternionAxis(engineFrame->orientation,2),
            engineQuaternionAxis(previousFrame->orientation,2)) : sub(axis(world,2),axis(previous,2));
        const Vec3 inherited=add(engine?mul(mul(turn,radius),inv):mul(turn,radius*inv),
                                mul(sub(position(world),position(previous)),inv));
        p.inheritedSpeed=length(inherited);
        // inheritVelocity has an exact zero-magnitude branch, distinct from
        // normalize(Vector)'s 1e-9/+X rule used by targeted movement.
        p.inheritedDirection=engine ? (p.inheritedSpeed==0.0f?Vec3{}:
            mul(inherited,1.0f/p.inheritedSpeed)) : unit(inherited,{0,0,0});
    }
    if(s.spread!=0) {
        const int count=engineIntAdd(integer(s.spread*0.5f*100),1);
        const float tilt=static_cast<float>(engineSignedRemainder(
            static_cast<std::int32_t>(rng.next()),count))*0.01f;
        const float azimuth=static_cast<float>(rng.next()%628u)*0.01f;
        if(engine) {
            p.orientation=engineQuaternionProduct(engineQuaternionAxisAngle(
                engineQuaternionAxis(engineFrame->orientation,0),tilt),p.orientation);
            p.orientation=engineQuaternionProduct(engineQuaternionAxisAngle(
                engineQuaternionAxis(engineFrame->orientation,2),azimuth),p.orientation);
            particleOrientationAxes(p);
        } else {
            const auto orient=[&](Vec3 v) {return rotate(rotate(v,axis(world,0),tilt),axis(world,2),azimuth);};
            p.right=orient(p.right);p.up=orient(p.up);p.direction=orient(p.direction);
        }
        applyDeadSpace(data,p,environment,engine);
    }
    if(engine) p.direction=engineQuaternionAxis(p.orientation,2);
    p.velocity=mul(p.direction,s.velocity+dv);
    p.frame=integer(s.frameStart);
    return p;
}
int nextFrame(const NodeEmitterState& s,int f) {
    if(s.frameStart==s.frameEnd) return -1;
    const int n=engineIntAdd(f,s.frameStart>s.frameEnd?-1:1);
    return (s.frameStart>s.frameEnd ? static_cast<float>(n)<s.frameEnd : static_cast<float>(n)>s.frameEnd) ? -1 : n;
}
int previousFrame(const NodeEmitterState& s,int f) {
    if(s.frameStart==s.frameEnd) return -1;
    const int n=engineIntAdd(f,s.frameStart>s.frameEnd?1:-1);
    return (s.frameStart>s.frameEnd ? static_cast<float>(n)>s.frameStart : static_cast<float>(n)<s.frameStart) ? -1 : n;
}
} // namespace

float calculateK2EmitterRadius(const EmitterRadiusInput& input) noexcept {
    if(input.forceUnbounded) return 10000.0f;
    if(!input.state) return 0.0f;
    const auto& state=*input.state;
    if(input.targetPosition) return length(sub(*input.targetPosition,input.emitterPosition));

    const float gravity=state.mass*-9.81f;
    const float gravityAcceleration=gravity<0.0f?-gravity:gravity;
    const float windMagnitude=length(input.globalWind);
    const float life=state.lifeExpectancy;
    const float travel=((state.velocity+state.randomVelocity+windMagnitude)*life)+
        (0.5f*gravityAcceleration*life*life);

    // MAXSS returns its second operand on equality/unordered input.
    // K1 and K2 use different inner/outer operand orders.
    const auto maximum=[&](float start,float mid,float end) {
        const auto maxss=[](float a,float b){return a>b?a:b;};
        if(input.game!=GameVersion::Kotor1) return maxss(mid,maxss(end,start));
        return maxss(maxss(start,end),mid);
    };
    const float maximumX=maximum(state.sizeStart,state.sizeMid,state.sizeEnd);
    const float maximumY=maximum(state.sizeStartY,state.sizeMidY,state.sizeEndY);
    const float halfX=maximumX*0.5f;
    const float authoredHalfY=maximumY*0.5f;
    // All four CalculateRadius implementations select X for Y <= 0, including
    // exactly zero; unordered Y is retained by CMPNLE/FCSEL HI.
    const float halfY=authoredHalfY<=0.0f ? halfX:authoredHalfY;
    const float particleRadius=std::sqrt(halfX*halfX+halfY*halfY);
    const float sourceX=state.xSize/100.0f;
    const float sourceY=state.ySize/100.0f;
    const float sourceRadius=std::sqrt(sourceX*sourceX+sourceY*sourceY);
    float result=(travel+particleRadius+sourceRadius)*1.15f;

    for(const auto& object:input.attachedObjects) {
        const float candidate=length(sub(object.center,input.emitterPosition))+object.radius;
        result=input.game==GameVersion::Kotor1 ? (result>candidate?result:candidate)
                                               : (candidate>result?candidate:result);
    }
    return result;
}

std::vector<std::size_t> k2EmitterRoomMembership(
    Vec3 currentOwnerPosition,Vec3 previousOwnerPosition,
    const std::vector<std::size_t>& cachedOwnerRooms,
    const std::vector<EmitterSceneRoom>& rooms) {
    if(rooms.empty()) return {};
    const bool unchanged=currentOwnerPosition.x==previousOwnerPosition.x &&
        currentOwnerPosition.y==previousOwnerPosition.y &&
        currentOwnerPosition.z==previousOwnerPosition.z;
    if(unchanged && !cachedOwnerRooms.empty() && cachedOwnerRooms.front()<rooms.size())
        return {cachedOwnerRooms.front()};

    std::vector<std::size_t> result;
    result.reserve(rooms.size());
    for(std::size_t index=0;index<rooms.size();++index) {
        const auto& room=rooms[index];
        if(currentOwnerPosition.x>=room.minimumX &&
           currentOwnerPosition.y>=room.minimumY &&
           currentOwnerPosition.x<=room.maximumX &&
           currentOwnerPosition.y<=room.maximumY) result.push_back(index);
    }
    if(result.empty()) {
        result.resize(rooms.size());
        for(std::size_t index=0;index<rooms.size();++index) result[index]=index;
    }
    return result;
}

bool k2EmitterOutside(Vec3 center,float radius,
                      const std::vector<EmitterScenePlane>& planes) noexcept {
    for(const auto& plane:planes) {
        const float above=((plane.normal.x*center.x+plane.distance)+
            plane.normal.y*center.y)+plane.normal.z*center.z;
        if(above-radius>0.0f) return true;
    }
    return false;
}


static bool moveEmitterParticleImpl(EmitterParticle& p,const NodeEmitterState& s,const Mat4& old,
    const Mat4& now,std::uint32_t flags,float dt,const ParticleEnvironment* environment,
    bool engine,bool hasChunk,const EngineFrame* oldFrame=nullptr,const EngineFrame* currentFrame=nullptr,
    const EngineFrame* inheritanceFrame=nullptr) {
    const bool targeted=(flags&emitter_flags::PointToPoint)!=0;
    // Only the standard mover skips settled particles. Targeted paths keep
    // following the object even after a collision set the resting flag.
    if(p.stopped && !targeted) return true;
    // MoveFunc does not substitute a standard mover when a target is absent.
    if(targeted && (!hasParticleTarget(environment,engine) ||
        (!engine && !finite(*environment->target)))) return true;
    const auto power=[&](float base,float exponent) {
        // Both desktop movers load double pow.
        return engine
            ? static_cast<float>(std::pow(static_cast<double>(base),static_cast<double>(exponent)))
            : std::pow(base,exponent);
    };
    const Vec3 original=p.position;
    Vec3 workingPosition=p.position;
    if(flags&emitter_flags::Inherit) {
        if(engine) {
            const auto rotation=engineQuaternionProduct(currentFrame->orientation,
                engineQuaternionInverse(oldFrame->orientation));
            workingPosition=add(currentFrame->position,engineQuaternionRotate(rotation,
                sub(workingPosition,oldFrame->position)));
            p.velocity=engineQuaternionRotate(rotation,p.velocity);
        } else {
            workingPosition=add(position(now),rotationDelta(sub(workingPosition,position(old)),old,now));
            p.velocity=rotationDelta(p.velocity,old,now);
        }
    }
    if(engine && (!targeted || !(flags&emitter_flags::PointToPointBezier))) {
        const auto* owner=environment && environment->engineOwnerFrame
            ? &*environment->engineOwnerFrame : nullptr;
        // Standard: owner translation only. Gravity: sequential inverse cached
        // quaternion then current OWNER quaternion, preserving raw arithmetic.
        const auto rotateInherited=[&](Vec3 value) {
            return engineQuaternionRotate(owner->orientation,
                engineQuaternionRotate(engineQuaternionInverse(inheritanceFrame->orientation),value));
        };
        if((flags&(emitter_flags::InheritLocal|emitter_flags::Inherit))==emitter_flags::InheritLocal && owner) {
            const auto delta=sub(owner->position,inheritanceFrame->position);
            if(targeted) workingPosition=rotateInherited(sub(workingPosition,inheritanceFrame->position));
            workingPosition=add(workingPosition,delta);
            p.tail=add(p.tail,delta);
            if(targeted) {
                workingPosition=add(workingPosition,inheritanceFrame->position);
                p.velocity=rotateInherited(p.velocity);
            }
        }
        // This is independent of Local; authored combined flags run both.
        if((flags&emitter_flags::InheritPart) && (!targeted || owner)) {
            const auto delta=sub(currentFrame->position,inheritanceFrame->position);
            if(targeted) workingPosition=rotateInherited(sub(workingPosition,inheritanceFrame->position));
            workingPosition=add(workingPosition,delta);
            p.tail=add(p.tail,delta);
            if(targeted) {
                p.tail=add(rotateInherited(sub(p.tail,owner->position)),owner->position);
                workingPosition=add(workingPosition,inheritanceFrame->position);
                p.velocity=rotateInherited(p.velocity);
            }
        }
    } else if(!engine && !targeted && (flags&emitter_flags::InheritPart)) {
        const auto delta=sub(position(now),position(old));
        workingPosition=add(workingPosition,delta);p.tail=add(p.tail,delta);
    }
    if(!engine) p.position=workingPosition;
    const auto start=workingPosition;
    Vec3 predicted=add(start,mul(p.velocity,dt));
    if((flags&emitter_flags::AffectedByWind) && environment && environment->windDisplacement) {
        const auto wind=environment->windDisplacement(start,dt);
        if(!engine && !finite(wind)) throw ModelError("Particle scene returned non-finite wind displacement");
        predicted=add(predicted,wind);
    }
    if(targeted) {
        const auto target=particleTargetPosition(*environment,engine);
        if(flags&emitter_flags::PointToPointBezier) {
            // Packed order: advance freely, then blend toward a cubic whose
            // handles follow the source and TARGET orientations respectively.
            const float ratio=engine || s.lifeExpectancy!=0 ? p.age/s.lifeExpectancy:1.0f;
            const float t=engine ? (ratio>1.0f?1.0f:ratio)
                : (s.lifeExpectancy!=0 ? std::min(1.0f,ratio) : 1.0f);
            const float u=1-t, t3=t*t*t;
            const auto source=position(now);
            const auto sourceForward=engine?engineQuaternionAxis(currentFrame->orientation,2):axis(now,2);
            const auto handleA=add(source,mul(sourceForward,s.bezier2));
            const auto handleB=add(target,mul(particleTargetForward(*environment,engine),s.bezier3));
            const auto curveStart=add(mul(source,u*u*u),mul(handleA,3*t*u*u));
            const auto curve=engine
                ? add(add(curveStart,mul(handleB,3*t*t*u)),mul(target,t3))
                : add(curveStart,add(mul(handleB,3*t*t*u),mul(target,t3)));
            const float weight=power(t3,s.combineTime);
            if(!engine && (!finite(curve) || !std::isfinite(weight))) throw ModelError("Invalid particle Bezier inputs");
            predicted=add(mul(curve,weight),mul(predicted,1-weight));
        } else {
            const auto delta=sub(target,start);
            const float threshold=std::fabs(s.targetThreshold);
            if(dot(delta,delta)<=threshold*threshold) {
                p.age=s.lifeExpectancy;
                // Engine exits the ENTIRE gravity mover here: it leaves the
                // working position uncommitted, remaining particles unmoved,
                // and C/Qc unchanged. Earlier tail/velocity writes persist.
                return !engine;
            }
            const float drag=power(s.drag,dt);
            if(!engine && !std::isfinite(drag)) throw ModelError("Invalid particle target drag");
            const auto acceleration=mul(engine ? engineLightningNormalize(delta)
                                               : unit(delta,{0,0,0}),s.gravity);
            const auto velocityDelta=mul(acceleration,dt);
            p.velocity=add(mul(p.velocity,drag),velocityDelta);
            predicted=add(predicted,engine?mul(velocityDelta,dt):mul(acceleration,dt*dt));
            const auto travel=sub(predicted,start);
            // The engine caller tests exact vector equality before Normalize;
            // segmentproject separately tests squared length against zero.
            const float travelSquared=dot(travel,travel);
            if(engine) {
                if(travel.x!=0.0f || travel.y!=0.0f || travel.z!=0.0f)
                    p.direction=engineLightningNormalize(travel);
            } else if(travelSquared>1.0e-12f) p.direction=unit(travel);
            const float fraction=(engine?travelSquared!=0.0f:travelSquared>1.0e-12f)
                ? std::clamp(dot(delta,travel)/dot(travel,travel),0.0f,1.0f) : 0;
            const auto closest=add(start,mul(travel,fraction));
            const auto miss=sub(target,closest);
            if(dot(miss,miss)<=threshold*threshold) p.age=s.lifeExpectancy;
        }
    }
    p.position=add(predicted,mul(p.inheritedDirection,p.inheritedSpeed*dt));
    p.velocity.z+=s.mass*(-9.81f)*dt;
    p.inheritedSpeed=std::fabs(p.inheritedSpeed*(1-dt));
    if((flags&emitter_flags::Bounce) && environment && environment->trace) {
        // Engine P2P movers extend only the collision query for an authored
        // chunkName, even when its chunk Gob is absent. Standard does not.
        const auto collisionEnd=engine && targeted && hasChunk
            ? add(p.position,mul(sub(p.position,original),s.sizeStart)) : p.position;
        const auto hit=environment->trace(original,collisionEnd);
        if(hit) {
            if(!(engine && hit->engineNormal) && (!finite(hit->normal) ||
                (!hit->engineNormal&&dot(hit->normal,hit->normal)<1.0e-12f) ||
                !std::isfinite(hit->fraction) || hit->fraction<0 || hit->fraction>1))
                throw ModelError("Particle scene returned an invalid collision contact");
            if(hit->enginePosition) {
                if(!engine && !finite(*hit->enginePosition))throw ModelError("Non-finite engine particle hit position");
                p.position=*hit->enginePosition;
            }
            const auto n=hit->engineNormal?hit->normal:unit(hit->normal);
            const auto reflected=sub(p.velocity,mul(n,2*dot(p.velocity,n)));
            // Keep the authored coefficient and the engine's raw hit normal.
            if(!engine && hit->engineNormal&&!std::isfinite(s.bounceCoefficient))
                throw ModelError("Non-finite engine particle bounce coefficient");
            const float coefficient=s.bounceCoefficient;
            bool settle=false;
            if(targeted || environment->bounceMode==ParticleBounceMode::Reflection) {
                p.velocity=mul(reflected,coefficient);
                if(targeted) {
                    p.position=add(p.position,mul(n,0.005f));
                    settle=dot(p.velocity,p.velocity)<0.5f;
                } else {
                    const float speed=length(p.velocity);
                    settle=std::fabs(n.x)<0.70710678f && std::fabs(n.y)<0.70710678f && speed<26*dt;
                }
            } else {
                const float scale=std::clamp(0.033f/dt,0.5f,1.0f);
                p.velocity={reflected.x*0.75f,reflected.y*0.75f,reflected.z*0.75f*coefficient*scale};
                if(coefficient==0) p.velocity={};
                settle=dot(p.velocity,p.velocity)<0.5f;
            }
            if(settle) {
                p.stopped=true;p.contactNormal=n;p.direction=n;
                p.position=add(p.position,mul(n,0.02f));
                // All three engine movers apply sizeStart after the fixed
                // contact offset, without owner scale or normal normalization.
                if(engine && hasChunk) p.position=add(p.position,mul(n,s.sizeStart));
                // Surface-facing basis for the modes that consume particle axes.
                // Shortest arc from world Z, without introducing an arbitrary
                // twist around the contact normal.
                if(engine && hit->engineNormal) {
                    p.orientation=engineQuaternionRotationArc({0,0,1},n);
                    particleOrientationAxes(p);
                } else if(n.z>-0.999999f) {
                    const float d=1+n.z;
                    p.right={1-n.x*n.x/d,-n.x*n.y/d,-n.x};
                    p.up={-n.x*n.y/d,1-n.y*n.y/d,-n.y};
                } else {p.right={1,0,0};p.up={0,-1,0};}
            }
        }
    }
    if(!targeted && environment && environment->weatherCameraPosition) {



        const auto camera=*environment->weatherCameraPosition;
        const auto relative=sub(p.position,camera);
        const float x=relative.x+5.0f,y=relative.y+5.0f;
        p.position={std::fmod(x,10.0f)+(x<0.0f?5.0f:-5.0f)+camera.x,
                    std::fmod(y,10.0f)+(y<0.0f?5.0f:-5.0f)+camera.y,
                    relative.z+camera.z};
    }
    return true;
}
void moveEmitterParticle(EmitterParticle& p,const NodeEmitterState& s,const Mat4& old,
    const Mat4& now,std::uint32_t flags,float dt,const ParticleEnvironment* environment) {
    moveEmitterParticleImpl(p,s,old,now,flags,dt,environment,false,false);
}
Vec3 particleBlastDisplacement(const ParticleBlast& blast,Vec3 point) noexcept {
    return pointSourceWindVector(
        {blast.center,blast.radius,blast.remaining,blast.strength},point);
}
std::uint32_t ParticleRandom::next() {
    if(source) return source();
    state=state*214013u+2531011u;return (state>>16u)&32767u;
}
EmitterPreviewSupport emitterPreviewSupport(const EmitterNodeData& data) {
    EmitterPreviewSupport result;




    const auto& update=data.updateMode;
    const auto& render=data.renderMode;
    const auto blend=lower(data.blendMode);
    if(update=="Fountain") result.update=EmitterUpdate::Fountain;
    else if(update=="Single") result.update=EmitterUpdate::Single;
    else if(update=="Explosion") result.update=EmitterUpdate::Explosion;
    else result.update=EmitterUpdate::Lightning;
    if(!data.chunkName.empty()) {
        if(result.update==EmitterUpdate::Lightning) {
            result.reason="Lightning with chunk models is not an authored combination supported by this runtime";
            return result;
        }
        result.supported=true;return result; // real model instance, not a sprite render mode
    }
    // Initialize also uses case-sensitive comparisons for the render mode.
    // An unmatched value leaves its engine member unwritten; retain the
    // existing unsupported boundary instead of assigning a render mode.
    if(render=="Normal") result.render=EmitterRender::Normal;
    else if(render=="Billboard_to_World_Z") result.render=EmitterRender::BillboardWorldZ;
    else if(render=="Billboard_to_Local_Z") result.render=EmitterRender::BillboardLocalZ;
    else if(render=="Aligned_to_World_Z") result.render=EmitterRender::AlignedWorldZ;
    else if(render=="Aligned_to_Particle_Dir") result.render=EmitterRender::AlignedParticleDirection;
    else if(render=="Motion_Blur") result.render=EmitterRender::MotionBlur;
    else if(render=="Linked") result.render=EmitterRender::Linked;
    else {result.reason="Unsupported render mode: "+data.renderMode;return result;}
    if(blend=="normal") result.blend=EmitterBlend::Normal;
    else if(blend=="punchthrough" || blend=="punch-through") result.blend=EmitterBlend::PunchThrough;



    else result.blend=EmitterBlend::Additive;
    result.supported=true;return result;
}
std::vector<std::string> emitterPreviewWarnings(const EmitterNodeData& data,const NodeEmitterState&) {
    std::vector<std::string> result;
    const auto support=emitterPreviewSupport(data);
    if(!support.supported) {result.push_back(support.reason);return result;}
    const auto flags=data.rawWords.back();
    if(flags&emitter_flags::PointToPoint) result.emplace_back("Point-to-point motion requires an explicit target transform");
    if(flags&emitter_flags::AffectedByWind) result.emplace_back("Affected by scene wind; requires a supplied wind field");
    if(flags&emitter_flags::Bounce) result.emplace_back("Collision uses the supplied scene proxy");
    if(flags&emitter_flags::Splat) result.emplace_back("Splat particles orient on contact; no surface-clipped decal projection");
    if(support.update==EmitterUpdate::Lightning) result.emplace_back(
        "Lightning requires an explicit target transform; branches are one-level child emitters");
    if(flags&emitter_flags::InheritLocal) result.emplace_back(
        "Inherit-local uses the caller-supplied owner translation delta");
    if(data.deadSpace!=0) result.emplace_back(
        "Dead-space emission is camera-relative and requires a supplied camera position");
    if((flags&emitter_flags::DepthTexture) || !data.depthTexture.empty())
        result.emplace_back("Depth-texture metadata is retained without a separate depth-texture render path");
    if(support.update==EmitterUpdate::Explosion && (data.blastRadius!=0 || data.blastLength!=0))
        result.emplace_back("Explosion blast wind requires scene point-source wind to be enabled");
    constexpr std::uint32_t known=0x1fffu;
    if(flags&~known) result.emplace_back("Additional emitter flags are retained without inferred behavior");
    return result;
}
void EmitterSimulation::reset(std::uint64_t newSeed) noexcept {
    particles.clear();accumulator=0;birthRemainder=0;seed=newSeed;random.state=static_cast<std::uint32_t>(newSeed);
    nextSerial=0;detonations=0;freeParticles=0;pendingBursts=0;
    world=previousWorld=identityMatrix();frame=previousFrame=inheritanceFrame={};distanceRemainder={};initialized=false;previousDetonate=false;game=GameVersion::Kotor2;
    ribbonStarts.clear();lightningControl={};lightningOffsets.clear();
    lightningPreviousSource=lightningPreviousDelta={};lightningBranches.clear();
    lightningFreeParticles.clear();lightningBirthRate=0;
    lightningAge=controlAge=10000.0f;lightningMainCount=lightningActiveBranches=0;lightningReady=false;
    renderTint={1,1,1};renderAlpha=1;
}
void EmitterSimulation::detonate() noexcept {pendingBursts=1;}
namespace {
Vec3 lightningTargetOffset(float targetSize,ParticleRandom& rng) {
    if(targetSize==0) return {};
    const int divisor=integer(targetSize*50);
    const auto component=[&]() {
        const auto random=rng.next();
        const float value=static_cast<float>(engineSignedRemainder(
            static_cast<std::int32_t>(random),divisor))/100.0f;
        return random&1u ? value : -value;
    };
    return {component(),component(),component()};
}
bool refreshLightningOffsets(std::vector<Vec3>& offsets,std::size_t count,
    Vec3 source,Vec3 target,Vec3 rotationAxis,Vec3 radial,const NodeEmitterState& s,float partScale,
    ParticleRandom& rng) {
    // +274 is a separate, persistent offset pool. Rendering Linked adds it;
    // Billboard/MotionBlur read the Particle position itself.
    if(offsets.size()<count) offsets.resize(count);
    const auto delta=sub(source,target);
    if(delta.x==0 && delta.y==0 && delta.z==0)return false;
    const auto direction=engineLightningNormalize(delta);
    const float halfSpacing=static_cast<float>(
        std::sqrt(static_cast<double>(dot(delta,delta)))/static_cast<double>(count)*0.5);
    const Vec3 longitudinal=mul(direction,halfSpacing*(s.lightningScale*partScale));
    const int zigzag=integer(s.lightningZigzag);
    std::int32_t angle=0;
    for(std::size_t i=1;i+1<count;++i) {
        if(zigzag) {
            const auto amount=engineSignedRemainder(static_cast<std::int32_t>(rng.next()),zigzag);
            angle=engineIntAdd(angle,rng.next()&1u ? engineIntAdd(360,-amount):amount);
            radial=engineLightningAxisRotate(radial,rotationAxis,static_cast<float>(angle)*(pi/180));
        }
        float envelope=static_cast<float>(i)/static_cast<float>(count);
        if(envelope>0.5f) envelope=1.0f-envelope;
        envelope=(envelope+envelope)*s.lightningRadius;
        envelope=(static_cast<float>(rng.next()%100u)*envelope)*0.01f;
        Vec3 along=mul(longitudinal,static_cast<float>(rng.next()%100u)*0.01f);
        if(rng.next()&1u) along=mul(along,-1.0f);
        offsets[i]=add(mul(radial,envelope),along);
    }
    return true;
}
void refreshLightningBranches(const EmitterNodeData& data,const NodeEmitterState& s,
    Vec3 target,EmitterSimulation& sim,ParticleRandom& rng,
    const std::vector<EmitterParticle>& particles) {
    if(data.branchCount==0) return;
    sim.lightningActiveBranches=static_cast<std::size_t>(engineSignedRemainder(
        static_cast<std::int32_t>(rng.next()),engineIntAdd(data.branchCount,1)));
    const auto& control=sim.lightningControl;
    for(std::size_t i=0;i<sim.lightningActiveBranches;++i) {
        auto& branch=sim.lightningBranches[i];
        branch.scale=static_cast<float>(rng.next()%80u)/100.0f+0.1f;
        auto& child=branch.properties;
        child.sizeStart=s.sizeStart*branch.scale;child.sizeMid=s.sizeMid*branch.scale;
        child.sizeEnd=s.sizeEnd*branch.scale;child.sizeStartY=s.sizeStartY*branch.scale;
        child.sizeMidY=s.sizeMidY*branch.scale;child.sizeEndY=s.sizeEndY*branch.scale;
        branch.attachment=static_cast<std::size_t>(integer(static_cast<float>(particles.size())*branch.scale));
        const Vec3 origin=particles[branch.attachment].position;
        branch.source=origin;
        branch.control.base.front()=origin;
        const float yaw=-static_cast<float>(rng.next()%45u);
        const float roll=static_cast<float>(rng.next()%360u);
        branch.tangentStart=engineLightningYawPitchRollRotate(mul(control.radial,s.tangentLength),yaw,0,roll);
        branch.control.baseTangents.front()=branch.tangentStart;
        if(branch.scale>0.5f) {
            branch.targetsMain=true;branch.target=target;
            child.targetSize=s.targetSize;
            branch.birthRate=length(sub(target,origin))*s.lightningSubdivisions+2.0f;
            branch.tangentEnd=control.baseTangents.back();
        } else {
            branch.targetsMain=false;
            const auto mainDelta=sub(particles.back().position,particles.front().position);
            const float halfDistance=length(mainDelta)*0.5f;
            const float radialScale=(static_cast<float>(rng.next()%75u)/100.0f+0.25f)*halfDistance;
            if(radialScale==0) {
                branch.birthRate=0;branch.target=origin;
            } else {
                const auto fork=engineLightningYawPitchRollRotate(mul(control.radial,radialScale),
                    0,static_cast<float>(rng.next()%360u),0);
                const float forward=static_cast<float>(rng.next()%25u)/100.0f+0.25f;
                branch.target=add(add(origin,mul(mainDelta,forward)),fork);
                branch.birthRate=length(sub(branch.target,origin))*s.lightningSubdivisions+2.0f;
                branch.tangentEnd=branch.tangentStart;
            }
            child.targetSize=0;
            // UpdateBranches really normalizes target minus itself here.
            const auto n=engineLightningNormalize(sub(branch.target,branch.target));
            branch.control.radial={-n.y,n.z,-n.x};
        }
        branch.control.base.back()=branch.target;
        branch.control.baseTangents.back()=branch.tangentEnd;
        branch.previousSource=origin;branch.previousDelta=sub(branch.target,origin);
        child.birthRate=branch.birthRate;
        branch.controlAge=s.controlPointDelay;
    }
}
void resizeLightningParticles(const EmitterNodeData& data,NodeEmitterState& state,
    const Mat4& world,float dt,std::vector<EmitterParticle>& particles,
    std::vector<EmitterParticle>& free,EmitterSimulation& sim,ParticleRandom& rng,
    const ParticleEnvironment* env,bool engine,const EngineFrame* engineFrame,
    const EngineFrame* previousFrame) {
    const auto count=static_cast<std::size_t>(integer(state.birthRate));
    while(particles.size()>count) {free.push_back(particles.back());particles.pop_back();}
    while(particles.size()<count) {
        EmitterParticle particle;
        if(!free.empty()) {particle=free.back();free.pop_back();}
        else {particle=initializeParticle(data,state,world,sim.previousWorld,dt,1.0f,rng,env,engine,
            engineFrame,previousFrame);particle.serial=sim.nextSerial++;}
        initialFrame(data,state,particle,rng);
        particles.push_back(particle);
    }
}
void tickLightningParticles(const EmitterNodeData& data,const NodeEmitterState& state,
    std::vector<EmitterParticle>& particles,float dt) {
    for(auto& p:particles) {
        if(p.age==p.lastFrameAge) p.age=p.frameOffset=p.lastFrameAge=0;
        p.age+=dt;
        if(data.renderMode=="Motion_Blur") {
            const float weight=state.blurLength<dt?state.blurLength:dt;
            const float fraction=weight/state.blurLength;
            p.tail=add(mul(p.position,fraction),mul(p.tail,1.0f-fraction));
        }
    }
}
void evaluateLightningInterior(std::vector<EmitterParticle>& particles,
    const EngineLightningControl& control,float blend,bool previousOnly=false) {
    for(std::size_t i=1;i<(previousOnly?particles.size():particles.size()-1);++i)
        particles[i].position=engineEvaluateLightningControl(control,i,particles.size(),blend,previousOnly);
}
void lightningEndpointRefresh(const EmitterNodeData& data,const NodeEmitterState& s,
    const Mat4& world,float ownerScale,Vec3 target,std::vector<EmitterParticle>& particles,
    EngineLightningControl& control,float controlAge,ParticleRandom& rng,
    const ParticleEnvironment* env,bool child,const EngineFrame* engineFrame) {
    const bool sourceJitter=s.xSize!=0 || s.ySize!=0;
    const bool targetJitter=s.targetSize!=0;
    particles.front().position=sourceJitter?randomParticlePosition(s,world,ownerScale,rng,engineFrame):position(world);
    particles.back().position=add(target,lightningTargetOffset(s.targetSize,rng));
    if(!sourceJitter && !targetJitter) return;
    if(control.base.size()==2) {
        // With two controls engine updates one endpoint, preferring the source
        // when both are randomized, then runs UpdateControlPoints again.
        if(sourceJitter)control.base.front()=particles.front().position;
        else control.base.back()=particles.back().position;
        engineRefreshLightningControl(control,position(world),target,env->localEmitterForward,
            engineFrame?engineQuaternionAxis(engineFrame->orientation,2):Vec3{world.values[8],world.values[9],world.values[10]},
            engineFrame?engineQuaternionAxis(env->engineTarget->orientation,2):
                Vec3{env->target->values[8],env->target->values[9],env->target->values[10]},s,rng,child);
        evaluateLightningInterior(particles,control,0,true);
        return;
    }
    const float blend=s.controlPointDelay==0?0:controlAge/s.controlPointDelay;
    if(sourceJitter) {
        control.base.front()=control.current.front()=particles.front().position;
        const float step=1.0f/static_cast<float>(control.base.size()-1);
        for(std::size_t i=1;i+1<particles.size();++i) {
            particles[i].position=engineEvaluateLightningControl(control,i,particles.size(),blend);
            // The engine cursor is checked after writing the first particle
            // in the following segment (K2 Desktop 0x123bcb).
            if(static_cast<float>(i)/static_cast<float>(particles.size()-1)>step) break;
        }
    } else {
        control.base.back()=control.current.back()=particles.back().position;
        const float lower=static_cast<float>(control.base.size()-2)/static_cast<float>(control.base.size()-1);
        const auto begin=static_cast<std::size_t>(engineIntAdd(integer(
            static_cast<float>(particles.size())*lower),1));
        for(auto i=begin;i+1<particles.size();++i)
            particles[i].position=engineEvaluateLightningLastControl(control,i,particles.size(),blend);
    }
    (void)data;
}
void moveLightning(std::vector<EmitterParticle>& particles,EngineLightningControl& control,
    Vec3 previousSource,Vec3 previousDelta,Vec3 source,Vec3 delta) {
    for(auto& p:particles) p.position=add(engineLightningArcRotate(sub(p.position,previousSource),previousDelta,delta),source);
    for(auto& p:control.base) p=add(engineLightningArcRotate(sub(p,previousSource),previousDelta,delta),source);
    for(auto& p:control.baseTangents) p=engineLightningArcRotate(p,previousDelta,delta);
}
void flattenLightningChildren(EmitterSimulation& sim) {
    for(std::size_t i=0;i<sim.lightningActiveBranches;++i) {
        auto& branch=sim.lightningBranches[i];
        if(branch.particles.empty()) continue;
        branch.flattenedStart=sim.particles.size();sim.ribbonStarts.push_back(sim.particles.size());
        sim.particles.insert(sim.particles.end(),branch.particles.begin(),branch.particles.end());
    }
}
void advanceLightning(const EmitterNodeData& data,NodeEmitterState& s,
    const Mat4& world,float dt,EmitterSimulation& sim,
    ParticleRandom& rng,const ParticleEnvironment* env,float ownerScale,bool engine) {
    for(auto& branch:sim.lightningBranches) branch.flattenedStart.reset();
    std::vector<EmitterParticle> particles;
    particles.assign(sim.particles.begin(),sim.particles.begin()+static_cast<std::ptrdiff_t>(sim.lightningMainCount));
    sim.particles.clear();sim.ribbonStarts.clear();
    sim.lightningAge+=dt;
    if(!hasParticleTarget(env,engine)) {
        sim.lightningBirthRate=s.birthRate;
        for(auto i=particles.size();i>0;--i) sim.lightningFreeParticles.push_back(particles[i-1]);
        sim.lightningMainCount=0;flattenLightningChildren(sim);return;
    }
    const auto source=position(world),target=particleTargetPosition(*env,engine),delta=sub(target,source);
    const Vec3 worldForward=engine?engineQuaternionAxis(sim.frame.orientation,2):
        Vec3{world.values[8],world.values[9],world.values[10]};
    const Vec3 targetForward=engine?engineQuaternionAxis(env->engineTarget->orientation,2):
        Vec3{env->target->values[8],env->target->values[9],env->target->values[10]};
    if(!sim.lightningReady) {
        s.birthRate=length(delta)*s.lightningSubdivisions+2.0f;
        engineInitializeLightningControl(sim.lightningControl,source,target,env->localEmitterForward,targetForward,s);
        sim.lightningPreviousSource=source;sim.lightningPreviousDelta=delta;
        sim.lightningBranches.resize(static_cast<std::size_t>(data.branchCount));
        for(auto& branch:sim.lightningBranches) {
            branch.properties=*env->baseEmitterState;
            branch.properties.xSize=branch.properties.ySize=branch.properties.targetSize=0;
            branch.lightningAge=branch.properties.lightningDelay;
            branch.controlAge=branch.properties.controlPointDelay;
            branch.control.base.resize(2);branch.control.baseTangents.resize(2);
            branch.control.previous.resize(2);branch.control.current.resize(2);
            branch.control.previousTangents.resize(2);branch.control.currentTangents.resize(2);
        }
        sim.lightningReady=true;
    }
    sim.lightningBirthRate=s.birthRate;
    resizeLightningParticles(data,s,world,dt,particles,sim.lightningFreeParticles,sim,rng,env,engine,
        engine?&sim.frame:nullptr,&sim.previousFrame);
    sim.lightningMainCount=particles.size();
    if(particles.empty()) {flattenLightningChildren(sim);return;}
    if(sim.lightningOffsets.size()<particles.size())sim.lightningOffsets.resize(particles.size());
    tickLightningParticles(data,s,particles,dt);
    sim.controlAge+=dt;
    const bool controlRefresh=sim.controlAge>s.controlPointDelay;
    if(controlRefresh) {
        sim.controlAge=0;
        sim.lightningControl.base.front()=source;sim.lightningControl.base.back()=target;
        engineRefreshLightningControl(sim.lightningControl,source,target,env->localEmitterForward,
            worldForward,targetForward,s,rng);
        evaluateLightningInterior(particles,sim.lightningControl,0,true);
        refreshLightningBranches(data,s,target,sim,rng,particles);
        sim.lightningAge=s.lightningDelay+0.01f;
    } else if(data.controlPointSmoothing) {
        evaluateLightningInterior(particles,sim.lightningControl,s.controlPointDelay==0?0:sim.controlAge/s.controlPointDelay);
    }
    const bool jitterRefresh=sim.lightningAge>s.lightningDelay;
    bool refreshedOffsets=false;
    if(jitterRefresh) {
        lightningEndpointRefresh(data,s,world,ownerScale,target,particles,sim.lightningControl,sim.controlAge,rng,env,false,
            engine?&sim.frame:nullptr);
        refreshedOffsets=refreshLightningOffsets(sim.lightningOffsets,particles.size(),particles.front().position,
            particles.back().position,delta,sim.lightningControl.radial,s,env->partScale,rng);
        if(refreshedOffsets)sim.lightningAge=0;
    } else {
        moveLightning(particles,sim.lightningControl,sim.lightningPreviousSource,sim.lightningPreviousDelta,source,delta);
        for(std::size_t i=0;i<sim.lightningActiveBranches;++i) {
            auto& branch=sim.lightningBranches[i];
            branch.target=add(engineLightningArcRotate(sub(branch.target,sim.lightningPreviousSource),sim.lightningPreviousDelta,delta),source);
        }
        sim.lightningPreviousSource=source;sim.lightningPreviousDelta=delta;
    }
    sim.particles=particles;sim.ribbonStarts.push_back(0);
    for(std::size_t i=0;i<sim.lightningActiveBranches;++i) {
        auto& branch=sim.lightningBranches[i];
        auto& child=branch.properties;
        if(!jitterRefresh || refreshedOffsets) {
            branch.source=add(particles[branch.attachment].position,sim.lightningOffsets[branch.attachment]);
            branch.control.previous.front()=branch.control.current.front()=branch.source;
        }
        const auto origin=branch.source;
        if(refreshedOffsets) {
            branch.previousSource=origin;branch.previousDelta=sub(branch.target,origin);
        }
        const float childDt=controlRefresh?s.lightningDelay+0.01f:dt;
        branch.birthRate=child.birthRate;
        Mat4 childWorld=identityMatrix();childWorld.values[12]=origin.x;childWorld.values[13]=origin.y;childWorld.values[14]=origin.z;
        const EngineFrame childFrame{origin,{},1.0f},previousChildFrame{branch.previousSource,{},1.0f};
        ParticleEnvironment childEnvironment=*env;
        childEnvironment.partScale=1;childEnvironment.localEmitterForward={0,0,1};
        childEnvironment.target=identityMatrix();
        childEnvironment.target->values[12]=branch.target.x;childEnvironment.target->values[13]=branch.target.y;childEnvironment.target->values[14]=branch.target.z;
        childEnvironment.engineTarget=EngineFrame{branch.target,{},1.0f};
        branch.lightningAge+=childDt;
        resizeLightningParticles(data,child,childWorld,childDt,branch.particles,branch.freeParticles,sim,rng,&childEnvironment,engine,
            engine?&childFrame:nullptr,&previousChildFrame);
        if(branch.particles.empty())continue;
        if(branch.offsets.size()<branch.particles.size())branch.offsets.resize(branch.particles.size());
        tickLightningParticles(data,child,branch.particles,childDt);
        branch.controlAge+=childDt;
        if(branch.controlAge>child.controlPointDelay) {
            branch.controlAge=0;
            branch.control.base.front()=origin;branch.control.base.back()=branch.target;
            engineRefreshLightningControl(branch.control,origin,branch.target,{0,0,1},{0,0,1},{0,0,1},child,rng,true);
            evaluateLightningInterior(branch.particles,branch.control,0,true);
            branch.lightningAge=child.lightningDelay+0.01f;
        }
        if(branch.lightningAge>child.lightningDelay) {
            lightningEndpointRefresh(data,child,childWorld,1,branch.target,branch.particles,branch.control,branch.controlAge,rng,&childEnvironment,true,
                engine?&childFrame:nullptr);
            if(refreshLightningOffsets(branch.offsets,branch.particles.size(),branch.particles.front().position,
                branch.particles.back().position,sub(branch.target,origin),branch.control.radial,child,1,rng))branch.lightningAge=0;
        } else {
            const auto childDelta=sub(branch.target,origin);
            moveLightning(branch.particles,branch.control,branch.previousSource,
                branch.previousDelta,origin,childDelta);
            branch.previousSource=origin;branch.previousDelta=childDelta;
        }
    }
    flattenLightningChildren(sim);
    sim.previousWorld=world;sim.previousFrame=sim.frame;sim.accumulator+=dt;
}

} // namespace

static void advanceEmitterWithBirthScale(const EmitterNodeData& data,NodeEmitterState& state,
    const Mat4& world,double elapsed,EmitterSimulation& sim,ParticleRandom* shared,const ParticleEnvironment* environment,float sourceScale,bool engineScale,
    const EngineFrame* engineFrame=nullptr) {
    if(!engineScale && (!std::isfinite(elapsed)||elapsed<0)) return;
    const auto support=emitterPreviewSupport(data);
    if(!support.supported || (!engineScale && !finite(world))) {sim.particles.clear();return;}
    const auto flags=data.rawWords.back();
    const auto* owner=engineScale && environment && environment->engineOwnerFrame
        ? &*environment->engineOwnerFrame : nullptr;
    // AnimateParticles reads owner-C before first-call initialization, with a
    // literal zero delta on that first call. Its cache is not prior source P/Q.
    const Vec3 engineOwnerDelta=sim.initialized && owner && (flags&emitter_flags::InheritLocal)
        ? sub(owner->position,sim.inheritanceFrame.position) : Vec3{};
    if(!sim.initialized) {
        sim.previousWorld=world;
        if(engineScale) {
            sim.previousFrame=*engineFrame;
            if(owner && (flags&(emitter_flags::InheritLocal|emitter_flags::InheritPart)))
                sim.inheritanceFrame=*owner;
            // Independent engine check: Part wins over Local at initialization.
            if(flags&emitter_flags::InheritPart) sim.inheritanceFrame=*engineFrame;
        }
        sim.initialized=true;
    }
    sim.world=world;
    if(engineScale) sim.frame=*engineFrame;
    sim.game=environment ? environment->game : GameVersion::Kotor2;
    if(environment) {
        const auto tint=environment->objectTint;
        sim.renderTint=(flags&emitter_flags::Tinted) && finite(tint)
            ? Vec3{bound(tint.x,0,1,1),bound(tint.y,0,1,1),bound(tint.z,0,1,1)}
            : Vec3{1,1,1};
        sim.renderAlpha=environment->objectAlpha*environment->tileAlpha;
    } else {sim.renderTint={1,1,1};sim.renderAlpha=1;}
    // Engine AnimateParticles forwards zero/negative elapsed to Update. Only
    // the explicitly selected inspection implementation skips pose-only work.
    if(!engineScale && elapsed==0) return;
    const float dt=static_cast<float>(elapsed);
    auto& rng=shared ? *shared : sim.random;
    Mat4 previousSource=sim.previousWorld;
    EngineFrame previousSourceFrame=sim.previousFrame;
    if(engineScale && (flags&emitter_flags::InheritLocal)) {
        previousSource.values[12]+=engineOwnerDelta.x;
        previousSource.values[13]+=engineOwnerDelta.y;
        previousSource.values[14]+=engineOwnerDelta.z;
        previousSourceFrame.position=add(previousSourceFrame.position,engineOwnerDelta);
    } else if(!engineScale && (flags&emitter_flags::InheritLocal) && environment &&
              finite(environment->ownerTranslationDelta)) {
        previousSource.values[12]+=environment->ownerTranslationDelta.x;
        previousSource.values[13]+=environment->ownerTranslationDelta.y;
        previousSource.values[14]+=environment->ownerTranslationDelta.z;
        previousSourceFrame.position=add(previousSourceFrame.position,environment->ownerTranslationDelta);
    }
    if(engineScale) {
        // AnimateParticles writes these fields before virtual Update, including
        // Lightning's particle constructors; they are not mover-only arguments.
        sim.previousWorld=previousSource;
        sim.previousFrame=previousSourceFrame;
    }
    if(support.update==EmitterUpdate::Lightning) {
        advanceLightning(data,state,world,dt,sim,rng,environment,sourceScale,engineScale);return;
    }
    const float rate=state.birthRate;
    const float life=state.lifeExpectancy;
    if(support.update==EmitterUpdate::Single && integer(rate)==0) {
        if(environment && environment->chunkDestroyed)
            for(const auto& particle:sim.particles) environment->chunkDestroyed(particle.serial);
        sim.particles.clear();sim.previousWorld=world;sim.previousFrame=sim.frame;sim.accumulator+=dt;return;
    }
    // Single's constructor path returns after chkRandom without MoveFunc.
    // Fountain/Explosion call MoveFunc even for an empty active array.
    const bool engineMoverCalled=support.update!=EmitterUpdate::Single || !sim.particles.empty();
    if(support.update!=EmitterUpdate::Single) {
        const auto before=sim.particles.size();
        // chkParticleLife traverses backwards; that order determines the free
        // ChunkyParticle stack and the corresponding Gob scene removals.
        if(environment && environment->chunkExpired)
            for(auto it=sim.particles.rbegin();it!=sim.particles.rend();++it)
                if(life>=0 && it->age>=life)
                    environment->chunkExpired(it->serial);
        sim.particles.erase(std::remove_if(sim.particles.begin(),sim.particles.end(),[&](const EmitterParticle& p) {
            return life>=0 && p.age>=life;
        }),sim.particles.end());
        sim.freeParticles+=before-sim.particles.size();
    }
    // K1 Desktop 0x2ec678/0x2f35ed and K2 Desktop 0x119b04/0x118b21 perturb
    // the target once before traversing the population. Each component uses
    // one random value for its sign and its remainder; no per-particle draws.
    ParticleEnvironment targetEnvironment;
    if((!engineScale || engineMoverCalled) && (flags&emitter_flags::PointToPoint) &&
       hasParticleTarget(environment,engineScale)) {
        targetEnvironment=*environment;
        const auto offset=lightningTargetOffset(state.targetSize,rng);
        if(engineScale) targetEnvironment.engineTarget->position=add(targetEnvironment.engineTarget->position,offset);
        else {
            targetEnvironment.target->values[12]+=offset.x;
            targetEnvironment.target->values[13]+=offset.y;
            targetEnvironment.target->values[14]+=offset.z;
        }
        environment=&targetEnvironment;
    }
    // The standard mover reuses a wind sample for blocks of six when more
    // than 49 particles are alive. Preserve population-index sampling (stopped
    // particles still occupy their indices), not a per-callback counter.
    ParticleEnvironment cachedEnvironment;
    const ParticleEnvironment* activeEnvironment=environment;
    std::size_t movingIndex=0;
    Vec3 cachedWind{};
    if(environment && environment->windDisplacement && sim.particles.size()>49 &&
       !(flags&emitter_flags::PointToPoint)) {
        cachedEnvironment=*environment;
        cachedEnvironment.windDisplacement=[&](Vec3 p,float seconds) {
            if(movingIndex%6==0) {
                const auto sample=environment->windDisplacement(p,seconds);
                if(!engineScale && !finite(sample)) throw ModelError("Particle scene returned non-finite wind displacement");
                cachedWind=sample;
            }
            return cachedWind;
        };
        activeEnvironment=&cachedEnvironment;
    }
    bool engineMoverCompleted=true;
    for(auto& p:sim.particles) {
        movingIndex=static_cast<std::size_t>(&p-sim.particles.data());
        p.lifetime=life;p.age+=dt;
        if(support.update!=EmitterUpdate::Single && p.age<0)
            p.age=p.lastFrameAge=0; // chkParticleLife: age/frame clock, not position validity
        if(support.update==EmitterUpdate::Single) {
            if(life>0 && p.age>life && data.loop) p.age=0;
            else if(life<=0 && 0>p.age) p.age=dt; // packed comparison; negative lifetimes normally continue
        }
        p.rotation+=state.rotation*dt;
        if(p.rotation>tau) p.rotation-=tau;
        else if(p.rotation<-tau) p.rotation+=tau;
        if(support.render==EmitterRender::MotionBlur) {
            const float t=std::min(state.blurLength,dt)/state.blurLength;
            p.tail=add(mul(p.position,t),mul(p.tail,1-t));
        }
        if(engineScale) {
            if(engineMoverCompleted)
                engineMoverCompleted=moveEmitterParticleImpl(p,state,sim.previousWorld,world,flags,dt,
                    activeEnvironment,true,!data.chunkName.empty(),&previousSourceFrame,&sim.frame,&sim.inheritanceFrame);
        } else {
            moveEmitterParticle(p,state,sim.previousWorld,world,flags,dt,activeEnvironment);
        }
    }
    if(engineScale && engineMoverCalled && engineMoverCompleted) {
        const bool targeted=(flags&emitter_flags::PointToPoint)!=0;
        // Bezier and a missing-target MoveFunc return never commit C/Qc.
        if(!targeted || (hasParticleTarget(environment,true) && !(flags&emitter_flags::PointToPointBezier))) {
            if(owner && (flags&emitter_flags::InheritLocal)) {
                sim.inheritanceFrame.position=owner->position;
                sim.inheritanceFrame.orientation=owner->orientation;
            } else if(flags&emitter_flags::InheritPart) {
                sim.inheritanceFrame.position=sim.frame.position;
                // Gravity's Part case still reads the OWNER quaternion. Its
                // engine caller requires an owner here; Standard uses source Q.
                sim.inheritanceFrame.orientation=targeted?owner->orientation:sim.frame.orientation;
            }
        }
    }
    const auto spawn=[&](std::optional<Vec3> overridePosition={}) {
        const auto serial=sim.nextSerial++;
        // New Fountain/Explosion particles run the constructor initializer and
        // then the virtual initializer. Recycled objects only run the latter.
        const bool recycled=support.update!=EmitterUpdate::Single && sim.freeParticles!=0;
        if(support.update!=EmitterUpdate::Single) {
            if(recycled) --sim.freeParticles;
            else {
                (void)initializeParticle(data,state,world,previousSource,dt,1.0f,rng,environment,engineScale,
                    engineFrame,&previousSourceFrame);
                if(environment && environment->chunkBirth)
                    environment->chunkBirth(serial,false,1.0f,false);
            }
        }


        const float birthScale=engineScale && recycled && support.update==EmitterUpdate::Explosion
            ? 1.0f : sourceScale;
        const float particleScale=data.chunkName.empty()?birthScale:1.0f;
        auto p=initializeParticle(data,state,world,previousSource,dt,particleScale,rng,environment,engineScale,
            engineFrame,&previousSourceFrame);
        p.serial=serial;p.lifetime=life;p.ownerScaleAtBirth=birthScale;
        if(environment && environment->chunkBirth)
            environment->chunkBirth(serial,recycled,birthScale,support.update!=EmitterUpdate::Single);
        initialFrame(data,state,p,rng);
        if(overridePosition) p.position=*overridePosition;
        sim.particles.push_back(p);
    };
    if(support.update==EmitterUpdate::Single) {
        if(sim.particles.empty() && integer(rate)!=0) spawn();
    } else if(support.update==EmitterUpdate::Explosion) {
        const bool high=sim.pendingBursts!=0 || (std::isfinite(state.detonate)&&state.detonate!=0);
        const bool trigger=high && !sim.previousDetonate && integer(rate)!=0;
        if(trigger) {
            ++sim.detonations;
            if(data.blastRadius!=0 && environment && environment->addPointSourceWind)
                environment->addPointSourceWind(position(world),data.blastRadius,data.blastLength,1.0f);
            for(int n=0;n<integer(rate);++n) spawn();
        }
        sim.pendingBursts=0;
        sim.previousDetonate=high;
    } else if(integer(rate)>0) {
        if(data.spawnType) {
            const auto delta=add(sim.distanceRemainder,sub(position(world),position(previousSource)));
            const float distance=length(delta);
            // Fountain::Update uses magnitude > 0 and a zero reciprocal,
            // not the normalize(Vector) helper's short-vector +X branch.
            const auto direction=engineScale?mul(delta,distance>0.0f?1.0f/distance:0.0f)
                                             :unit(delta,{0,0,0});
            const int count=integer(rate*distance);
            for(int n=0;n<count;++n) {
                if(state.xSize==0 && state.ySize==0)
                    spawn(sub(position(world),mul(direction,distance-
                        (sim.particles.empty()?0.0f:static_cast<float>(n+1)/rate))));
                else spawn();
            }
            sim.distanceRemainder=sub(delta,mul(direction,static_cast<float>(count)/rate));
        } else {
            sim.birthRemainder+=dt;
            if(sim.birthRemainder>=1/rate) {
                float varied=rate;
                const int variance=std::max(0,integer(state.randomBirthRate));
                if(variance) {
                    const bool negative=rng.next()%2u==0;
                    const auto amount=static_cast<float>(rng.next()%static_cast<unsigned>(variance));
                    varied=std::max(0.0f,varied+(negative?-amount:amount));
                }
                const int divisor=engineIntAdd(integer(varied),1);
                const int count=engineSignedRemainder(integer(sim.birthRemainder*varied),
                    divisor);
                for(int n=0;n<count;++n) spawn();
                sim.birthRemainder=0; // intentional discarded remainder, not a fractional-birth accumulator
            }
        }
    }
    sim.previousWorld=world;sim.previousFrame=sim.frame;sim.accumulator+=dt;
}
void advanceEmitter(const EmitterNodeData& data,NodeEmitterState& state,
    const EngineFrame& frame,double elapsed,EmitterSimulation& sim,
    ParticleRandom* shared,const ParticleEnvironment* environment,float ownerScale) {


    const float birthScale=emitterPreviewSupport(data).update==EmitterUpdate::Single
        ? 1.0f : ownerScale;
    // AnimateParticles saves its retained current frame before GetWorld and
    // virtual Update, even if the preceding Lightning update had no target.
    if(sim.initialized) {sim.previousFrame=sim.frame;sim.previousWorld=sim.world;}
    const auto world=nodeTransform(frame.position,frame.orientation);
    advanceEmitterWithBirthScale(data,state,world,elapsed,sim,shared,environment,birthScale,true,&frame);
}
void advanceLegacyEmitter(const EmitterNodeData& data,NodeEmitterState& state,
    const Mat4& world,double elapsed,EmitterSimulation& sim,
    ParticleRandom* shared,const ParticleEnvironment* environment) {
    const float sourceScale=length(transformDirection(world,{1,0,0}));
    advanceEmitterWithBirthScale(data,state,world,elapsed,sim,shared,environment,sourceScale,false);
}
namespace {
const LightningBranchState* lightningBranchAt(const EmitterSimulation& sim,std::size_t index) {
    for(const auto& branch:sim.lightningBranches)
        if(branch.flattenedStart && index>=*branch.flattenedStart &&
           index-*branch.flattenedStart<branch.particles.size()) return &branch;
    return nullptr;
}
Vec3 lightningLinkedPosition(const EmitterSimulation& sim,std::size_t index) {
    if(const auto* branch=lightningBranchAt(sim,index))
        return add(sim.particles[index].position,branch->offsets[index-*branch->flattenedStart]);
    return add(sim.particles[index].position,sim.lightningOffsets[index]);
}
}
void updateEmitterParticleFrames(const EmitterNodeData& data,const NodeEmitterState& state,
    EmitterSimulation& sim,ParticleRandom* shared) {
    // Engine updateParticleFrame exits for an empty active particle array.
    if(sim.particles.empty()) return;
    const auto flags=data.rawWords.back();const bool randomInitial=(flags&emitter_flags::RandomFrame)!=0;
    auto& rng=shared ? *shared : sim.random;
    const bool linkedLightning=sim.lightningReady && data.renderMode=="Linked";
    const auto count=sim.lightningReady&&!linkedLightning?sim.lightningMainCount:sim.particles.size();
    for(std::size_t order=0;order<count;++order) {


        const auto index=linkedLightning
            ? (order+sim.lightningMainCount)%sim.particles.size() : order;
        const auto* branch=sim.lightningReady?lightningBranchAt(sim,index):nullptr;
        const auto& s=branch?branch->properties:state;
        const float fps=s.framesPerSecond;
        if(fps<=0 || s.frameStart==s.frameEnd) continue;
        const float interval=1/fps;
        const int first=integer(s.frameStart),last=integer(s.frameEnd);
        if(interval==0) continue;
        const bool descending=s.frameStart>s.frameEnd;
        auto& p=sim.particles[index];
        const float elapsed=p.age-(randomInitial?p.lastFrameAge:p.frameOffset);
        if(!(elapsed>=interval)) continue;
        if(flags&emitter_flags::RandomOrder) {
            const int span=engineIntAdd(engineIntAbs(integer(s.frameEnd-s.frameStart)),1);
            p.frame=engineSignedRemainder(static_cast<std::int32_t>(rng.next()),span); // no +first in the engine
        } else {
            const float frameSteps=std::floor(elapsed/interval);
            const int steps=integer(descending?-frameSteps:frameSteps);
            p.frame=engineIntAdd(p.frame,steps);
            if(randomInitial) {
                p.frameOffset+=static_cast<float>(steps)*interval;
                // The descending boundary remains a float comparison; the
                // ascending path compares against the truncated end frame.
                // Engine ADD/LEA wrap in 32 bits on both platforms.
                const int difference=engineIntAdd(first,static_cast<std::int32_t>(
                    0u-static_cast<std::uint32_t>(last)));
                if(descending) {
                    const int span=engineIntAdd(difference,1);
                    while(static_cast<float>(p.frame)<s.frameEnd)
                        p.frame=engineIntAdd(p.frame,span);
                } else {
                    const int span=engineIntAdd(difference,-1);
                    while(p.frame>last) p.frame=engineIntAdd(p.frame,span);
                }
            } else if(descending) {



                if(sim.game==GameVersion::Kotor2 && p.frame<first && data.loop) p.frame=last;
                p.frame=std::max(p.frame,first);
            } else {
                if(sim.game==GameVersion::Kotor2 && p.frame>last && data.loop) p.frame=first;
                p.frame=std::min(p.frame,last);
            }
        }
        if(!randomInitial) p.frameOffset=static_cast<float>(p.frame)*interval;
        if(!(s.lifeExpectancy>=0)) p.age=p.lastFrameAge=0;
        else p.lastFrameAge=p.age;
    }
    // Flattening is only the mesh representation: engine child emitters retain
    // the frame clocks modified by their own RenderLinked/updateParticleFrame.
    for(auto& branch:sim.lightningBranches) {
        if(!branch.flattenedStart) continue;
        const auto firstParticle=sim.particles.begin()+
            static_cast<std::ptrdiff_t>(*branch.flattenedStart);
        std::copy_n(firstParticle,branch.particles.size(),branch.particles.begin());
    }
}
ParticleAppearance emitterParticleAppearance(const EmitterNodeData& data,
    const NodeEmitterState& s,const EmitterParticle& p,
    std::optional<float> normalizedPosition) noexcept {
    ParticleAppearance a;const auto key=segment(s,p,normalizedPosition);
    // RenderLinked/RenderMotionBlurred/RenderStandard compare each color
    // against zero and one; unordered values pass through the comparisons.
    const auto clampColor=[](float value) {return value<0?0.0f:value>1?1.0f:value;};
    for(std::size_t i=0;i<3;++i) a.color[i]=clampColor(evaluate({s.colorStart[i],s.colorMid[i],s.colorEnd[i]},key));
    a.color[3]=clampColor(evaluate({s.alphaStart,s.alphaMid,s.alphaEnd},key));
    a.size.x=evaluate({s.sizeStart,s.sizeMid,s.sizeEnd},key);
    const std::array<float,3> ys{{s.sizeStartY,s.sizeMidY,s.sizeEndY}};
    const bool motionBlur=data.renderMode=="Motion_Blur";
    // MotionBlur's held lifetime branch checks only start/end Y. Linked and
    // Standard also consult the middle Y controller (K2Desktop1164ac/1151c4).
    const bool useX=!(s.lifeExpectancy>=0)
        ? ys[0]==0&&ys[2]==0&&(motionBlur||ys[1]==0)
        : ys[key.first]==0&&ys[key.second]==0;
    a.size.y=useX ? a.size.x : evaluate(ys,key);
    if((data.rawWords.back()&emitter_flags::Splat) &&
       data.renderMode!="Linked" && !motionBlur) {
        a.size.x=p.stopped?s.sizeEnd:s.sizeStart;
        const float y=p.stopped?s.sizeEndY:s.sizeStartY;
        a.size.y=y==0?a.size.x:y;
    }


    a.columns=data.xGrid?data.xGrid:1;a.rows=data.yGrid?data.yGrid:1;
    a.frame=static_cast<unsigned>(p.frame);
    return a;
}
std::array<ParticleFrameSample,5> emitterParticleFrames(const EmitterNodeData& data,
    const NodeEmitterState& s,const EmitterParticle& p) noexcept {
    std::array<ParticleFrameSample,5> result{};
    const bool blend=data.frameBlending && data.renderMode!="Linked" && data.renderMode!="Motion_Blur";
    result[0]={p.frame,blend?0.5f:1.0f};
    if(!blend) return result;
    const float interval=s.framesPerSecond>0 ? 1.0f/s.framesPerSecond : 0.0f;
    const float phase=(p.age-p.frameOffset)/interval;
    const int next=nextFrame(s,p.frame),previous=previousFrame(s,p.frame);
    result[1]={next,next>=0 ? phase*0.5f : 0};
    result[2]={previous,previous>=0 ? 1-phase*0.5f : 0};
    // Invalid first neighbors are still passed through the original helper.
    const int next2=nextFrame(s,next),previous2=previousFrame(s,previous);
    result[3]={next2,next2>=0 ? phase*0.25f : 0};
    result[4]={previous2,previous2>=0 ? (1-phase)*0.25f : 0};
    return result;
}
static void buildEmitterMeshWithScale(const EmitterNodeData& data,const NodeEmitterState& state,
    const EmitterSimulation& sim,const Mat4& view,Mesh& output,
    unsigned pass,float ownerScale,bool engineOwnerScale,const Quaternion* cameraOrientation=nullptr) {
    output.vertices.clear();output.indices.clear();output.castsShadow=false;
    output.hasVertexColors=true;output.render=true;output.diffuse={1,1,1};
    const auto support=emitterPreviewSupport(data);
    if(!support.supported||!data.chunkName.empty()||pass>4 ||
       (pass && (!data.frameBlending || support.render==EmitterRender::Linked ||
                 support.render==EmitterRender::MotionBlur))) return;
    const bool lightning=support.update==EmitterUpdate::Lightning;
    const bool linked=support.render==EmitterRender::Linked;
    const auto count=lightning&&!linked?sim.lightningMainCount:sim.particles.size();
    output.vertices.reserve(count*4);output.indices.reserve(count*6);
    // Original CPU renderer fills its vertex array backwards. Keep this stable
    // source order instead of sorting particles anew when the camera moves.
    const bool splat=(data.rawWords.back()&emitter_flags::Splat)!=0;
    std::size_t previousSegment=std::numeric_limits<std::size_t>::max();
    const bool streak=support.render==EmitterRender::MotionBlur;
    for(std::size_t order=0;order<count;++order) {
        const auto index=linked && lightning
            ? (order+sim.lightningMainCount)%count
            : (linked || streak)?order:count-order-1;
        if(linked && index+1>=count) continue;
        if(linked && lightning && std::binary_search(sim.ribbonStarts.begin(),sim.ribbonStarts.end(),index+1)) continue;
        const auto& p=sim.particles[index];
        const auto* branch=lightning?lightningBranchAt(sim,index):nullptr;
        const auto& appearanceState=branch?branch->properties:state;
        std::optional<float> normalizedPosition;
        if(lightning && (linked || streak)) {



            const auto localIndex=branch?index-*branch->flattenedStart:index;
            const auto localCount=branch?branch->particles.size():sim.lightningMainCount;
            normalizedPosition=static_cast<float>(localIndex)*(1.0f/static_cast<float>(localCount));
        }
        const auto appearance=emitterParticleAppearance(data,appearanceState,p,normalizedPosition);
        const auto frame=emitterParticleFrames(data,appearanceState,p)[pass];
        const auto eye=transformPoint(view,lightning&&linked?lightningLinkedPosition(sim,index):p.position);




        const auto scale=branch?1.0f:engineOwnerScale
            ? ownerScale : p.sourceScale;
        float hx=appearance.size.x*scale*0.5f;
        float hy=appearance.size.y*scale*0.5f;
        Vec3 right{1,0,0},up{0,1,0};
        const auto render=lightning&&!linked&&!streak?EmitterRender::Normal:support.render;
        if(engineOwnerScale && !linked && !streak) {
            // RenderStandard preserves quaternion multiplication before its
            // xdir access. Mixing already-derived columns is not equivalent
            // for authored non-unit orientations. Camera is passed raw too.
            Quaternion base=*cameraOrientation;
            if(render==EmitterRender::BillboardWorldZ) base={};
            else if(render==EmitterRender::BillboardLocalZ) base=sim.frame.orientation;
            const auto forward=engineQuaternionAxis(base,2);
            const auto spin=engineQuaternionProduct(engineQuaternionAxisAngle(
                p.stopped?p.direction:forward,p.rotation),
                p.stopped?p.orientation:base);
            if(render==EmitterRender::AlignedParticleDirection) {
                right=engineQuaternionAxis(p.orientation,0);
                up=engineQuaternionAxis(p.orientation,2);
            } else {
                right=engineQuaternionAxis(spin,0);
                up=render==EmitterRender::AlignedWorldZ
                    ? Vec3{0,0,engineQuaternionAxis(*cameraOrientation,1).z<0.0f?-1.0f:1.0f}
                    : cross(splat?p.direction:forward,right);
            }
            right=transformDirection(view,right);up=transformDirection(view,up);
        } else {
        if(render==EmitterRender::Normal && p.stopped) {
            right=transformDirection(view,p.right);up=transformDirection(view,p.up);
        } else if(render==EmitterRender::BillboardWorldZ) {
            right=transformDirection(view,{1,0,0});up=transformDirection(view,{0,1,0});
        } else if(render==EmitterRender::BillboardLocalZ) {
            right=transformDirection(view,axis(sim.world,0));up=transformDirection(view,axis(sim.world,1));
        } else if(render==EmitterRender::AlignedParticleDirection) {
            right=transformDirection(view,p.right);up=transformDirection(view,p.direction);
        } else if(render==EmitterRender::AlignedWorldZ) {
            // Only the UP axis is replaced with world Z. The packed retains
            // the rotated camera-right axis, including its world-Z component.
            right={std::cos(p.rotation),std::sin(p.rotation),0};
            up=transformDirection(view,{0,0,1});
        }
        if(render!=EmitterRender::AlignedParticleDirection &&
           render!=EmitterRender::AlignedWorldZ && !linked && !streak) {
            const float c=std::cos(p.rotation),sn=std::sin(p.rotation);
            const auto a=right;right=add(mul(a,c),mul(up,sn));up=add(mul(up,c),mul(a,-sn));
        }
        if(splat && !linked && !streak && render!=EmitterRender::AlignedWorldZ &&
           render!=EmitterRender::AlignedParticleDirection)
            up=cross(transformDirection(view,p.direction),right);
        }
        std::array<Vec3,4> corners;
        float tailAlpha=1;
        if(linked || streak) {
            const auto other=transformPoint(view,linked ?
                (lightning?lightningLinkedPosition(sim,index+1):sim.particles[index+1].position) : p.tail);
            const auto start=linked?eye:other, finish=linked?other:eye;
            const auto delta=sub(finish,start);
            const float projected=std::sqrt(delta.x*delta.x+delta.y*delta.y);
            // The engine threshold is a double 1e-5. MotionBlur additionally
            // keeps the camera axes for a projected length exactly one.
            const bool rotateAxes=static_cast<double>(projected)>1.0e-5 &&
                (!streak || projected!=1.0f);
            const Vec3 along=rotateAxes ? Vec3{delta.x/projected,delta.y/projected,0} : Vec3{0,1,0};
            const Vec3 across{along.y,-along.x,0};
            // Linked/motion renderers retain an observable doubled endpoint
            // width for held percentage keys; interpolated spans use half size.
            const auto k=segment(appearanceState,p,normalizedPosition);
            const float held=appearanceState.lifeExpectancy>=0 && appearanceState.percentStart!=255 && k.first==k.second ? 2.0f : 1.0f;
            hx*=held;hy*=held;
            const auto width=mul(across,2*hx), extension=mul(along,0.5f*hy);
            if(streak) {
                corners={{add(add(finish,width),extension),add(sub(finish,width),extension),
                          sub(sub(start,width),extension),sub(add(start,width),extension)}};
                const float unscaledWidth=appearance.size.x*held;
                const float ratio=unscaledWidth/(projected==0?1.0f:projected);
                tailAlpha=ratio>1.0f?1.0f:ratio;
            } else {
                corners={{sub(add(start,width),extension),add(add(finish,width),extension),
                          add(sub(finish,width),extension),sub(sub(start,width),extension)}};
                if(output.vertices.size()>=4 && previousSegment+1==index) {
                    corners[0]=output.vertices[output.vertices.size()-3].position;
                    corners[3]=output.vertices[output.vertices.size()-2].position;
                }
            }
        } else {
            right=mul(right,hx);up=mul(up,hy);
            corners={{sub(sub(eye,right),up),sub(add(eye,right),up),add(add(eye,right),up),add(sub(eye,right),up)}};
        }
        previousSegment=index;
        // Engine frame blending writes zero UV coordinates for absent neighbor
        // frames. The base frame and out-of-grid authored frames are not capped.
        const int f=pass && frame.frame<0 ? 0 : frame.frame;





        const auto columns=static_cast<std::int32_t>(appearance.columns);
        const auto rows=static_cast<std::int32_t>(appearance.rows);
        const float du=1.0f/static_cast<float>(columns),dv=1.0f/static_cast<float>(rows);
        const int row=integer(static_cast<float>(f)*du);
        const auto column=static_cast<std::int32_t>(static_cast<std::uint32_t>(f)-
            static_cast<std::uint32_t>(row)*appearance.columns);
        const float u0=static_cast<float>(column)*du;
        const float u1=static_cast<float>(engineIntAdd(column,1))*du;
        const float v0=1.0f-static_cast<float>(engineIntAdd(row,1))*dv;
        const float v1=1.0f-static_cast<float>(row)*dv;
        std::array<Vec2,4> uv{{{u0,v0},{u1,v0},{u1,v1},{u0,v1}}};
        if(streak) uv={{{u1,v1},{u0,v1},{u0,v0},{u1,v0}}};
        if(linked) uv={{{u1,v0},{u1,v1},{u0,v1},{u0,v0}}};
        const auto first=static_cast<std::uint32_t>(output.vertices.size());
        for(std::size_t n=0;n<4;++n) {
            Vertex vertex;vertex.position=corners[n];vertex.texcoord0=uv[n];
            std::uint32_t rgba=0;
            for(std::size_t c=0;c<4;++c) {
                float value=appearance.color[c];
                if(c<3) {
                    // Engine particle renderers pack controller RGB directly;
                    // owner alpha is consumed below, but owner tint is absent.
                    if(!engineOwnerScale && !branch)
                        value*=c==0?sim.renderTint.x:c==1?sim.renderTint.y:sim.renderTint.z;
                } else value*=(branch?1.0f:sim.renderAlpha)*frame.weight*(streak && n>=2 ? tailAlpha : 1.0f);
                // RenderStandard clamps evaluated colors before owner/pass
                // modulation, then truncates, shifts and ORs the four integers.
                rgba|=static_cast<std::uint32_t>(integer(value*255.0f))<<(c*8);
            }
            for(std::size_t c=0;c<4;++c) vertex.color[c]=static_cast<std::uint8_t>(rgba>>(c*8));
            output.vertices.push_back(vertex);
        }
        for(const unsigned n:{0u,1u,2u,0u,2u,3u}) output.indices.push_back(first+n);
    }
}
void buildEmitterMesh(const EmitterNodeData& data,const NodeEmitterState& state,
    const EmitterSimulation& sim,const Mat4& view,const Quaternion& cameraOrientation,Mesh& output,
    std::uint32_t,std::uint32_t,unsigned pass,float ownerScale) {
    buildEmitterMeshWithScale(data,state,sim,view,output,pass,ownerScale,true,&cameraOrientation);
}
void buildLegacyEmitterMesh(const EmitterNodeData& data,const NodeEmitterState& state,
    const EmitterSimulation& sim,const Mat4& view,Mesh& output,
    std::uint32_t,std::uint32_t,unsigned pass) {
    buildEmitterMeshWithScale(data,state,sim,view,output,pass,1.0f,false);
}
} // namespace neomdl::model
