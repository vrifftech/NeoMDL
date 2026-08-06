#include "core/AreaEnvironment.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace neomdl {
namespace {
bool finite(float v) noexcept {return std::isfinite(v);}
bool finite(model::Vec3 v) noexcept {return finite(v.x)&&finite(v.y)&&finite(v.z);}
float clamp01(float v) noexcept {return std::clamp(v,0.0f,1.0f);}
model::Vec3 mix(model::Vec3 a,model::Vec3 b,float t) noexcept {
    return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};
}
std::optional<model::Vec3> mixed(const std::optional<model::Vec3>& a,
                                 const std::optional<model::Vec3>& b,float t) {
    if(a&&b)return mix(*a,*b,t);
    if(t>=1.0f)return b?b:a;
    return a?a:b;
}
std::optional<float> mixed(const std::optional<float>& a,
                           const std::optional<float>& b,float t) {
    if(a&&b)return *a+(*b-*a)*t;
    if(t>=1.0f)return b?b:a;
    return a?a:b;
}
bool endpointBool(const std::optional<bool>& value,bool fallback) noexcept {
    return value.value_or(fallback);
}
AreaTimeOfDay modulePhase(std::uint32_t hour,std::uint8_t dawn,std::uint8_t dusk) noexcept {
    // CSWSModule::UpdateTime K1 0x24f0f8 / K2 0x3f0c25.
    if(dawn==dusk)return AreaTimeOfDay::Day;
    if(hour==dawn)return AreaTimeOfDay::Dawn;
    if(hour==dusk)return AreaTimeOfDay::Dusk;
    const bool day=dawn<dusk?(hour>dawn&&hour<dusk):(hour>dawn||hour<dusk);
    return day?AreaTimeOfDay::Day:AreaTimeOfDay::Night;
}
std::uint32_t moduleMinutesPerHour(const ModuleTimeSettings& settings) noexcept {
    // CWorldTimer::SetMinutesPerHour substitutes five only in its own clock
    // conversion. The module's raw byte still determines transition duration.
    return settings.minutesPerHour?settings.minutesPerHour:5u;
}
bool sameCamera(const std::optional<AreaReplayCamera>& a,
                const std::optional<AreaReplayCamera>& b) noexcept {
    if(bool(a)!=bool(b))return false;
    if(!a)return true;
    if(a->active!=b->active||a->viewAngle!=b->viewAngle||a->nearClip!=b->nearClip||
       a->farClip!=b->farClip||a->currentRoom!=b->currentRoom)return false;
    for(std::size_t i=0;i<a->world.values.size();++i)
        if(a->world.values[i]!=b->world.values[i])return false;
    return true;
}
}

const char* areaTimeOfDayName(AreaTimeOfDay value) noexcept {
    switch(value){
    case AreaTimeOfDay::Day:return "day";
    case AreaTimeOfDay::Night:return "night";
    case AreaTimeOfDay::Dawn:return "dawn";
    case AreaTimeOfDay::Dusk:return "dusk";
    }
    return "unknown";
}
std::optional<std::uint32_t> AreaEnvironmentRuntime::worldDayMilliseconds() const noexcept {
    if(!worldTime_||!definition_.moduleTime)return {};
    return moduleMinutesPerHour(*definition_.moduleTime)*1440000u;
}

model::Mat4 inverseAreaReplayCamera(const model::Mat4& m) noexcept {
    const model::Vec3 x{m.values[0],m.values[1],m.values[2]};
    const model::Vec3 y{m.values[4],m.values[5],m.values[6]};
    const model::Vec3 z{m.values[8],m.values[9],m.values[10]};
    const model::Vec3 p{m.values[12],m.values[13],m.values[14]};
    auto r=model::identityMatrix();
    r.values[0]=x.x;r.values[1]=y.x;r.values[2]=z.x;
    r.values[4]=x.y;r.values[5]=y.y;r.values[6]=z.y;
    r.values[8]=x.z;r.values[9]=y.z;r.values[10]=z.z;
    r.values[12]=-(x.x*p.x+x.y*p.y+x.z*p.z);
    r.values[13]=-(y.x*p.x+y.y*p.y+y.z*p.z);
    r.values[14]=-(z.x*p.x+z.y*p.y+z.z*p.z);
    return r;
}

