#include "core/ShadowScene.hpp"
#include "core/SceneQueries.hpp"
#include "core/ModelDocument.hpp"
#include "core/ReferenceScene.hpp"
#include "core/AreaScene.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace neomdl {
namespace {
bool finite(model::Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
float squared(model::Vec3 a,model::Vec3 b) {const auto x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return (x*x+y*y)+z*z;}
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(std::string("Scene shadows: ")+text);}
}
std::vector<SceneShadowCaster> collectSceneShadowCasters(
        const ModelDocument& document,const model::Pose& pose,const ReferenceScene& references,
        const AreaScene& area,bool includeReferences,float radius,const model::DanglySimulationState* dangly,
        const ParticleScene* inspectionParticles) {
    return collectSceneOwners(document,pose,references,area,includeReferences,radius,dangly,inspectionParticles);
}
void validateSceneShadowInputs(const SceneShadowInputs& s) {
    require(finite(s.containerSphere.position)&&std::isfinite(s.containerSphere.radius)&&s.containerSphere.radius>=0,
            "invalid independent light-container sphere");
    require(finite(s.scenePosition)&&finite(s.casterRegion),"non-finite scene or caster-region position");
    require(!s.maximumCasterDistance||(std::isfinite(*s.maximumCasterDistance)&&*s.maximumCasterDistance>=0),"invalid shadowdistance");
    require(!s.containerOwner||*s.containerOwner!=ShadowScene::independentContainer,"reserved container identity cannot name a caster");
    require(s.containerSource==LightQuerySource::Global||s.containerSource==LightQuerySource::Room,
            "independent container source must be global or collected-room lights");
}
void ShadowScene::setInputs(SceneShadowInputs inputs) {validateSceneShadowInputs(inputs);inputs_=std::move(inputs);selectedOwnerLost_=false;}
void ShadowScene::replaceCasters(std::vector<SceneShadowCaster> casters) {
    std::set<LightOwnerId> ids;
    for(const auto& c:casters) {
        require(c.id!=independentContainer&&ids.insert(c.id).second,"duplicate/reserved caster identity");
        require(c.asset,"a caster requires a live asset");
        require(std::isfinite(c.scale)&&c.scale>0,"invalid owner scale");
        require(finite(c.lightSphere.position)&&std::isfinite(c.lightSphere.radius)&&c.lightSphere.radius>=0,"invalid caster light sphere");
        require(!c.rangePosition||finite(*c.rangePosition),"non-finite cutscene range position");
    }
    auto enabled=enabled_;
    for(auto i=enabled.begin();i!=enabled.end();) if(!ids.count(i->first))i=enabled.erase(i);else++i;
    if(inputs_.containerOwner&&!ids.count(*inputs_.containerOwner))selectedOwnerLost_=true;
    casters_=std::move(casters);enabled_=std::move(enabled);
}
void ShadowScene::setCasterEnabled(LightOwnerId owner,bool enabled) {
    require(std::any_of(casters_.begin(),casters_.end(),[&](const auto& c){return c.id==owner;}),"caster owner is not registered");
    enabled_[owner]=enabled;
}
bool ShadowScene::casterEnabled(LightOwnerId owner) const noexcept {const auto i=enabled_.find(owner);return i==enabled_.end()||i->second;}
model::Bounds ShadowScene::focusBounds() noexcept {
    model::Bounds bounds;bounds.minimum={0,0,0};bounds.maximum={1,1,1};bounds.valid=true;
    return bounds;
}
bool ShadowScene::focusVisible(const AreaScene& area,
        const std::vector<model::TriggerPlane>& frustum) const {
    auto world=model::identityMatrix();
    world.values[12]=sceneFocus_.x;world.values[13]=sceneFocus_.y;world.values[14]=sceneFocus_.z;
    if(!area.ownerVisible(0,focusBounds(),world))return false;
    // ProcessParts -> GobOutside: the focus Gob keeps radius 1, local
    // center zero and ShouldWeCheckVisibilityHACK=false from its constructor.
    for(const auto& plane:frustum) {
        const float above=(((plane.normal.x*sceneFocus_.x+plane.distance)+
                            plane.normal.y*sceneFocus_.y)+plane.normal.z*sceneFocus_.z)-1.0f;
        if(above>0.0f)return false;
    }
    return true;
}
void ShadowScene::retainFocusContainer(LightScene& lights) const {
    lights.retainOwnerForFrame(focusContainer);
}
void ShadowScene::updateFocusContainer(LightScene& lights) const {
    // c_FocusGob is a registered Gob, not the player's VFX light returned
    // (and discarded) by CSWCArea::UpdateShadowingLights in all four images.
    LightOwnerQuery query;query.source=LightQuerySource::Gob;
    query.sphere=LightQuerySphere{sceneFocus_,1.0f};
    lights.setOwnerQuery(focusContainer,query);
    lights.selectOwner(focusContainer,sceneFocus_,1.0f);
}
void ShadowScene::retainContainer(LightScene& lights) const {
    if(inputs_.mode==SceneShadowInputMode::SceneFocus)retainFocusContainer(lights);
    else if(!inputs_.containerOwner)lights.retainOwnerForFrame(independentContainer);
    else if(!selectedOwnerLost_&&std::any_of(casters_.begin(),casters_.end(),[&](const auto& c){return c.id==*inputs_.containerOwner;}))
        lights.retainOwnerForFrame(*inputs_.containerOwner);
}
void ShadowScene::clear(LightScene& lights) {lights.removeOwner(independentContainer);lights.removeOwner(focusContainer);
    sceneFocus_={};casters_.clear();enabled_.clear();
    if(inputs_.containerOwner)selectedOwnerLost_=true;
}
SceneShadowFrame ShadowScene::buildFrame(LightScene& lights,const model::Mat4& view,float farClip,
    const SceneShadowInputs* frameInputs,const SceneShadowOwnerAdmission* admission) const {
    auto inputs=frameInputs?*frameInputs:inputs_;
    validateSceneShadowInputs(inputs);require(std::isfinite(farClip)&&farClip>0,"invalid camera far clip");
    SceneShadowFrame frame;
    LightOwnerId container=independentContainer;LightQuerySphere sphere=inputs.containerSphere;
    if(inputs.mode==SceneShadowInputMode::SceneFocus) {
        container=focusContainer;
        inputs.scenePosition=sceneFocus_;
        inputs.casterRegion=sceneFocus_;
    } else if(inputs.containerOwner) {
        const auto it=std::find_if(casters_.begin(),casters_.end(),[&](const auto& c){return c.id==*inputs.containerOwner;});
        if(selectedOwnerLost_||it==casters_.end()) {frame.diagnostic="Selected shadow-container owner is no longer registered or its lifetime expired; explicitly select it again. No main-owner fallback was used.";return frame;}
        container=it->id;sphere=it->lightSphere;
    } else {
        LightOwnerQuery query;query.source=inputs.containerSource;query.sphere=sphere;
        lights.setOwnerQuery(container,query);
    }
    if(inputs.mode==SceneShadowInputMode::Explicit)
        lights.selectOwner(container,sphere.position,sphere.radius);
    frame.light=lights.shadowLight(container);
    if(!frame.light) {
        if(const auto* selected=lights.ownerSelection(container))frame.diagnostic=selected->diagnostic;
        return frame;
    }
    const auto& light=*frame.light;


    if(!(light.position.z>inputs.scenePosition.z+1.75f))return frame;
    const float radius=light.shadowRadius>0?light.shadowRadius:light.radius;
    if(!(radius>0))return frame;
    const auto& v=view.values;
    const model::ShadowPlane plane=light.radius>1000.f?model::ShadowPlane{{0,0,-1},-.1f}:
        model::ShadowPlane{{-v[2],-v[6],-v[10]},-v[14]-farClip+.05f};
    // DoGobShadowsForThisLight captures the initial reverse cursor, animates
    // behavior owners, then rereads the live count AND reloads that slot before
    // any caster/distance/geometry admission (K2 Desktop 0xc3420/0xc3423/0xc342e).
    const auto count=[&] {return admission?admission->count():casters_.size();};
    const auto load=[&](std::size_t slot) {return admission?admission->load(slot):casters_[slot];};
    for(auto cursor=count();cursor!=0;--cursor) {
        if(cursor>count())continue;
        if(admission) {
            const auto owner=load(cursor-1).id;
            admission->animate(owner);
            if(cursor>count())continue;
        }
        auto c=load(cursor-1);auto decision=ShadowCasterDecision::Admitted;
        if(c.resolveFrame)c.frame=c.resolveFrame();
        if(c.resolveWorld)c.world=c.resolveWorld();
        if(c.resolveScale)c.scale=c.resolveScale();
        const auto origin=model::transformPoint(c.world,{});
        const auto range=(c.resolveRangePosition?c.resolveRangePosition():c.rangePosition).value_or(origin);
        if(!c.enabled||!casterEnabled(c.id))decision=ShadowCasterDecision::Disabled;
        else if(!c.visible)decision=ShadowCasterDecision::Hidden;
        else if(!(c.asset->classification&0x24u))decision=ShadowCasterDecision::Classification;
        // K1 Desktop DoGobShadowsForThisLight (0x353fb6) has no region gate.
        // K2 tests Scene's embedded focus Gob origin at +0x1bc.
        else if(c.asset->game==model::GameVersion::Kotor2&&
                squared(origin,inputs.casterRegion)>1100.f)decision=ShadowCasterDecision::Region;
        else if(inputs.maximumCasterDistance&&std::sqrt(squared(range,inputs.scenePosition))>*inputs.maximumCasterDistance)
            decision=ShadowCasterDecision::Distance;
        else if(!(std::sqrt(squared(range,light.position))<radius))decision=ShadowCasterDecision::LightRange;
        frame.casters.push_back({c.id,decision});if(decision!=ShadowCasterDecision::Admitted)continue;
        model::GobShadowInput input;input.ownerFrame=c.frame;input.ownerFrame.scale=c.scale;
        input.worldLight={light.position.x,light.position.y,light.position.z+light.verticalDisplacement};
        input.projectionDistance=radius;input.worldPlane=plane;input.enabledParts=c.enabledParts;
        auto volumes=model::buildGobShadowVolumes(*c.asset,c.pose,c.dangly,input);
        for(auto& volume:volumes)frame.volumes.push_back(std::move(volume));
    }
    frame.fadeFan=model::buildShadowFadeFan(inputs.scenePosition,light.position,radius,light.fade);
    return frame;
}
} // namespace neomdl
