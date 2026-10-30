#include "core/AreaScene.hpp"
#include <model/AnimMesh.hpp>
#include <model/AsciiModel.hpp>
#include "core/SceneQueries.hpp"
#include "core/RoomSurfaceMesh.hpp"
#include "core/AreaLoadTables.hpp"

#include "model/SceneOrdering.hpp"
#include <neoshared/PathUtf8.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace neomdl {
struct AreaWorldState {
    model::ParticleRandom random;
    model::WindManager wind;
    std::shared_ptr<SceneAnimation> animation=std::make_shared<SceneAnimation>();

    std::optional<model::GameVersion> game;
    bool captured{};
    AreaWorldState() { wind.setRandomSource([this] { return random.next(); }); wind.reset(); }
    explicit AreaWorldState(const AreaRuntimeServices& services)
        :random(*services.random),wind(*services.wind),game(services.game),captured(true) {
        // Copy values on the UI thread, then remove every closure into its
        // live runtime before this snapshot is transferred to a worker.
        if(services.animation)animation->setCombatPaused(services.animation->combatPaused());
        wind.clearWindyEmitters();
        wind.setRandomSource([this] {return random.next();});
    }
};
namespace {
// Construction can consume the enclosing scene's random stream. Failure must
// deregister temporary owners first and then restore that stream, not reroll
// the still-open scene. Keep this guard before the pending AreaScene variable.
struct RandomTransaction {
    model::ParticleRandom* random{};
    std::uint32_t state{};
    bool committed{};
    explicit RandomTransaction(model::ParticleRandom* p):random(p),state(p?p->state:0){}
    ~RandomTransaction(){if(random&&!committed)random->state=state;}
};
constexpr std::size_t roomInstanceBase = 0x80000000ull;

std::string key(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Area: " + message);
}
bool finite(model::Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
void extend(model::Bounds& b, model::Vec3 p) {
    require(finite(p), "non-finite transformed room vertex");
    if (!b.valid) { b.minimum = b.maximum = p; b.valid = true; return; }
    b.minimum = {std::min(b.minimum.x,p.x),std::min(b.minimum.y,p.y),std::min(b.minimum.z,p.z)};
    b.maximum = {std::max(b.maximum.x,p.x),std::max(b.maximum.y,p.y),std::max(b.maximum.z,p.z)};
}
std::optional<std::filesystem::path> localResource(const std::filesystem::path& directory,
                                                   const std::string& name) {
    std::optional<std::filesystem::path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (key(neoshared::pathToUtf8(it->path().filename())) != key(name)) continue;
        require(!result, "ambiguous case-insensitive local resource: " + name);
        result = it->path();
    }
    if (error) throw std::runtime_error("Area directory: " + error.message());
    return result;
}
std::vector<std::string> lines(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(stream,line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos) result.push_back(line.substr(first));
    }
    return result;
}
void validResref(const std::string& name) {
    require(!name.empty() && name != "." && name != ".." &&
            name.find_first_of("/\\:") == std::string::npos,
            "invalid room resource name: " + name);
}
float above(const model::Bounds& box, const model::TriggerPlane& p) {
    // BoxAbovePlane, Desktop 0x3134b8: minimum supporting corner along the normal.
    const auto& n = p.normal;
    return n.x * (n.x > 0 ? box.minimum.x : box.maximum.x) +
           n.y * (n.y > 0 ? box.minimum.y : box.maximum.y) +
           n.z * (n.z > 0 ? box.minimum.z : box.maximum.z) + p.distance;
}
struct VisualTargetOwner {
    const model::Model* asset{};
    const model::Pose* pose{};
    model::EngineFrame frame{};
};
} // namespace

// Memberwise move assignment would release the old owned WindManager before
// replacing its emitter objects. Transfer whole lifetime domains before any
// destruction; callbacks are deregistered while their original manager lives.
// As for clear/load, callers unregister external LightScene state first.
void AreaScene::swap(AreaScene& other) noexcept {
    using std::swap;
    swap(animationHost_,other.animationHost_);
    animationHost_->scene=this;other.animationHost_->scene=&other;
    swap(worldState_,other.worldState_);swap(services_,other.services_);
    swap(trace_,other.trace_);swap(identity_,other.identity_);swap(game_,other.game_);swap(placements_,other.placements_);
    swap(triggers_,other.triggers_);swap(cameras_,other.cameras_);
    swap(sounds_,other.sounds_);
    swap(triggerRuntime_,other.triggerRuntime_);swap(triggerEvents_,other.triggerEvents_);
    swap(diagnostics_,other.diagnostics_);swap(visualSettings_,other.visualSettings_);
    swap(environment_,other.environment_);swap(environmentLightDocument_,other.environmentLightDocument_);
    swap(environmentLightPose_,other.environmentLightPose_);swap(environmentLightWorld_,other.environmentLightWorld_);
    swap(resolver_,other.resolver_);swap(grass_,other.grass_);swap(ownsWind_,other.ownsWind_);
    swap(weather_,other.weather_);swap(weatherResources_,other.weatherResources_);
    swap(weatherObjects_,other.weatherObjects_);swap(detachedWeather_,other.detachedWeather_);
    swap(nextWeatherInstance_,other.nextWeatherInstance_);swap(weatherCamera_,other.weatherCamera_);
    swap(objects_,other.objects_);swap(rooms_,other.rooms_);swap(walkableMaterials_,other.walkableMaterials_);swap(path_,other.path_);
    swap(trackAnimations_,other.trackAnimations_);swap(worldAnimationEvents_,other.worldAnimationEvents_);
    swap(worldAnimationEvent_,other.worldAnimationEvent_);
    swap(miniGame_,other.miniGame_);swap(trackFollowers_,other.trackFollowers_);
    swap(trackAnimationCallbacks_,other.trackAnimationCallbacks_);
    swap(worldAnimationFrameInput_,other.worldAnimationFrameInput_);
    swap(currentRoom_,other.currentRoom_);swap(automaticCurrentRoom_,other.automaticCurrentRoom_);
    swap(currentRoomOverride_,other.currentRoomOverride_);
    swap(ignoreVisibility_,other.ignoreVisibility_);
    swap(activeRooms_,other.activeRooms_);swap(ownedLightIds_,other.ownedLightIds_);
}
AreaScene& AreaScene::operator=(AreaScene&& other) noexcept {
    if(this!=&other) {AreaScene old;swap(old);swap(other);}
    return *this;
}
void AreaScene::prepareRuntime(AreaRuntimeServices services) {
    worldState_=std::make_shared<AreaWorldState>(services);
}
void AreaScene::commitRuntime(AreaRuntimeServices services) {
    // Rebinding live wind/random services preserves the authoring directory
    // selected by load(), including future appearance/weather/mine loads.
    services.asciiResources=services_.asciiResources;
    if(!services.animation)services.animation=services_.animation;
    // Transfer the scheduling mode established by module/layout loading;
    // localized area names do not determine desktop animation order.
    const bool interleaved=services_.animation->interleaved();
    services_.animation->transferOwnersTo(services.animation);
    services.animation->setInterleaved(interleaved);
    if(worldState_&&worldState_->captured) {
        *services.random=worldState_->random;
        worldState_->captured=false;
    }
    for(const auto& object:objects_) {
        object->setSceneAnimation(services.animation);
        object->references().setSceneRandom(services.random);
        object->particles().setSceneRandom(services.random);
        object->particles().setSceneWind(services.wind);
        object->animationOwner().bindWind(&object->particles().windManager(),
            [owner=object.get()]{return owner->world();},object->document()->model().game);
        object->setQueries(services.queries);
    }
    for(const auto& object:detachedWeather_)if(object) {
        object->setSceneAnimation(services.animation);
        object->references().setSceneRandom(services.random);
        object->particles().setSceneRandom(services.random);
        object->particles().setSceneWind(services.wind);
        object->animationOwner().bindWind(&object->particles().windManager(),
            [owner=object.get()]{return owner->world();},object->document()->model().game);
        object->setQueries(services.queries);
    }
    services_=services;ownsWind_=false;

    environment_.setAutomaticWorldTime(worldTimeRuntimeSupported());
}
void AreaScene::setWeatherAttached(std::size_t slot,bool attached) {
    auto* object=weatherObjects_[slot];
    if(!object)return;
    if(attached) {
        if(detachedWeather_[slot]) {
            object->setSceneMember(true);
            objects_.push_back(std::move(detachedWeather_[slot]));
        }
    } else if(!detachedWeather_[slot]) {
        const auto found=std::find_if(objects_.begin(),objects_.end(),[&](const auto& entry){return entry.get()==object;});
        detachedWeather_[slot]=std::move(*found);objects_.erase(found);
        object->setSceneMember(false);
    }
}
WeatherVisualCallbacks AreaScene::weatherCallbacks(const std::function<void(AreaObject&)>& initialize) {
    WeatherVisualCallbacks host;
    host.create=[this,&initialize](std::size_t slot,const std::string& name) {
        const auto asset=weatherResources_.models.find(name);
        if(asset==weatherResources_.models.end()) {
            const auto message="Engine weather model unavailable: "+name;
            if(std::find(diagnostics_.begin(),diagnostics_.end(),message)==diagnostics_.end())diagnostics_.push_back(message);
            return false;
        }
        for(const auto& object:objects_)nextWeatherInstance_=std::max(nextWeatherInstance_,
            object->instanceIndex()+1u+object->document()->references().size());
        for(const auto& object:detachedWeather_)if(object)nextWeatherInstance_=std::max(nextWeatherInstance_,
            object->instanceIndex()+1u+object->document()->references().size());
        auto object=std::make_unique<AreaObject>(nextWeatherInstance_,AreaObjectKind::Weather,
            slot==6?"weatherlight":"weather",asset->second,model::EngineFrame{},std::nullopt,services_,false);
        nextWeatherInstance_+=1u+object->document()->references().size();
        object->particles().setWeather(game_==model::GameVersion::Kotor2&&slot<6);
        object->particles().setWeatherCamera(weatherCamera_);
        if(initialize)initialize(*object);
        object->setSceneMember(false);
        weatherObjects_[slot]=object.get();detachedWeather_[slot]=std::move(object);return true;
    };
    host.destroy=[this](std::size_t slot) {
        releaseWorldAnimationOwner(weatherObjects_[slot]->instanceIndex());
        setWeatherAttached(slot,false);detachedWeather_[slot].reset();weatherObjects_[slot]=nullptr;
    };
    host.attach=[this](std::size_t slot,bool attached) {setWeatherAttached(slot,attached);};
    host.setFrame=[this](std::size_t slot,const model::EngineFrame& value) {
        auto frame=value;frame.scale=weatherObjects_[slot]->frame().scale;
        weatherObjects_[slot]->setFrame(frame);
    };
    host.setDensity=[this](std::size_t slot,const std::string& name,float rate) {
        auto& object=*weatherObjects_[slot];
        object.play(name,0.0f,0x80u,0.0f);object.play(name,rate,0u,0.0f);
        const auto animation=object.document()->findAnimation(name);
        const auto duration=animation?object.document()->modelAt(animation->modelIndex).animations[animation->animationIndex].length:-1.0f;
        object.animateExternal(duration);object.play(name,0.0f,0u,0.0f);
    };
    host.flash=[this](std::size_t slot,const std::string& name) {
        auto& object=*weatherObjects_[slot];const auto animation=object.document()->findAnimation(name);
        const float duration=animation?object.document()->modelAt(animation->modelIndex).animations[animation->animationIndex].length:-1.0f;
        object.play(name,1.0f,game_==model::GameVersion::Kotor1?0x03u:0x43u,0.0f);return duration;
    };
    host.camera=[this](std::optional<model::Vec3> camera) {
        weatherCamera_=camera;
        for(auto* object:weatherObjects_)if(object)object->particles().setWeatherCamera(camera);
    };
    return host;
}
bool AreaScene::setWeatherImmediate(std::uint8_t type,float density,const std::function<void(AreaObject&)>& initialize) {
    return weather_.setWeather(type,density,weatherCallbacks(initialize));
}
WeatherFrameOutput AreaScene::advanceWeather(const WeatherFrameInput& input,const std::function<void(AreaObject&)>& initialize) {
    return weather_.advance(input,*services_.random,weatherCallbacks(initialize));
}
std::vector<ParticleScene*> AreaScene::particleSceneRoots() {
    std::vector<ParticleScene*> result;
    for(const auto& object:objects_)result.push_back(&object->particles());
    for(const auto& object:detachedWeather_)if(object)result.push_back(&object->particles());
    return result;
}
std::vector<const ParticleScene*> AreaScene::particleSceneRoots() const {
    std::vector<const ParticleScene*> result;
    for(const auto& object:objects_)result.push_back(&object->particles());
    for(const auto& object:detachedWeather_)if(object)result.push_back(&object->particles());
    return result;
}

