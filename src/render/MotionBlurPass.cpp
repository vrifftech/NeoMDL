#include "render/MotionBlurPass.hpp"
#include <model/LightningControl.hpp>
#include <cmath>
#include <stdexcept>

namespace neomdl::render {
bool MotionBlurQueue::record(MotionBlurScene& scene,const std::vector<MeshSubmission>& submissions,
        const std::vector<std::size_t>& indices,std::string& error) {
    if(indices.empty())return true;
    try {
        const auto sceneOwner=submissions.at(indices.front()).sceneOwner;
        if(!sceneOwner)return true;
        std::vector<MeshSubmission> batch;batch.reserve(indices.size());
        for(const auto i:indices) {
            const auto& s=submissions.at(i);
            requireSceneOwnerLifetime(s.ownerLifetime);
            if(s.sceneOwner!=sceneOwner||s.parameters.particlePass||
               (s.parameters.lightsaberPass&&!s.parameters.engineLightsaberMaterial))
                throw std::runtime_error("Non-Gob or mixed-owner blur append");
            batch.push_back(s);
        }
        // Canvas sceneOwner is lightOwner+1; zero is reserved for static Parts.
        // Copies are complete before modifying history, including allocation failures.
        batches_.reserve(batches_.size()+1);
        if(scene.ordinaryRendered(sceneOwner-1u))batches_.push_back(std::move(batch));
        return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
const std::vector<MeshSubmission>& MotionBlurQueue::batch(std::size_t ordinal) const {return batches_.at(ordinal);}
MeshSubmission engineBlurSubmission(const MeshSubmission& input,const MotionBlurDraw& owner,float sceneElapsed) {
    auto result=input;resolveSceneSubmission(result);
    // This copy already reads the live owner. A second resolution in drawQueue
    // would replace the blur parameters with ordinary-draw parameters.
    result.resolveOwner={};
    const auto& s=result;
    if((!s.mesh&&!s.ownerOnly)||s.sceneOwner!=owner.owner+1u)
        throw std::runtime_error("Invalid engine blur owner/submission inputs");
    auto& p=result.parameters;p.engineBlurTraversal=true;p.selected=false;p.outlineOnly=false;p.basisDebug=0;
    if(result.trailingShell) {result.trailingShell->selected=false;result.trailingShell->outlineOnly=false;}
    if(s.ownerOnly||!s.mesh->skinned||!p.exactK2SkinVertexProgram)return result;
    result.drawPrimary=true;p.ownerFragment=OwnerFragmentEffect::None;
    p.skinEffect=SkinRenderEffect::MotionBlur;
    // The existing consumer streams posed skin geometry in owner/model space.
    // Rotate the engine Part-local displacement/camera back into that same
    // space. Gob scale is NOT part of GetPosAndOri or motion magnitude.
    const auto inverse=engineInverse(owner.current.orientation);
    p.skinMotion=engineRotate(inverse,owner.worldMotion);
    const auto camera=model::transformPoint(p.worldFromEye,{});
    p.skinCameraPosition=engineRotate(inverse,{camera.x-owner.current.position.x,
        camera.y-owner.current.position.y,camera.z-owner.current.position.z});
    const bool k1=p.material.engineGame==material::EngineMaterialGame::Kotor1;
    const auto dot=[k1](model::Vec3 a,model::Vec3 b) {
        const float x=a.x*b.x,y=a.y*b.y,z=a.z*b.z;
        return k1?(x+y)+z:z+(y+x);
    };
    const auto length=[k1](model::Vec3 v){
        const float x=v.x*v.x,y=v.y*v.y,z=v.z*v.z;
        return std::sqrt(k1?(x+y)+z:z+(y+x));
    };
    const auto magnitude=length(owner.worldMotion);
    p.engineSkinBlur=true;p.skinBlurMotionAdmitted=magnitude>0.0f;
    p.skinBlurSpeed=magnitude/(2.0f*sceneElapsed);
    const auto localLength=length(p.skinMotion);
    if(static_cast<double>(localLength)<1.0e-9)p.skinBlurDirection={1,0,0};
    else {
        const float reciprocal=1.0f/localLength;
        p.skinBlurDirection={p.skinMotion.x*reciprocal,p.skinMotion.y*reciprocal,p.skinMotion.z*reciprocal};
    }
    const auto z=model::engineQuaternionAxis(owner.current.orientation,2);
    const auto& d=owner.worldMotion;
    p.skinBlurSideDirection=engineRotate(inverse,{d.y*z.z-d.z*z.y,
        d.z*z.x-d.x*z.z,d.x*z.y-d.y*z.x});
    // The posed stream is in Gob/model space. c17's Gob origin is therefore
    // zero; the camera enters only the c13.x/winding producer below.
    const model::Vec3 toOwner{-p.skinCameraPosition.x,-p.skinCameraPosition.y,-p.skinCameraPosition.z};
    const auto& direction=p.skinBlurDirection;
    const auto along=dot(toOwner,direction);
    const model::Vec3 projected{direction.x*along,direction.y*along,direction.z*along};
    p.skinBlurSideScale=0;p.skinBlurClockwise=false;
    if(dot(projected,projected)>(dot(toOwner,toOwner)*0.75f)*0.75f &&
       length({direction.x+toOwner.x,direction.y+toOwner.y,direction.z+toOwner.z})>
       length({toOwner.x-direction.x,toOwner.y-direction.y,toOwner.z-direction.z})) {
        p.skinBlurSideScale=1;p.skinBlurClockwise=true;
    }
    p.skinMaximumBlurLength=owner.maximumLength;
    p.lighting=false;p.lightCount=0;p.hasColorOverride=true;p.colorOverride={{1,1,1,1}};
    p.material.selfIllumination={{0,0,0}};p.material.textureAlphaIsOpacity=true;
    p.material.pass=material::RenderPass::Translucent;p.material.blending=true;
    p.material.depthWrite=false;
    p.material.blendFunction=material::BlendFunction::Alpha;
    p.material.useLightmap=false;p.material.useBumpMap=false;p.material.useEnvironmentMap=false;
    // Zero movement reaches PartSkin::Mat and the optional shell, but not the
    // blur helper's texture/program/draw body. draw() preserves that guard.
    return result;
}
bool drawMotionBlurOwners(ShaderMeshRenderer& renderer,MotionBlurScene& scene,const MotionBlurQueue& queue,
        const SubmissionPreparation& prepare,const BlurOwnerPreparation& beforeOwner,std::string& error,
        float sceneElapsed) {
    try {
        // Evaluate previous state at CONSUMPTION, not append: duplicate Gob
        // entries see the history committed by the preceding RenderBlur call.
        while(scene.completedCount()<scene.pending().size()) {
            const auto input=scene.nextDraw();
            std::vector<MeshSubmission> submissions;
            for(const auto& s:queue.batch(input.ordinal))submissions.push_back(engineBlurSubmission(s,input,sceneElapsed));
            if(beforeOwner&&!beforeOwner(input,error))return false;
            if(!renderer.drawQueue(submissions,error,QueueOrdering::Submission,prepare))return false;
            scene.completeBlur(input);
        }
        return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
} // namespace neomdl::render
