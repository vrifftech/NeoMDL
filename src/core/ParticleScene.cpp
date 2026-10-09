#include <material/ProceduralTexture.hpp>
#include <model/AsciiModel.hpp>
#include "core/ParticleScene.hpp"
#include "core/EmitterBucket.hpp"
#include "core/SceneQueries.hpp"
#include "core/ReferenceScene.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <limits>
#include <atomic>

namespace neomdl {
namespace m=neomdl::model;
static_assert(m::kParticleRuntimeVersion>=9,"Update NeoShared to the emitter scene runtime release");
namespace {
std::atomic<std::uint64_t> nextChunkIdentity{0xa0000000u};
std::atomic<std::uint64_t> nextEmitterRegistration{1};
constexpr float kDegreesToRadians=0.01745329238474369f;
bool finiteVector(m::Vec3 v) {
    return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);
}
std::string lowerName(std::string s) {
    for(auto& c:s) if(c>='A'&&c<='Z') c=static_cast<char>(c-'A'+'a');
    return s;
}
std::int32_t emitterSceneCategory(const m::EmitterNodeData& data) {
    const auto blend=lowerName(data.blendMode);
    if(blend=="normal") return 0;
    if(blend=="punchthrough" || blend=="punch-through") return 1;
    // PartEmitter::Initialize uses category 2 for every remaining value;
    // authored K2 content uses Lighten/Additive for this branch.
    return 2;
}
m::Vec3 matrixPosition(const m::Mat4& matrix) noexcept {
    return {matrix.values[12],matrix.values[13],matrix.values[14]};
}

float vectorLength(m::Vec3 value) noexcept {
    return std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
}
std::optional<m::EmitterAttachedObjectSphere> transformedBoundsSphere(
    const m::Bounds& bounds,const m::Mat4& world) noexcept {
    if(!bounds.valid) return std::nullopt;
    const m::Vec3 localCenter{
        (bounds.minimum.x+bounds.maximum.x)*0.5f,
        (bounds.minimum.y+bounds.maximum.y)*0.5f,
        (bounds.minimum.z+bounds.maximum.z)*0.5f};
    const auto center=m::transformPoint(world,localCenter);
    if(!finiteVector(center)) return std::nullopt;
    float radius=0.0f;
    for(unsigned corner=0;corner<8u;++corner) {
        const auto point=m::transformPoint(world,{
            (corner&1u)?bounds.maximum.x:bounds.minimum.x,
            (corner&2u)?bounds.maximum.y:bounds.minimum.y,
            (corner&4u)?bounds.maximum.z:bounds.minimum.z});
        if(!finiteVector(point)) continue;
        radius=std::max(radius,vectorLength({point.x-center.x,point.y-center.y,point.z-center.z}));
    }
    return m::EmitterAttachedObjectSphere{center,radius};
}
bool finiteMatrix(const m::Mat4& value) noexcept {
    return std::all_of(value.values.begin(),value.values.end(),[](float x){return std::isfinite(x);});
}
bool routeMatches(const ParticleEmitterTargetInput& route,const ParticleEmitterInstance& emitter,
                  const ModelDocument& document) {
    if(!route.allSourceOwners) {
        if(!route.sourceReference || emitter.reference!=route.sourceReference)return false;
    }
    if(route.emitterName.empty())return true;
    const auto& asset=emitter.reference ? document.references()[*emitter.reference].model : document.model();
    return emitter.nodeIndex<asset.nodes.size() && lowerName(asset.nodes[emitter.nodeIndex].name)==lowerName(route.emitterName);
}
bool sameTargetInput(const ParticleEmitterTargetInput& a,const ParticleEmitterTargetInput& b) noexcept {
    return lowerName(a.emitterName)==lowerName(b.emitterName) &&
        a.allSourceOwners==b.allSourceOwners && a.sourceReference==b.sourceReference &&
        a.target.values==b.target.values &&
        std::tie(a.frame.position.x,a.frame.position.y,a.frame.position.z,a.frame.orientation.x,
                 a.frame.orientation.y,a.frame.orientation.z,a.frame.orientation.w,a.frame.scale)==
        std::tie(b.frame.position.x,b.frame.position.y,b.frame.position.z,b.frame.orientation.x,
                 b.frame.orientation.y,b.frame.orientation.z,b.frame.orientation.w,b.frame.scale);
}
bool sameObjectVisualInputs(const ParticleObjectVisualInputs& a,const ParticleObjectVisualInputs& b) noexcept {
    if(a.objectTint.x!=b.objectTint.x||a.objectTint.y!=b.objectTint.y||a.objectTint.z!=b.objectTint.z||
       a.objectAlpha!=b.objectAlpha||a.tileAlpha!=b.tileAlpha||a.forceUnbounded!=b.forceUnbounded||
       a.targets.size()!=b.targets.size())return false;
    for(std::size_t i=0;i<a.targets.size();++i)if(!sameTargetInput(a.targets[i],b.targets[i]))return false;
    return true;
}
}
bool ParticleSceneSettings::operator==(const ParticleSceneSettings& b) const noexcept {
    return std::tie(targetEnabled,targetNode,targetPosition.x,targetPosition.y,targetPosition.z,
        targetOrientation.x,targetOrientation.y,targetOrientation.z,targetOrientation.w,
        windVelocity.x,windVelocity.y,windVelocity.z,windMaximumDeviationZDegrees,
        windMaximumDeviationXDegrees,windInterpolationPeriod,
        objectTint.x,objectTint.y,objectTint.z,objectAlpha,tileAlpha,
        blastWind,bounceMode)==
        std::tie(b.targetEnabled,b.targetNode,b.targetPosition.x,b.targetPosition.y,b.targetPosition.z,
        b.targetOrientation.x,b.targetOrientation.y,b.targetOrientation.z,b.targetOrientation.w,
        b.windVelocity.x,b.windVelocity.y,b.windVelocity.z,b.windMaximumDeviationZDegrees,
        b.windMaximumDeviationXDegrees,b.windInterpolationPeriod,
        b.objectTint.x,b.objectTint.y,b.objectTint.z,b.objectAlpha,b.tileAlpha,
        b.blastWind,b.bounceMode);
}
bool validParticleSceneSettings(const ParticleSceneSettings& s) noexcept {
    const auto q=s.targetOrientation;
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    return finiteVector(s.targetPosition)&&finiteVector(s.windVelocity)&&finiteVector(s.objectTint)&&
        std::isfinite(s.windMaximumDeviationZDegrees)&&
        std::isfinite(s.windMaximumDeviationXDegrees)&&
        std::isfinite(s.windInterpolationPeriod)&&s.windInterpolationPeriod>0.0f&&
        std::isfinite(s.objectAlpha)&&std::isfinite(s.tileAlpha)&&
        std::isfinite(norm)&&norm>0.0f&&
        (s.bounceMode==m::ParticleBounceMode::Legacy||s.bounceMode==m::ParticleBounceMode::Reflection);
}
bool validParticleObjectVisualInputs(const ParticleObjectVisualInputs& inputs,std::string& error) noexcept {
    error.clear();
    if(!finiteVector(inputs.objectTint)||!std::isfinite(inputs.objectAlpha)||!std::isfinite(inputs.tileAlpha)) {
        error="Particle owner tint and alpha must be finite";return false;
    }
    std::vector<std::tuple<std::string,bool,std::optional<std::size_t>>> selectors;
    for(const auto& route:inputs.targets) {
        if(!finiteMatrix(route.target)) {
            error="Particle target route is invalid";return false;
        }
        if(route.allSourceOwners&&route.sourceReference) {
            error="An all-owner particle target cannot also select one reference";return false;
        }
        if(!route.allSourceOwners&&!route.sourceReference) {
            error="A reference-selective particle target requires a reference index";return false;
        }
        const auto key=std::make_tuple(lowerName(route.emitterName),route.allSourceOwners,route.sourceReference);
        if(std::find(selectors.begin(),selectors.end(),key)!=selectors.end()) {
            error="Duplicate particle target route selector";return false;
        }
        selectors.push_back(key);
    }
    return true;
}
ParticleScene::ParticleScene() {
    configureWind();
}
void ParticleScene::setSceneAnimation(std::shared_ptr<SceneAnimation> scene) {
    sceneAnimation_=std::move(scene);
    for(auto& emitter:emitters_)for(auto& chunk:emitter->chunks) {
        if(chunk->animationOwner.sceneMember())chunk->animationOwner.setScene(sceneAnimation_);
        chunk->references->setSceneAnimation(sceneAnimation_);
        chunk->particles->setSceneAnimation(sceneAnimation_);
    }
}
ChunkParticleInstance::~ChunkParticleInstance() {
    animationOwner.setScene(nullptr);
    if(references)references->setOwnerSceneMember(false);
    lifetime.reset();
}
ParticleScene::~ParticleScene() {
    if(rootScene()->resourcesDestroyed_) rootScene()->resourcesDestroyed_(*this);
    for(auto& emitter:emitters_) {
        emitter->chunks.clear();
        windManager().deregisterWindyEmitter(emitter.get());
        if(rootScene()->emitterRemovalSink_) rootScene()->emitterRemovalSink_(emitter.get());
    }
}
void ParticleScene::deregisterWindyEmitters() noexcept {
    for (const auto& emitter : emitters_)
        windManager().deregisterWindyEmitter(emitter.get());
}
void ParticleScene::registerWindyEmitters() {
    if(!sceneMember_) return;
    for (const auto& emitter : emitters_) {
        auto* runtime = emitter.get();
        if (engineRuntime(runtime->game) && runtime->data &&
            (runtime->data->rawWords.back() & m::emitter_flags::AffectedByWind))
            windManager().registerWindyEmitter(runtime, [runtime]() {
                runtime->radiusInvalidated = true;
            });
    }
}
void ParticleScene::setSceneRandom(m::ParticleRandom* random) noexcept {
    sceneRandom_=random;
    for(auto& emitter:emitters_) for(auto& chunk:emitter->chunks) if(chunk->particles) {
        chunk->references->setSceneRandom(&runtimeRandom());
        chunk->particles->setSceneRandom(&runtimeRandom());
    }
}
void ParticleScene::setSceneWind(m::WindManager* wind) {
    if (sceneWind_ == wind) return;
    deregisterWindyEmitters();
    sceneWind_ = wind;
    for(auto& emitter:emitters_) for(auto& chunk:emitter->chunks)
        if(chunk->particles) chunk->particles->setSceneWind(&windManager());
    registerWindyEmitters();
    for (auto& emitter : emitters_) emitter->radiusInvalidated = true;
}
void ParticleScene::configureWind() {
    // Borrowed scene services are neither reset nor privately generated by
    // an owner. Constructing both fields consumes 682 shared rand calls.
    if(sceneWind_)return;
    // K1 Desktop and K2 share the engine wind field lifecycle. The scene
    // supplies their random stream before object construction.
    if(document_ && engineRuntime(document_->model().game))
        wind_.setRandomSource([this]() { return runtimeRandom().next(); });
    else
        wind_.clearRandomSource();
    wind_.reset(settings_.windVelocity,
        settings_.windMaximumDeviationZDegrees*kDegreesToRadians,
        settings_.windMaximumDeviationXDegrees*kDegreesToRadians,
        settings_.windInterpolationPeriod,1u);
}
bool ParticleScene::setSettings(const ParticleSceneSettings& settings) {
    if(!validParticleSceneSettings(settings)) return false;
    if(settings_==settings) return true;
    settings_=settings;resetSimulation();return true;
}
bool ParticleScene::setObjectVisualInputs(const ParticleObjectVisualInputs& inputs,std::string& error) {
    if(!validParticleObjectVisualInputs(inputs,error))return false;
    if(sameObjectVisualInputs(objectVisualInputs_,inputs))return true;
    objectVisualInputs_=inputs;
    // Targets and the unidentified owner-model radius flag affect only the
    // next spatial synchronization; existing particles and RNG stay intact.
    for(auto& emitter:emitters_)emitter->radiusInvalidated=true;
    prepared_=false;
    return true;
}
void ParticleScene::setChunkLightsaberSettings(LightsaberSceneSettings settings) {
    chunkLightsaberSettings_=settings;
    for(auto& emitter:emitters_) for(auto& chunk:emitter->chunks) {
        chunk->lightsabers.setSettings(settings);
        if(chunk->particles)chunk->particles->setChunkLightsaberSettings(settings);
    }
}
void ParticleScene::setDanglyEnabled(bool enabled) {
    if(danglyEnabled_==enabled) return;
    danglyEnabled_=enabled;
    for(auto& emitter:emitters_) for(auto& chunk:emitter->chunks) {
        chunk->danglyState={};
        if(chunk->references) chunk->references->resetDangly();
        if(chunk->particles) chunk->particles->setDanglyEnabled(enabled);
    }
}
m::Vec3 ParticleScene::windDisplacement(m::Vec3 position, float seconds) const noexcept {
    if(!std::isfinite(seconds) || seconds<0.0f) seconds=0.0f;
    if(document_ && !engineRuntime(document_->model().game)) {
        // Unsupported runtime profiles retain the explicit inspection displacement.
        m::Vec3 result{settings_.windVelocity.x*seconds,
                       settings_.windVelocity.y*seconds,
                       settings_.windVelocity.z*seconds};
        if(settings_.blastWind) for(const auto& source:windManager().pointSources()) {
            const m::Vec3 delta{position.x-source.position.x,
                                position.y-source.position.y,
                                position.z-source.position.z};
            const float distance=std::max({std::fabs(delta.x),std::fabs(delta.y),
                                           std::fabs(delta.z)});
            if(source.radius<=0.0f || source.remainingLifetime<0.0f ||
               distance>=source.radius) continue;
            const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
            if(length<=1.0e-10f || !std::isfinite(length)) continue;
            const float scale=(source.radius-distance)*source.strength/(source.radius*length);
            result.x+=delta.x*scale;result.y+=delta.y*scale;result.z+=delta.z*scale;
        }
        return result;
    }
    return settings_.blastWind ? windManager().getGlobalWind(position,seconds)
                               : windManager().getGlobalWindNoPoint(position,seconds);
}