void AreaScene::load(const std::filesystem::path& layout, const ResourceResolver& resolver,
                     AreaRuntimeServices services,const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,0,"Reading area layout "+neoshared::pathToUtf8(layout.filename()));
    const auto directory=layout.parent_path().empty()?std::filesystem::path("."):layout.parent_path();
    services.asciiResources=looseAsciiResources(directory);
    const auto scoped=directoryResources(directory,resolver);
    const auto resource=scoped(neoshared::pathToUtf8(layout.stem()),0xbb8);
    require(resource.has_value(),"missing layout: "+neoshared::pathToUtf8(layout));
    loadResource(*resource,scoped,services);
    path_=layout;
}

void AreaScene::loadResource(neoshared::ResourceDocument layout, const ResourceResolver& supplied,
                             AreaRuntimeServices services,const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,0,"Preparing area "+layout.fileName);
    require(layout.type==0xbb8,"expected a LYT resource");
    const std::string text(layout.bytes.begin(),layout.bytes.end());
    require(text.find('\0')==std::string::npos,"embedded NUL in layout");
    auto trace=std::make_shared<ResourceTrace>();
    trace->record(layout.identity,layout.fileName,layout.type,&layout);
    const auto resolver=traceResources(supplied,trace,layout.identity);
    require(bool(services.wind)==bool(services.random),"supply scene wind and random services together");
    RandomTransaction transaction(services.random);
    AreaScene pending;
    pending.worldState_ = worldState_?worldState_:std::make_shared<AreaWorldState>();
    pending.ownsWind_ = !services.wind&&!pending.worldState_->captured;
    if(!services.wind&&pending.worldState_->captured) {
        if(!services.game)services.game=pending.worldState_->game;
    }
    if(!services.wind) services.wind = &pending.worldState_->wind;
    if(!services.random) services.random = &pending.worldState_->random;
    if(!services.animation)services.animation=pending.worldState_->animation;
    pending.services_ = services;pending.resolver_=resolver;
    if(services.game)pending.game_=*services.game;
    pending.weather_.setGame(pending.game_);

    pending.environment_.setDefinition({});
    pending.identity_=layout.identity.empty()?"neomdl:layout:"+layout.fileName:layout.identity;
    pending.trace_=std::move(trace);
    if(resolver) {
        if(const auto surface=resolver("surfacemat",0x07e1u))
            pending.walkableMaterials_=engineWalkableMaterialMask(surface->bytes);
        else pending.diagnostics_.push_back(
            "surfacemat.2da is unavailable; automatic WalkCheck room queries have no material input.");
    }
    const auto input = lines(text);
    bool sawRooms = false, sawTracks = false;
    std::size_t nextInstance=0x90000000ull;
    std::unordered_map<std::string,std::shared_ptr<ModelDocument>> assets;
    const auto loadModel=[&](const std::string& name) {
        job.check();
        if(const auto found=assets.find(key(name));found!=assets.end()) return found->second;
        auto document=std::make_shared<ModelDocument>();
        {
            auto resource=resolver?resolver(name,kMdlResourceType):std::nullopt;
            require(resource.has_value(),"missing area model: "+name);
            document->openResource(std::move(*resource),resolver,job,services.game,services.asciiResources,true,false);
        }
        require(document->isOpen(),"failed to load area model: "+name);
        const auto game=document->model().game;
        require(engineAreaRuntimeSupported(game),
                "Unsupported area runtime: "+name);
        if(services.game)require(game==*services.game,"area model belongs to a different game: "+name);
        else {
            services.game=game;pending.services_.game=game;pending.game_=game;pending.weather_.setGame(game);
        }
        assets.emplace(key(name),document);return document;
    };
    const auto addObject=[&](AreaObjectKind kind,const std::string& name,
            std::shared_ptr<ModelDocument> document,const model::EngineFrame& frame,
            std::optional<std::size_t> room) {
        auto object=std::make_unique<AreaObject>(nextInstance,kind,name,std::move(document),frame,room,services);
        nextInstance+=1u+object->document()->references().size();
        auto* result=object.get();pending.objects_.push_back(std::move(object));return result;
    };
    std::unordered_map<std::string, LightRoomId> byName;
    for (std::size_t line = 0; line < input.size(); ++line) {
        job.check();
        std::istringstream header(input[line]);
        std::string tag;
        header >> tag;
        tag=key(tag); // SpawnRooms compares firstword with __stricmp.
        if (tag != "roomcount" && tag != "trackcount") continue;
        long long count = -1;
        require(static_cast<bool>(header >> count) && count >= 0, "invalid " + tag);
        require(static_cast<unsigned long long>(count)<=input.size()-line-1u,
                "truncated "+tag+" block");
        if(tag=="roomcount") { require(!sawRooms,"duplicate roomcount block"); sawRooms=true; }
        else { require(!sawTracks,"duplicate trackcount block"); sawTracks=true; }
        for (long long index = 0; index < count; ++index) {
            job.update(static_cast<std::size_t>(index),static_cast<std::size_t>(count),"Loading area "+tag);
            require(++line < input.size(), "truncated "+tag+" block");
            std::istringstream record(input[line]);
            AreaRoom room;
            require(static_cast<bool>(record >> room.name >> room.position.x >> room.position.y >> room.position.z) &&
                    finite(room.position), "invalid room entry");
            validResref(room.name);
            room.world = model::nodeTransform(room.position, {});
            const auto source=loadModel(room.name);
            if(tag=="trackcount") {
                // SpawnRooms creates track CAurObjects, not rooms/VIS entries,
                // and does not request an initial animation on these Gobs.
                addObject(AreaObjectKind::Track,room.name,source,{room.position,{}},{});
                continue;
            }
            room.id = static_cast<LightRoomId>(index) + 1u;
            require(byName.emplace(key(room.name),room.id).second, "duplicate room name: " + room.name);
            source->markRoomModel();
            if(const auto walkmesh=resolver?resolver(room.name,0x07e0u):std::nullopt)
                room.walkmesh=neoshared::bwm::readBwm(walkmesh->bytes,walkmesh->fileName,job);
            else pending.diagnostics_.push_back(room.name+
                ": engine WOK resource is unavailable; its room-position query has no triangles.");
            auto partition=partitionK2Room(source->model());
            room.document=ModelDocument::sceneView(source,std::move(partition.staticParts),":static-room");
            room.runtime=addObject(AreaObjectKind::StaticRoomParts,room.name,room.document,{room.position,{}},room.id);
            if(partition.animatedParts) {
                auto animated=ModelDocument::sceneView(source,std::move(*partition.animatedParts),":animated-room");
                addObject(AreaObjectKind::AnimatedRoom,room.name,std::move(animated),{room.position,{}},room.id);
            }
            const auto& asset=room.document->model();
            const auto collapsed=engineCollapsedHitPose(asset,room.pose(),room.runtime->frame());
            bool hasStaticAabb=false;
            // Both SpawnRoom implementations preserve these inverted engine
            // endpoints when no static AABB Part survives the collapse.
            room.bounds={{100000.0f,100000.0f,100000.0f},
                         {-100000.0f,-100000.0f,-100000.0f},true};
            for(const auto& mesh:asset.meshes) {
                job.check();
                if(mesh.vertices.empty() || mesh.animated) continue;
                require(mesh.nodeIndex<collapsed.size(),"invalid static room mesh node");
                hasStaticAabb|=asset.nodes[mesh.nodeIndex].aabb.has_value();
                if(mesh.backgroundGeometry) {
                    // K2 adds CAurRoom+0xb8; that field/branch is absent in K1.
                    room.background=asset.game==model::GameVersion::Kotor2;
                    continue;
                }
                const auto box=engineStaticHitBounds(mesh,collapsed[mesh.nodeIndex]);
                if(box.valid) { extend(room.bounds,box.minimum);extend(room.bounds,box.maximum); }
            }
            if(!hasStaticAabb)room.bounds={{100000.0f,100000.0f,100000.0f},
                                         {-100000.0f,-100000.0f,-100000.0f},true};
            room.meshBounds.resize(asset.meshes.size());
            auto hitGeometry=std::make_shared<HitRoomGeometry>();
            for(auto& part:engineStaticHitParts(asset,room.pose(),room.runtime->frame(),job)) {
                room.meshBounds[*asset.nodes[part.node].mesh]=part.worldBounds;
                if(asset.nodes[part.node].aabb)hitGeometry->aabb=std::move(part);
                else hitGeometry->meshes.push_back(std::move(part));
            }
            room.hitGeometry=std::move(hitGeometry);
            pending.rooms_.push_back(std::move(room));
        }
    }
    require(sawRooms, "missing roomcount block");
    const auto visibilityName = neoshared::pathToUtf8(std::filesystem::u8path(layout.fileName).stem());
    std::string visibility;
    if (resolver) {
        if (const auto resource = resolver(visibilityName,0x0bb9u)) {
            visibility.assign(resource->bytes.begin(),resource->bytes.end());
            require(visibility.find('\0') == std::string::npos, "embedded NUL in visibility resource");
        }
    }
    const auto visLines = lines(visibility);
    for (std::size_t line = 0; line < visLines.size(); ++line) {
        job.update(line,visLines.size(),"Reading area visibility");
        std::istringstream header(visLines[line]);
        std::string source;
        long long count = -1;
        require(static_cast<bool>(header >> source >> count) && count >= 0, "invalid visibility header");
        require(static_cast<unsigned long long>(count)<=visLines.size()-line-1u,
                "truncated visibility block");
        const auto first = byName.find(key(source));
        require(first != byName.end(), "visibility names unknown room: " + source);
        for (long long index = 0; index < count; ++index) {
            job.check();
            require(++line < visLines.size(), "truncated visibility block");
            std::istringstream record(visLines[line]);
            std::string destination;
            record >> destination;
            const auto second = byName.find(key(destination));
            require(second != byName.end(), "visibility names unknown room: " + destination);
            auto& forward = pending.rooms_[static_cast<std::size_t>(first->second-1u)].influencingRooms;
            if (std::find(forward.begin(),forward.end(),second->second) != forward.end()) continue;
            // Scene::SetVisibility adds both directions in insertion order.
            forward.push_back(second->second);
            pending.rooms_[static_cast<std::size_t>(second->second-1u)].influencingRooms.push_back(first->second);
        }
    }
    // Caller unregisters the old host before moving a successfully loaded replacement.
    require(ownedLightIds_.empty(), "load into a fresh host or clear registered state first");
    job.check();
    pending.bindAnimationOwners();
    *this = std::move(pending);
    transaction.committed=true;
}

