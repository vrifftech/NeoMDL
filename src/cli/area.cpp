#include "core/GameResourceSession.hpp"
#include "core/AreaScene.hpp"
#include <neoshared/PathUtf8.hpp>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
void usage() {
    std::cout << "Usage: neomdl-area-info <installation> [module [area]] [--game=k1|k2] [--allow-partial] [--no-grass]\n"
                 "Lists discovered module scopes, lists scoped AREs, or loads a visual area.\n"
                 "The default game is K2; K1 area playback uses the Desktop runtime profile.\n"
                 "--help shows usage; --version shows the application version.\n"
                 "No resource extraction or game/save modification is performed.\n";
}
std::string hex(std::uint32_t value) {
    std::ostringstream out;out<<"0x"<<std::hex<<value;return out.str();
}
void printAnimation(const neomdl::ObjectAnimationCommand& animation,const char* prefix) {
    std::cout<<prefix<<"state="<<unsigned(animation.serializedState)
             <<" server="<<hex(animation.serverAnimation);
    if(animation.clientAnimation)std::cout<<" client="<<hex(*animation.clientAnimation);
    if(!animation.animationName.empty())std::cout<<" clip="<<animation.animationName;
    if(!animation.diagnostic.empty())std::cout<<" diagnostic="<<animation.diagnostic;
    std::cout<<'\n';
}
void printVec(const char* label,const std::optional<neomdl::model::Vec3>& value) {
    if(value)std::cout<<label<<value->x<<','<<value->y<<','<<value->z<<'\n';
}
void printEnvironment(const neomdl::AreaScene& scene) {
    const auto& definition=scene.visualSettings().environment;
    std::cout<<"Environment: cycle="<<(definition.dayNightCycle?"yes":"no")
             <<" authored-start="<<(definition.startsNight?"night":"day")
             <<" shadow-opacity="<<unsigned(definition.shadowOpacity)<<'\n';
    printVec("  scene ambient: ",definition.sceneAmbient);
    printVec("  day global ambient: ",definition.day.globalAmbient);
    printVec("  day diffuse: ",definition.day.diffuse);
    printVec("  day fog color: ",definition.day.fogColor);
    if(definition.day.fogNear&&definition.day.fogFar)
        std::cout<<"  day fog range: "<<*definition.day.fogNear<<" .. "<<*definition.day.fogFar<<'\n';
    if(definition.day.fogEnabled)std::cout<<"  day fog: "<<(*definition.day.fogEnabled?"on":"off")<<'\n';
    if(definition.day.shadowsEnabled)std::cout<<"  day shadows: "<<(*definition.day.shadowsEnabled?"on":"off")<<'\n';
    printVec("  night global ambient: ",definition.night.globalAmbient);
    printVec("  night diffuse: ",definition.night.diffuse);
    printVec("  night fog color: ",definition.night.fogColor);
    if(definition.night.fogNear&&definition.night.fogFar)
        std::cout<<"  night fog range: "<<*definition.night.fogNear<<" .. "<<*definition.night.fogFar<<'\n';
    if(definition.night.fogEnabled)std::cout<<"  night fog: "<<(*definition.night.fogEnabled?"on":"off")<<'\n';
    if(definition.night.shadowsEnabled)std::cout<<"  night shadows: "<<(*definition.night.shadowsEnabled?"on":"off")<<'\n';
    if(definition.weather.chanceRain||definition.weather.chanceSnow||definition.weather.chanceLightning||
       definition.weather.currentWeather||definition.weather.disabled) {
        std::cout<<"  weather: rain="<<unsigned(definition.weather.chanceRain.value_or(0))
                 <<" snow="<<unsigned(definition.weather.chanceSnow.value_or(0))
                 <<" lightning="<<unsigned(definition.weather.chanceLightning.value_or(0));
        if(definition.weather.currentWeather)std::cout<<" current="<<unsigned(*definition.weather.currentWeather);
        if(definition.weather.disabled)std::cout<<" disabled="<<(*definition.weather.disabled?"yes":"no");
        std::cout<<'\n';
    }
    const auto& sample=scene.environment().sample();
    std::cout<<"  effective: "<<neomdl::areaTimeOfDayName(sample.phase)
             <<" transition="<<sample.transition
             <<" fog="<<(sample.fogEnabled?"on":"off")
             <<" shadows="<<(sample.shadowsEnabled?"on":"off")
             <<" shadow-opacity="<<sample.shadowOpacity
             <<" force-bright="<<(sample.forceBrightSunAndMoon?"yes":"no")<<'\n';
    printVec("    scene ambient: ",sample.sceneAmbient);
    printVec("    global ambient: ",sample.globalAmbient);
    printVec("    diffuse: ",sample.diffuse);
    printVec("    fog color: ",sample.fogColor);
    if(sample.fogNear&&sample.fogFar)std::cout<<"    fog range: "<<*sample.fogNear<<" .. "<<*sample.fogFar<<'\n';
    std::cout<<"  current room: ";
    if(scene.currentRoom())std::cout<<*scene.currentRoom();else std::cout<<"all rooms";
    std::cout<<(scene.currentRoomOverridden()?" (manual override)":" (replay or supplied position)")<<'\n';
    if(const auto* camera=scene.activeReplayCamera())
        std::cout<<"  replay camera: active position="<<camera->world.values[12]<<','<<camera->world.values[13]<<','<<camera->world.values[14]
                 <<" fov="<<camera->viewAngle<<" clips="<<camera->nearClip<<" .. "<<camera->farClip<<'\n';
    for(const auto& entry:scene.environment().trace())
        std::cout<<"  environment-trace #"<<entry.sequence<<' '<<entry.operation<<" / "
                 <<(entry.applied?"applied":"not applied")<<": "<<entry.detail<<'\n';
}
void printEffective(const neomdl::AreaObject& object) {
    const auto& state=object.effectiveVisualState();
    if(state.animation)printAnimation(*state.animation,"  animation: ");
    if(state.colorShift.enabled)
        std::cout<<"  tweak-color: "<<state.colorShift.color.x<<','<<state.colorShift.color.y<<','
                 <<state.colorShift.color.z<<" amount="<<state.colorShift.amount
                 <<" recursive="<<(state.colorShift.propagateToReferences?"yes":"no")<<'\n';
    if(state.opacity!=1.0f)std::cout<<"  opacity: "<<state.opacity<<'\n';
    if(!state.environmentMap.empty())std::cout<<"  environment-map: "<<state.environmentMap<<'\n';
    if(state.textureRotationDegrees!=0.0f)std::cout<<"  texture-rotation: "<<state.textureRotationDegrees<<" degrees\n";
    if(state.motionBlur.enabled)std::cout<<"  motion-blur: maximum="<<state.motionBlur.maximumLength<<'\n';
    if(state.distortion)std::cout<<"  distortion: enabled\n";
    if(state.hologram)std::cout<<"  hologram: enabled\n";
    if(state.forceEmitterRadiusUnbounded)std::cout<<"  emitter-radius override: explicit unbounded\n";
    if(!state.emitterTargets.empty())std::cout<<"  emitter-target routes: "<<state.emitterTargets.size()<<'\n';
    for(const auto& trace:object.visualState().trace())
        std::cout<<"  visual-trace #"<<trace.sequence<<' '<<neomdl::objectVisualStateSourceName(trace.source)
                 <<" / "<<trace.operation<<" / "<<(trace.applied?"applied":"not applied")
                 <<": "<<trace.detail<<'\n';
}
}
int main(int argc,char** argv) {
    if(argc==2&&(std::string_view(argv[1])=="--help"||std::string_view(argv[1])=="-h")){usage();return 0;}
    if(argc==2&&std::string_view(argv[1])=="--version") {
        std::cout<<"NeoMDL "<<NEOMDL_VERSION<<'\n';return 0;
    }
    try {
        neomdl::AreaLoadOptions options;neomdl::AreaRuntimeServices services;std::vector<std::string> args;
        for(int i=1;i<argc;++i) {
            const std::string a=argv[i];
            if(a=="--allow-partial")options.allowPartialVisuals=true;
            else if(a=="--no-grass")options.loadGrass=false;
            else if(a=="--game=k1")options.game=neomdl::model::GameVersion::Kotor1;
            else if(a=="--game=k2")options.game=neomdl::model::GameVersion::Kotor2;
            else if(a.rfind("--",0)==0)throw std::runtime_error("Unknown option: "+a);
            else args.push_back(a);
        }
        if(args.empty()||args.size()>3){usage();return 2;}
        neoshared::game::GameInstall install;
        install.id=neoshared::game::gameIdString(options.game==neomdl::model::GameVersion::Kotor1
            ? neoshared::game::GameId::Kotor1 : neoshared::game::GameId::Kotor2);
        install.installPath=std::filesystem::u8path(args[0]);
        neomdl::GameResourceSession base;std::string error;
        if(!base.open(install,error))throw std::runtime_error(error);
        const auto modules=base.modules();
        if(args.size()==1) {
            for(const auto& module:modules){std::cout<<module.name<<'\n';for(const auto& path:module.archives)std::cout<<"  "<<neoshared::genericPathToUtf8(path)<<'\n';}
            return 0;
        }
        const auto module=std::find_if(modules.begin(),modules.end(),[&](const auto& m){return neomdl::resourceKey(m.name)==neomdl::resourceKey(args[1]);});
        if(module==modules.end())throw std::runtime_error("Module scope was not discovered: "+args[1]);
        neomdl::GameResourceSession scoped;if(!scoped.openModule(base,*module,error))throw std::runtime_error(error);
        std::cout<<"Scope: "<<scoped.scopeIdentity()<<'\n';
        if(args.size()==2){for(const auto& area:scoped.areaResrefs())std::cout<<area<<'\n';return 0;}
        neomdl::AreaScene scene;scene.loadModuleArea(args[2],scoped.scopeIdentity(),scoped.resolver(),options,services);
        std::cout<<"Area: "<<scene.identity()<<"\nRooms: "<<scene.rooms().size()<<"\nRuntime owners: "<<scene.objects().size()<<'\n';
        std::cout<<neomdl::describeDirtyArea(scene.visualSettings().dirty);
        printEnvironment(scene);
        std::cout<<"Triggers: "<<scene.triggers().size()<<'\n';
        for(const auto& trigger:scene.triggers()) {
            std::cout<<"  ["<<trigger.sourceIndex<<"] "<<trigger.label
                     <<" object="<<hex(trigger.objectId)<<" type="<<trigger.type
                     <<" points="<<trigger.worldFootprint.size()
                     <<" highlight-height="<<trigger.highlightHeight<<'\n';
            std::cout<<"    position="<<trigger.position.x<<','<<trigger.position.y<<','<<trigger.position.z;
            if(!trigger.templateResref.empty())std::cout<<" template="<<trigger.templateResref;
            if(!trigger.transitionDestination.empty())std::cout<<" transition="<<trigger.transitionDestination;
            std::cout<<'\n';
            for(const auto& diagnostic:trigger.diagnostics)std::cout<<"    diagnostic: "<<diagnostic<<'\n';
        }
        std::cout<<"Placeable cameras: "<<scene.cameras().size()<<'\n';
        for(const auto& camera:scene.cameras()) {
            std::cout<<"  ["<<camera.sourceIndex<<"] id="<<camera.cameraId
                     <<" position="<<camera.position.x<<','<<camera.position.y<<','<<camera.position.z
                     <<" orientation="<<camera.orientation.x<<','<<camera.orientation.y<<','
                     <<camera.orientation.z<<','<<camera.orientation.w
                     <<" fov="<<camera.fieldOfView<<" pitch="<<camera.pitch
                     <<" height="<<camera.height<<" mic-range="<<camera.microphoneRange<<'\n';
            for(const auto& diagnostic:camera.diagnostics)std::cout<<"    diagnostic: "<<diagnostic<<'\n';
        }
        for(const auto& p:scene.placements()) {
            std::cout<<p.label<<": "<<p.modelResref;
            if(p.runtimeInstance)std::cout<<" [instance "<<*p.runtimeInstance<<']';
            else std::cout<<(p.visible?" [unresolved]":" [engine invisible model]");
            std::cout<<'\n';
            if(p.authoredVisualState.animation)printAnimation(*p.authoredVisualState.animation,"  authored animation: ");
            if(p.runtimeInstance)if(const auto* object=scene.object(*p.runtimeInstance))printEffective(*object);
        }
        for(const auto& d:scene.diagnostics())std::cout<<"diagnostic: "<<d<<'\n';
        for(const auto& d:scene.dependencies()) {
            const auto* status=d.status==neomdl::DependencyStatus::Found?"found":d.status==neomdl::DependencyStatus::Missing?"missing":"failed";
            std::cout<<status<<' '<<d.resref<<'.'<<neomdl::resourceExtension(d.type)<<" <- "<<d.requester<<'\n';
            if(!d.source.empty())std::cout<<"  "<<d.source<<'\n';
            if(!d.error.empty())std::cout<<"  "<<d.error<<'\n';
            for(const auto& path:d.inputs)std::cout<<"  input: "<<neoshared::genericPathToUtf8(path)<<'\n';
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"NeoMDL area: "<<e.what()<<'\n';return 1;}
}