std::shared_ptr<const neoshared::texture::TextureData> assembleParticleAtlas(
    const neoshared::texture::TextureData& texture,std::uint32_t columns,std::uint32_t rows,
    std::string& diagnostic) {
    diagnostic.clear();
    if(texture.cubeMap || columns==0 || rows==0 ||
       texture.layers.size()!=static_cast<std::uint64_t>(columns)*rows || texture.layers.empty()) {
        diagnostic="Particle texture layers do not match the authored 2D frame grid";return {};
    }
    const auto width=texture.layers.front().width,height=texture.layers.front().height;
    const std::uint64_t atlasWidth=static_cast<std::uint64_t>(width)*columns;
    const std::uint64_t atlasHeight=static_cast<std::uint64_t>(height)*rows;
    const auto maximumBytes=std::vector<std::uint8_t>{}.max_size();
    if(!width || !height || atlasWidth>std::numeric_limits<std::uint32_t>::max() ||
       atlasHeight>std::numeric_limits<std::uint32_t>::max() ||
       atlasWidth>maximumBytes/4u/atlasHeight) {
        diagnostic="Particle frame atlas dimensions exceed its image storage";return {};
    }
    for(const auto& layer:texture.layers) if(layer.width!=width || layer.height!=height ||
       layer.rgba.size()!=static_cast<std::uint64_t>(width)*height*4) {
        diagnostic="Particle texture has inconsistent decoded frame dimensions";return {};
    }
    auto result=std::make_shared<neoshared::texture::TextureData>();
    result->kind=texture.kind;result->hasAlpha=texture.hasAlpha;
    result->canvasWidth=static_cast<std::uint32_t>(atlasWidth);result->canvasHeight=static_cast<std::uint32_t>(atlasHeight);
    neoshared::texture::TextureLayer out;out.width=result->canvasWidth;out.height=result->canvasHeight;
    out.rgba.resize(static_cast<std::size_t>(atlasWidth*atlasHeight)*4);
    const auto rowBytes=static_cast<std::size_t>(width)*4u;
    for(std::size_t frame=0;frame<texture.layers.size();++frame) {
        const auto& in=texture.layers[frame];
        for(std::size_t y=0;y<height;++y) {
            const auto offset=((frame/columns*height+y)*out.width+frame%columns*width)*4;
            std::copy_n(in.rgba.begin()+static_cast<std::ptrdiff_t>(y*rowBytes),rowBytes,
                        out.rgba.begin()+static_cast<std::ptrdiff_t>(offset));
        }
    }
    result->layers.push_back(std::move(out));return result;
}
void ParticleScene::setDocument(const ModelDocument* document) {
    if(rootScene()->resourcesDestroyed_)rootScene()->resourcesDestroyed_(*this);
    deregisterWindyEmitters();emitters_.clear();diagnostics_.clear();
    if(!rootScene_) atlases_.clear();
    owners_.clear();emitterBucket_.clear();emitterAdmissions_.clear();visibilityInitialized_=false;
    referenceRegistrations_.clear();
    prewarmed_=false;deterministicReplay_=false;document_=document;random_.state=1;
    configureWind();
    prepared_ = false; preparedEnvironment_ = {}; preparedTarget_.reset();
    objectVisualInputs_={};
    replayDiagnostic_.clear();
    missingTargets_=0;
    if(!document_ || !document_->isOpen()) return;
    auto& atlases=rootScene()->atlases_;
    const auto add=[&](const m::Model& model,std::optional<std::size_t> reference) {
        for(std::size_t n=0;n<model.nodes.size();++n) {
            if(!model.nodes[n].emitter) continue;



            auto e=std::make_unique<ParticleEmitterInstance>();
            e->reference=reference;e->nodeIndex=n;e->game=model.game;
            e->registrationOrder=sceneMember_ ? nextEmitterRegistration.fetch_add(1) : 0;
            e->sceneIdentity=identityBase_+static_cast<std::uint64_t>(emitters_.size())+1u;
            e->ownerIdentity=reference ? identityBase_+static_cast<std::uint64_t>(*reference)+2u : (staticPrimary_ ? 0u : identityBase_+1u);
            e->cullingBypass=model.game==m::GameVersion::Kotor2 &&
                lowerName(model.name).find("baodur")!=std::string::npos;
            e->data=&*model.nodes[n].emitter;e->modelFog=model.fog;
            e->baseProperties=m::bindNodeEmitter(model.nodes[n],true);
            e->properties=std::make_shared<m::NodeEmitterState>(m::bindNodeEmitter(model.nodes[n]));
            e->support=m::emitterPreviewSupport(*e->data);
            // PartEmitter::Initialize stores the parsed blend mode in +0x4c:
            // Normal=0, PunchThrough=1, every other value (normally
            // Additive/Lighten)=2. This also applies to chunk emitters.
            e->sceneCategory=emitterSceneCategory(*e->data);
            e->texture=neomdl::material::makeMaterial(e->data->texture,"",
                [&](const std::string& key){return document_->textureMetadata(key);}).diffuse;
            e->diagnostic=e->support.supported ? e->data->updateMode+" / "+e->data->renderMode : e->support.reason;
            for(const auto& warning:m::emitterPreviewWarnings(*e->data,*e->properties))
                if(warning!=e->diagnostic) e->diagnostic+="; "+warning;
            if(!e->data->chunkName.empty()) {
                e->chunkAsset=rootScene()->document_->chunkModel(e->data->chunkName);
                e->textureSupported=e->chunkAsset!=nullptr;
                if(!e->chunkAsset) e->diagnostic+="; chunk model missing or rejected: "+e->data->chunkName;
                else {
                    // Request the actual inherited clip, including the
                    // terminal-supermodel default fallback. A fallback can be
                    // hidden by a child default in the visible clip list.
                    if(e->support.update!=m::EmitterUpdate::Single)
                        e->chunkClip=e->chunkAsset->findAnimation("impact");
                    if(e->support.update==m::EmitterUpdate::Single)
                        e->diagnostic+="; Single constructor does not request impact: bind-pose model";
                    else if(!e->chunkClip) e->diagnostic+="; recycled impact/default missing: bind-pose model";
                    else {
                        const auto& ref=*e->chunkClip;
                        const auto& clip=e->chunkAsset->modelAt(ref.modelIndex).animations[ref.animationIndex];
                        e->diagnostic+="; recycled chunk animation: "+clip.name;
                    }
                }
            } else {
            const auto* image=document_->texture(e->texture.key);
            bool procedural=false;
            try {
                if(engineRuntime(e->game) && e->texture.metadata)
                    procedural=bool(material::procedureDefinition(e->texture.metadata->txi,
                        e->game==m::GameVersion::Kotor1 ? material::EngineMaterialGame::Kotor1 :
                            material::EngineMaterialGame::Kotor2).type);
            } catch(const std::exception& error) {
                e->textureSupported=false;e->diagnostic+="; procedural texture: "+std::string(error.what());
            }
            if(!image || !image->hasPixels()) e->diagnostic+="; texture missing: untextured preview";
            else if(image->cubeMap) {
                e->textureSupported=false;e->diagnostic+="; particle textures cannot be cubemaps";
            } else if(procedural) {
                // Bind the SAME engine controller/TID as other consumers. An
                // emitter's authored x/y grid is independent of Cycle's TIDs;
                // assembling those TIDs into a second static atlas freezes the
                // controller and changes both its RNG and image lifetime.
                e->textureWidth=image->canvasWidth?image->canvasWidth:image->layers[0].width;
                e->textureHeight=image->canvasHeight?image->canvasHeight:image->layers[0].height;
            } else if(image->layers.size()>1) {
                const AtlasKey key{image,e->data->xGrid,e->data->yGrid};
                const auto found=atlases.find(key);
                if(found!=atlases.end()) e->atlas=found->second;
                else {
                    std::string message;
                    e->atlas=assembleParticleAtlas(*image,e->data->xGrid,e->data->yGrid,message);
                    if(e->atlas) atlases.emplace(key,e->atlas);
                    else e->diagnostic+="; "+message;
                }
                e->textureSupported=static_cast<bool>(e->atlas);
                if(e->atlas) {e->textureWidth=e->atlas->canvasWidth;e->textureHeight=e->atlas->canvasHeight;}
            } else {e->textureWidth=image->layers[0].width;e->textureHeight=image->layers[0].height;}
            }
            e->resourceDiagnostic=e->diagnostic;
            e->mesh.nodeIndex=n;
            e->simulation.reset(static_cast<std::uint64_t>(emitters_.size())+1);
            diagnostics_.push_back(model.name+" / "+model.nodes[n].name+": "+e->diagnostic);
            auto* runtime=e.get();
            emitters_.push_back(std::move(e));
            if(sceneMember_ && engineRuntime(runtime->game) &&
               (runtime->data->rawWords.back()&m::emitter_flags::AffectedByWind)!=0u)
                windManager().registerWindyEmitter(runtime,[runtime]() {
                    runtime->radiusInvalidated=true;
                });
        }
    };
    add(document_->model(),std::nullopt);
    for(std::size_t r=0;r<document_->references().size();++r) add(document_->references()[r].model,r);
    if (!sceneWind_) wind_.setGlobalWind(settings_.windVelocity);
    prepareFrame(m::bindPose(document_->model()),0.0,true);
}
void ParticleScene::resetSimulation() {
    random_.state=1;configureWind();prepared_=false;preparedEnvironment_={};preparedTarget_.reset();replayDiagnostic_.clear();
    missingTargets_=0;
    owners_.clear();emitterBucket_.clear();emitterAdmissions_.clear();visibilityInitialized_=false;
    prewarmed_=false;deterministicReplay_=false;
    for(auto& e:emitters_) {
        e->simulation.reset(e->simulation.seed);e->mesh.vertices.clear();e->mesh.indices.clear();
        e->visible=true;e->roomMembership.clear();e->calculatedRadius=1.0f;
        e->ownerTranslationDelta={};e->lastAnimatedOwnerPosition={};
        e->lastAnimatedOwnerPositionValid=false;e->radiusInvalidated=true;
        for(auto& mesh:e->frameBlendMeshes) {mesh.vertices.clear();mesh.indices.clear();}
        e->freeChunks.clear();e->chunks.clear();

    }
}
void ParticleScene::setSceneMember(bool member) {
    if(sceneMember_==member) return; // Gob::SetScene's identical-scene fast path.
    sceneMember_=member;
    if(chunkOwner_) {
        chunkOwner_->animationOwner.setScene(member?sceneAnimation_:nullptr);
        chunkOwner_->references->setOwnerSceneMember(member);
    }
    if(member) {
        if(chunkOwner_) chunkOwner_->sceneRegistration=nextEmitterRegistration.fetch_add(1);
        for(auto& emitter:emitters_) emitter->registrationOrder=nextEmitterRegistration.fetch_add(1);
        synchronizeReferenceRegistrations(true);
        registerWindyEmitters();
    } else {


        for(auto& emitter:emitters_) emitter->registrationOrder=0;
    }
}
void ParticleScene::synchronizeReferenceRegistrations(bool force) {
    if(!references_ || !document_ || !engineRuntime(document_->model().game)) return;
    referenceRegistrations_.resize(references_->instances().size());
    for(std::size_t index=0;index<referenceRegistrations_.size();++index) {
        const auto& reference=references_->instances()[index];
        if(reference.sceneMember) continue;
        referenceRegistrations_[index]=0;
        for(auto& emitter:emitters_) if(emitter->reference==index)
            emitter->registrationOrder=0;
    }
    // UpdateScenes appends the child Gob's Parts when that Gob rejoins the
    // scene. Reattach therefore changes registration order, not particle age.
    for(const auto index:references_->sceneRegistrationOrder()) {
        const auto& reference=references_->instances()[index];
        if(!force && referenceRegistrations_[index]==reference.sceneRegistration) continue;
        referenceRegistrations_[index]=reference.sceneRegistration;
        for(auto& emitter:emitters_) if(emitter->reference==index)
            emitter->registrationOrder=sceneMember_ ? nextEmitterRegistration.fetch_add(1) : 0;
    }
}
void ParticleScene::appendActiveChunkScenes(std::vector<ParticleScene*>& result) {
    for(auto& emitter:emitters_) for(auto& chunk:emitter->chunks) if(chunk->particles) {
        if(chunk->particles->sceneMember_) result.push_back(chunk->particles.get());
        // A nested ChunkyParticle owns a separate Gob, not an attachment. Its
        // membership survives pooling of an ancestor ChunkyParticle.
        chunk->particles->appendActiveChunkScenes(result);
    }
}
void ParticleScene::appendActiveChunkScenes(std::vector<const ParticleScene*>& result) const {
    for(const auto& emitter:emitters_) for(const auto& chunk:emitter->chunks) if(chunk->particles) {
        if(chunk->particles->sceneMember_) result.push_back(chunk->particles.get());
        static_cast<const ParticleScene&>(*chunk->particles).appendActiveChunkScenes(result);
    }
}
void ParticleScene::appendAllocatedChunkScenes(std::vector<const ParticleScene*>& result) const {
    for(const auto& emitter:emitters_) for(const auto& chunk:emitter->chunks) if(chunk->particles) {
        result.push_back(chunk->particles.get());
        chunk->particles->appendAllocatedChunkScenes(result);
    }
}
void ParticleScene::finishPrewarm() noexcept {
    prewarmed_=true;
    for(auto& emitter:emitters_) for(auto& chunk:emitter->chunks)
        if(chunk->particles) chunk->particles->finishPrewarm();
}
bool ParticleScene::advanceChunkOwner(float elapsedSeconds,bool evaluate) {
    auto* chunk=chunkOwner_;
    const auto lifetime=*chunk->animationOwner.lifetime();
    const bool applied=chunk->animationRuntime.advance(elapsedSeconds,evaluate);
    if(lifetime.expired())return applied;
    chunk->pose=chunk->animationRuntime.pose();
    chunk->time=chunk->animationRuntime.primaryTime();
    chunk->playImpact=!chunk->animationRuntime.empty();
    chunk->animationRuntime.takeEvents(); // event effects already dispatched
    chunk->references->synchronize(chunk->pose,false);
    return applied;
}
void ParticleScene::advanceChunkParts(float elapsedSeconds) {
    auto* chunk=chunkOwner_;
    if(danglyEnabled_) {
        m::DanglyEnvironment environment;
        environment.objectFrame=chunk->frame;
        environment.windDisplacement=[this](m::Vec3 position,float seconds) {
            return windDisplacement(position,seconds);
        };
        m::simulateDanglyMeshes(chunk->asset->model(),chunk->pose,chunk->danglyState,elapsedSeconds,environment);
    }
    chunk->lightsabers.advancePrimary(chunk->pose,elapsedSeconds);
    chunk->lightsabers.refreshTransforms(chunk->pose);
}