void AreaScene::loadModuleArea(const std::string& area,const std::string& scope,
    const ResourceResolver& resolver,AreaLoadOptions options,AreaRuntimeServices services,
    const neoshared::bif::JobControl& job) {
    // The installed Aurora resource bridge rejects .lmt. Module loads do not
    // inherit a loose authoring-directory context from another scene.
    services.asciiResources={};
    ResourceLoadScope work(job);job.update(0,0,"Preparing module area "+area);
    require(ownedLightIds_.empty(),"load a module into a fresh host or clear registered state first");
    RandomTransaction transaction(services.random);
    const auto& selectedGame=services.game || !worldState_ || !worldState_->captured
        ? services.game : worldState_->game;
    require(!selectedGame||options.game==*selectedGame,"module game differs from the prepared scene");
    auto plan=readAreaLoad(area,scope,resolver,options);
    services.game=plan.game;
    AreaScene pending;pending.worldState_=worldState_;pending.loadResource(std::move(plan.layout),plan.resolver,services);
    pending.identity_=scope+"/area/"+plan.area;
    pending.trace_=plan.trace;
    pending.resolver_=plan.resolver;
    pending.visualSettings_=plan.settings;
    pending.diagnostics_.insert(pending.diagnostics_.end(),plan.diagnostics.begin(),plan.diagnostics.end());
    pending.environment_.setDefinition(plan.settings.environment,worldTimeRuntimeSupported());
    if(weatherRuntimeSupported(pending.game_))
        pending.weather_.configure(plan.settings.environment.weather,plan.settings.environment.weather.areaFlags,
                                  plan.settings.wind.value_or(0),pending.game_);
    // LoadArea creates a scene-owned gidy_sun object at the fixed engine
    // transform. Missing input remains an explicit diagnostic; no direction or
    // replacement light is fabricated from SunDiffuseColor.
    if(auto sun=plan.resolver("gidy_sun",kMdlResourceType)) {
        try {
            auto document=std::make_shared<ModelDocument>();document->openResource(std::move(*sun),plan.resolver,job,pending.game_,pending.services_.asciiResources,true,false);
            require(document->model().game==pending.game_,"gidy_sun belongs to a different game");
            pending.environmentLightPose_=model::bindPose(document->model(),job);
            pending.environmentLightDocument_=std::move(document);
            bool diffuse=false,ambient=false;
            for(const auto& node:pending.environmentLightDocument_->model().nodes) {
                const auto name=key(node.name);
                diffuse|=name=="gidy_sun_diff"&&node.light.has_value();
                ambient|=name=="gidy_sun_amb"&&node.light.has_value();
            }
            if(!diffuse||!ambient) pending.diagnostics_.push_back(
                "gidy_sun lacks one or both engine light nodes gidy_sun_diff/gidy_sun_amb; only present channels are used.");
        } catch(const neoshared::bif::JobCancelled&) {throw;
        } catch(const std::exception& e) {
            if(!options.allowPartialVisuals)throw;
            pending.diagnostics_.push_back(std::string("Engine global-light model gidy_sun could not be loaded: ")+e.what());
        }
    } else pending.diagnostics_.push_back(
        "Engine global-light model gidy_sun is unavailable; diffuse/global-ambient tuples are retained without a generated directional light.");
    std::size_t nextInstance=0x90000000ull;
    for(const auto& object:pending.objects_)nextInstance=std::max(nextInstance,
        object->instanceIndex()+1u+object->document()->references().size());
    std::map<std::string,std::shared_ptr<ModelDocument>> assets;
    const auto load=[&](const std::string& name) {
        job.check();
        if(const auto i=assets.find(key(name));i!=assets.end())return i->second;
        auto resource=plan.resolver(name,kMdlResourceType);
        require(bool(resource),"missing scoped object model: "+name);
        auto doc=std::make_shared<ModelDocument>();doc->openResource(std::move(*resource),plan.resolver,job,pending.game_,pending.services_.asciiResources,true,false);
        require(doc->model().game==pending.game_,"GIT object belongs to a different game: "+name);
        assets.emplace(key(name),doc);return doc;
    };
    std::size_t placementIndex=0;
    for(auto& placement:plan.placements) {
        job.update(placementIndex++,plan.placements.size(),"Loading "+placement.label);
        if(!placement.modelResref.empty()&&placement.visible)try {
            std::shared_ptr<ModelDocument> doc;
            if(placement.creatureAppearance) {
                doc=assembleCreatureAppearance(*placement.creatureAppearance,plan.resolver,":git:"+placement.label,job,pending.services_.asciiResources);
            } else {
                doc=load(placement.modelResref);
                if(!placement.texture.empty()||!placement.head.empty()) {
                    const auto head=placement.head.empty()?std::shared_ptr<ModelDocument>{}:load(placement.head);
                    doc=ModelDocument::visualView(doc,":git:"+placement.label,placement.texture,head,placement.headTexture);
                }
            }
            auto object=std::make_unique<AreaObject>(nextInstance,placement.kind,placement.label,doc,
                placement.frame,std::nullopt,pending.services_);
            if(placement.creatureState)
                object->initializeCreatureForceSight(placement.creatureState->alignment,placement.creatureState->race);
            auto authored=placement.authoredVisualState;
            std::string visualDiagnostic;
            if(!pending.prepareVisualState(*object,authored,visualDiagnostic))
                throw std::runtime_error(visualDiagnostic);
            if(!visualDiagnostic.empty())placement.diagnostics.push_back(visualDiagnostic);
            std::string visualError;
            if(!object->applyAuthoredVisualState(authored,visualError))
                throw std::runtime_error("Invalid authored visual state: "+visualError);
            placement.authoredVisualState=std::move(authored);
            placement.runtimeInstance=nextInstance;
            nextInstance+=1u+doc->references().size();
            pending.objects_.push_back(std::move(object));
            for(const auto& message:doc->messages())placement.diagnostics.push_back(message);
        } catch(const neoshared::bif::JobCancelled&) {throw;
        } catch(const std::exception& e) {
            if(!options.allowPartialVisuals)throw;
            placement.diagnostics.push_back(e.what());
        }
        for(const auto& message:placement.diagnostics)pending.diagnostics_.push_back(placement.label+": "+message);
    }
    // The engine trigger client registers mines as ordinary model owners;
    // non-mine footprints are installed separately into TriggerScene.
    for(auto& trigger:plan.triggers)if(trigger.type==2) {
        job.update(0,0,"Loading "+trigger.label);
        try {
            auto document=load(trigger.modelResref);
            const auto points=engineTriggerClientFootprint(trigger.worldFootprint);
            auto minimum=points.front(),maximum=minimum;
            for(std::size_t i=1;i<points.size();++i) {
                minimum.x=std::min(minimum.x,points[i].x);maximum.x=std::max(maximum.x,points[i].x);
                minimum.y=std::min(minimum.y,points[i].y);maximum.y=std::max(maximum.y,points[i].y);
                minimum.z=std::min(minimum.z,points[i].z);maximum.z=std::max(maximum.z,points[i].z);
            }
            // CSWCTrigger::UpdatePosition K1 0xcefc0 / K2 Desktop 0x1e8230.
            const model::Vec3 center{minimum.x+(maximum.x-minimum.x)*0.5f,
                minimum.y+(maximum.y-minimum.y)*0.5f,minimum.z+(maximum.z-minimum.z)*0.5f};
            auto object=std::make_unique<AreaObject>(nextInstance,AreaObjectKind::Trigger,
                trigger.label,document,model::EngineFrame{center,{}},std::nullopt,pending.services_);
            // Server trigger constructor initializes 10143 (K1 0x294b16,
            // K2 Desktop 0x44b0ac); the checked client map selects animations[347].
            ObjectVisualStateLayer authored;authored.animation=triggerServerAnimation(10143u);
            std::string diagnostic;
            if(!pending.prepareVisualState(*object,authored,diagnostic))
                throw std::runtime_error(diagnostic);
            if(!diagnostic.empty())trigger.diagnostics.push_back(diagnostic);
            std::string error;
            if(!object->applyAuthoredVisualState(authored,error))throw std::runtime_error(error);
            trigger.runtimeInstance=nextInstance;
            nextInstance+=1u+document->references().size();
            pending.objects_.push_back(std::move(object));
            for(const auto& message:document->messages())trigger.diagnostics.push_back(message);
        } catch(const neoshared::bif::JobCancelled&) {throw;
        } catch(const std::exception& error) {
            if(!options.allowPartialVisuals)throw;
            trigger.diagnostics.push_back(error.what());
        }
        for(const auto& message:trigger.diagnostics)pending.diagnostics_.push_back(trigger.label+": "+message);
    }
    pending.miniGame_=std::move(plan.miniGame);
    pending.services_.animation->setInterleaved(!pending.miniGame_.has_value());
    if(pending.miniGame_)for(std::size_t i=0;i<pending.miniGame_->followers.size();++i) {
        const auto& definition=pending.miniGame_->followers[i];
        const auto label=definition.kind==TrackFollowerKind::Player?std::string("MiniGame Player"):
            "MiniGame Enemy "+std::to_string(definition.sourceIndex);
        job.update(i,pending.miniGame_->followers.size(),"Loading "+label);
        try {
            TrackFollowerInstance follower;follower.definition=i;
            TrackAnimationState animation;animation.kind=definition.kind;animation.mode=definition.loops;
            std::vector<std::unique_ptr<AreaObject>> created;
            const auto create=[&](const std::string& name,AreaObjectKind kind,const model::EngineFrame& frame) {
                const auto document=load(name);
                const auto instance=nextInstance;nextInstance+=1u+document->references().size();
                created.push_back(std::make_unique<AreaObject>(instance,kind,label+": "+name,
                    document,frame,std::nullopt,pending.services_));
                return created.back().get();
            };
            const auto mode=definition.kind==TrackFollowerKind::Player?
                static_cast<std::uint8_t>(pending.miniGame_->type):std::uint8_t{};
            for(const auto& entry:definition.models) {
                const auto* model=create(entry.model,AreaObjectKind::TrackModel,model::EngineFrame{});
                follower.models.push_back({model->instanceIndex(),entry.rotating?mode:std::uint8_t{}, {}, {}});
                animation.models.push_back(model->instanceIndex());
            }
            // GetProtoTrack selects the first matching LYT record. A missing
            // prototype leaves NewCAurObject's zero position unchanged.
            model::Vec3 position{};
            for(const auto& prototype:pending.objects_)
                if(prototype->kind()==AreaObjectKind::Track&&key(prototype->label())==key(definition.track)) {
                    position=prototype->frame().position;break;
                }
            const auto* track=create(definition.track,AreaObjectKind::Track,{position,{}});
            follower.track=track->instanceIndex();
            const auto& nodes=track->document()->model().nodes;
            const auto hook=std::find_if(nodes.begin(),nodes.end(),[](const auto& node){return key(node.name)=="modelhook";});
            // ForceSetTrack dereferences this Part unconditionally. Report an
            // unavailable engine input instead of substituting a root frame.
            require(hook!=nodes.end(),"MiniGame track has no engine modelhook: "+definition.track);
            follower.hook=static_cast<std::size_t>(hook-nodes.begin());
            if(!definition.camera.empty()) {
                const auto* camera=create(definition.camera,AreaObjectKind::TrackModel,model::EngineFrame{});
                follower.camera=TrackModelAttachment{camera->instanceIndex(),definition.cameraRotating?mode:std::uint8_t{}, {}, {}};
                animation.playerModel=camera->instanceIndex();
            }
            for(auto& owner:created)pending.objects_.push_back(std::move(owner));
            pending.captureTrackAnimation(follower.track,std::move(animation));
            pending.synchronizeTrackFollower(follower);
            follower.initialHook=model::enginePartWorld(track->document()->model(),track->pose(),track->frame(),follower.hook).position;
            if(definition.kind==TrackFollowerKind::Player)
                pending.trackAnimationCallbacks_[follower.track].push_back({follower.track,true});
            pending.trackFollowers_.push_back(std::move(follower));
        } catch(const neoshared::bif::JobCancelled&) {throw;
        } catch(const std::exception& error) {
            if(!options.allowPartialVisuals)throw;
            pending.diagnostics_.push_back(label+": "+error.what());
        }
    }
    pending.placements_=std::move(plan.placements);
    pending.triggers_=std::move(plan.triggers);
    // Client mine metadata is shared by both games/platforms. Prepare it in
    // the loading job so profile changes never perform resource reads.
    pending.triggerRuntime_.load(pending.triggers_,pending.resolver_,pending.game_,
            [&](std::size_t source,const std::string& name) {
                const auto record=std::find_if(pending.triggers_.begin(),pending.triggers_.end(),
                    [&](const auto& trigger){return trigger.sourceIndex==source;});
                const auto& document=*pending.object(*record->runtimeInstance)->document();
                const auto animation=document.findAnimation(name);
                return animation?document.modelAt(animation->modelIndex).animations[animation->animationIndex].length:0.0f;
            });
    std::vector<AreaObjectTrapRecord> objectTraps;
    for(const auto& placement:pending.placements_)if(placement.trap)
        objectTraps.push_back(*placement.trap);
    pending.triggerRuntime_.loadObjectTraps(objectTraps,pending.resolver_);
    pending.cameras_=std::move(plan.cameras);
    pending.sounds_=std::move(plan.sounds);
    if(plan.grass)try {pending.configureGrass(*plan.grass,false);}
        catch(const neoshared::bif::JobCancelled&){throw;} catch(const std::exception& e){if(!options.allowPartialVisuals)throw;pending.diagnostics_.push_back(e.what());}
    pending.synchronizeObjectTargets();pending.bindAnimationOwners();
    // Wind settings are committed with the area, not during resource decoding.
    // The scene coordinator applies them after replacing the previous host.
    job.check();*this=std::move(pending);
    if(ownsWind_&&visualSettings_.wind)wind().applyK2AreaWind(*visualSettings_.wind);
    transaction.committed=true;
}
std::vector<model::Vec3> AreaScene::newMineFootprint(model::Vec3 position) const {
    require(walkableMaterials_.has_value(),"mine placement requires surfacemat WalkCheck input");
    std::vector<model::Vec3> points{{position.x-2.0f,2.0f+position.y,0.0f},
        {position.x-2.0f,position.y-2.0f,0.0f},{2.0f+position.x,position.y-2.0f,0.0f},
        {2.0f+position.x,2.0f+position.y,0.0f}};
    for(auto& point:points) {
        RoomSurfaceHitInfo hit;
        const model::Vec3 start{point.x,point.y,point.z+1000.0f};
        const model::Vec3 end{point.x,point.y,point.z-1000.0f};
        for(const auto& entry:rooms_)if(entry.walkmesh&&
            engineRoomHitWalkable(*entry.walkmesh,start,end,hit,*walkableMaterials_)) {
            point.z=engineRoomFaceHeight(*entry.walkmesh,hit.face,point);break;
        }
        // Engine CreateNewGeometry substitutes the actor height only when
        // ComputeHeight returns exactly zero, including an absent room.
        if(point.z==0.0f)point.z=position.z;
    }
    return points;
}
PreparedTriggerMine AreaScene::prepareTriggerMine(AreaTriggerRecord record,
    TriggerMineState constructorState,const ResourceResolver& resolver,model::GameVersion game,
    const neoshared::bif::JobControl& job,const model::AsciiResourceResolver& asciiResources) {
    ResourceLoadScope work(job);job.update(0,0,"Preparing placed mine");
    detail::AreaLoadTables tables{resolver,{}};
    record.modelResref=resourceKey(tables.lookup("traps",record.trapType,"Model").value_or("v_mnfrag"));
    auto resource=resolver(record.modelResref,kMdlResourceType);
    require(bool(resource),"missing scoped mine model: "+record.modelResref);
    auto document=std::make_shared<ModelDocument>();document->openResource(std::move(*resource),resolver,job,game,asciiResources,true,false);
    require(document->model().game==game,"mine model belongs to a different game");
    auto metadata=TriggerRuntime::prepareMine(record,resolver,game,
        [&](std::size_t,const std::string& name) {
            const auto animation=document->findAnimation(name);
            return animation?document->modelAt(animation->modelIndex).animations[animation->animationIndex].length:0.0f;
        });
    // SetMine supplied the server state before AddToArea. Resource preparation
    // supplies only the client/radius metadata, preserving its computed DCs,
    // creator and selected script instead of reloading the UTT/DC defaults.
    constructorState.sourceIndex=record.sourceIndex;
    constructorState.objectId=record.objectId;constructorState.position=record.position;
    constructorState.clientPosition=metadata.clientPosition;
    constructorState.nearestPointRadius=metadata.nearestPointRadius;
    constructorState.explosionSound=std::move(metadata.explosionSound);
    constructorState.recoveredItemResref=std::move(metadata.recoveredItemResref);
    constructorState.animations=std::move(metadata.animations);
    PreparedTriggerMine result;result.game=game;result.record=std::move(record);
    result.runtime=std::move(constructorState);result.document=std::move(document);
    result.authored.animation=triggerServerAnimation(10143u);
    std::string diagnostic;
    resolveObjectAnimation(*result.authored.animation,resolver,*result.document,diagnostic);
    if(!diagnostic.empty())result.record.diagnostics.push_back(std::move(diagnostic));
    job.check();return result;
}
std::size_t AreaScene::addTriggerMine(PreparedTriggerMine prepared) {
    require(prepared.game==game_,"prepared mine belongs to a different game");
    RandomTransaction transaction(services_.random);
    std::size_t instance=0x90000000ull;
    for(const auto& owner:objects_)instance=std::max(instance,
        owner->instanceIndex()+1u+owner->document()->references().size());
    for(const auto& owner:detachedWeather_)if(owner)instance=std::max(instance,
        owner->instanceIndex()+1u+owner->document()->references().size());
    const auto source=triggers_.size();
    prepared.record.sourceIndex=source;prepared.record.runtimeInstance=instance;
    prepared.runtime.sourceIndex=source;
    auto owner=std::make_unique<AreaObject>(instance,AreaObjectKind::Trigger,
        prepared.record.label,prepared.document,model::EngineFrame{prepared.runtime.clientPosition,{}},
        std::nullopt,services_);
    std::string error;
    if(!owner->applyAuthoredVisualState(prepared.authored,error))throw std::runtime_error(error);
    for(const auto& message:prepared.record.diagnostics)
        diagnostics_.push_back(prepared.record.label+": "+message);
    objects_.push_back(std::move(owner));triggers_.push_back(std::move(prepared.record));
    triggerRuntime_.appendMine(std::move(prepared.runtime),game_);
    synchronizeObjectTargets();transaction.committed=true;return source;
}
void AreaScene::dispatchTriggerEvents(const TriggerPreDestroy& preDestroy) {
    for(auto& event:triggerRuntime_.takeEvents()) {
        const auto record=std::find_if(triggers_.begin(),triggers_.end(),
            [&](const auto& trigger){return trigger.sourceIndex==event.sourceIndex;});
        if(record!=triggers_.end() && record->runtimeInstance) {
            if(event.kind==TriggerRuntimeEvent::Kind::Animation) {
                object(*record->runtimeInstance)->play(event.resource,1.0f,
                    event.nonLooping?model::kAnimationRunNonLooping:0u,0.0f);
            } else if(event.kind==TriggerRuntimeEvent::Kind::MoveMine) {
                auto frame=object(*record->runtimeInstance)->frame();frame.position=event.position;
                object(*record->runtimeInstance)->setFrame(frame);
            } else if(event.kind==TriggerRuntimeEvent::Kind::Destroy) {
                const auto owner=std::find_if(objects_.begin(),objects_.end(),
                    [&](const auto& candidate){return candidate->instanceIndex()==*record->runtimeInstance;});
                preDestroy(**owner);
                const auto instance=(*owner)->instanceIndex();
                releaseWorldAnimationOwner(instance);
                objects_.erase(owner);record->runtimeInstance.reset();
                synchronizeObjectTargets();
            }
        }
        triggerEvents_.push_back(std::move(event));
    }
}
void AreaScene::updateTriggerObserver(const std::optional<TriggerObserver>& observer,
    const TriggerReputation& reputation) {
    triggerRuntime_.updateObserver(observer,reputation);dispatchTriggerEvents({});
}
TriggerDetectionResult AreaScene::checkTriggerDetection(const TriggerDetectionInput& input) {
    return triggerRuntime_.checkDetection(input);
}
bool AreaScene::updateTriggerMineObserver(std::size_t source,const std::optional<TriggerObserver>& observer,
    std::int32_t reputation) {
    const bool accepted=triggerRuntime_.updateMineObserver(source,observer,reputation);
    dispatchTriggerEvents({});return accepted;
}
std::vector<TriggerMineRequest> AreaScene::enterTriggerMine(std::size_t source,
    const TriggerMineEnterInput& input) const {
    return triggerRuntime_.enterMine(source,input);
}
bool AreaScene::enterTriggerMineFromInventory(std::size_t source,const TriggerMineEnterInput& input,
    const TriggerRuntime::RequestSink& sink) {
    return triggerRuntime_.enterMineFromInventory(source,input,sink);
}
bool AreaScene::flagObjectTrap(TrapObjectTarget target,std::uint8_t value) {
    return triggerRuntime_.flagObjectTrap(target,value);
}
bool AreaScene::setObjectTrapId(TrapObjectTarget target,std::uint64_t identity) {
    return triggerRuntime_.setObjectTrapId(target,identity);
}
bool AreaScene::flagTriggerMine(std::size_t source,bool value) {
    return triggerRuntime_.flagMine(source,value);
}
bool AreaScene::setTriggerCreator(std::size_t source,std::uint64_t creator) {
    return triggerRuntime_.setCreator(source,creator);
}
bool AreaScene::setTriggerObjectId(std::size_t source,std::uint64_t identity) {
    return triggerRuntime_.setObjectId(source,identity);
}
bool AreaScene::deleteTrigger(std::size_t source,std::uint8_t reason,
    const TriggerPreDestroy& preDestroy) {
    const bool accepted=triggerRuntime_.deleteTrigger(source,reason);
    dispatchTriggerEvents(preDestroy);return accepted;
}
void AreaScene::advanceTriggers(std::uint32_t elapsed,const TriggerPreDestroy& preDestroy) {
    triggerRuntime_.advanceServer(elapsed);
    triggerRuntime_.advanceClient(elapsed);dispatchTriggerEvents(preDestroy);
}
std::vector<TriggerRuntimeEvent> AreaScene::takeTriggerEvents() {
    std::vector<TriggerRuntimeEvent> events;events.swap(triggerEvents_);return events;
}
bool AreaScene::prepareVisualState(AreaObject& object,ObjectVisualStateLayer& state,
                                   std::string& diagnostic) {
    diagnostic.clear();
    std::string validation;
    if(!validObjectVisualStateLayer(state,validation)) {
        diagnostic=validation;return false;
    }
    if(state.emitterTargets) for(const auto& route:*state.emitterTargets) {
        if(route.sourceReference && *route.sourceReference>=object.document()->references().size()) {
            diagnostic="Emitter target source reference is outside the owning model";return false;
        }
    }
    if(state.animation && !state.animation->resolved()) {
        std::string resolved;
        if(!resolveObjectAnimation(*state.animation,resolver_,*object.document(),resolved)) {
            state.animation->diagnostic=resolved;
            diagnostic=resolved;
        } else if(!resolved.empty()) diagnostic=resolved;
    }
    const auto loadTexture=[&](const std::string& name,const char* purpose) {
        if(name.empty())return true;
        std::string error;
        if(object.document()->loadRuntimeTexture(name,resolver_,error))return true;
        diagnostic=std::string(purpose)+": "+error;return false;
    };
    if(state.environmentMap && !state.environmentMap->empty() &&
       !loadTexture(*state.environmentMap,"Object environment map"))return false;
    if(state.hologram.value_or(false) && !loadTexture("holotex","Hologram filter"))return false;
    return true;
}
bool AreaScene::prepareVisualEvent(AreaObject& object,ObjectVisualEvent& event,
                                   std::string& diagnostic) {
    diagnostic.clear();
    if(event.kind==ObjectVisualEventKind::Animation) {
        if(!event.animation) {diagnostic="Animation event has no numeric engine command";return false;}
        if(!event.animation->resolved()) {
            std::string resolved;
            if(!resolveObjectAnimation(*event.animation,resolver_,*object.document(),resolved)) {
                diagnostic=resolved;return false;
            }
            diagnostic=resolved;
        }
    }
    if(event.kind==ObjectVisualEventKind::SetEnvironmentMap && !event.text.empty()) {
        std::string error;
        if(!object.document()->loadRuntimeTexture(event.text,resolver_,error)) {
            diagnostic="Object environment map: "+error;return false;
        }
    }
    if(event.kind==ObjectVisualEventKind::SetHologram && event.enabled) {
        std::string error;
        if(!object.document()->loadRuntimeTexture("holotex",resolver_,error)) {
            diagnostic="Hologram filter: "+error;return false;
        }
    }
    if(event.kind==ObjectVisualEventKind::SetEmitterTarget && event.target &&
       event.target->sourceReference &&
       *event.target->sourceReference>=object.document()->references().size()) {
        diagnostic="Emitter target source reference is outside the owning model";return false;
    }
    return true;
}
bool AreaScene::applyObjectVisualState(std::size_t instance,ObjectVisualStateLayer state,
                                       std::string& error) {
    error.clear();auto* runtime=object(instance);
    if(!runtime) {error="Unknown area object instance";return false;}
    if(instance!=runtime->instanceIndex()) {error="Visual state must address the owning area object, not one of its references";return false;}
    std::string diagnostic;
    if(!prepareVisualState(*runtime,state,diagnostic)) {
        error=diagnostic;runtime->noteVisualDiagnosticOnce("live-state:"+diagnostic,
            ObjectVisualStateSource::LiveAdapter,"prepare live visual state",diagnostic,false);return false;
    }
    if(!runtime->applyLiveVisualState(state,error))return false;
    if(!diagnostic.empty())runtime->noteVisualDiagnosticOnce("live-state-warning:"+diagnostic,
        ObjectVisualStateSource::LiveAdapter,"prepare live visual state",diagnostic,true);
    synchronizeObjectTargets();return true;
}
bool AreaScene::applyPreparedObjectVisualState(AreaObject& owner,const ObjectVisualStateLayer& state,
                                               std::string& error) {
    if(!owner.applyLiveVisualState(state,error))return false;
    synchronizeObjectTargets();return true;
}
bool AreaScene::dispatchObjectVisualEvent(std::size_t instance,ObjectVisualEvent event,
                                          std::string& error) {
    error.clear();auto* runtime=object(instance);
    if(!runtime) {error="Unknown area object instance";return false;}
    if(instance!=runtime->instanceIndex()) {error="Visual events must address the owning area object, not one of its references";return false;}
    std::string diagnostic;
    if(!prepareVisualEvent(*runtime,event,diagnostic)) {
        error=diagnostic;runtime->noteVisualDiagnosticOnce(std::string("event:")+objectVisualEventName(event.kind)+":"+diagnostic,
            ObjectVisualStateSource::LiveAdapter,objectVisualEventName(event.kind),diagnostic,false);return false;
    }
    if(!runtime->dispatchVisualEvent(event,error))return false;
    if(!diagnostic.empty())runtime->noteVisualDiagnosticOnce(std::string("event-warning:")+objectVisualEventName(event.kind)+":"+diagnostic,
        ObjectVisualStateSource::LiveAdapter,objectVisualEventName(event.kind),diagnostic,true);
    synchronizeObjectTargets();return true;
}
bool AreaScene::clearObjectVisualState(std::size_t instance,std::string& error) {
    error.clear();auto* runtime=object(instance);
    if(!runtime) {error="Unknown area object instance";return false;}
    if(instance!=runtime->instanceIndex()) {error="Visual state must address the owning area object, not one of its references";return false;}
    runtime->clearLiveVisualState();synchronizeObjectTargets();return true;
}
std::optional<std::size_t> AreaScene::applyCreatureAppearance(std::size_t instance,
    const CreatureAppearanceState& state,std::string& error) {
    error.clear();
    const auto placement=std::find_if(placements_.begin(),placements_.end(),[&](const auto& p){return p.runtimeInstance==instance;});
    const auto owner=std::find_if(objects_.begin(),objects_.end(),[&](const auto& p){return p->instanceIndex()==instance;});
    if(placement==placements_.end()||owner==objects_.end()||!placement->creatureState||!placement->creatureAppearance) {
        error="Selected owner is not an loaded creature";return {};
    }
    try {
        const bool fieldsChanged=creatureAppearanceChangesGeometry(*placement->creatureState,state);
        auto appearance=resolveCreatureAppearance(state,resolver_,fieldsChanged?nullptr:&*placement->creatureAppearance);
        const bool replace=fieldsChanged||appearance.appearance!=placement->creatureAppearance->appearance;
        const auto updatePlacement=[&](const CreatureAppearance& next) {
            placement->creatureState=state;placement->creatureAppearance=next;
            placement->appearance=next.appearance;placement->modelResref=next.bodyModel;placement->texture=next.bodyTexture;
            placement->head.clear();placement->headTexture.clear();
            for(const auto& attachment:next.attachments)if(attachment.kind==VisualAttachmentKind::CreatureHead) {
                placement->head=attachment.model;placement->headTexture=attachment.texture;break;
            }
        };
        if(!replace) {
            std::string headTexture;
            for(const auto& attachment:appearance.attachments)if(attachment.kind==VisualAttachmentKind::CreatureHead)
                headTexture=attachment.texture;
            (*owner)->document()->applyVisualTextures(appearance.bodyTexture,headTexture);
            (*owner)->appearanceBindingsChanged();updatePlacement(appearance);return instance;
        }
        RandomTransaction transaction(services_.random);
        // Host identities are allocation state, not the game's object IDs.
        // Allocate after existing ranges so new references cannot alias owners.
        std::size_t nextInstance=0x90000000ull;
        for(const auto& object:objects_)nextInstance=std::max(nextInstance,
            object->instanceIndex()+1u+object->document()->references().size());
        auto document=assembleCreatureAppearance(appearance,resolver_,":appearance:"+placement->label+":"+std::to_string(nextInstance),resourceLoadJob(),services_.asciiResources);
        auto replacement=std::make_unique<AreaObject>(nextInstance,AreaObjectKind::Creature,placement->label,
            std::move(document),(*owner)->frame(),(*owner)->room(),services_);
        replacement->initializeCreatureForceSight(state.alignment,state.race);
        auto authored=(*owner)->visualState().authored();auto live=(*owner)->visualState().live();
        std::string diagnostic;
        if(!prepareVisualState(*replacement,authored,diagnostic)||
           !replacement->applyAuthoredVisualState(authored,error))throw std::runtime_error(error.empty()?diagnostic:error);
        if(!prepareVisualState(*replacement,live,diagnostic)||
           !replacement->applyLiveVisualState(live,error))throw std::runtime_error(error.empty()?diagnostic:error);



        if(const auto& loop=(*owner)->creatureLoopAnimation())
            replacement->play(loop->name,loop->playbackRate,0u,0.0f);
        replacement->forceAnimateCreatureAppearance();
        replacement->setPaused((*owner)->paused());
        releaseWorldAnimationOwner(instance);
        *owner=std::move(replacement);placement->runtimeInstance=nextInstance;updatePlacement(appearance);
        synchronizeObjectTargets();transaction.committed=true;return nextInstance;
    } catch(const neoshared::bif::JobCancelled&) {throw;
    } catch(const std::exception& exception) {error=exception.what();return {};}
}
void AreaScene::synchronizeObjectTargets() {
    const auto ownerView=[&](std::uint64_t id)->std::optional<VisualTargetOwner> {
        for(const auto& candidate:objects_) {
            if(candidate->ownerId()==id)
                return VisualTargetOwner{&candidate->document()->model(),&candidate->pose(),candidate->frame()};
            const auto& references=candidate->references().instances();
            for(std::size_t index=0;index<references.size();++index) if(candidate->ownerId(index)==id) {
                const auto& runtime=references[index];
                if(!runtime.active||index>=candidate->document()->references().size())return std::nullopt;
                return VisualTargetOwner{&candidate->document()->references()[index].model,&runtime.pose,runtime.frame};
            }
        }
        return std::nullopt;
    };
    for(auto& source:objects_) {
        std::vector<ParticleEmitterTargetInput> resolved;
        const auto& routes=source->effectiveVisualState().emitterTargets;
        resolved.reserve(routes.size());
        for(std::size_t routeIndex=0;routeIndex<routes.size();++routeIndex) {
            const auto& route=routes[routeIndex];
            model::EngineFrame target{};
            std::string failure;
            if(route.explicitWorld) target=engineRigidFrame(*route.explicitWorld);
            else if(!route.targetOwner) failure="target route has neither an owner nor an explicit transform";
            else if(const auto view=ownerView(*route.targetOwner)) {
                target=view->frame;
                if(!route.targetNode.empty()) {
                    std::optional<std::size_t> node;
                    for(std::size_t index=0;index<view->asset->nodes.size();++index)
                        if(key(view->asset->nodes[index].name)==key(route.targetNode)) {
                            if(node) {failure="target node name is ambiguous";break;}
                            node=index;
                        }
                    if(failure.empty()&&!node)failure="target node is absent from the selected owner";
                    if(failure.empty()&&*node>=view->pose->worldTransforms.size())
                        failure="target node pose is incomplete";
                    if(failure.empty()) {
                        target=model::enginePartWorld(*view->asset,*view->pose,view->frame,*node);
                    }
                }
            } else failure="target owner is not active in this area scene";
            if(!failure.empty()) {
                std::ostringstream id;id<<"target:"<<routeIndex<<':'
                    <<(route.targetOwner?std::to_string(*route.targetOwner):std::string("explicit"))<<':'
                    <<key(route.targetNode)<<':'<<failure;
                source->noteVisualDiagnosticOnce(id.str(),ObjectVisualStateSource::LiveAdapter,
                    "resolve emitter target",failure,false);
                continue;
            }
            ParticleEmitterTargetInput input;input.emitterName=route.emitterName;
            input.allSourceOwners=route.allSourceOwners;input.sourceReference=route.sourceReference;
            input.frame=target;input.target=model::nodeTransform(target.position,target.orientation);
            resolved.push_back(std::move(input));
        }
        std::string error;
        if(!source->setResolvedParticleTargets(std::move(resolved),error))
            source->noteVisualDiagnosticOnce("resolved-targets:"+error,ObjectVisualStateSource::LiveAdapter,
                "resolve emitter targets",error,false);
    }
}

