#include "core/TrackScene.hpp"
#include <gff/GFFFile.hpp>
#include <cmath>
#include <stdexcept>

namespace neomdl {
namespace {
using namespace neogff;
template<class T> const T* field(const GffStruct& record,const char* name) {
    return dynamic_cast<const T*>(record.GetFieldByLabel(name));
}
std::string resource(const GffStruct& record,const char* name) {
    const auto* value=field<GffResRefField>(record,name);
    return value?resourceKey(value->GetString()):std::string{};
}
bool byte(const GffStruct& record,const char* name,bool initial) {
    const auto* value=field<GffByteField>(record,name);return value?value->value!=0:initial;
}
float number(const GffStruct& record,const char* name) {
    const auto* value=field<GffFloatField>(record,name);return value?value->value:0.0f;
}
std::optional<TrackFollowerDefinition> follower(const GffStruct& record,
    TrackFollowerKind kind,std::size_t index,const neoshared::bif::JobControl& job) {
    TrackFollowerDefinition result;result.kind=kind;result.sourceIndex=index;
    const auto* models=field<GffList>(record,"Models");
    if(!models)return {};
    for(const auto& entry:models->allStructs()) {
        job.check();auto name=resource(*entry,"Model");
        if(!name.empty())result.models.push_back({std::move(name),byte(*entry,"RotatingModel",true)});
    }
    if(result.models.empty())return {};
    result.track=resource(record,"Track");if(result.track.empty())return {};
    const auto* loops=field<GffIntField>(record,"Num_Loops");
    const auto value=loops?loops->value:-10;
    if(kind==TrackFollowerKind::Player?(value>=-1&&value!=0):(value==1||value==-1))result.loops=value;
    if(kind==TrackFollowerKind::Player) {
        result.startOffset={number(record,"Start_Offset_X"),number(record,"Start_Offset_Y"),number(record,"Start_Offset_Z")};
        result.camera=resource(record,"Camera");result.cameraRotating=byte(record,"CameraRotate",false);
    }
    return result;
}
model::Quaternion product(model::Quaternion a,model::Quaternion b) noexcept {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
model::Quaternion axis(model::Vec3 direction,float degrees) noexcept {
    const float half=(degrees*0.01745329238474369f)*0.5f;
    const float sine=std::sin(half);
    return {direction.x*sine,direction.y*sine,direction.z*sine,std::cos(half)};
}
}

std::optional<MiniGameDefinition> readTrackMiniGame(const neogff::GffStruct& area,
    const neoshared::bif::JobControl& job) {
    // CSWSArea::LoadAreaHeader tests the actual ARE MiniGame struct. The
    // client then demands that same ARE, not the GIT or an inferred filename.
    const auto* record=field<GffStruct>(area,"MiniGame");if(!record)return {};
    MiniGameDefinition result;
    if(const auto* type=field<GffUInt32Field>(*record,"Type"))
        if(type->value==1||type->value==2)result.type=type->value;
    const auto* player=field<GffStruct>(*record,"Player");
    auto parsed=player?follower(*player,TrackFollowerKind::Player,0,job):std::nullopt;
    if(!parsed || (result.type==2&&parsed->camera.empty()))
        throw std::runtime_error("Engine MiniGame::Load rejected Player Models/Track/Camera inputs");
    if(result.type!=2)parsed->camera.clear();
    result.followers.push_back(std::move(*parsed));
    if(const auto* enemies=field<GffList>(*record,"Enemies")) {
        for(std::size_t i=0;i<enemies->count();++i) {
            job.check();auto enemy=follower(*enemies->GetStruct(i),TrackFollowerKind::Enemy,i,job);
            if(enemy)result.followers.push_back(std::move(*enemy));
        }
    }
    return result;
}

HitPartTransform trackAttachmentFrame(HitPartTransform hook,const TrackModelAttachment& attachment) {
    if(attachment.mode==1) {
        auto offset=attachment.offset;
        if(!attachment.worldSpaceOffset && (offset.x!=0.0f||offset.y!=0.0f||offset.z!=0.0f))
            throw std::runtime_error("Engine translate attachment has an uncaptured world/local offset flag");
        if(attachment.worldSpaceOffset && !*attachment.worldSpaceOffset)offset=engineRotate(hook.orientation,offset);
        hook.position={hook.position.x+offset.x,hook.position.y+offset.y,hook.position.z+offset.z};
    } else if(attachment.mode==2) {
        // AttachRotate::Control uses YawPitchRoll(offset.z,offset.x,offset.y),
        // then multiplies the hook quaternion by that rotation.
        const auto rotation=product(product(axis({0,0,1},attachment.offset.z),
            axis({1,0,0},attachment.offset.x)),axis({0,1,0},attachment.offset.y));
        hook.orientation=product(hook.orientation,rotation);
    }
    return hook;
}
} // namespace neomdl