void ParticleScene::prepareChunkFrame() {
    if(!chunkOwner_) return;
    const auto* root=rootScene();
    sceneQueries_=root->sceneQueries_;
    rooms_=root->rooms_;
    cameraPosition_=root->cameraPosition_;
    std::vector<bool> referenceAnimation;
    for(const auto& reference:chunkOwner_->references->instances())
        referenceAnimation.push_back(reference.isAnimating());
    prepareFrame(chunkOwner_->pose,0.0,true,chunkOwner_->animationOwner.isAnimating(),referenceAnimation);
}
void ParticleScene::prepareChunkCallbacks(ParticleEmitterInstance& emitter,m::ParticleEnvironment& environment) {
    if(!engineRuntime(emitter.game) || !emitter.chunkAsset) return;
    environment.chunkBirth=[this,&emitter](std::uint64_t serial,bool recycled,float scale,bool initialize) {
        chunkBirth(emitter,serial,recycled,scale,initialize);
    };
    environment.chunkExpired=[this,&emitter](std::uint64_t serial) {chunkExpired(emitter,serial);};
    environment.chunkDestroyed=[this,&emitter](std::uint64_t serial) {
        const auto found=std::find_if(emitter.chunks.begin(),emitter.chunks.end(),
            [serial](const auto& chunk){return chunk->serial==serial;});
        emitter.chunks.erase(found);
        if(rootScene()->chunkMembershipChanged_) rootScene()->chunkMembershipChanged_();
    };
}
void ParticleScene::chunkBirth(ParticleEmitterInstance& emitter,std::uint64_t serial,
                               bool recycled,float scale,bool initialize) {
    ChunkParticleInstance* instance=nullptr;
    if(recycled) {
        instance=emitter.freeChunks.back();emitter.freeChunks.pop_back();
    } else if(initialize) {
        const auto found=std::find_if(emitter.chunks.begin(),emitter.chunks.end(),
            [serial](const auto& chunk){return chunk->serial==serial;});
        instance=found->get();
    }
    if(!initialize) {
        auto chunk=std::make_unique<ChunkParticleInstance>();instance=chunk.get();
        instance->asset=emitter.chunkAsset;
        const auto& model=instance->asset->model();
        // ChunkyParticle's constructor creates the Gob; initialize and free
        // list reuse retain it and must not repeat its skin-seam pass.
        m::processAsciiSkinSeams(model,resourceLoadJob());
        std::vector<const m::Model*> hierarchy;
        for(std::size_t index=0;index<instance->asset->modelHierarchySize();++index)
            hierarchy.push_back(&instance->asset->modelAt(index));

        instance->animationRuntime.setModel(&model,std::move(hierarchy));
        instance->pose=instance->animationRuntime.pose();
        instance->engineOwnerRadius=engineConstructedOwnerSphere(model).radius;
        instance->references=std::make_shared<ReferenceScene>();
        instance->references->setSceneAnimation(sceneAnimation_);
        instance->references->setParentAnimationOwner(&instance->animationOwner,&instance->animationRuntime);
        instance->references->setOwnerSceneMember(false);
        instance->references->setSceneRandom(&runtimeRandom());
        instance->references->setDocument(instance->asset);

        instance->lightsabers.setSettings(chunkLightsaberSettings_);
        instance->lightsabers.setReferenceScene(instance->references.get());
        instance->lightsabers.setDocument(instance->asset);





        instance->animationRuntime.advanceProceduralUv(runtimeRandom(),0.0f);
        instance->lightsabers.sampleConstructedOwners(instance->pose);
        std::size_t emitterCount=0;
        const auto count=[&](const m::Model& asset) {
            for(const auto& node:asset.nodes) if(node.emitter) ++emitterCount;
        };
        count(model);for(const auto& ref:instance->asset->references()) count(ref.model);
        const auto base=nextChunkIdentity.fetch_add(std::max(emitterCount,instance->asset->references().size()+1u)+1u);
        instance->ownerIdentity=base+1u;
        instance->particles=std::make_unique<ParticleScene>();
        auto& child=*instance->particles;
        child.rootScene_=rootScene();child.chunkOwner_=instance;
        child.setSceneAnimation(sceneAnimation_);
        instance->animationOwner.configure(model.classification,[instance](float seconds,bool evaluate) {
            return instance->particles->advanceChunkOwner(seconds,evaluate);
        });
        instance->animationOwner.setProcedural([instance](float seconds) {
            instance->animationRuntime.advanceProceduralUv(instance->particles->runtimeRandom(),seconds);
        });
        instance->animationOwner.setHasAnimationRuns([instance]{return !instance->animationRuntime.empty();});
        instance->animationOwner.setPartAnimation([instance](float seconds){instance->particles->advanceChunkParts(seconds);});
        instance->animationOwner.setDynamicLights(std::any_of(model.nodes.begin(),model.nodes.end(),
            [](const auto& node){return node.light&&(node.light->dynamicType||node.light->affectDynamic);}));
        instance->animationRuntime.setEventCallback([instance](const m::AnimationRuntimeEvent& event) {
            instance->pose=instance->animationRuntime.pose();
            instance->time=instance->animationRuntime.primaryTime();
            if(event.hierarchyUpdated)instance->animationOwner.markHierarchyUpdated();
            if(event.kind==m::AnimationRuntimeEventKind::AnimationEvent)
                instance->particles->dispatchEvent(event.name);
        },*instance->animationOwner.lifetime());
        instance->references->setAnimationEventCallback([instance](const ReferenceAnimationEvent& event) {
            if(event.event.kind==m::AnimationRuntimeEventKind::AnimationEvent)
                instance->particles->dispatchEvent(event.event.name,event.reference);
        });
        instance->references->setOwnerAnimated([instance](std::size_t,float) {
            instance->references->takeAnimationEvents();
        });
        instance->references->setPartsAnimated([instance](std::size_t reference,float seconds) {
            auto& child=*instance->particles;
            if(child.danglyEnabled_&&seconds>0) {
                m::DanglyEnvironment environment;
                environment.windDisplacement=[&child](m::Vec3 position,float dt){return child.windDisplacement(position,dt);};
                instance->references->simulateDanglyInstance(reference,seconds,environment);
            }
            if(seconds>0)instance->lightsabers.advanceReference(instance->pose,reference,seconds);
        });
        child.settings_=settings_;child.danglyEnabled_=danglyEnabled_;
        child.chunkLightsaberSettings_=chunkLightsaberSettings_;

        child.setSceneRandom(&runtimeRandom());child.setSceneWind(&windManager());
        child.setIdentityBase(base);child.setReferenceScene(instance->references.get());
        child.setDocument(instance->asset);
        instance->animationOwner.setScene(sceneAnimation_);
        instance->references->setOwnerSceneMember(true);
        instance->sceneRegistration=nextEmitterRegistration.fetch_add(1);
        // UpdateEmitters belongs to the containing scene's first Render only.
        child.prewarmed_=rootScene()->prewarmed_;
        emitter.chunks.push_back(std::move(chunk));
    }
    instance->serial=serial;instance->active=true;instance->everAssigned=true;
    if(initialize) {
        instance->particles->setSceneMember(true);
        instance->ownerScale=scale;instance->frame.scale=scale;
        instance->references->setOwnerScale(scale);
        instance->animationRuntime.play("impact",1.0f,m::kAnimationRunNonLooping,0.0f);
        instance->pose=instance->animationRuntime.pose();
        instance->playImpact=!instance->animationRuntime.empty();
        instance->time=instance->animationRuntime.primaryTime();
        instance->references->synchronize(instance->pose,false);
    }
    instance->lightsabers.refreshTransforms(instance->pose);
    instance->particles->prepareChunkFrame();
    if(rootScene()->chunkMembershipChanged_) rootScene()->chunkMembershipChanged_();
}
void ParticleScene::chunkExpired(ParticleEmitterInstance& emitter,std::uint64_t serial) {
    const auto found=std::find_if(emitter.chunks.begin(),emitter.chunks.end(),
        [serial](const auto& chunk){return chunk->serial==serial;});
    auto& chunk=**found;
    chunk.active=false;chunk.particles->setSceneMember(false);
    emitter.freeChunks.push_back(&chunk);
    if(rootScene()->chunkMembershipChanged_) rootScene()->chunkMembershipChanged_();
}