std::vector<ResourceDependency> AreaScene::dependencies() const {
    std::vector<ResourceDependency> result;
    std::set<std::tuple<std::string,std::string,std::uint16_t,DependencyStatus,std::string,std::string>> seen;
    const auto append=[&](std::vector<ResourceDependency> entries) {
        for(auto& d:entries)if(seen.emplace(d.requester,d.resref,d.type,d.status,d.identity,d.error).second)
            result.push_back(std::move(d));
    };
    if(trace_)append(trace_->entries());
    std::set<const ModelDocument*> documents;
    for(const auto& object:objects_)if(documents.insert(object->document().get()).second)
        append(object->document()->dependencies());
    for(const auto& entry:weatherResources_.models)if(documents.insert(entry.second.get()).second)
        append(entry.second->dependencies());
    if(environmentLightDocument_&&documents.insert(environmentLightDocument_.get()).second)
        append(environmentLightDocument_->dependencies());
    return result;
}
std::vector<std::filesystem::path> AreaScene::openPaths() const {
    std::vector<std::filesystem::path> result;
    for(const auto& d:dependencies())for(const auto& p:d.inputs)
        if(std::find(result.begin(),result.end(),p)==result.end())result.push_back(p);
    return result;
}

const AreaRoom* AreaScene::room(LightRoomId id) const noexcept {
    const auto found = std::find_if(rooms_.begin(),rooms_.end(),[id](const auto& r){return r.id==id;});
    return found == rooms_.end() ? nullptr : &*found;
}
std::optional<LightRoomId> AreaScene::findRoomAt(model::Vec3 position,bool allSurfaces) const {
    require(allSurfaces||walkableMaterials_.has_value(),
            "room-position query requires surfacemat WalkCheck input");
    const auto mask=allSurfaces?0xffffffffu:*walkableMaterials_;
    for(const auto& entry:rooms_)
        if(entry.walkmesh&&engineRoomContainsPoint(*entry.walkmesh,position,mask))return entry.id;
    return {};
}
std::optional<model::Vec3> AreaScene::walkmeshHit(model::Vec3 start,model::Vec3 end) const {
    const auto materials=walkableMaterials_.value();
    RoomSurfaceHitInfo hit;bool found=false;
    for(const auto& room:rooms_)if(room.walkmesh) {
        hit.clearOutput();
        if(engineRoomHitWalkable(*room.walkmesh,start,end,hit,materials)) {end=hit.position;found=true;}
    }
    return found?std::optional<model::Vec3>{end}:std::nullopt;
}
std::optional<model::Vec3> AreaScene::nearestWalkmeshPoint(model::Vec3 position) const {
    const auto materials=walkableMaterials_.value();
    float distance=std::numeric_limits<float>::max();std::optional<model::Vec3> result;
    for(const auto& room:rooms_)if(room.walkmesh) {
        const auto& mesh=*room.walkmesh;std::vector<std::int32_t> pending;
        if(!mesh.nodes.empty())pending.push_back(mesh.root);
        while(!pending.empty()) {
            const auto& node=mesh.nodes.at(pending.back());pending.pop_back();
            if(position.x<node.minimum[0]||position.x>node.maximum[0]||position.y<node.minimum[1]||position.y>node.maximum[1])continue;
            if(node.face==-1) {pending.push_back(node.firstChild);pending.push_back(node.secondChild);continue;}
            const auto& face=mesh.faces.at(node.face);
            if(!(materials&(1u<<(face.material&31u))))continue;
            const auto z=engineRoomFaceHeight(mesh,node.face,position);
            std::array<model::Vec3,3> triangle;
            for(std::size_t i=0;i<3;++i) {
                const auto& vertex=mesh.vertices.at(face.vertices[i]);triangle[i]={vertex[0],vertex[1],vertex[2]};
            }
            model::Vec3 end{position.x,position.y,z-1.0f};
            if(engineHitTriangle(triangle,{position.x,position.y,z+1.0f},end)&&std::abs(end.z-position.z)<distance) {
                distance=std::abs(end.z-position.z);result=end;
            }
        }
    }
    return result;
}
void AreaScene::synchronizeCurrentRoom() {
    if(currentRoomOverride_)return;
    const auto replay=environment_.replayCurrentRoom();
    currentRoom_=replay?replay:automaticCurrentRoom_;
}
void AreaScene::updateCurrentRoomFromPosition(model::Vec3 position,bool allSurfaces) {
    automaticCurrentRoom_=findRoomAt(position,allSurfaces);
    synchronizeCurrentRoom();
}
void AreaScene::updateCurrentRoomFromWorld(const WorldRoomInputs& input) {
    if(ignoreVisibility_)return;
    const auto position=engineWorldRoomPosition(input);
    if(!position)return;
    const auto found=findRoomAt(*position);
    if(!found)return;
    automaticCurrentRoom_=found;
    synchronizeCurrentRoom();
}
void AreaScene::setCurrentRoom(std::optional<LightRoomId> id) {
    require(!id || room(*id), "unknown current room");
    currentRoomOverride_=true;currentRoom_=id;
}
void AreaScene::useReplayedCurrentRoom() {
    currentRoomOverride_=false;
    const auto id=environment_.replayCurrentRoom();
    require(!id||room(*id),"environment replay references an unknown current room");
    synchronizeCurrentRoom();
}
bool AreaScene::applyEnvironmentReplay(AreaEnvironmentReplayState state,std::string& error) {
    error.clear();
    if(state.currentRoom&&!room(*state.currentRoom)) {
        error="Area environment replay references an unknown current room";return false;
    }
    if(state.camera&&state.camera->active&&state.camera->currentRoom&&
       !room(*state.camera->currentRoom)) {
        error="Area replay camera references an unknown current room";return false;
    }
    if(!environment_.applyReplay(std::move(state),error))return false;
    synchronizeCurrentRoom();
    return true;
}
void AreaScene::clearEnvironmentReplay() {
    environment_.clearReplay();
    synchronizeCurrentRoom();
}
void AreaScene::advanceWorldTime(EngineWorldTime current,float clientSeconds,bool paused) {
    environment_.advanceWorldTime(current,clientSeconds,paused);
    // synchronize() consumes the new diffuse/ambient tuple when it rebuilds
    // the actual gidy_sun PartLights for this frame.
}
void AreaScene::resetWorldTime() {
    environment_.resetWorldTime();
}
model::Bounds AreaScene::bounds() const noexcept {
    model::Bounds result;
    const auto point=[&](model::Vec3 p) {
        if(!result.valid) {result.minimum=result.maximum=p;result.valid=true;return;}
        result.minimum={std::min(result.minimum.x,p.x),std::min(result.minimum.y,p.y),std::min(result.minimum.z,p.z)};
        result.maximum={std::max(result.maximum.x,p.x),std::max(result.maximum.y,p.y),std::max(result.maximum.z,p.z)};
    };
    const auto box=[&](const model::Bounds& b,const model::Mat4& w) {
        if(b.valid) for(unsigned bits=0;bits<8;++bits)
            point(model::transformPoint(w,{bits&1?b.maximum.x:b.minimum.x,bits&2?b.maximum.y:b.minimum.y,
                                         bits&4?b.maximum.z:b.minimum.z}));
    };
    for(const auto& object:objects_) {
        box(object->pose().bounds,object->world());
        box(object->references().bounds(),model::identityMatrix());
        box(object->lightsabers().bounds(),model::identityMatrix());
        box(object->particles().bounds(),model::identityMatrix());
    }
    return result;
}
std::vector<AreaMeshInstance> AreaScene::meshInstances(bool includeReferences,bool dangly,
        const std::vector<model::TriggerPlane>& planes) const {
    std::vector<AreaMeshInstance> result;
    for(const auto& object:objects_) {
        const auto& document=*object->document();
        const auto append=[&](const model::Model& asset,const model::Pose& pose,const model::Mat4& outer,
                              const model::EngineFrame& frame,std::optional<std::size_t> reference,float time) {
            const bool isStatic=object->staticPrimary()&&!reference;
            if(isStatic) {
                if(object->room()&&!roomActive(static_cast<LightRoomId>(*object->room()))) return;
            }
            const auto* staticRoom=isStatic&&object->room()?room(static_cast<LightRoomId>(*object->room())):nullptr;
            const auto* animationOwner=isStatic?nullptr:reference
                ?&object->references().animationOwner(*reference):&object->animationOwner();
            const bool transparent=model::ownerTreeHasTransparency(asset,
                asset.game==model::GameVersion::Kotor1?nullptr:&pose);
            for(std::size_t i=0;i<asset.meshes.size();++i) {
                const auto& mesh=asset.meshes[i];
                const auto& indices=asset.game==model::GameVersion::Kotor1&&!mesh.animated
                    ?mesh.engineDrawIndices:mesh.indices;
                if(mesh.nodeIndex>=pose.worldTransforms.size()||mesh.nodeIndex>=asset.nodes.size()) continue;
                // PartAABB consumes the authored CPU face pool, independently
                // of the ordinary part's MDX/index drawing stream.
                if(asset.nodes[mesh.nodeIndex].aabb ? mesh.faces.empty() : indices.empty()) continue;
                if(!asset.sceneNodeAdmission.empty()&&!asset.sceneNodeAdmission[mesh.nodeIndex]) continue;
                if(staticRoom&&asset.game==model::GameVersion::Kotor2&&!asset.nodes[mesh.nodeIndex].aabb&&
                   std::any_of(planes.begin(),planes.end(),[&](const auto& plane){return above(staticRoom->meshBounds[i],plane)>0.0f;}))continue;
                AreaMeshInstance draw;
                draw.document=&document;draw.asset=&asset;draw.pose=&pose;draw.mesh=&mesh;
                draw.animationOwner=animationOwner;
                draw.material=reference?&document.referenceMaterial(*reference,i):&document.material(i);
                draw.state=pose.nodeMaterials[mesh.nodeIndex];draw.ownerWorld=outer;
                draw.ownerFrame=frame;draw.ownerScale=frame.scale;
                if((dangly||!mesh.dangly||isStatic)&&i<pose.deformedMeshes.size()&&pose.deformedMeshes[i].valid)
                    draw.deformed=&pose.deformedMeshes[i];
                auto drawWorld=outer;
                if(draw.ownerScale!=1.0f) {
                    auto scale=model::identityMatrix();scale.values[0]=scale.values[5]=scale.values[10]=draw.ownerScale;
                    drawWorld=model::multiply(outer,scale);
                }
                draw.world=draw.deformed?drawWorld:model::multiply(drawWorld,pose.worldTransforms[mesh.nodeIndex]);
                draw.staticPart=isStatic;draw.hasTransparency=transparent;draw.time=time;
                draw.ownerEffects=isStatic?nullptr:object->ownerVisualEffects(reference);
                draw.textureAngleDegrees=isStatic?0.0f:object->textureRotationDegrees(mesh);
                draw.owner=isStatic?sceneDynamicLightOwner:object->ownerId(reference);
                // Several track Gobs can share one source mesh with different
                // palettes at the same frame counter. Include owner identity.
                draw.revision=(object->ownerId(reference)<<32u)^object->revision();
                result.push_back(std::move(draw));
            }
        };
        append(document.model(),object->pose(),object->world(),object->frame(),{},static_cast<float>(object->animation().empty()
            ?object->elapsed():object->animation().primaryTime()));
        if(includeReferences) for(std::size_t i=0;i<object->references().instances().size();++i) {
            const auto& ref=object->references().instances()[i];
            if(ref.active&&ref.sceneMember) append(document.references()[i].model,ref.pose,ref.world,ref.frame,i,ref.animationTime);
        }
    }
    return result;
}
std::vector<LightRoomId> AreaScene::roomsAt(model::Vec3 point) const {
    require(finite(point), "non-finite room membership point");
    std::vector<LightRoomId> result;
    for (const auto& r : rooms_) if (point.x >= r.bounds.minimum.x && point.x <= r.bounds.maximum.x &&
                                   point.y >= r.bounds.minimum.y && point.y <= r.bounds.maximum.y)
        result.push_back(r.id);
    if (result.empty()) for (const auto& r : rooms_) result.push_back(r.id);
    return result;
}
std::vector<LightRoomId> AreaScene::ownerRooms(std::uint16_t classification,
                                              const model::Bounds& localBounds,
                                              const model::Mat4& world) const {
    if ((classification & 0x100u) != 0u) {
        std::vector<LightRoomId> result;
        for (const auto& r : rooms_) result.push_back(r.id);
        return result;
    }
    model::Vec3 local{};
    if ((classification & model::kK2ModelCharacter) == 0u) {
        local = {(localBounds.minimum.x+localBounds.maximum.x)*0.5f,
                 (localBounds.minimum.y+localBounds.maximum.y)*0.5f,
                 (localBounds.minimum.z+localBounds.maximum.z)*0.5f};
    }
    return roomsAt(model::transformPoint(world,local));
}
bool AreaScene::roomActive(LightRoomId id) const noexcept {
    return std::find(activeRooms_.begin(),activeRooms_.end(),id) != activeRooms_.end();
}
bool AreaScene::ownerVisible(std::uint16_t classification,const model::Bounds& localBounds,
                             const model::Mat4& world) const {
    if(rooms_.empty()) return true;
    const auto membership = ownerRooms(classification,localBounds,world);
    return std::any_of(membership.begin(),membership.end(),[this](auto id){return roomActive(id);});
}
void AreaScene::synchronize(LightScene& lights,const std::vector<model::TriggerPlane>& planes) {
    // Add owned room PartLights before constructing the membership tables.
    std::vector<RuntimeLight> pending;
    std::vector<LightRoomRegistration> registrations;
    std::unordered_set<std::uint64_t> ownIds;
    std::unordered_set<std::uint64_t> detachedIds;
    for (std::size_t index = 0; index < rooms_.size(); ++index) {
        const auto& r = rooms_[index];
        const auto begin = pending.size();
        lights.appendModelLights(r.document->model(),r.pose(),r.world,roomInstanceBase+index,pending);
        LightRoomRegistration registration;
        registration.id = r.id;
        registration.influencingRooms = r.influencingRooms;
        for (std::size_t l = begin; l < pending.size(); ++l) {
            auto& light = pending[l];
            light.hasOwner = false;
            ownIds.insert(light.id);
            registration.primaryLights.push_back(light.id);
            if (lights.find(light.id) && std::find(ownedLightIds_.begin(),ownedLightIds_.end(),light.id) == ownedLightIds_.end())
                throw std::runtime_error("Area light identity collides with an external registration");
        }
        registrations.push_back(std::move(registration));
    }
    if(environmentLightDocument_&&environmentLightPose_) {
        const auto begin=pending.size();
        lights.appendModelLights(environmentLightDocument_->model(),*environmentLightPose_,
                                environmentLightWorld_,environmentLightInstance,pending);
        const auto& sample=environment_.sample();
        for(std::size_t i=begin;i<pending.size();++i) {
            auto& light=pending[i];
            const auto& node=environmentLightDocument_->model().nodes[light.nodeIndex];
            const auto name=key(node.name);
            if(name=="gidy_sun_diff"&&sample.diffuse)light.color=*sample.diffuse;
            else if(name=="gidy_sun_amb"&&sample.globalAmbient)light.color=*sample.globalAmbient;
            light.hasOwner=false;
            light.registeredRender=light.color.x!=0.0f||light.color.y!=0.0f||light.color.z!=0.0f;
            ownIds.insert(light.id);
            for(auto& registration:registrations)registration.primaryLights.push_back(light.id);
            if(lights.find(light.id)&&std::find(ownedLightIds_.begin(),ownedLightIds_.end(),light.id)==ownedLightIds_.end())
                throw std::runtime_error("Area environment light identity collides with an external registration");
        }
    }
    for(const auto& object:objects_) {
        const auto& document=*object->document();
        const auto begin=pending.size();
        if(!object->staticPrimary())
            lights.appendModelLights(document.model(),object->pose(),object->world(),object->instanceIndex(),pending,object->references().ownerScale());
        for(std::size_t i=0;i<object->references().instances().size();++i) {
            const auto& ref=object->references().instances()[i];
            if(ref.active) {
                const auto first=pending.size();
                lights.appendModelLights(document.references()[i].model,ref.pose,ref.world,
                                         object->instanceIndex(i),pending,ref.scale);
                if(!ref.sceneMember)
                    for(auto j=first;j<pending.size();++j)detachedIds.insert(pending[j].id);
            }
        }
        for(std::size_t i=begin;i<pending.size();++i) {
            const auto id=pending[i].id;ownIds.insert(id);
            require(!lights.find(id)||std::find(ownedLightIds_.begin(),ownedLightIds_.end(),id)!=ownedLightIds_.end(),
                    "area Gob light identity collides with an external registration");
        }
    }
    for (const auto id : ownedLightIds_) if (!ownIds.count(id)) lights.removeLight(id);
    ownedLightIds_.clear();
    for (auto& light : pending) {
        const auto id=light.id;
        ownedLightIds_.push_back(id);lights.registerLight(std::move(light),false);
        if(detachedIds.count(id))lights.setLightSceneMember(id,false);
    }
    for (const auto& light : lights.lights()) {
        // gidy_sun is a scoped area-global light container. It has already
        // been admitted to every room's primary list above; treating its
        // dynamic-type bits as an ordinary spatial secondary registration
        // would duplicate the same engine light in each room.
        if(light.instanceIndex==environmentLightInstance)continue;
        if (!light.registeredDynamic && !(ownIds.count(light.id) && light.dynamicType != 0)) continue;
        const auto memberships = roomsAt(light.position);
        for (const auto id : memberships) {
            auto& registration = registrations[static_cast<std::size_t>(id-1u)];
            registration.secondaryLights.push_back(light.id);
        }
    }
    for (auto& registration : registrations) lights.registerRoom(std::move(registration),false);
    lights.collectLightsForGivenRoom(ignoreVisibility_ ? std::nullopt : currentRoom_);
    activeRooms_.clear();
    if (!currentRoom_ || ignoreVisibility_) {
        for (const auto& r : rooms_) activeRooms_.push_back(r.id);
    } else {
        const auto* current = room(*currentRoom_);
        require(current != nullptr, "current room was removed");
        activeRooms_.push_back(current->id);
        for (const auto id : current->influencingRooms) {
            const auto* candidate = room(id);
            if (!candidate) continue;
            // K1 CollectActiveRooms appends every VIS neighbor. K2 added
            // room-box rejection and the background-room exemption.
            const bool outside = game_==model::GameVersion::Kotor2 && !candidate->background && std::any_of(planes.begin(),planes.end(),
                [&](const auto& plane){return above(candidate->bounds,plane)>0.0f;});
            if (!outside) activeRooms_.push_back(id);
        }
    }
}

