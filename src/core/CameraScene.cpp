#include "core/CameraScene.hpp"

#include <algorithm>
#include <cmath>

namespace neomdl {
namespace m = neomdl::model;
namespace {

m::Vec3 subtract(m::Vec3 left, m::Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
m::Vec3 scale(m::Vec3 value, float amount) noexcept {
    return {value.x * amount, value.y * amount, value.z * amount};
}
float dot(m::Vec3 left, m::Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
std::string lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

m::Quaternion product(m::Quaternion a, m::Quaternion b) noexcept {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
            a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
            a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
float engineYaw(m::Quaternion q) noexcept {
    const float y = -2.0f*(q.x*q.y-q.w*q.z);
    const float x = 1.0f-2.0f*(q.x*q.x+q.z*q.z);
    // Yaw(Quaternion) explicitly returns zero for (0,0).
    return y==0.0f && x==0.0f ? 0.0f : std::atan2(y,x)*57.2957763671875f;
}
float enginePitch(m::Quaternion q) noexcept {
    const float a=2.0f*(q.x*q.y-q.w*q.z);
    const float b=1.0f-2.0f*(q.x*q.x+q.z*q.z);
    return std::atan2(2.0f*(q.y*q.z+q.w*q.x),std::sqrt(a*a+b*b))*57.2957763671875f;
}
m::Quaternion axisAngle(m::Vec3 axis,float degrees) noexcept {
    const float angle=(degrees*0.01745329238474369f)*0.5f;
    const float sine=std::sin(angle);
    return {axis.x*sine,axis.y*sine,axis.z*sine,std::cos(angle)};
}
m::Quaternion placeableOrientation(m::Quaternion q,float pitchOffset) noexcept {
    // UpdateShotCamera 0x2a6742 and SetComputerCameraShot 0x2a7da0:
    // YawPitchRoll(Yaw(q), Pitch(q)+Pitch, Roll(q)), all angles in degrees.
    // Roll (0x837b90) removes yaw and pitch before extracting rotation about Y.
    const float yaw=engineYaw(q);
    auto residual=product(axisAngle({0,0,1},-yaw),q);
    residual=product(axisAngle({1,0,0},-enginePitch(residual)),residual);
    const float roll=std::atan2(-2.0f*(residual.x*residual.z-residual.w*residual.y),
        1.0f-2.0f*(residual.y*residual.y+residual.z*residual.z))*57.2957763671875f;
    return product(product(axisAngle({0,0,1},yaw),
        axisAngle({1,0,0},enginePitch(q)+pitchOffset)),axisAngle({0,1,0},roll));
}

m::Mat4 inverseRigid(const m::Mat4& matrix) noexcept {
    // GLRender::SetViewTransform applies the inverse orientation followed by
    // translation by -position. This is the equivalent column-major matrix.
    const m::Vec3 x{matrix.values[0], matrix.values[1], matrix.values[2]};
    const m::Vec3 y{matrix.values[4], matrix.values[5], matrix.values[6]};
    const m::Vec3 z{matrix.values[8], matrix.values[9], matrix.values[10]};
    const m::Vec3 position{matrix.values[12], matrix.values[13],
                           matrix.values[14]};
    auto result = m::identityMatrix();
    result.values[0] = x.x;
    result.values[1] = y.x;
    result.values[2] = z.x;
    result.values[4] = x.y;
    result.values[5] = y.y;
    result.values[6] = z.y;
    result.values[8] = x.z;
    result.values[9] = y.z;
    result.values[10] = z.z;
    result.values[12] = -dot(x, position);
    result.values[13] = -dot(y, position);
    result.values[14] = -dot(z, position);
    return result;
}

m::Vec3 add(m::Vec3 a,m::Vec3 b) noexcept {return {a.x+b.x,a.y+b.y,a.z+b.z};}
m::Vec3 cameraNormalize(m::Vec3 v) noexcept {
    // Vector::Normalize: K1 Desktop 0x3a5b92 / K2 Desktop 0x313640
    // use the small-vector +X rule.
    const float length=std::sqrt(dot(v,v));
    return static_cast<double>(length)<1.0e-9?m::Vec3{1,0,0}:scale(v,1.0f/length);
}
m::Vec3 cameraPoint(const DialogueCameraActor& actor,bool oldHitCheck) {
    auto point=actor.position;
    point.z+=0.1f;
    if(!oldHitCheck) {
        point.z+=actor.cameraHeight;
        if(actor.cameraHook) point=*actor.cameraHook;
    }
    return point;
}
SceneHitInfo cameraHitInfo(const DialogueCameraActor& a,const DialogueCameraActor& b) {
    SceneHitInfo info;
    info.surfaceMask=0xbfffffffu;
    info.ignoredOwners=a.ignoredOwners;
    info.ignoredOwners.insert(info.ignoredOwners.end(),b.ignoredOwners.begin(),b.ignoredOwners.end());
    return info;
}
RuntimeAreaCamera cameraFrame(m::Vec3 position,m::Quaternion orientation) {
    RuntimeAreaCamera frame;
    frame.authored.position=position;
    frame.authored.orientation=orientation;
    frame.orientation=orientation;
    // SetShot resets the ordinary dialogue camera to 55 degrees.
    frame.authored.fieldOfView=55.0f;
    frame.position=position;
    frame.world=m::nodeTransform(position,orientation);
    frame.view=inverseRigid(frame.world);
    frame.right={frame.world.values[0],frame.world.values[1],frame.world.values[2]};
    frame.up={frame.world.values[4],frame.world.values[5],frame.world.values[6]};
    frame.forward={-frame.world.values[8],-frame.world.values[9],-frame.world.values[10]};
    return frame;
}

} // namespace

model::Vec3 engineCameraMouseDirection(const CameraHitState& camera,int x,int y,
                                      model::GameVersion game) noexcept {
    // MouseDir: K1 Desktop 0x2e790a loads float tan; K2 Desktop 0x12a768
    // loads double tan using the already rounded float argument.
    const float radians=(camera.viewAngle*0.5f)*0.01745329238474369f;
    const float tangent=game==m::GameVersion::Kotor1?std::tan(radians):
        static_cast<float>(std::tan(static_cast<double>(radians)));
    const auto& viewport=camera.viewport;
    const float width=static_cast<float>(viewport.width);
    const float height=static_cast<float>(viewport.height);
    const float denominator=height*0.5f;
    const float px=static_cast<float>(x-viewport.x);
    const float py=static_cast<float>(y-viewport.y);
    const bool k1=game==m::GameVersion::Kotor1;
    const float dx=k1?px+width*(-0.5f):px-width*0.5f;
    const float dy=k1?py+height*(-0.5f):py-height*0.5f;
    const m::Vec3 local{(dx*tangent)/denominator,(dy*tangent)/denominator,-1.0f};
    return cameraNormalize(engineRotate(camera.orientation,local));
}
std::optional<CameraHitSegment> engineCameraHitSegment(const CameraHitState& camera,
    int x,int y,model::GameVersion game) noexcept {
    // Camera::HitCheck K1 0x2e7a2e / K2 Desktop 0x12a4da invokes visual
    // Scene::HitCheck directly. Game SceneHitCheck is a different caller.
    if(camera.viewAngle>0.0f) {
        const auto direction=engineCameraMouseDirection(camera,x,y,game);
        return CameraHitSegment{add(camera.position,scale(direction,camera.nearClip)),
                                add(camera.position,scale(direction,camera.farClip))};
    }
    const auto& viewport=camera.viewport;
    const int px=x-viewport.x,py=y-viewport.y;
    if(px<0||py<0||px>=viewport.width||py>=viewport.height)return {};
    const float height=static_cast<float>(viewport.height);
    const float wx=((static_cast<float>(px)+0.5f)/height)*camera.orthographicHeight+camera.position.x;
    const float wy=((static_cast<float>(py)+0.5f)/height)*camera.orthographicHeight+camera.position.y;
    const float nearZ=-camera.nearClip;
    return CameraHitSegment{{wx,wy,nearZ+camera.position.z},
                           {wx,wy,(nearZ-camera.farClip)+camera.position.z}};
}

// K1 Desktop 0x203ee4/0x204024 uses the same hash and 19-angle sequence.
void DialogueCameraSequence::initialize(const std::string& engineResref,
    std::vector<std::uint32_t> authoredAngles,model::GameVersion game) {
    cursor_.initialize(engineResref,game);
    firstEntry_=firstReplies_=true;
    angles_=std::move(authoredAngles);
}
std::uint32_t DialogueCameraSequence::next() {
    // Constructor 0x6b1ee2 supplies this 19-value array. AssignPseudoRandom-
    // CameraAngle 0x6b82b0 clips at the opposite end, rather than modulo 19.
    constexpr std::array<std::uint32_t,19> sequence={1,3,1,3,2,2,1,3,1,2,1,2,1,3,2,3,1,1,1};
    return cursor_.next(sequence);
}
void DialogueCameraSequence::beginEntry(std::size_t lineIndex) {
    const auto value=next();
    auto& angle=angles_.at(lineIndex);
    if(angle==0u) angle=firstEntry_?2u:value;
    firstEntry_=false;
}
void DialogueCameraSequence::prepareReplies(const std::vector<std::size_t>& lineIndices) {
    const auto value=next();
    for(const auto index:lineIndices) {
        auto& angle=angles_.at(index);
        if(angle==0u) angle=firstReplies_?2u:value;
    }
    firstReplies_=false;
}

std::optional<m::Vec3> dialogueCreatureCameraHook(const m::Model& body,
    const m::Pose& pose,const m::EngineFrame& owner) {
    for(std::size_t i=0;i<body.nodes.size();++i) {
        if(lower(body.nodes[i].name)!="camerahook") continue;
        return m::enginePartWorld(body,pose,owner,i).position;
    }
    return std::nullopt;
}

float dialogueCreatureHeadHeight(const m::Model* body,const m::Pose& pose,
    const m::EngineFrame& owner,float creaturePositionZ,float appearanceHeight) {
    // K1 Desktop 0x84548 / K2 GetHeadHeightOffset 0x34f130. Its first
    // branch uses Gob::GetPartLocalPosition 0x701e50, which copies the raw
    // Part local position; HEAD_G uses Gob::GetPartPosition instead.
    if(!body) return 0.0f;
    for(std::size_t i=0;i<body->nodes.size();++i)
        if(lower(body->nodes[i].name)=="camerahook") return pose.positions.at(i).z;
    float headZ=0.0f;
    for(std::size_t i=0;i<body->nodes.size();++i) {
        if(lower(body->nodes[i].name)!="head_g") continue;
        headZ=m::enginePartWorld(*body,pose,owner,i).position.z;
        break;
    }
    if(headZ==0.0f) return appearanceHeight;
    const float height=headZ-creaturePositionZ;
    return height<0.0f?appearanceHeight:height;
}

void DialogueCameraScene::reset() noexcept {
    lines_={};options_={};shot_=0u;obstructed_=false;fixedPosition_={};frame_.reset();
    speakerHeight_=listenerHeight_=0.0f;
    retainedPullback_=0.0f;
}
void DialogueCameraScene::setPlaceableCamera(const RuntimeAreaCamera& camera) {
    // Fixed SetShot 0x4d6a10 owns the +0x40 position read by Control.
    fixedPosition_=camera.position;
    frame_=camera;
    shot_=6u;
}

float DialogueCameraScene::selectPullback(DialogueActorShot shot,
    std::optional<float> tableValue,bool speakerAppearance565,m::GameVersion game) {
    // UpdateShotCamera 0x2a66ba..0x2a671d and 0x2a6aa3..0x2a6c24.
    if(shot!=DialogueActorShot::Closeup) {retainedPullback_=0.0f;return 0.0f;}
    float value=0.0f;
    if(tableValue) {
        value=*tableValue;
        if(static_cast<double>(value)<0.001 && static_cast<double>(value)>-0.001)
            value=0.0f;
        // The appearance-565 adjustment is absent from K1 UpdateShotCamera 0x51476.
        if(game==m::GameVersion::Kotor2 && speakerAppearance565) value+=0.25f;
        if(value==0.0f) retainedPullback_=0.0f;
    }
    if(retainedPullback_>0.0f) return retainedPullback_;
    retainedPullback_=value;
    return value;
}

DialogueCameraScene::Parameters DialogueCameraScene::parameters(
    std::uint32_t shot,std::uint32_t side,m::Vec3 a,m::Vec3 b,
    const DialogueCameraActor& speaker,const DialogueCameraActor& listener,
    const SceneQueries& queries,m::Vec3 retainedPosition) {
    // GetCameraParameters 0x4d2b10. Equal participant points are deliberately
    // separated by the engine (1.5,1.5,0.1), and mark the shot obstructed.
    if(a.x==b.x && a.y==b.y && a.z==b.z) {
        b=add(a,{1.5f,1.5f,0.1f});
        obstructed_=true;
    }
    Parameters result;
    result.position=retainedPosition;
    const bool closeup=shot==1u || (!options_.oldHitCheck && obstructed_);
    if(closeup) {
        if(options_.oldHitCheck) {
            a.z+=speaker.cameraHeight;
            b.z+=speaker.cameraHeight;
        } else b.z=a.z;
        result.target=subtract(a,scale(cameraNormalize(subtract(a,b)),0.2f));
        result.target.z+=-0.04f+options_.targetHeightOffset;
        result.direction=cameraNormalize(engineRotate(axisAngle({0,0,1},side==2u?-30.0f:30.0f),subtract(a,b)));
        const float pullback=options_.oldHitCheck || obstructed_?0.0f:options_.pullback;
        result.target.z-=0.2f*pullback;
        result.position=subtract(result.target,scale(cameraNormalize(result.direction),0.5f+pullback));
        result.position.z+=options_.heightOffset+0.2f*pullback;
    } else {
        const float distance=std::sqrt(dot(subtract(b,a),subtract(b,a)));
        const bool twoShot=shot==3u;
        const bool overShoulder=shot==2u;
        // K1 0xf11df..0xf12c0 has no K2 global shot overrides.
        const bool k2=options_.game==m::GameVersion::Kotor2;
        const float targetFraction=k2 && options_.targetFraction!=0.0f?options_.targetFraction:(twoShot?0.5f:overShoulder?0.3f:1.0f);
        const float distanceFraction=k2 && options_.distanceFraction!=0.0f?options_.distanceFraction:(twoShot?1.5f:overShoulder?0.8f:0.5f);
        // K1 initializes unhandled angle4 to zero; K2 retains 30 degrees.
        float angle=k2 && options_.yawDegrees!=0.0f?options_.yawDegrees:(twoShot?90.0f:(overShoulder || k2)?30.0f:0.0f);
        if(side==2u) angle=-angle;
        if(options_.oldHitCheck) {a.z+=speaker.cameraHeight;b.z+=listener.cameraHeight;}
        if(overShoulder) {a.z+=distance*-0.1f;b.z+=distance*-0.05f;}
        result.target=add(b,scale(subtract(a,b),targetFraction));
        result.target.z+=(twoShot?distance*-0.2f:0.0f)+options_.targetHeightOffset;
        result.direction=engineRotate(axisAngle({0,0,1},angle),subtract(a,b));
        auto offset=scale(result.direction,distanceFraction);
        if(overShoulder) offset=add(offset,scale(cameraNormalize(result.direction),0.15f));
        // Angle 4 has no positioning branch in GetCameraParameters. Engine
        // preserves the supplied position, then still adds the height offset.
        if(twoShot || overShoulder) result.position=subtract(result.target,offset);
        result.position.z+=options_.heightOffset+(twoShot?distance*0.3f:0.0f);
    }
    // Only OldHitCheck's obstructed path shortens the target-to-camera ray.
    if(options_.oldHitCheck && obstructed_) {
        auto hit=cameraHitInfo(speaker,listener);
        if(queries.hit(result.target,result.position,hit))
            result.position=subtract(hit.position,scale(cameraNormalize(subtract(hit.position,result.target)),0.1f));
    }
    const auto direction=cameraNormalize(result.direction);
    const float yaw=direction.x==0.0f && direction.y==0.0f?0.0f:
        std::atan2(-direction.x,direction.y)*57.2957763671875f;
    const auto toTarget=cameraNormalize(subtract(result.target,result.position));
    const float pitch=std::atan2(toTarget.z,std::sqrt(toTarget.x*toTarget.x+toTarget.y*toTarget.y))*57.2957763671875f+90.0f;
    result.orientation=product(axisAngle({0,0,1},yaw),axisAngle({1,0,0},pitch));
    return result;
}

std::uint32_t DialogueCameraScene::savedSide(HitOwnerId speaker,HitOwnerId listener) const noexcept {
    for(const auto& line:lines_) {
        if(line.side==0u) break;
        if(line.speaker==speaker && line.listener==listener) return line.side;
        if(line.speaker==listener && line.listener==speaker) return line.side==2u?1u:2u;
    }
    return 0u;
}

std::uint32_t DialogueCameraScene::sideFor(const DialogueCameraActor& speaker,
    const DialogueCameraActor& listener,const SceneQueries& queries) {
    // AddCalculatedLineOfAction 0x4d68e0 retains four pairs. OldHitCheck
    // retains the original side even when the speaker/listener order reverses.
    std::size_t replacement=0u;
    for(std::size_t i=0;i<lines_.size();++i) {
        const auto& line=lines_[i];
        if(line.side==0u) {replacement=i;break;}
        if((line.speaker==speaker.object && line.listener==listener.object) ||
           (line.listener==speaker.object && line.speaker==listener.object))
            return options_.oldHitCheck?line.side:savedSide(speaker.object,listener.object);
    }
    const auto a=cameraPoint(speaker,options_.oldHitCheck);
    const auto b=cameraPoint(listener,options_.oldHitCheck);
    auto middle=scale(add(speaker.position,listener.position),0.5f);
    middle.z+=0.1f+speaker.cameraHeight+(speaker.cameraHeight-listener.cameraHeight)*0.5f;
    auto ray=[&](m::Vec3 start,m::Vec3 end) {
        auto info=cameraHitInfo(speaker,listener);
        return queries.hit(start,end,info)?1:0;
    };
    // GetSideUnobstructed 0x4d46e0 scores the two-shot and both reverse
    // over-shoulder shots on each side; a tie selects side 2.
    auto score=[&](std::uint32_t side) {
        auto camera=parameters(3u,side,a,b,speaker,listener,queries);
        int count=ray(middle,camera.position);
        if(!options_.oldHitCheck) {count+=ray(a,camera.position);count+=ray(b,camera.position);}
        camera=parameters(2u,side,a,b,speaker,listener,queries);
        count+=ray(a,camera.position);
        camera=parameters(2u,side==2u?1u:2u,b,a,speaker,listener,queries);
        count+=ray(b,camera.position);
        return count;
    };
    const auto second=score(2u);
    const auto first=score(1u);
    const std::uint32_t side=second>first?1u:2u;
    lines_[replacement]={speaker.object,listener.object,side};
    return side;
}

bool DialogueCameraScene::setShot(DialogueActorShot requested,const DialogueCameraActor& speaker,
    const DialogueCameraActor& listener,const DialogueCameraOptions& options,
    const SceneQueries& queries) {
    const auto shot=static_cast<std::uint32_t>(requested);
    // SetShot deliberately keeps consecutive two-shots unchanged.
    if(shot==3u && shot_==3u) return false;
    options_=options;
    speakerHeight_=speaker.cameraHeight;
    listenerHeight_=listener.cameraHeight;
    obstructed_=false;
    const auto side=sideFor(speaker,listener,queries);
    auto a=cameraPoint(speaker,options_.oldHitCheck),b=cameraPoint(listener,options_.oldHitCheck);
    if(options_.oldHitCheck) {a=speaker.position;b=listener.position;}
    const auto camera=parameters(shot,side,a,b,speaker,listener,queries);
    auto rawA=speaker.position,rawB=listener.position;
    rawA.z+=0.1f;rawB.z+=0.1f;
    auto ray=[&](m::Vec3 start) {
        auto info=cameraHitInfo(speaker,listener);
        return queries.hit(start,camera.position,info);
    };
    bool blocked;
    if(shot==3u) {
        auto middle=scale(add(rawA,rawB),0.5f);
        middle.z+=speaker.cameraHeight+(speaker.cameraHeight-listener.cameraHeight)*0.5f;
        blocked=ray(middle);
        if(!options_.oldHitCheck) {const bool first=ray(a),second=ray(b);blocked=blocked||first||second;}
    } else blocked=ray(options_.oldHitCheck?rawA:a);
    if(blocked) obstructed_=true;
    shot_=shot;
    update(speaker,listener,queries);
    return true;
}

void DialogueCameraScene::update(const DialogueCameraActor& currentSpeaker,
    const DialogueCameraActor& currentListener,const SceneQueries& queries) {
    // SetShot caches the virtual height getters at +0x14/+0x18; Control
    // follows fresh owner/hook positions without recomputing those heights.
    auto speaker=currentSpeaker,listener=currentListener;
    speaker.cameraHeight=speakerHeight_;
    listener.cameraHeight=listenerHeight_;
    // Control 0x4d276c requires the recorded line of action; it does not
    // choose a new one while following animated hooks.
    const auto side=savedSide(speaker.object,listener.object);
    if(side==0u) return;
    const auto a=options_.oldHitCheck?speaker.position:cameraPoint(speaker,false);
    const auto b=options_.oldHitCheck?listener.position:cameraPoint(listener,false);
    const auto camera=parameters(shot_,side,a,b,speaker,listener,queries,fixedPosition_);
    frame_=cameraFrame(camera.position,camera.orientation);
}

void CameraScene::setDocument(const ModelDocument* document) {
    document_ = document;
    reset();
}

void CameraScene::reset() {
    attachments_.clear();
    areaCameras_.clear();
    activeAttachment_.reset();
    activeFrame_.reset();
    activeAreaCamera_.reset();
    ++attachmentRevision_;
    engineRuntime_ = document_ && document_->isOpen();
    ++revision_;
}

void CameraScene::appendObject(const m::Model& model, const m::Pose& pose,
                               const m::EngineFrame& owner,
                               std::optional<std::size_t> reference) {
    for (std::size_t nodeIndex = 0u; nodeIndex < model.nodes.size(); ++nodeIndex) {
        const auto& node = model.nodes[nodeIndex];
        const bool cameraNode = node.camera.has_value();
        const bool engineHook = !reference &&
            lower(node.name) == kK2AnimatedCameraHookName;
        if (!cameraNode && !engineHook) continue;

        RuntimeCameraAttachment runtime;
        runtime.attachment = {reference, nodeIndex};
        runtime.nodeName = node.name;
        runtime.cameraNode = cameraNode;
        runtime.engineHook = engineHook;
        const auto part=m::enginePartWorld(model,pose,owner,nodeIndex);
        runtime.orientation=part.orientation;
        runtime.world=m::nodeTransform(part.position,part.orientation);
        runtime.view = inverseRigid(runtime.world);
        runtime.position = {runtime.world.values[12], runtime.world.values[13],
                            runtime.world.values[14]};
        runtime.right = {runtime.world.values[0], runtime.world.values[1],
                         runtime.world.values[2]};
        runtime.up = {runtime.world.values[4], runtime.world.values[5],
                      runtime.world.values[6]};
        // OpenGL's identity view looks along local -Z. The camera object uses
        // the same convention through GLRender::SetViewTransform.
        runtime.forward = {-runtime.world.values[8], -runtime.world.values[9],
                           -runtime.world.values[10]};
        attachments_.push_back(std::move(runtime));
    }
}

void CameraScene::synchronize(const m::Pose& mainPose, bool includeReferences,
                              bool updateAttachment) {
    attachments_.clear();
    if (!engineRuntime_ || !document_ || !document_->isOpen()) {
        ++revision_;
        return;
    }

    const auto mainFrame = references_ ? references_->mainFrame() : m::EngineFrame{};
    appendObject(document_->model(), mainPose, mainFrame, std::nullopt);

    if (includeReferences && references_) {
        const auto& definitions = document_->references();
        for (std::size_t reference = 0u;
             reference < references_->instances().size() &&
             reference < definitions.size(); ++reference) {
            const auto& instance = references_->instances()[reference];
            if (!instance.active) continue;
            appendObject(definitions[reference].model, instance.pose,
                         instance.frame,reference);
        }
    }
    // Only this Camera's attachment Control (or an explicit inspection sync)
    // writes its frame. Parent controller updates refresh candidates without
    // overwriting a Camera already sampled during synchronous event reentry.
    if (updateAttachment && activeAttachment_)
        if (const auto* selected = find(*activeAttachment_)) activeFrame_ = *selected;
    ++revision_;
}

const RuntimeCameraAttachment* CameraScene::find(
    const CameraAttachment& attachment) const noexcept {
    const auto found = std::find_if(attachments_.begin(), attachments_.end(),
        [&attachment](const RuntimeCameraAttachment& runtime) {
            return runtime.attachment == attachment;
        });
    return found == attachments_.end() ? nullptr : &*found;
}

bool CameraScene::activate(const CameraAttachment& attachment) {
    const auto* runtime = find(attachment);
    if (!runtime || (!runtime->cameraNode && !runtime->engineHook)) return false;
    activeAttachment_ = attachment;
    activeFrame_ = *runtime;
    activeAreaCamera_.reset();
    ++attachmentRevision_;
    return true;
}

bool CameraScene::activateMainCameraNode(std::size_t nodeIndex) {
    const CameraAttachment attachment{std::nullopt, nodeIndex};
    const auto* runtime = find(attachment);
    if (!runtime || !runtime->cameraNode) return false;
    activeAttachment_ = attachment;
    activeFrame_ = *runtime;
    activeAreaCamera_.reset();
    ++attachmentRevision_;
    return true;
}

bool CameraScene::activateEngineHook() {
    const auto* hook = engineHook();
    if (!hook) return false;
    activeAttachment_ = hook->attachment;
    activeFrame_ = *hook;
    activeAreaCamera_.reset();
    ++attachmentRevision_;
    return true;
}

bool CameraScene::setAreaCameras(const std::vector<AreaCameraRecord>& records,
                                 std::string& error) {
    error.clear();
    const std::size_t capacity=document_ && document_->model().game==m::GameVersion::Kotor1?50u:70u;
    if(records.size()>capacity) {error="CameraList exceeds engine "+std::to_string(capacity)+"-camera registry";return false;}
    std::vector<RuntimeAreaCamera> pending;pending.reserve(records.size());
    for(const auto& record:records) {
        RuntimeAreaCamera runtime;runtime.authored=record;
        auto position=record.position;
        position.z+=record.height;
        // The engine GIT consumer passes the authored quaternion directly to
        // Yaw/Pitch/Roll; there is no quaternion normalization or matrix repair.
        runtime.orientation=placeableOrientation(record.orientation,record.pitch);
        runtime.world=m::nodeTransform(position,runtime.orientation);
        runtime.view=inverseRigid(runtime.world);
        runtime.position={runtime.world.values[12],runtime.world.values[13],runtime.world.values[14]};
        runtime.right={runtime.world.values[0],runtime.world.values[1],runtime.world.values[2]};
        runtime.up={runtime.world.values[4],runtime.world.values[5],runtime.world.values[6]};
        runtime.forward={-runtime.world.values[8],-runtime.world.values[9],-runtime.world.values[10]};
        pending.push_back(std::move(runtime));
    }
    areaCameras_=std::move(pending);
    if(activeAreaCamera_)++attachmentRevision_;
    activeAreaCamera_.reset();++revision_;return true;
}

void CameraScene::clearAreaCameras() {
    if(areaCameras_.empty()&&!activeAreaCamera_)return;
    if(activeAreaCamera_)++attachmentRevision_;
    areaCameras_.clear();activeAreaCamera_.reset();++revision_;
}

bool CameraScene::activateAreaCamera(std::size_t index) {
    if(index>=areaCameras_.size())return false;
    activeAreaCamera_=index;activeAttachment_.reset();activeFrame_.reset();
    ++attachmentRevision_;++revision_;return true;
}

bool CameraScene::activateAreaCameraId(std::int32_t cameraId) {
    // Engine UpdateShotCamera/SetComputerCameraShot scan CameraList in order.
    for(std::size_t i=0;i<areaCameras_.size();++i)
        if(areaCameras_[i].authored.cameraId==cameraId) return activateAreaCamera(i);
    return false;
}

const RuntimeCameraAttachment* CameraScene::active() const noexcept {
    return activeFrame_ ? &*activeFrame_ : nullptr;
}

const RuntimeAreaCamera* CameraScene::activeAreaCamera() const noexcept {
    return activeAreaCamera_&&*activeAreaCamera_<areaCameras_.size()
        ?&areaCameras_[*activeAreaCamera_]:nullptr;
}

const RuntimeCameraAttachment* CameraScene::mainNode(
    std::size_t nodeIndex) const noexcept {
    return find({std::nullopt, nodeIndex});
}

const RuntimeCameraAttachment* CameraScene::engineHook() const noexcept {
    const auto found = std::find_if(attachments_.begin(), attachments_.end(),
        [](const RuntimeCameraAttachment& runtime) {
            return runtime.engineHook;
        });
    return found == attachments_.end() ? nullptr : &*found;
}

bool CameraScene::activeUsesEngineHook() const noexcept {
    const auto* runtime = active();
    return runtime && runtime->engineHook;
}

} // namespace neomdl