void ParticleScene::setEmitterRooms(std::vector<m::EmitterSceneRoom> rooms) {
    rooms_=std::move(rooms);
    for(auto& owner:owners_) owner.cachedRooms.clear();
    for(auto& emitter:emitters_) emitter->roomMembership.clear();
    visibilityInitialized_=false;
    rebuildEmitterBucket();
}

ParticleScene::OwnerSpatialState& ParticleScene::ownerState(std::uint64_t identity) {
    const auto found=std::find_if(owners_.begin(),owners_.end(),
        [&](const auto& owner){return owner.identity==identity;});
    if(found!=owners_.end()) return *found;
    owners_.push_back({});
    owners_.back().identity=identity;
    return owners_.back();
}

void ParticleScene::synchronizeEmitterSpatialState(
    ParticleEmitterInstance& emitter,const std::optional<m::Mat4>& target) {
    m::EmitterRadiusInput input;
    input.game=emitter.game;

    input.state=emitter.properties.get();
    input.emitterPosition=emitter.center;
    input.forceUnbounded=emitter.preparedForceUnbounded;
    if((emitter.data->rawWords.back()&m::emitter_flags::AffectedByWind)!=0u)
        input.globalWind=windManager().globalWind();
    const bool targeted=(emitter.data->rawWords.back()&m::emitter_flags::PointToPoint)!=0u ||
        emitter.support.update==m::EmitterUpdate::Lightning;
    if(targeted && target) input.targetPosition=m::transformPoint(*target,{0,0,0});
    if(!input.targetPosition) {
        for(const auto& chunk:emitter.chunks) if(chunk->active && chunk->asset) {
            if(engineRuntime(emitter.game)) {




                input.attachedObjects.push_back({matrixPosition(chunk->world),
                                                  chunk->engineOwnerRadius});
                continue;
            }
            if(const auto sphere=transformedBoundsSphere(chunk->pose.bounds,chunk->world))
                input.attachedObjects.push_back(*sphere);
            if(chunk->references) {
                for(std::size_t index=0;index<chunk->asset->references().size();++index) {
                    const auto* runtime=chunk->references->instance(index);
                    if(!runtime || !runtime->active) continue;
                    if(const auto sphere=transformedBoundsSphere(runtime->pose.bounds,
                       runtime->world))
                        input.attachedObjects.push_back(*sphere);
                }
            }
        }
    }
    emitter.calculatedRadius=m::calculateK2EmitterRadius(input);
    emitter.radiusInvalidated=false;
}

void ParticleScene::rebuildEmitterBucket() {
    emitterBucket_.clear();emitterAdmissions_.clear();
    if(!document_ || !document_->isOpen()) return;
    emitterAdmissions_.reserve(emitters_.size());
    for(std::size_t index=0;index<emitters_.size();++index) {
        auto& emitter=*emitters_[index];
        emitter.visible=emitter.active;
        if(!emitter.active) continue;
        if(engineRuntime(emitter.game)) {
            const bool k1=emitter.game==m::GameVersion::Kotor1;
            // K1 SetGlobalWind synchronously calls CalculateRadius before
            // ProcessParts performs its ordinary plane tests. A dirty radius
            // is not a engine admission bypass.
            if(k1 && emitter.radiusInvalidated)
                synchronizeEmitterSpatialState(emitter,emitter.preparedTarget);
            // SetGlobalWind invalidates registered windy-emitter radii before
            // it calls back into the scene. Do not reject an emitter using its
            // preceding radius if a host rebuilds the bucket before the next
            // pose/spatial synchronization.
            bool visible=!k1 && emitter.radiusInvalidated;
            if(rooms_.empty()) {
                if(!visible) visible=!m::k2EmitterOutside(
                    emitter.center,emitter.calculatedRadius,
                    scenePlanes_);
                if(emitter.cullingBypass) visible=true;
            } else {
                bool considered=false;
                for(const auto roomIndex:emitter.roomMembership) {
                    if(visible) break;
                    if(roomIndex>=rooms_.size() || !rooms_[roomIndex].active) continue;
                    considered=true;
                    const auto& room=rooms_[roomIndex];
                    const auto& planes=!k1 && room.visibilityPlanes.empty()
                        ? scenePlanes_ : room.visibilityPlanes;
                    if(!m::k2EmitterOutside(emitter.center,emitter.calculatedRadius,
                       planes) || emitter.cullingBypass) {
                        visible=true;
                        break;
                    }
                }
                // An owner with no usable room record follows the scene's
                // no-room emitter list rather than disappearing silently.
                if(!k1 && !visible && !considered && emitter.roomMembership.empty())
                    visible=!m::k2EmitterOutside(emitter.center,emitter.calculatedRadius,
                        scenePlanes_) || emitter.cullingBypass;
            }
            emitter.visible=visible;
        }
        if(!emitter.visible) continue;
        emitterAdmissions_.push_back(index);
    }
    emitterBucket_=emitterAdmissions_;
    visibilityInitialized_=true;
}