AreaObject* AreaScene::object(std::size_t instance) noexcept {
    return const_cast<AreaObject*>(static_cast<const AreaScene*>(this)->object(instance));
}
const AreaObject* AreaScene::object(std::size_t instance) const noexcept {
    for(const auto& object:objects_) {
        const auto first=object->instanceIndex();
        if(instance>=first && instance<=first+object->document()->references().size()) return object.get();
    }
    return nullptr;
}
const ModelDocument* AreaScene::lightDocument(std::size_t instance) const noexcept {
    if(instance==environmentLightInstance)return environmentLightDocument_.get();
    if(const auto* runtime=object(instance)) return runtime->document().get();
    if(instance>=roomInstanceBase && instance-roomInstanceBase<rooms_.size())
        return rooms_[instance-roomInstanceBase].document.get();
    return nullptr;
}
void AreaScene::setQueries(const SceneQueries* queries) noexcept {
    services_.queries=queries;
    for(auto& object:objects_) object->setQueries(queries);
    for(auto& object:detachedWeather_)if(object)object->setQueries(queries);
}
void AreaScene::playRoomAnimation(const std::string& roomName,std::int32_t animation) {
    // K1 ExecuteCommandPlayRoomAnimation 0x2a4292 and GetRoom 0x62750;
    // K2 Desktop 0x34a932 / 0x2703b0. CSWRoom retains a 16-byte CResRef.
    const auto name=key(roomName);
    const auto found=std::find_if(rooms_.begin(),rooms_.end(),
        [&](const auto& room){return key(room.name.substr(0,16))==name;});
    if(found==rooms_.end())return;
    for(const auto& owner:objects_)
        if(owner->kind()==AreaObjectKind::AnimatedRoom&&owner->room()==found->id) {
            owner->playRoomAnimation(static_cast<std::uint16_t>(animation));
            return;
        }
}
AreaObject* AreaScene::worldAnimationObject(std::size_t instance) noexcept {
    // Captured follower bindings retain Gobs, independently of scene membership.
    // Weather detachment keeps its Gob alive; only destruction drops bindings.
    for(const auto& owner:objects_)if(owner->instanceIndex()==instance)return owner.get();
    for(const auto& owner:detachedWeather_)if(owner&&owner->instanceIndex()==instance)return owner.get();
    return nullptr;
}
void AreaScene::releaseWorldAnimationOwner(std::size_t instance) {
    // Engine followers own their model/track Gobs (K1 0x17d7fa, K2 0x255f92),
    // and Gob destruction deletes its callbacks (0x3322a0 / 0xa9d6c).
    // Captured host bindings instead refer to independent owners: invalidate
    // only the destroyed identity before releasing that owner's lifetime.
    trackAnimations_.erase(instance);
    for(auto& entry:trackAnimations_) {
        auto& state=entry.second;
        state.models.erase(std::remove(state.models.begin(),state.models.end(),instance),state.models.end());
        if(state.playerModel==instance)state.playerModel.reset();
        if(state.lastCustomSource==instance)state.lastCustomSource.reset();
    }
    trackAnimationCallbacks_.erase(instance);
    for(auto& callbacks:trackAnimationCallbacks_)
        callbacks.second.erase(std::remove_if(callbacks.second.begin(),callbacks.second.end(),
            [instance](const auto& callback){return callback.track==instance;}),callbacks.second.end());
}
WorldAnimationDispatch AreaScene::worldAnimationDispatch() {
    return [this](std::size_t instance,const std::string& name,float rate,std::uint32_t flags) {
        // Bindings are concrete retained AreaObject identities, established
        // by the host capture rather than guessed from model classification.
        auto& owner=*worldAnimationObject(instance);
        const auto lifetime=*owner.animationOwner().lifetime();
        const auto result=owner.playWorldAnimation(name,rate,flags);
        if(lifetime.expired())return;
        if(!result&&!result.diagnostic.empty())owner.noteVisualDiagnosticOnce(
            "world animation:"+name,ObjectVisualStateSource::LiveAdapter,
            "world animation",result.diagnostic,false);
    };
}
void AreaScene::captureTrackAnimation(std::size_t track,TrackAnimationState state) {
    trackAnimations_.insert_or_assign(track,std::move(state));
}
void AreaScene::synchronizeTrackFollower(TrackFollowerInstance& follower) {
    const auto& track=*worldAnimationObject(follower.track);
    const auto hook=model::enginePartWorld(track.document()->model(),track.pose(),track.frame(),follower.hook);
    const auto attach=[&](const TrackModelAttachment& attachment) {
        auto frame=trackAttachmentFrame(hook,attachment);
        auto* owner=worldAnimationObject(attachment.instance);frame.scale=owner->frame().scale;
        owner->setFrame(frame);
    };
    for(const auto& attachment:follower.models)attach(attachment);
    if(follower.camera)attach(*follower.camera);
}
void AreaScene::startMiniGameTracks() {
    // CSWMiniGame::Go: player first, then enemies in reverse object-array order.
    for(const auto& follower:trackFollowers_)
        if(miniGame_->followers[follower.definition].kind==TrackFollowerKind::Player)
            startTrackAnimation(follower.track);
    for(auto follower=trackFollowers_.rbegin();follower!=trackFollowers_.rend();++follower)
        if(miniGame_->followers[follower->definition].kind==TrackFollowerKind::Enemy)
            startTrackAnimation(follower->track);
}
void AreaScene::stopMiniGameTracks() {
    // Host batch of the already-ported engine follower Stop operation.
    for(const auto& follower:trackFollowers_)stopTrackAnimation(follower.track);
}
void AreaScene::setMiniGamePlayerOffset(model::Vec3 offset,bool worldSpace) {
    for(auto& follower:trackFollowers_) {
        if(miniGame_->followers[follower.definition].kind!=TrackFollowerKind::Player)continue;
        for(auto& attachment:follower.models)if(attachment.mode==1||attachment.mode==2) {
            attachment.offset=offset;attachment.worldSpaceOffset=worldSpace;
        }
        // Control writes the camera hook only for Rotate behavior (type1003).
        if(follower.camera&&follower.camera->mode==2)follower.camera->offset=offset;
    }
}
void AreaScene::forceMiniGameTrackAttachment(std::size_t index) {
    auto& follower=trackFollowers_.at(index);
    const auto& definition=miniGame_->followers[follower.definition];
    const auto mode=definition.kind==TrackFollowerKind::Player?
        static_cast<std::uint8_t>(miniGame_->type):std::uint8_t{};
    for(std::size_t i=0;i<follower.models.size();++i) {
        auto& attachment=follower.models[i];
        attachment.mode=definition.models[i].rotating?mode:std::uint8_t{};
        attachment.offset={};attachment.worldSpaceOffset.reset();
    }
    // ForceSetTrack passes its mode directly for the retained camera Gob;
    // CameraRotate is consulted only by the separate SetCameraHook caller.
    if(follower.camera) {
        follower.camera->mode=mode;follower.camera->offset={};follower.camera->worldSpaceOffset.reset();
    }
    trackAnimations_.at(follower.track).running=false;
    synchronizeTrackFollower(follower);
    const auto& track=*worldAnimationObject(follower.track);
    follower.initialHook=model::enginePartWorld(track.document()->model(),track.pose(),track.frame(),follower.hook).position;
    if(definition.kind==TrackFollowerKind::Player)
        trackAnimationCallbacks_[follower.track].push_back({follower.track,true});
    bindAnimationOwners();
}
void AreaScene::replaceMiniGameTrack(std::size_t index,std::shared_ptr<ModelDocument> document,
    model::Vec3 prototypePosition) {
    auto& follower=trackFollowers_.at(index);
    require(document->model().game==game_,"mini-game track belongs to a different game");
    const auto& nodes=document->model().nodes;
    const auto hook=std::find_if(nodes.begin(),nodes.end(),[](const auto& node){return key(node.name)=="modelhook";});
    require(hook!=nodes.end(),"replacement MiniGame track has no engine modelhook");
    std::size_t instance=0x90000000ull;
    for(const auto& owner:objects_)instance=std::max(instance,
        owner->instanceIndex()+1u+owner->document()->references().size());
    auto replacement=std::make_unique<AreaObject>(instance,AreaObjectKind::Track,
        "MiniGame track: "+document->model().name,document,model::EngineFrame{prototypePosition,{}},
        std::nullopt,services_);
    const auto previous=follower.track;
    auto state=std::move(trackAnimations_.at(previous));trackAnimations_.erase(previous);
    if(state.lastCustomSource==previous)state.lastCustomSource.reset();
    // Model callbacks retain the follower itself. Only callbacks owned by the
    // destroyed old track disappear; their surviving follower target moves.
    trackAnimationCallbacks_.erase(previous);
    for(auto& callbacks:trackAnimationCallbacks_)for(auto& callback:callbacks.second)
        if(callback.track==previous)callback.track=instance;
    objects_.erase(std::find_if(objects_.begin(),objects_.end(),
        [&](const auto& owner){return owner->instanceIndex()==previous;}));
    objects_.push_back(std::move(replacement));
    follower.track=instance;follower.hook=static_cast<std::size_t>(hook-nodes.begin());
    trackAnimations_.emplace(instance,std::move(state));
    forceMiniGameTrackAttachment(index);
}
const TrackAnimationState* AreaScene::trackAnimation(std::size_t track) const noexcept {
    const auto found=trackAnimations_.find(track);
    return found==trackAnimations_.end()?nullptr:&found->second;
}
void AreaScene::startTrackAnimation(std::size_t track) {
    // Go registers each model immediately before Ready_01, then registers
    // the track immediately before PlayAnimation(track). Command callbacks
    // therefore see precisely the registrations already installed.
    neomdl::startTrackAnimation(track,trackAnimations_.at(track),worldAnimationDispatch(),
        [this,track](std::size_t source) {
            auto* owner=worldAnimationObject(source);
            owner->setAnimationEventCallback([host=animationHost_,owner](const model::AnimationRuntimeEvent& event) {
                host->scene->dispatchWorldAnimationEvent(*owner,event);
            });
            trackAnimationCallbacks_[source].push_back({track,false});
        });
}
void AreaScene::stopTrackAnimation(std::size_t track) {
    neomdl::stopTrackAnimation(track,trackAnimations_.at(track),worldAnimationDispatch());
}
void AreaScene::setTrackSpeed(std::size_t track,float speed) {
    setTrackAnimationSpeed(track,trackAnimations_.at(track),speed,worldAnimationDispatch());
}
void AreaScene::playTrackFollowerAnimation(std::size_t track,const std::string& name,
    int looping,int queued,int overlay) {
    playTrackScriptAnimation(trackAnimations_.at(track),game_,name,looping,queued,overlay,worldAnimationDispatch());
}
void AreaScene::removeTrackFollowerAnimation(std::size_t track,const std::string& name) {
    removeTrackScriptAnimation(trackAnimations_.at(track),name,worldAnimationDispatch());
}
void AreaScene::dispatchWorldAnimationEvent(AreaObject& owner,const model::AnimationRuntimeEvent& event) {
    const auto source=owner.instanceIndex();
    const auto lifetime=*owner.animationOwner().lifetime();
    const auto host=animationHost_;
    const auto found=trackAnimationCallbacks_.find(source);
    if(found==trackAnimationCallbacks_.end())return; // this Gob has no registered callbacks
    const auto& frame=owner.references().ownerFrame();
    const auto distance=worldAnimationCallbackDistance(
        frame.position,worldAnimationFrameInput_,game_);
    // Registered callbacks all use range9999 and visible-only=false.
    if(worldAnimationFrameInput_.checkEvents&&!(9999.0f>distance))return;
    // Gob::DoEvent captures the callback count, rereads the live callback
    // array, and checks its SafePointer before each next callback. Additions
    // wait for a later DoEvent; destroying this source ends this dispatch.
    for(auto count=found->second.size();count!=0;--count) {
        if(lifetime.expired())return;
        auto& scene=*host->scene;
        const auto registration=scene.trackAnimationCallbacks_.at(source).at(count-1);
        if(registration.captureInitialHook) {
            if(key(event.name)=="starttrack")
                for(auto& follower:scene.trackFollowers_)if(follower.track==registration.track)
                    follower.initialHook=model::enginePartWorld(owner.document()->model(),owner.animation().pose(),owner.frame(),follower.hook).position;
        } else {
            auto& state=scene.trackAnimations_.at(registration.track);
            if(auto output=trackAnimationEvent(registration.track,state,source,event.name,scene.worldAnimationDispatch())) {
                scene.worldAnimationEvents_.push_back(*output);
                const auto callback=scene.worldAnimationEvent_;
                if(callback)callback(*output);
            }
        }
    }
}
std::vector<WorldAnimationEvent> AreaScene::takeWorldAnimationEvents() {
    std::vector<WorldAnimationEvent> result;
    result.swap(worldAnimationEvents_);return result;
}
void AreaScene::bindAnimationOwners() {
    for(const auto& owner:objects_)
        owner->setAnimationEventCallback([host=animationHost_,object=owner.get()](const model::AnimationRuntimeEvent& event) {
            host->scene->dispatchWorldAnimationEvent(*object,event);
        });
    for(std::size_t index=0;index<trackFollowers_.size();++index) {
        const auto& follower=trackFollowers_[index];
        const auto bind=[this,index](std::size_t instance,std::optional<std::size_t> modelIndex) {
            auto* owner=worldAnimationObject(instance);
            const auto lifetime=*owner->animationOwner().lifetime();
            const auto revision=owner->animationOwner().controlRevision()+1;
            owner->animationOwner().setControl([host=animationHost_,index,modelIndex,owner,lifetime,revision](float seconds) {
                auto* target=host->scene->worldAnimationObject(host->scene->trackFollowers_[index].track);
                const auto targetLifetime=*target->animationOwner().lifetime();
                if(SceneAnimation::dependenciesAnimating())target->animationOwner().animate(seconds);
                if(lifetime.expired())return;
                if(owner->animationOwner().controlRevision()!=revision)return;
                if(targetLifetime.expired()) {owner->animationOwner().setControl({});return;}
                // Attachment Control writes only its own Gob. Another model's
                // callback may have changed this live behavior or its offset.
                auto& follower=host->scene->trackFollowers_[index];
                const auto& attachment=modelIndex?follower.models[*modelIndex]:*follower.camera;
                const auto hook=model::enginePartWorld(target->document()->model(),target->animation().pose(),target->frame(),follower.hook);
                auto frame=trackAttachmentFrame(hook,attachment);frame.scale=owner->frame().scale;
                owner->setFrame(frame);
            });
        };
        if(follower.camera)bind(follower.camera->instance,std::nullopt);
        for(std::size_t modelIndex=0;modelIndex<follower.models.size();++modelIndex)
            bind(follower.models[modelIndex].instance,modelIndex);
    }
}
void AreaScene::prepareAnimationFrame(const AreaFrameSettings& settings) {
    for(const auto& owner:objects_)owner->prepareAnimationFrame(settings);
    for(const auto& owner:detachedWeather_)if(owner)owner->prepareAnimationFrame(settings);
}