bool validAreaReplayCamera(const AreaReplayCamera& c,std::string& error) noexcept {
    error.clear();
    if(!finite(c.viewAngle)||c.viewAngle<=0.0f||c.viewAngle>=179.0f) {
        error="Area replay camera has an invalid vertical view angle";return false;
    }
    if(!finite(c.nearClip)||!finite(c.farClip)||c.nearClip<=0.0f||c.farClip<=c.nearClip) {
        error="Area replay camera has invalid clipping distances";return false;
    }
    for(float v:c.world.values)if(!finite(v)) {
        error="Area replay camera contains a non-finite transform";return false;
    }
    // Rigid orthonormal transform only. The engine camera setter consumes a
    // position/orientation, not arbitrary scale or shear.
    const model::Vec3 x{c.world.values[0],c.world.values[1],c.world.values[2]};
    const model::Vec3 y{c.world.values[4],c.world.values[5],c.world.values[6]};
    const model::Vec3 z{c.world.values[8],c.world.values[9],c.world.values[10]};
    const auto dot=[](model::Vec3 a,model::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    constexpr float epsilon=1.0e-3f;
    if(std::fabs(dot(x,x)-1.0f)>epsilon||std::fabs(dot(y,y)-1.0f)>epsilon||
       std::fabs(dot(z,z)-1.0f)>epsilon||std::fabs(dot(x,y))>epsilon||
       std::fabs(dot(x,z))>epsilon||std::fabs(dot(y,z))>epsilon) {
        error="Area replay camera transform is not rigid and orthonormal";return false;
    }
    return true;
}

void AreaEnvironmentRuntime::record(std::string operation,std::string detail,bool applied) {
    trace_.push_back({++sequence_,std::move(operation),std::move(detail),applied});
}

void AreaEnvironmentRuntime::setDefinition(AreaEnvironmentDefinition definition,bool automaticWorldTime) {
    definition_=std::move(definition);replay_={};trace_.clear();sequence_=0;
    automaticWorldTime_=automaticWorldTime;worldTime_.reset();
    ++cameraHistoryEpoch_;resetWorldTime();
    record("load authored area environment",definition_.startsNight?"initial endpoint: night":"initial endpoint: day",true);
}

void AreaEnvironmentRuntime::resetWorldTime() {
    worldTime_.reset();
    if(automaticWorldTime_&&definition_.moduleTime) {
        const auto& settings=*definition_.moduleTime;
        ModuleTimeState state;
        // CWorldTimer::SetWorldTime K1 0x3be8f6 / K2 0x4a1c76. These
        // substitutions are engine input rules, including second 56.
        const auto year=settings.startYear<=32000u?settings.startYear:1340u;
        const auto month=settings.startMonth>=1u&&settings.startMonth<=12u?settings.startMonth:6u;
        const auto day=settings.startDay>=1u&&settings.startDay<=28u?settings.startDay:1u;
        const auto hour=settings.startHour<=23u?settings.startHour:23u;
        const auto minute=settings.startMinute<=59u?settings.startMinute:59u;
        const auto second=settings.startSecond<=59u?settings.startSecond:56u;
        const auto dayMilliseconds=moduleMinutesPerHour(settings)*1440000u;
        const auto time=hour*moduleMinutesPerHour(settings)*60000u+minute*60000u+
                        second*1000u+settings.startMillisecond;
        state.calendarDay=year*336u+(month-1u)*28u+day-1u+time/dayMilliseconds;
        state.timeOfDayMilliseconds=time%dayMilliseconds;
        // The module stores its raw loaded calendar/hour fields separately
        // from CWorldTimer; the first UpdateTime message reconciles them.
        state.year=settings.startYear;state.month=settings.startMonth;
        state.day=settings.startDay;state.hour=settings.startHour;
        state.phase=modulePhase(state.hour,settings.dawnHour,settings.duskHour);
        state.transitionMilliseconds=settings.transitionMilliseconds;
        state.transitionSeconds=float(settings.transitionMilliseconds)/1000.0f;
        state.transitionDurationSeconds=float(settings.minutesPerHour)*60.0f;
        worldTime_=state;
    }
    rebuildSample();
}

void AreaEnvironmentRuntime::setAutomaticWorldTime(bool enabled) {
    if(automaticWorldTime_==enabled)return;
    automaticWorldTime_=enabled;
    resetWorldTime();
}

void AreaEnvironmentRuntime::advanceWorldTime(EngineWorldTime current,float clientSeconds,bool paused) {
    if(!worldTime_||paused)return;
    auto& state=*worldTime_;const auto& settings=*definition_.moduleTime;
    const auto minutesPerHour=moduleMinutesPerHour(settings);
    const auto dayMilliseconds=minutesPerHour*1440000u;
    // CSWSModule::AIUpdate retains its own previous stamp across pause-state
    // commands. UpdateTime also receives the absolute stamp, so a complete
    // elapsed day advances the calendar even when its DWORD remainder is zero.
    const auto elapsedMilliseconds=engineWorldTimeRemainderMilliseconds(current,
        {state.calendarDay,state.timeOfDayMilliseconds},dayMilliseconds);
    state.calendarDay=current.calendarDay;
    state.timeOfDayMilliseconds=current.timeOfDayMilliseconds;
    // ConvertFromCalendarDay's year limit differs from SetWorldTime's limit.
    const auto calendarYear=state.calendarDay/336u;
    const auto year=calendarYear<32768u?calendarYear:1340u;
    const auto month=(state.calendarDay/28u)%12u+1u;
    const auto day=state.calendarDay%28u+1u;
    const auto hour=(state.timeOfDayMilliseconds/60000u)/minutesPerHour;
    const auto phase=modulePhase(hour,settings.dawnHour,settings.duskHour);
    if(state.phase==AreaTimeOfDay::Dawn||state.phase==AreaTimeOfDay::Dusk)
        state.transitionMilliseconds+=elapsedMilliseconds;
    else state.transitionMilliseconds=0;
    const bool phaseChanged=phase!=state.phase;
    const bool notify=phaseChanged||hour!=state.hour||day!=state.day||
                      month!=state.month||year!=state.year;
    state.phase=phase;state.hour=hour;state.day=day;state.month=month;state.year=year;
    if(phaseChanged) {
        // SendServerToPlayerModuleUpdate_Time writes phase/elapsed only for
        // bit 0. Hour/calendar-only packets leave the accumulator untouched;
        // endpoint phase packets omit elapsed and the client supplies zero.
        state.transitionSeconds=phase==AreaTimeOfDay::Dawn||phase==AreaTimeOfDay::Dusk?
            float(state.transitionMilliseconds)/1000.0f:0.0f;
    }
    if(notify) {
        record("engine module time",std::string(areaTimeOfDayName(phase))+" hour="+std::to_string(hour),true);
    }
    // CSWCModule::Update K1 0xbb138 / K2 0x2a2047 accumulates float seconds
    // before the area's interpolation caller, independently of integer time.
    state.transitionSeconds+=clientSeconds;
    rebuildSample();
}

const AreaReplayCamera* AreaEnvironmentRuntime::activeCamera() const noexcept {
    return replay_.camera&&replay_.camera->active?&*replay_.camera:nullptr;
}

std::optional<std::uint64_t> AreaEnvironmentRuntime::replayCurrentRoom() const noexcept {
    if(replay_.currentRoom)return replay_.currentRoom;
    if(const auto* camera=activeCamera();camera&&camera->currentRoom)return camera->currentRoom;
    return {};
}

bool AreaEnvironmentRuntime::applyReplay(AreaEnvironmentReplayState state,std::string& error) {
    error.clear();
    if(!finite(state.transition)||state.transition<0.0f||state.transition>1.0f) {
        error="Area time-of-day transition must be finite and between zero and one";
        record("apply environment replay",error,false);return false;
    }
    if(state.phase&&( *state.phase==AreaTimeOfDay::Dawn||*state.phase==AreaTimeOfDay::Dusk) &&
       !definition_.dayNightCycle) {
        error="Dawn/dusk replay requires an ARE with DayNightCycle enabled";
        record("apply environment replay",error,false);return false;
    }
    for(const auto* value:{&state.sceneAmbient,&state.globalAmbient,&state.diffuse,&state.fogColor})
        if(*value&&!finite(**value)) {
            error="Area environment replay contains a non-finite color";
            record("apply environment replay",error,false);return false;
        }
    if((state.fogNear&&!finite(*state.fogNear))||(state.fogFar&&!finite(*state.fogFar))||
       (state.shadowOpacity&&!finite(*state.shadowOpacity))) {
        error="Area environment replay contains a non-finite scalar";
        record("apply environment replay",error,false);return false;
    }
    if((state.fogNear&&*state.fogNear<0.0f)||(state.fogFar&&*state.fogFar<0.0f)) {
        error="Area environment replay fog distances must be non-negative";
        record("apply environment replay",error,false);return false;
    }
    if(state.shadowOpacity&&*state.shadowOpacity<0.0f) {
        error="Area environment replay shadow opacity must be non-negative";
        record("apply environment replay",error,false);return false;
    }
    if(state.camera) {
        if(!validAreaReplayCamera(*state.camera,error)) {
            record("apply environment replay",error,false);return false;
        }
    }
    const bool cameraChanged=!sameCamera(replay_.camera,state.camera);
    const bool cut=state.camera&&state.camera->cut;
    if(state.camera)state.camera->cut=false;
    replay_=std::move(state);
    rebuildSample();
    // Engine ARE fog defaults have near == far == 10000. SetFog forwards
    // both distances as authored; it does not reject their ordering.
    if(cameraChanged||cut)++cameraHistoryEpoch_;
    std::ostringstream detail;
    detail<<areaTimeOfDayName(sample_.phase);
    if(sample_.phase==AreaTimeOfDay::Dawn||sample_.phase==AreaTimeOfDay::Dusk)
        detail<<" transition="<<sample_.transition;
    if(replayCurrentRoom())detail<<" room="<<*replayCurrentRoom();
    if(activeCamera())detail<<" camera=active";
    if(sample_.forceBrightSunAndMoon)detail<<" force-bright";
    if(replay_.sceneAmbient||replay_.globalAmbient||replay_.diffuse||replay_.fogColor||
       replay_.fogNear||replay_.fogFar||replay_.shadowOpacity)detail<<" direct-setters";
    if(cut)detail<<" cut";
    record("apply environment replay",detail.str(),true);return true;
}

void AreaEnvironmentRuntime::clearReplay() {
    const bool cameraWasActive=activeCamera()!=nullptr;
    replay_={};if(cameraWasActive)++cameraHistoryEpoch_;rebuildSample();
    record("clear environment replay",worldTime_?"restored automatic module time":
           definition_.startsNight?"restored authored night endpoint":"restored authored day endpoint",true);
}

void AreaEnvironmentRuntime::rebuildSample() {
    auto automaticPhase=definition_.startsNight?AreaTimeOfDay::Night:AreaTimeOfDay::Day;
    float automaticTransition=0.0f;
    if(worldTime_&&definition_.dayNightCycle) {
        automaticPhase=worldTime_->phase;
        if(automaticPhase==AreaTimeOfDay::Dawn||automaticPhase==AreaTimeOfDay::Dusk) {
            // UpdateDayNightCycle compares before dividing, including engine
            // zero-duration modules, then clears the transition at its endpoint.
            if(worldTime_->transitionSeconds>=worldTime_->transitionDurationSeconds)
                automaticPhase=automaticPhase==AreaTimeOfDay::Dawn?AreaTimeOfDay::Day:AreaTimeOfDay::Night;
            else automaticTransition=worldTime_->transitionSeconds/worldTime_->transitionDurationSeconds;
        }
    }
    const auto phase=replay_.phase.value_or(automaticPhase);
    sample_={};sample_.phase=phase;
    sample_.sceneAmbient=definition_.sceneAmbient;
    // K1 Desktop 0x62ad8/0x62bf8 and K2 Desktop 0x26dce0/0x26dbe0.
    sample_.shadowOpacity=float(definition_.shadowOpacity)/100.0f;
    sample_.forceBrightSunAndMoon=replay_.forceBrightSunAndMoon;
    const AreaEnvironmentPreset* source=&definition_.day;
    const AreaEnvironmentPreset* destination=&definition_.day;
    float t=0.0f;
    switch(phase) {
    case AreaTimeOfDay::Day:source=destination=&definition_.day;break;
    case AreaTimeOfDay::Night:source=destination=&definition_.night;break;
    case AreaTimeOfDay::Dawn:source=&definition_.night;destination=&definition_.day;t=replay_.phase?clamp01(replay_.transition):automaticTransition;break;
    case AreaTimeOfDay::Dusk:source=&definition_.day;destination=&definition_.night;t=replay_.phase?clamp01(replay_.transition):automaticTransition;break;
    }
    sample_.transition=t;
    sample_.globalAmbient=mixed(source->globalAmbient,destination->globalAmbient,t);
    sample_.diffuse=mixed(source->diffuse,destination->diffuse,t);
    sample_.fogColor=mixed(source->fogColor,destination->fogColor,t);
    sample_.fogNear=mixed(source->fogNear,destination->fogNear,t);
    sample_.fogFar=mixed(source->fogFar,destination->fogFar,t);
    // The engine bright helper replaces both endpoint light tuples, then
    // re-applies the current endpoint. It does not alter fog or scene ambient.
    if(replay_.forceBrightSunAndMoon) {
        sample_.globalAmbient=model::Vec3{1.0f,1.0f,1.0f};
        sample_.diffuse=model::Vec3{};
    }
    // Explicit captured setter outputs are applied last and therefore remain
    // authoritative over endpoint interpolation and ForceBrightSunAndMoon.
    if(replay_.sceneAmbient)sample_.sceneAmbient=replay_.sceneAmbient;
    if(replay_.globalAmbient)sample_.globalAmbient=replay_.globalAmbient;
    if(replay_.diffuse)sample_.diffuse=replay_.diffuse;
    if(replay_.fogColor)sample_.fogColor=replay_.fogColor;
    if(replay_.fogNear)sample_.fogNear=replay_.fogNear;
    if(replay_.fogFar)sample_.fogFar=replay_.fogFar;
    if(replay_.shadowOpacity)sample_.shadowOpacity=*replay_.shadowOpacity;
    const auto hasFog=[](const AreaEnvironmentPreset& p) {
        return p.fogColor||p.fogNear||p.fogFar||p.fogEnabled;
    };
    sample_.hasFogState=hasFog(*source)||hasFog(*destination)||replay_.fogEnabled.has_value()||
                        replay_.fogColor.has_value()||replay_.fogNear.has_value()||
                        replay_.fogFar.has_value();
    sample_.hasShadowState=replay_.shadowsEnabled.has_value()||definition_.shadowOpacity!=100u||
                           replay_.shadowOpacity.has_value();
    // LoadArea sets the scene-enable bit from SunFogOn once (K1 Desktop
    // 0x68147 / K2 Desktop 0x26cea2). Night/dawn/dusk setters change the
    // numeric tuple only; MoonFogOn is retained authored metadata.
    sample_.fogEnabled=replay_.fogEnabled.value_or(endpointBool(definition_.day.fogEnabled,false));
    // SunShadows/MoonShadows are retained metadata in all four clients; their
    // area update/phase paths do not read them.
    // Only an explicit live scene input
    // selects the enable state; the engine opacity producer remains active.
    sample_.shadowsEnabled=replay_.shadowsEnabled.value_or(true);
}

} // namespace neomdl