void ParticleScene::refreshEngineEmitterFrame(ParticleEmitterInstance& emitter,
                                             m::ParticleEnvironment& environment) {
    if(!references_)return; // Explicit resource-pose inspection has no live Gob runtime.
    const auto& owner=emitter.reference?references_->instance(*emitter.reference)->frame
                                       :references_->mainFrame();
    environment.engineOwnerFrame=staticPrimary_ && !emitter.reference
        ? std::nullopt : std::optional<m::EngineFrame>{owner};
    emitter.ownerScale=owner.scale;
    // AnimateParticles invokes GetWorld at the call, after any earlier Gob or
    // RenderNothing update. Bucket preparation is only an admission snapshot.
    emitter.frame=references_->enginePartFrame(emitter.reference,emitter.nodeIndex);
    emitter.world=m::nodeTransform(emitter.frame.position,emitter.frame.orientation);
    emitter.center=emitter.frame.position;
    emitter.ownerPosition=staticPrimary_ && !emitter.reference?emitter.center:owner.position;
}

void ParticleScene::prewarmEmitter(ParticleEmitterInstance& emitter) {
    if(!emitter.active || !engineRuntime(emitter.game)) return;
    auto localEnvironment=emitter.preparedEnvironment;



    for(unsigned step=0;step<100u;++step) {
        refreshEngineEmitterFrame(emitter,localEnvironment);
        m::advanceEmitter(*emitter.data,*emitter.properties,emitter.frame,0.1,
            emitter.simulation,&runtimeRandom(),&localEnvironment,emitter.ownerScale);
    }
    if(emitter.chunkAsset) synchronizeEmitterSpatialState(emitter,emitter.preparedTarget);
}

void ParticleScene::prewarmRegisteredEmitter(std::size_t index,ParticlePrewarmStep& step) {
    auto& emitter=*emitters_.at(index);
    step.emitterIndex=index;step.particlesBefore=emitter.simulation.particles.size();
    step.randomBefore=runtimeRandom().state;
    try {
        prewarmEmitter(emitter);step.completed=true;
    } catch(...) {
        step.particlesAfter=emitter.simulation.particles.size();step.randomAfter=runtimeRandom().state;
        throw;
    }
    step.particlesAfter=emitter.simulation.particles.size();step.randomAfter=runtimeRandom().state;
}

void ParticleScene::prewarmPrepared(std::vector<ParticlePrewarmStep>* output) {
    if(!prewarmPending()) return;
    for(std::size_t index=0;index<emitters_.size();++index) {
        const auto& emitter=*emitters_[index];
        if(!emitter.active || !engineRuntime(emitter.game)) continue;
        ParticlePrewarmStep step;
        try {
            prewarmRegisteredEmitter(index,step);
        } catch(...) {
            if(output)output->push_back(step);
            throw;
        }
        if(output)output->push_back(step);
    }
    prewarmed_=true;
    rebuildEmitterBucket();
}

std::optional<m::ParticleContact> ParticleScene::traceEngineScene(m::Vec3 from,m::Vec3 to) const {
    if(!sceneQueries_)return {};
    SceneHitInfo info;info.staticOnly=true;info.surfaceMask=0xbfffffffu;
    if(!sceneQueries_->hit(from,to,info))return {};
    const m::Vec3 delta{to.x-from.x,to.y-from.y,to.z-from.z};
    const m::Vec3 travel{info.position.x-from.x,info.position.y-from.y,info.position.z-from.z};
    const float squared=(delta.x*delta.x+delta.y*delta.y)+delta.z*delta.z;
    const float fraction=squared>0?std::clamp(((travel.x*delta.x+travel.y*delta.y)+travel.z*delta.z)/squared,0.0f,1.0f):1.0f;
    return m::ParticleContact{info.normal,fraction,info.position,true};
}

void ParticleScene::prepareFrame(const m::Pose& pose,double elapsed,bool includeReferences,
                                 bool primaryOwnerAnimating,
                                 const std::vector<bool>& referenceOwnerAnimating) {
    if(!document_ || !document_->isOpen()) return;
    if(!engineRuntime(document_->model().game) && (!std::isfinite(elapsed) || elapsed<0)) return;
    synchronizeReferenceRegistrations();
    const auto bases=includeReferences && !references_
        ? document_->referenceBases(pose) : std::vector<std::optional<m::Mat4>>{};
    const auto frames=includeReferences && !references_
        ? document_->referenceFrames(pose) : std::vector<std::optional<m::EngineFrame>>{};
    missingTargets_=0;

    std::optional<m::Mat4> fallbackTarget;
    std::optional<m::EngineFrame> fallbackTargetFrame;
    if(settings_.targetEnabled) {
        if(settings_.targetNode.empty()) {
            fallbackTargetFrame=m::EngineFrame{settings_.targetPosition,settings_.targetOrientation,1.0f};
            fallbackTarget=m::nodeTransform(settings_.targetPosition,settings_.targetOrientation);
        }
        else {
            const auto name=lowerName(settings_.targetNode);
            for(std::size_t n=0;n<document_->model().nodes.size()&&n<pose.worldTransforms.size();++n)
                if(lowerName(document_->model().nodes[n].name)==name) {
                    fallbackTargetFrame=m::enginePartWorld(document_->model(),pose,
                        references_?references_->mainFrame():m::EngineFrame{},n);
                    fallbackTarget=m::nodeTransform(fallbackTargetFrame->position,fallbackTargetFrame->orientation);
                    break;
                }
        }
    }

    m::ParticleEnvironment environment;
    environment.target=fallbackTarget;environment.engineTarget=fallbackTargetFrame;environment.cameraPosition=cameraPosition_;
    environment.bounceMode=settings_.bounceMode;
    environment.game=document_->model().game;

    environment.objectTint={settings_.objectTint.x*objectVisualInputs_.objectTint.x,
                            settings_.objectTint.y*objectVisualInputs_.objectTint.y,
                            settings_.objectTint.z*objectVisualInputs_.objectTint.z};
    environment.objectAlpha=settings_.objectAlpha*objectVisualInputs_.objectAlpha;
    environment.tileAlpha=settings_.tileAlpha*objectVisualInputs_.tileAlpha;
    environment.windDisplacement=[this](m::Vec3 position,float seconds) {
        return windDisplacement(position,seconds);
    };
    if(settings_.blastWind)
        environment.addPointSourceWind=[this](m::Vec3 position,float radius,float lifetime,float strength) {
            // Explosion::Update dispatches before particle construction and
            // passes authored values directly (K2 Desktop 0x11f74a..0x11f7f4).
            windManager().addPointSourceWind(position,radius,lifetime,strength);
        };
    if(sceneQueries_&&engineRuntime(document_->model().game))
        environment.trace=[this](m::Vec3 from,m::Vec3 to) {return traceEngineScene(from,to);};

    // Synchronize Parts and owning objects without running PartEmitter. Engine
    // AnimateParticles receives the actual Gob frame and maintains its own
    // mover-specific inheritance cache independently of these pose updates.
    for(auto& emitterPointer:emitters_) {
        auto& emitter=*emitterPointer;
        const m::Pose* source=&pose;
        m::Mat4 base=references_ ? references_->mainWorld() : m::identityMatrix();
        m::EngineFrame baseFrame=references_?references_->mainFrame():m::EngineFrame{};
        emitter.active=emitter.support.supported && emitter.textureSupported;
        emitter.diagnostic=emitter.resourceDiagnostic;
        emitter.preparedTarget=fallbackTarget;emitter.preparedTargetFrame=fallbackTargetFrame;
        for(const auto& route:objectVisualInputs_.targets) if(routeMatches(route,emitter,*document_)) {
            emitter.preparedTarget=route.target;emitter.preparedTargetFrame=route.frame;break;
        }
        emitter.preparedEnvironment=environment;
        emitter.preparedEnvironment.baseEmitterState=&emitter.baseProperties;
        emitter.preparedEnvironment.target=emitter.preparedTarget;
        emitter.preparedEnvironment.engineTarget=emitter.preparedTargetFrame;
        emitter.preparedEnvironment.weatherCameraPosition=emitter.game==m::GameVersion::Kotor2 &&
            weather_ && !emitter.reference
            ? weatherCameraPosition_ : std::nullopt;
        prepareChunkCallbacks(emitter,emitter.preparedEnvironment);
        const auto& ownerModel=emitter.reference
            ? document_->references()[*emitter.reference].model : document_->model();
        emitter.preparedForceUnbounded=objectVisualInputs_.forceUnbounded ||
            (emitter.ownerIdentity!=0 && (ownerModel.classification&1u)!=0);
        if(((emitter.data->rawWords.back()&m::emitter_flags::PointToPoint)!=0u ||
            emitter.support.update==m::EmitterUpdate::Lightning) && !emitter.preparedTarget) {
            // MoveFunc skips only movement when a P2P target is null; ordinary
            // engine Update still ages/spawns/renders the population. Lightning
            // instead drains its main pool while retaining its children.
            if(!engineRuntime(emitter.game) && emitter.support.update!=m::EmitterUpdate::Lightning)
                emitter.active=false;
            ++missingTargets_;
            emitter.diagnostic+="; no target: supply an owner/event route or Particle Scene Settings target";
        }
        if((emitter.data->rawWords.back()&m::emitter_flags::Bounce)!=0u && !environment.trace)
            emitter.diagnostic+="; no engine scene query or explicit inspection collision input";
        bool ownerAnimating=primaryOwnerAnimating;
        if(emitter.reference) {
            const auto reference=*emitter.reference;
            ownerAnimating=reference<referenceOwnerAnimating.size() && referenceOwnerAnimating[reference];
            if(!includeReferences) emitter.active=false;
            else if(references_) {
                const auto* runtime=references_->instance(reference);
                if(!runtime || !runtime->active ||
                   (engineRuntime(emitter.game) && !runtime->sceneMember)) emitter.active=false;
                else {base=runtime->world;baseFrame=runtime->frame;source=&runtime->pose;}
            } else if(engineRuntime(emitter.game)) {
                if(reference>=frames.size() || !frames[reference])emitter.active=false;
                else {baseFrame=*frames[reference];base=m::nodeTransform(baseFrame.position,baseFrame.orientation);
                    source=&document_->references()[reference].pose;}
            } else if(reference>=bases.size() || !bases[reference])emitter.active=false;
            else {base=*bases[reference];source=&document_->references()[reference].pose;}
        }
        if(emitter.nodeIndex>=source->worldTransforms.size()) emitter.active=false;
        if(!emitter.active) {
            emitter.visible=false;
            emitter.roomMembership.clear();
            if(!engineRuntime(emitter.game)) {
                emitter.simulation.reset(emitter.simulation.seed);
                emitter.lastAnimatedOwnerPositionValid=false;
            }
            clearEmitterMeshes(emitter);
            continue;
        }
        if(engineRuntime(emitter.game)) {
            const auto& asset=emitter.reference
                ? document_->references()[*emitter.reference].model : document_->model();
            emitter.ownerScale=emitter.reference
                ? (references_ ? references_->instance(*emitter.reference)->scale
                               : baseFrame.scale)
                : (references_ ? references_->ownerScale() : 1.0f);
            baseFrame.scale=emitter.ownerScale;
            // Collapsed static Parts have no Gob. Reference and chunk Parts
            // retain their independently owned object's raw frame.
            emitter.preparedEnvironment.engineOwnerFrame=staticPrimary_ && !emitter.reference
                ? std::nullopt : std::optional<m::EngineFrame>{baseFrame};
            emitter.frame=m::enginePartWorld(asset,*source,baseFrame,emitter.nodeIndex);
            emitter.world=m::nodeTransform(emitter.frame.position,emitter.frame.orientation);
        } else emitter.world=m::multiply(base,source->worldTransforms[emitter.nodeIndex]);
        // Construction and controllers share PartEmitter storage with Update.
        // K2 Desktop CreateInstanceTreeR 0xa800e applies base controls once;
        // AnimateHierarchy 0xfd3ed dispatches only authored animation controls.
        // Pose synchronization must not restore an earlier birth-rate copy.
        // Without a ReferenceScene this compatibility path has only a static
        // resource pose; retain this scene's independently constructed Part.
        if((!emitter.reference || references_) && emitter.nodeIndex<source->nodeEmitters.size())
            emitter.properties=source->nodeEmitters[emitter.nodeIndex];
        emitter.preparedEnvironment.partScale=source->scales[emitter.nodeIndex];
        const auto localEmitter=m::nodeTransform({},source->orientations[emitter.nodeIndex]);
        emitter.preparedEnvironment.localEmitterForward={localEmitter.values[8],
            localEmitter.values[9],localEmitter.values[10]};
        emitter.center=matrixPosition(emitter.world);
        emitter.ownerPosition=staticPrimary_ && !emitter.reference ? emitter.center : matrixPosition(base);
        const auto roomOwner=staticPrimary_ && !emitter.reference
            ? (emitter.sceneIdentity | (1ull<<63u)) : emitter.ownerIdentity;
        auto& owner=ownerState(roomOwner);
        if(!owner.initialized) {
            owner.current=emitter.ownerPosition;
            owner.previous=emitter.ownerPosition;
            owner.initialized=true;
        } else owner.current=emitter.ownerPosition;
        if(emitter.ownerIdentity!=0) owner.pendingAnimating=owner.pendingAnimating||ownerAnimating;
        if(!engineRuntime(emitter.game)) {
            if(!emitter.lastAnimatedOwnerPositionValid) {
                emitter.lastAnimatedOwnerPosition=emitter.ownerPosition;
                emitter.lastAnimatedOwnerPositionValid=true;
                emitter.ownerTranslationDelta={};
            } else emitter.ownerTranslationDelta={
                emitter.ownerPosition.x-emitter.lastAnimatedOwnerPosition.x,
                emitter.ownerPosition.y-emitter.lastAnimatedOwnerPosition.y,
                emitter.ownerPosition.z-emitter.lastAnimatedOwnerPosition.z};
        }
    }

    for(auto& owner:owners_) if(owner.initialized) {
        owner.cachedRooms=m::k2EmitterRoomMembership(owner.current,owner.previous,
            owner.cachedRooms,rooms_);
    }
    for(auto& emitterPointer:emitters_) {
        auto& emitter=*emitterPointer;
        if(!emitter.active) continue;
        const auto roomOwner=staticPrimary_ && !emitter.reference
            ? (emitter.sceneIdentity | (1ull<<63u)) : emitter.ownerIdentity;
        auto& owner=ownerState(roomOwner);
        if(engineRuntime(emitter.game)) {
            emitter.roomMembership=owner.cachedRooms;
            synchronizeEmitterSpatialState(emitter,emitter.preparedTarget);
        } else emitter.roomMembership.clear();
    }
    for(auto& owner:owners_) if(owner.initialized) owner.previous=owner.current;
    preparedEnvironment_=std::move(environment);
    preparedTarget_=fallbackTarget;
    prepared_=true;
    rebuildEmitterBucket();
}