void AreaScene::advancePose(float seconds,const AreaFrameSettings& settings) {
    prepareAnimationFrame(settings);
    if(services_.animation)services_.animation->animate(seconds);
}

void AreaScene::advanceEffects(float seconds,const AreaFrameSettings& settings) {
    synchronizeObjectTargets();
    for(auto& object:objects_) object->advanceEffects(seconds,settings);
    // Parent animations are detached; child ChunkyParticle Gobs are separate
    // scene members and still receive their ordinary scene update.
    for(auto& object:detachedWeather_)if(object)object->particles().prepareFrame(
        object->pose(),seconds,settings.references,false);
}
void AreaScene::finishWindFrame(float seconds) {
    require(std::isfinite(seconds)&&seconds>=0,"invalid scene wind step");
    if(ownsWind_ && services_.wind) services_.wind->updateTimer(seconds);
}
model::WindManager& AreaScene::wind() {
    require(services_.wind!=nullptr,"area has no scene wind manager");return *services_.wind;
}

void AreaScene::configureGrass(const GrassSettings& settings,bool applyWind,
    const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,0,"Preparing area grass");
    require(!empty(),"load an area before configuring grass");
    std::vector<GrassRoomSource> sources;
    for(const auto& r:rooms_)sources.push_back({r.id,r.document,&r.pose(),r.world,r.bounds});
    GrassScene pending;pending.rebuild(sources,settings);
    // Resolve authored textures against their room's directory, then the same
    // installation/host resolver used to load the area's model assets.
    if(settings.enabled)for(const auto& b:pending.bins()) {
        job.check();
        if(!b.bladeCount)continue;
        const auto& name=settings.surfaces[b.surface]->texture;
        std::string error;
        if(!b.document->texture(name))
            require(b.document->loadRuntimeTexture(name,resolver_,error),error);
        if(settings.textured && settings.lightmaps)for(const auto& t:b.triangles)
            if(t.lightmapped && !b.document->texture(t.lightmap))
                require(b.document->loadRuntimeTexture(t.lightmap,resolver_,error),error);
    }
    grass_=std::move(pending);
    if(applyWind&&settings.areaWind)require(wind().applyK2AreaWind(*settings.areaWind),"invalid area wind mode");
}
void AreaScene::loadGrassArea(const std::filesystem::path& are,const neoshared::bif::JobControl& job) {
    ResourceLoadScope work(job);job.update(0,0,"Reading area grass settings");
    require(!empty(),"load an area before its grass settings");
    std::vector<std::uint8_t> surface;
    const auto directory=are.parent_path().empty()?std::filesystem::path("."):are.parent_path();
    if(auto local=localResource(directory,"surfacemat.2da"))surface=readResourceFile(*local);
    else if(!path_.empty()) {if(auto local=localResource(path_.parent_path().empty()?std::filesystem::path("."):path_.parent_path(),"surfacemat.2da"))surface=readResourceFile(*local);}
    if(surface.empty()&&resolver_) {if(auto resource=resolver_("surfacemat",0x07e1u))surface=std::move(resource->bytes);}
    require(!surface.empty(),"surfacemat.2da is required; supply it locally or through the area resource resolver");
    configureGrass(grassSettingsFromArea(readResourceFile(are),surface,job,game_));
}
void AreaScene::collectGrass(model::Vec3 camera,const std::vector<model::TriggerPlane>& planes) {
    if(empty())return;
    require(services_.random!=nullptr,"area has no shared random source");
    std::vector<std::uint64_t> ids(activeRooms_.begin(),activeRooms_.end());
    grass_.collect(camera,ids,planes,*services_.random);
}
void AreaScene::advanceGrassWind(float seconds) {
    if(!empty())grass_.advanceWind(seconds,wind());
}