std::size_t ParticleScene::particleCount() const noexcept {
    std::size_t total=0;
    for(const auto& emitter:emitters_) total+=emitter->simulation.particles.size();
    return total;
}

bool ParticleScene::preparedOwnerAnimating(std::size_t emitterIndex) const noexcept {
    if(emitterIndex>=emitters_.size()) return false;
    const auto ownerIdentity=emitters_[emitterIndex]->ownerIdentity;
    if(ownerIdentity==0) return false;
    const auto found=std::find_if(owners_.begin(),owners_.end(),
        [&](const auto& owner){return owner.identity==ownerIdentity;});
    return found!=owners_.end() && found->pendingAnimating;
}

bool ParticleScene::animatePreparedEmitter(std::size_t emitterIndex,double elapsed) {
    if(!prepared_ || emitterIndex>=emitters_.size()) return false;
    auto& emitter=*emitters_[emitterIndex];
    if(!engineRuntime(emitter.game) && (!std::isfinite(elapsed) || elapsed<0)) return false;
    if(!emitter.active || (engineRuntime(emitter.game) && !emitter.visible && !deterministicReplay_))
        return false;
    auto localEnvironment=emitter.preparedEnvironment;
    if(!engineRuntime(emitter.game))localEnvironment.ownerTranslationDelta=emitter.ownerTranslationDelta;




    const float emitterDt=m::engineEmitterBucketElapsed(static_cast<float>(elapsed),
        emitter.game);
    if(engineRuntime(emitter.game)) {
        refreshEngineEmitterFrame(emitter,localEnvironment);
        m::advanceEmitter(*emitter.data,*emitter.properties,emitter.frame,emitterDt,
            emitter.simulation,&runtimeRandom(),&localEnvironment,emitter.ownerScale);
    } else m::advanceLegacyEmitter(*emitter.data,*emitter.properties,emitter.world,emitterDt,
        emitter.simulation,&runtimeRandom(),&localEnvironment);
    if(!engineRuntime(emitter.game) && emitterDt>0) {
        emitter.lastAnimatedOwnerPosition=emitter.ownerPosition;
        emitter.lastAnimatedOwnerPositionValid=true;
        emitter.ownerTranslationDelta={};
    }
    if(!engineRuntime(emitter.game)) updateEmitterChunks(emitter,static_cast<float>(emitterDt));
    if(engineRuntime(emitter.game) && emitter.chunkAsset)
        synchronizeEmitterSpatialState(emitter,emitter.preparedTarget);
    return true;
}

void ParticleScene::advance(const m::Pose& pose,double elapsed,bool includeReferences,bool finishWind) {
    if(!document_ || !document_->isOpen() || !std::isfinite(elapsed) || elapsed<0) return;
    sceneAnimation_->animate(static_cast<float>(elapsed));
    sceneAnimation_->beginRender();
    std::vector<bool> references(document_->references().size(),elapsed>0);
    prepareFrame(pose,elapsed,includeReferences,elapsed>0,references);
    prewarmPrepared();
    // Scene::Render updates wind before RenderSinglePass dispatches the
    // emitter bucket; births in this frame must see that updated wind field.
    if(finishWind) finishWindFrame(static_cast<float>(elapsed));
    sceneAnimation_->animateRemainingForRender();
    if(elapsed>0) {
        // Compatibility/headless path. Engine scene hosts use the merged sorted
        // coordinator instead; this retains the declared original admission order.
        for(const auto index:emitterAdmissions_)
            animatePreparedEmitter(index,elapsed);
    }
    finishBucketFrame();
    sceneAnimation_->endRender();
}

void ParticleScene::finishWindFrame(float elapsedSeconds) {




    if (!sceneWind_) wind_.updateTimer(elapsedSeconds);
}

ChunkMeshDraw chunkParticleMesh(const ChunkParticleInstance& instance,
                                std::optional<std::size_t> reference,
                                std::size_t meshIndex) {
    const auto& model=reference ? instance.asset->references().at(*reference).model
                                : instance.asset->model();
    const auto* runtime=reference&&instance.references
        ? instance.references->instance(*reference) : nullptr;
    const auto& pose=reference ? runtime ? runtime->pose
        : instance.asset->references().at(*reference).pose : instance.pose;
    const auto owner=reference ? runtime ? runtime->world
        : m::multiply(instance.world,instance.referenceBases.at(*reference).value()) : instance.world;
    const auto scale=reference ? runtime ? runtime->scale
        : instance.ownerScale*instance.asset->references().at(*reference).visualScale : instance.ownerScale;
    const auto localTime=runtime&&instance.particles ? runtime->animationTime : instance.time;
    const auto base=m::multiply(owner,m::nodeTransform({}, {},scale));
    const auto& mesh=model.meshes.at(meshIndex);
    const m::DeformedMesh* deformed=nullptr;
    if((instance.danglyEnabled || !mesh.dangly) && meshIndex<pose.deformedMeshes.size()) {
        const auto& candidate=pose.deformedMeshes[meshIndex];
        if(candidate.valid && candidate.positions.size()==mesh.vertices.size() &&
           candidate.normals.size()==mesh.vertices.size())deformed=&candidate;
    }
    const auto* material=reference?&instance.asset->referenceMaterial(*reference,meshIndex)
                                  :&instance.asset->material(meshIndex);
    const auto state=mesh.nodeIndex<pose.nodeMaterials.size()?pose.nodeMaterials[mesh.nodeIndex]
        :m::bindNodeMaterial(model.nodes[mesh.nodeIndex]);
    return {&mesh,deformed,material,instance.asset,state,model.game,
        deformed?base:m::multiply(base,pose.worldTransforms.at(mesh.nodeIndex)),localTime,model.fog,owner,
        instance.ownerIdentity+(reference ? *reference+1u : 0u)};
}

std::vector<ChunkMeshDraw> chunkParticleMeshes(const ChunkParticleInstance& instance) {
    std::vector<ChunkMeshDraw> result;
    if(!instance.active || !instance.asset) return result;
    const auto append=[&](const m::Model& model,const m::Pose& pose,
                          std::optional<std::size_t> reference) {
        for(std::size_t index=0;index<model.meshes.size();++index) {
            if(model.meshes[index].nodeIndex>=pose.worldTransforms.size())continue;
            result.push_back(chunkParticleMesh(instance,reference,index));
        }
    };
    append(instance.asset->model(),instance.pose,{});
    if(instance.references) {
        for(std::size_t r=0;r<instance.asset->references().size();++r) {
            const auto* runtime=instance.references->instance(r);
            if(!runtime || !runtime->active) continue;
            const auto& reference=instance.asset->references()[r];
            append(reference.model,runtime->pose,r);
        }
    } else {
        for(std::size_t r=0;r<instance.referenceBases.size();++r) if(instance.referenceBases[r]) {
            const auto& reference=instance.asset->references()[r];
            append(reference.model,reference.pose,r);
        }
    }
    return result;
}

void ParticleScene::updateEmitterChunks(ParticleEmitterInstance& emitter,
                                        float elapsedSeconds) {
    auto* e=&emitter;
    if(engineRuntime(e->game)) {
        // RenderNothing skips its Gob-transform writes while TileData alpha
        // is below one (K1 Desktop 0x2e9e55, K2 Desktop 0x114a3f).
        if(e->preparedEnvironment.tileAlpha<1.0f) return;
        for(const auto& particle:e->simulation.particles) {
            const auto found=std::find_if(e->chunks.begin(),e->chunks.end(),
                [&](const auto& chunk){return chunk->serial==particle.serial;});
            auto& chunk=**found;
            if(!particle.stopped && e->properties->rotation!=0) {
                chunk.frame.orientation=m::engineQuaternionAxisAngle(particle.direction,
                    particle.rotation);
                chunk.orientation=m::nodeTransform({},chunk.frame.orientation);
            }
            chunk.frame.position=particle.position;chunk.frame.scale=chunk.ownerScale;
            chunk.world=chunk.orientation;
            chunk.world.values[12]=particle.position.x;chunk.world.values[13]=particle.position.y;
            chunk.world.values[14]=particle.position.z;
            chunk.danglyEnabled=danglyEnabled_;
            chunk.references->setOwnerFrame(chunk.frame,false);
            chunk.references->synchronize(chunk.pose,false);
            // RenderNothing changes Gob position/orientation after admission;
            // PartLightsaber::Animate, not Render, owns the history samples.
            chunk.lightsabers.refreshTransforms(chunk.pose);
            chunk.particles->prepareChunkFrame();
        }
        // Gob SetPosition/SetOrientation update the same scene consumers as
        // construction. Publish the completed RenderNothing transform batch.
        if(rootScene()->chunkMembershipChanged_) rootScene()->chunkMembershipChanged_();
        return;
    }
    for(auto& chunk:e->chunks) chunk->active=false;
    if(!e->active || !e->chunkAsset) {
        for(auto& chunk:e->chunks) {chunk->pose={};chunk->referenceBases.clear();}
        return;
    }
    if(e->game==m::GameVersion::Kotor2 && visibilityInitialized_ &&
       !e->visible && !deterministicReplay_) return;
    const auto& model=e->chunkAsset->model();
    for(const auto& p:e->simulation.particles) {
        ChunkParticleInstance* instance=nullptr;
        for(auto& chunk:e->chunks) if(chunk->serial==p.serial) {instance=chunk.get();break;}
        if(!instance) for(auto& chunk:e->chunks) if(!chunk->active &&
            std::none_of(e->simulation.particles.begin(),e->simulation.particles.end(),[&](const m::EmitterParticle& value){return value.serial==chunk->serial;})) {
            instance=chunk.get();break;
        }
        if(!instance) {e->chunks.push_back(std::make_unique<ChunkParticleInstance>());instance=e->chunks.back().get();}
        const bool assetChanged=instance->asset!=e->chunkAsset;
        const float previousChunkTime=instance->time;
        const bool serialChanged=instance->serial!=p.serial || assetChanged || !instance->everAssigned;
        if(assetChanged) {
            instance->orientation=m::identityMatrix();instance->everAssigned=false;instance->playImpact=false;




            if(e->game==m::GameVersion::Kotor2)
                instance->engineOwnerRadius=engineConstructedOwnerSphere(model).radius;
            instance->danglyState={};
            instance->references=std::make_shared<ReferenceScene>();
        instance->references->setSceneAnimation(sceneAnimation_);
        instance->references->setOwnerSceneMember(false);
            instance->references->setDocument(e->chunkAsset);
        }
        if(serialChanged) {
            if(e->game==m::GameVersion::Kotor2) {



                instance->ownerScale=p.ownerScaleAtBirth;
                instance->references->setOwnerScale(instance->ownerScale);
            }




            instance->playImpact=(e->game==m::GameVersion::Kotor2 || instance->everAssigned) &&
                e->support.update!=m::EmitterUpdate::Single && e->chunkClip.has_value();
            instance->everAssigned=true;
        }
        const bool animate=instance->playImpact && e->chunkClip.has_value();
        const bool changed=serialChanged || instance->pose.worldTransforms.empty() ||
            (animate && instance->time!=p.age);
        instance->serial=p.serial;instance->asset=e->chunkAsset;instance->time=p.age;
        if(changed) {
            if(animate) {
                const auto& clip=*e->chunkClip;
                instance->pose=m::evaluateAnimation(model,e->chunkAsset->modelAt(clip.modelIndex),
                    clip.animationIndex,p.age,clip.positionScale,false);
            } else instance->pose=m::bindPose(model);
            instance->referenceBases=e->chunkAsset->referenceBases(instance->pose);
        }
        if(instance->references) {
            instance->references->setOwnerWorld(instance->world);
            if(assetChanged || serialChanged)
                instance->references->synchronize(instance->pose);
            else
                instance->references->advance(instance->pose,
                    std::max(0.0f,p.age-previousChunkTime),runtimeRandom());
        }
        if(!p.stopped && e->properties->rotation!=0) {
            const float half=p.rotation*0.5f, sn=std::sin(half);
            instance->orientation=m::nodeTransform({},m::Quaternion{
                p.direction.x*sn,p.direction.y*sn,p.direction.z*sn,std::cos(half)});
        }
        instance->world=instance->orientation;
        instance->world.values[12]=p.position.x;instance->world.values[13]=p.position.y;instance->world.values[14]=p.position.z;
        instance->references->setOwnerWorld(instance->world);
        instance->references->synchronize(instance->pose);
        instance->danglyEnabled=danglyEnabled_;
        if(danglyEnabled_ && elapsedSeconds>0.0f) {
            m::DanglyEnvironment danglyEnvironment;

            danglyEnvironment.objectFrame=engineRigidFrame(instance->world);
            danglyEnvironment.objectFrame.scale=instance->ownerScale;
            danglyEnvironment.windDisplacement=[this](m::Vec3 position,float seconds) {
                return windDisplacement(position,seconds);
            };
            m::simulateDanglyMeshes(model,instance->pose,instance->danglyState,
                                    elapsedSeconds,danglyEnvironment);
            if(instance->references) {
                m::DanglyEnvironment referenceEnvironment=danglyEnvironment;
                referenceEnvironment.objectFrame={};
                instance->references->simulateDangly(elapsedSeconds,referenceEnvironment);
            }
        }
        instance->active=true;
    }
    for(auto& chunk:e->chunks) if(!chunk->active) {
        chunk->pose={};chunk->referenceBases.clear();
    }
}

void ParticleScene::dispatchEvent(const std::string& name,std::optional<std::size_t> reference) {
    std::string folded=name;for(auto& c:folded) if(c>='A'&&c<='Z') c=static_cast<char>(c-'A'+'a');
    if(folded!="detonate") return;
    for(auto& e:emitters_) if(e->reference==reference && e->support.update==m::EmitterUpdate::Explosion)
        e->simulation.detonate();
}
void ParticleScene::detonateAll(bool includeReferences) {
    for(auto& e:emitters_) if((!e->reference || includeReferences) && e->support.update==m::EmitterUpdate::Explosion)
        e->simulation.detonate();
}
bool ParticleScene::replay(std::optional<std::size_t> clip,double seconds,bool loop,bool includeReferences,
                           const ParticleReplayContext& context) {
    if(!document_ || !document_->isOpen() || !std::isfinite(seconds) || seconds<0 ||
       (clip && *clip>=document_->animations().size())) {replayDiagnostic_="Invalid particle replay target";return false;}
    const double steps=std::ceil(seconds*60.0);
    if(!std::isfinite(steps) || steps>=static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        replayDiagnostic_="Particle replay frame count is not representable";return false;
    }
    resetSimulation();
    // A seek reconstructs only this inspection particle tree. Its temporary
    // scene must not advance live area/main owners sharing the ordinary scene.
    struct ReplaySceneScope {
        ParticleScene& host;
        std::shared_ptr<SceneAnimation> previous;
        ~ReplaySceneScope() {
            host.sceneAnimation()->transferOwnersTo(previous);
            host.setSceneAnimation(previous);
        }
    } replayScene{*this,sceneAnimation_};
    setSceneAnimation(std::make_shared<SceneAnimation>());
    deterministicReplay_=true;
    struct ReplayScope final {
        bool& flag;
        ~ReplayScope() { flag=false; }
    } replayScope{deterministicReplay_};




    m::AnimationRuntime replayAnimation;

    std::vector<const m::Model*> hierarchy;
    for(std::size_t index=0;index<document_->modelHierarchySize();++index)
        hierarchy.push_back(&document_->modelAt(index));
    replayAnimation.setModel(&document_->model(),std::move(hierarchy));
    const auto replayLifetime=std::make_shared<const unsigned char>(0);
    replayAnimation.setEventCallback([this](const m::AnimationRuntimeEvent& event) {
        if(event.kind==m::AnimationRuntimeEventKind::AnimationEvent)dispatchEvent(event.name);
    },replayLifetime);
    if(clip) replayAnimation.play(document_->animationReference(*clip),1.0f,
        m::kAnimationRunImmediate|(loop?0u:m::kAnimationRunNonLooping),0.0f);
    replayAnimation.takeEvents();
    const std::vector<ParticleScene*> inspectionRoots{this};
    const auto prepare=[&](const m::Pose& pose,float elapsed) {
        if(context.prepareReferences) context.prepareReferences(pose,elapsed);
        std::vector<bool> references(document_->references().size(),elapsed>0);
        prepareFrame(pose,elapsed,includeReferences,elapsed>0,references);
    };
    prepare(replayAnimation.pose(),0);
    double previous=0;
    for(std::size_t step=0;step<static_cast<std::size_t>(steps);++step) {
        const double time=std::min(seconds,static_cast<double>(step+1)/60.0);
        const auto elapsed=static_cast<float>(time-previous);
        sceneAnimation_->animate(elapsed);
        sceneAnimation_->beginRender();
        replayAnimation.advance(elapsed);
        replayAnimation.takeEvents();
        prepare(replayAnimation.pose(),elapsed);
        finishWindFrame(elapsed);
        executeSceneEmitterBucket(inspectionRoots,context.view,context.planes,
            {elapsed,true,context.cameraFrame});
        sceneAnimation_->endRender();
        previous=time;
    }
    // Continuing playback after a seek must continue this generated state,
    // not run the scene-load ten-second prewarm on the following frame.
    finishPrewarm();
    if(context.commitEmitterState)context.commitEmitterState(replayAnimation.pose());
    replayDiagnostic_="Inspection particle tree replayed at 60 Hz through the engine emitter bucket; external scene history retained";
    return true;
}
model::EmitterBucketEntry emitterSortEntry(const ParticleEmitterInstance& e,std::size_t sourceIndex) {
    if(!e.data)throw std::invalid_argument("Emitter sort admission has no authored node");
    model::EmitterBucketEntry entry;
    entry.emitterIndex=sourceIndex;entry.identity=e.sceneIdentity;entry.ownerIdentity=e.ownerIdentity;
    entry.hasOwner=e.ownerIdentity!=0;entry.renderOrder=static_cast<std::int16_t>(e.data->renderOrder);
    entry.category=e.sceneCategory;entry.ownerPosition=e.ownerPosition;entry.emitterPosition=e.center;
    return entry;
}
void ParticleScene::clearEmitterMeshes(ParticleEmitterInstance& emitter) noexcept {
    emitter.mesh.vertices.clear();emitter.mesh.indices.clear();
    for(auto& mesh:emitter.frameBlendMeshes) {
        mesh.vertices.clear();mesh.indices.clear();
    }
}