std::vector<model::EmitterSceneRoom> AreaScene::emitterRooms(
    const std::vector<model::TriggerPlane>& planes) const {
    std::vector<model::EmitterSceneRoom> result;
    for (const auto& r : rooms_) {
        model::EmitterSceneRoom entry;
        entry.identity = r.id;
        entry.minimumX = r.bounds.minimum.x; entry.maximumX = r.bounds.maximum.x;
        entry.minimumY = r.bounds.minimum.y; entry.maximumY = r.bounds.maximum.y;
        entry.active = roomActive(r.id);
        for (const auto& p : planes) entry.visibilityPlanes.push_back({p.normal,p.distance});
        result.push_back(std::move(entry));
    }
    return result;
}
void AreaScene::clear(LightScene& lights) {
    for (const auto id : ownedLightIds_) lights.removeLight(id);
    for (const auto& r : rooms_) lights.removeRoom(r.id);
    for(const auto& object:objects_) {
        if(!object->staticPrimary()) lights.removeOwner(object->ownerId());
        for(std::size_t i=0;i<object->document()->references().size();++i) lights.removeOwner(object->ownerId(i));
    }
    // Deregister emitter callbacks before any owned scene service is released.
    objects_.clear();
    lights.removeOwner(sceneLightOwner);
    lights.removeOwner(sceneDynamicLightOwner);
    lights.clearRoomCollection();
    *this = AreaScene{};
}
} // namespace neomdl