void ParticleScene::buildEmitterMeshes(ParticleEmitterInstance& emitter,
                                       const m::Mat4& view,const m::EngineFrame& camera) {
    const bool renderable=emitter.active &&
        (!engineRuntime(emitter.game) || emitter.visible);
    if(!renderable || emitter.chunkAsset) {
        clearEmitterMeshes(emitter);return;
    }
    const auto build=[&](m::Mesh& mesh,unsigned pass) {
        if(engineRuntime(emitter.game))
            m::buildEmitterMesh(*emitter.data,*emitter.properties,emitter.simulation,view,camera.orientation,
                mesh,emitter.textureWidth,emitter.textureHeight,pass,emitter.ownerScale);
        else m::buildLegacyEmitterMesh(*emitter.data,*emitter.properties,emitter.simulation,view,
                mesh,emitter.textureWidth,emitter.textureHeight,pass);
    };
    build(emitter.mesh,0);
    for(unsigned pass=1;pass<=4;++pass)build(emitter.frameBlendMeshes[pass-1],pass);
}

void ParticleScene::renderPreparedEmitter(std::size_t emitterIndex,
                                          const m::Mat4& view,const m::EngineFrame& camera) {
    if(emitterIndex>=emitters_.size()) throw std::out_of_range("Particle emitter render index");
    auto& emitter=*emitters_[emitterIndex];
    if(!emitter.active || (engineRuntime(emitter.game) && !emitter.visible && !deterministicReplay_)) {
        clearEmitterMeshes(emitter);return;
    }
    // RenderFunc's independent target gate applies only to the authored
    // point-to-point flag, before frame updates and all draw dispatch.
    if((emitter.data->rawWords.back()&m::emitter_flags::PointToPoint)!=0u && !emitter.preparedTarget) {
        clearEmitterMeshes(emitter);return;
    }
    // PartEmitter::RenderFunc advances authored frame animation even when its
    // Scene/Gob animation guard skipped AnimateParticles for this frame.
    if(engineRuntime(emitter.game) && emitter.chunkAsset) {
        updateEmitterChunks(emitter,0.0f);
        synchronizeEmitterSpatialState(emitter,emitter.preparedTarget);
        // RenderFunc branches directly to RenderNothing for chunk models
        // (K1 Desktop 0x2f9f46..0x2f9f60); it does not enter sprite frame updates.
        clearEmitterMeshes(emitter);
        return;
    }
    m::updateEmitterParticleFrames(*emitter.data,*emitter.properties,
        emitter.simulation,&runtimeRandom());
    buildEmitterMeshes(emitter,view,camera);
}

void ParticleScene::finishBucketFrame() noexcept {
    for(auto& owner:owners_) owner.pendingAnimating=false;
}

void ParticleScene::prepareBucket(const m::Mat4& view,
                                  const std::vector<m::EmitterScenePlane>& scenePlanes,
                                  const m::EngineFrame& camera) {
    (void)view;
    cameraPosition_=camera.position;
    if(prepared_) preparedEnvironment_.cameraPosition=cameraPosition_;
    scenePlanes_=scenePlanes;

    rebuildEmitterBucket();
}

void ParticleScene::buildMeshes(const m::Mat4& view,
                                const std::vector<m::EmitterScenePlane>& scenePlanes,
                                bool sortLocalBucket) {
    auto camera=engineRigidFrame(view);
    camera.orientation=m::engineQuaternionInverse(camera.orientation);
    camera.position=m::engineQuaternionRotate(camera.orientation,
        {-camera.position.x,-camera.position.y,-camera.position.z});
    prepareBucket(view,scenePlanes,camera);
    const auto eye=cameraPosition_.value_or(m::Vec3{});
    if(sortLocalBucket&&document_&&document_->isOpen()&&
       engineRuntime(document_->model().game)) {
        std::vector<m::EmitterBucketEntry> entries;entries.reserve(emitterAdmissions_.size());
        for(auto index:emitterAdmissions_)entries.push_back(emitterSortEntry(*emitters_[index],index));
        try {emitterBucket_=m::engineEmitterBucketDrawOrder(entries,eye);}
        catch(...) {emitterBucket_.clear();throw;}
    }
    for(auto& emitter:emitters_) clearEmitterMeshes(*emitter);
    for(const auto index:emitterBucket_) renderPreparedEmitter(index,view,camera);
}
ParticleSceneStatistics ParticleScene::statistics() const noexcept {
    ParticleSceneStatistics result;result.emitters=emitters_.size();
    for(const auto& e:emitters_) {
        if(!e->support.supported || !e->textureSupported) ++result.unsupported;
        for(const auto& chunk:e->chunks) if(chunk->active) ++result.chunkInstances;
        result.particles+=e->simulation.particles.size();
    }
    result.activeBlasts=windManager().pointSources().size();
    result.registeredWindyEmitters=windManager().registeredWindyEmitters();
    result.visibleEmitters=emitterBucket_.size();result.prewarmed=prewarmed_;
    for(const auto& emitter:emitters_) result.roomRegistrations+=emitter->roomMembership.size();
    result.missingTargets=missingTargets_;return result;
}
m::Bounds ParticleScene::bounds() const noexcept {
    m::Bounds result;
    for(const auto& e:emitters_) if(e->active && !e->chunkAsset) for(const auto& p:e->simulation.particles) {
        const auto index=static_cast<std::size_t>(&p-e->simulation.particles.data());
        const bool lightning=e->support.update==m::EmitterUpdate::Lightning;
        const bool linked=e->support.render==m::EmitterRender::Linked;
        const m::LightningBranchState* branch=nullptr;
        if(lightning && index>=e->simulation.lightningMainCount) {
            if(!linked)continue;
            for(const auto& candidate:e->simulation.lightningBranches)
                if(candidate.flattenedStart && index>=*candidate.flattenedStart &&
                   index-*candidate.flattenedStart<candidate.particles.size()) {branch=&candidate;break;}
        }
        const auto& properties=branch?branch->properties:*e->properties;
        std::optional<float> normalizedPosition;
        if(lightning && (linked || e->support.render==m::EmitterRender::MotionBlur)) {
            const auto localIndex=branch?index-*branch->flattenedStart:index;
            const auto localCount=branch?branch->particles.size():e->simulation.lightningMainCount;
            normalizedPosition=static_cast<float>(localIndex)*(1.0f/static_cast<float>(localCount));
        }
        const auto a=m::emitterParticleAppearance(*e->data,properties,p,normalizedPosition);
        const auto scale=branch?1.0f:engineRuntime(e->game)?e->ownerScale:p.sourceScale;
        auto position=p.position;
        if(lightning && linked) {
            const auto offset=branch?branch->offsets[index-*branch->flattenedStart]
                :e->simulation.lightningOffsets[index];
            position={position.x+offset.x,position.y+offset.y,position.z+offset.z};
        }
        const float radius=std::max(a.size.x,a.size.y)*scale;
        const m::Vec3 lo{position.x-radius,position.y-radius,position.z-radius};
        const m::Vec3 hi{position.x+radius,position.y+radius,position.z+radius};
        if(!result.valid) {result.minimum=lo;result.maximum=hi;result.valid=true;}
        else {
            result.minimum={std::min(result.minimum.x,lo.x),std::min(result.minimum.y,lo.y),std::min(result.minimum.z,lo.z)};
            result.maximum={std::max(result.maximum.x,hi.x),std::max(result.maximum.y,hi.y),std::max(result.maximum.z,hi.z)};
        }
    }
    const auto extend=[&](const m::Bounds& bounds,const m::Mat4& world) {
        if(!bounds.valid) return;
        for(unsigned n=0;n<8;++n) {
            const auto point=m::transformPoint(world,{(n&1)?bounds.maximum.x:bounds.minimum.x,
                (n&2)?bounds.maximum.y:bounds.minimum.y,(n&4)?bounds.maximum.z:bounds.minimum.z});
            if(!finiteVector(point)) continue;
            if(!result.valid) {result.minimum=result.maximum=point;result.valid=true;}
            else {
                result.minimum={std::min(result.minimum.x,point.x),std::min(result.minimum.y,point.y),std::min(result.minimum.z,point.z)};
                result.maximum={std::max(result.maximum.x,point.x),std::max(result.maximum.y,point.y),std::max(result.maximum.z,point.z)};
            }
        }
    };
    for(const auto& e:emitters_) if(e->active) for(const auto& chunk:e->chunks) if(chunk->active) {
        extend(chunk->pose.bounds,m::multiply(chunk->world,m::nodeTransform({}, {},chunk->ownerScale)));
        if(chunk->references) {
            for(std::size_t r=0;r<chunk->asset->references().size();++r) {
                const auto* runtime=chunk->references->instance(r);
                if(runtime && runtime->active)
                    extend(runtime->pose.bounds,m::multiply(runtime->world,
                        m::nodeTransform({}, {},runtime->scale)));
            }
        } else {
            for(std::size_t r=0;r<chunk->referenceBases.size();++r) if(chunk->referenceBases[r])
                extend(chunk->asset->references()[r].pose.bounds,
                    m::multiply(m::multiply(chunk->world,*chunk->referenceBases[r]),
                        m::nodeTransform({}, {},chunk->ownerScale*chunk->asset->references()[r].visualScale)));
        }
    }
    return result;
}
} // namespace neomdl
