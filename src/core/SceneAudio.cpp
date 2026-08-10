#include "core/SceneAudio.hpp"
#include "core/AreaEnvironment.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace neomdl {
std::string engineSceneAudioResRef(const std::string& value) {
    std::string result;
    for(std::size_t i=0;i<16u&&i<value.size()&&value[i];++i) {
        const auto c=value[i];result.push_back(c>='A'&&c<='Z'?char(c+('a'-'A')):c);
    }
    return result;
}
std::vector<SceneAudioPriority> loadSceneAudioPriorities(const ResourceResolver& resolver) {
    const auto resource=resolver("PriorityGroups",0x7e1);if(!resource)return {};
    neo2da::TwoDAFile table;table.loadBytes(resource->bytes);resourceLoadJob().check();
    // Engine row count is transported into an unsigned byte before allocation.
    std::vector<SceneAudioPriority> result(std::uint8_t(table.rowCount()));
    for(std::size_t i=0;i<result.size();++i) {
        auto& p=result[i];int value{};float low{},high{};
        if(engine2da::getInt(table,int(i),"Priority",value))p.priority=std::uint8_t(value);
        if(engine2da::getInt(table,int(i),"Volume",value))p.volume=std::uint8_t(value);
        if(engine2da::getInt(table,int(i),"MaxPlaying",value))p.maxPlaying=std::uint8_t(value);
        if(engine2da::getInt(table,int(i),"Interrupt",value))p.interrupt=value;
        if(engine2da::getInt(table,int(i),"FadeTime",value))p.fadeMilliseconds=std::uint16_t(value);
        if(engine2da::getFloat(table,int(i),"MaxVolumeDist",low)&&
           engine2da::getFloat(table,int(i),"MinVolumeDist",high)) {
            p.minimumDistance=low;p.maximumDistance=high;
        }
        if(engine2da::getFloat(table,int(i),"PlaybackVariance",low))p.pitchVariation=std::clamp(low,0.0f,1.0f);
    }
    return result;
}
float engineSceneAudioGain(const SceneAudioPlay& p,const SceneAudioPriority& priority,float slider,float balance) noexcept {
    if(p.streaming)return ((slider*(float(priority.volume)/127.0f))*float(p.volume))/127.0f;
    const int scaled=int((float(p.volume)*float(priority.volume))/127.0f);
    if(p.positional) {if(balance<1.0f)slider*=balance;}
    else if(balance>1.0f)slider*=2.0f-balance;
    const float value=(float(scaled)*slider)/127.0f;
    if(p.positional)
        return float(std::pow(double(value),double(0.6f)));
    return value;
}
std::uint32_t engineSceneAudioFrequency(const SceneAudioPlay& p,const SceneAudioPriority& priority,
        unsigned rate,const std::function<std::uint32_t()>& random) {
    if(p.streaming)return rate;
    float variance=priority.pitchVariation,fixed=0;
    const auto setPitch=[&](float value) {variance=std::clamp(value,0.0f,1.0f);fixed=0;};
    const auto setFixed=[&](float value) {
        if(value>=0.5f&&value<=2.0f) {fixed=value;variance=0;}
    };
    if(p.oneShot) {
        if(p.fixedVariance!=0)setFixed(p.fixedVariance);
        if(p.pitchVariation!=0)setPitch(p.pitchVariation);
    } else if(p.applyVariations) {setPitch(p.pitchVariation);setFixed(p.fixedVariance);}
    std::uint32_t frequency=rate;
    if(variance!=0) {
        const bool positive=(random()&1u)!=0;
        const float base=positive?float(rate):float(rate)*0.5f;
        const auto amount=std::uint32_t(base*variance);
        if(amount) {const auto delta=random()%amount;frequency=positive?frequency+delta:frequency-delta;}
    } else if(fixed!=0)frequency=std::uint32_t(float(rate)*fixed);
    return frequency;
}
float engineSceneAudioPitch(const SceneAudioPlay& p,const SceneAudioPriority& priority,
        unsigned rate,const std::function<std::uint32_t()>& random) {
    return float(engineSceneAudioFrequency(p,priority,rate,random))/float(rate);
}
SceneAudioPlay mineExplosionAudio(std::uint64_t source,std::string name,model::Vec3 position,
        const SceneAudioPriority& priority) {
    SceneAudioPlay p;p.source=source;p.resref=engineSceneAudioResRef(name);p.oneShot=true;
    p.positional=true;p.position=position;p.priorityGroup=22;p.fixedVariance=0;
    p.minimumDistance=priority.minimumDistance;p.maximumDistance=priority.maximumDistance;
    // The engine prefix comparison uses uppercase characters in CResRef's
    // normalized string. These requests share one streaming-source slot.
    if(p.resref.size()>1&&p.resref[1]=='_') {
        const auto c=p.resref[0];p.streaming=c=='c'||c=='C'||c=='p'||c=='P'||c=='n'||c=='N';
    }
    return p;
}
std::optional<SceneAudioClock> sceneAudioClock(const AreaEnvironmentRuntime& environment) noexcept {
    const auto& time=environment.worldTime();const auto day=environment.worldDayMilliseconds();
    if(!time||!day)return {};
    std::uint8_t phase{};
    switch(time->phase) {
    case AreaTimeOfDay::Day:phase=1;break;
    case AreaTimeOfDay::Night:phase=2;break;
    case AreaTimeOfDay::Dawn:phase=3;break;
    case AreaTimeOfDay::Dusk:phase=4;break;
    }
    return SceneAudioClock{time->calendarDay,time->timeOfDayMilliseconds,*day,time->hour,phase};
}
model::Vec3 engineAudioProviderPosition(model::Vec3 v) noexcept {v.z=-v.z;return v;}
std::optional<SceneAudioStreamMix> engineStreamingAudioMix(const SceneAudioPlay& p,
        const SceneAudioListener& listener,float base,int mode,model::GameVersion game,float obstruction) noexcept {
    const auto position=p.position;
    if((position.x==0&&position.y==0&&position.z==0)||!p.positional||
       !(base>0)||!(p.maximumDistance>0)||!(p.maximumDistance>p.minimumDistance))return {};
    if(mode==3||mode==4)return SceneAudioStreamMix{0,0,false};
    model::Vec3 delta{position.x-listener.position.x,position.y-listener.position.y,position.z-listener.position.z};
    const float distance=std::sqrt((delta.x*delta.x+delta.y*delta.y)+delta.z*delta.z);
    if(distance>p.minimumDistance) {
        if(game==model::GameVersion::Kotor2&&distance>p.maximumDistance)base=0;
        else base*= (p.maximumDistance-distance)/(p.maximumDistance-p.minimumDistance);
    }
    const auto normalize=[](model::Vec3 v) {
        const float magnitude=std::sqrt((v.x*v.x+v.y*v.y)+v.z*v.z);
        if(double(magnitude)<1.0e-9)return model::Vec3{1,0,0};
        const float inverse=1.0f/magnitude;
        return model::Vec3{v.x*inverse,v.y*inverse,v.z*inverse};
    };
    delta=normalize(delta);const auto front=normalize(listener.forward);
    const float dot=front.x*delta.x+front.y*delta.y;
    float compared=delta.x;bool right{};
    if(front.y>0) {
        if(delta.y>0)right=delta.x>front.x;
        else {compared=-compared;right=front.x>compared;}
    } else if(delta.y<0)right=front.x>compared;
    else {compared=-compared;right=compared>front.x;}
    float pan=right?1.0f+std::fabs(dot)*-0.5f:std::fabs(dot)*0.5f;
    pan=std::clamp(pan,0.05f,0.95f);
    if(dot<0)base*=1.0f+dot*0.125f;
    const float input=(1.0f-obstruction)*base;
    // K2 adds explicit negative/zero branches before its shared pow call.
    if(game==model::GameVersion::Kotor2) {
        if(input<0)return SceneAudioStreamMix{0.25f,pan};
        if(input==0)return SceneAudioStreamMix{input,pan};
    }
    float volume=float(std::pow(double(input),double(0.6f)));
    // Preserve MAXSS/MINSS's source order for unordered values.
    volume=std::clamp(volume,0.0f,1.0f);
    return SceneAudioStreamMix{volume,pan};
}
SceneAudioListener engineAudioProviderListener(SceneAudioListener value) noexcept {
    value.position=engineAudioProviderPosition(value.position);
    value.forward=engineAudioProviderPosition(value.forward);
    value.up=engineAudioProviderPosition(value.up);return value;
}
SceneAudioListener placeableCameraAudioListener(model::Vec3 p,model::Quaternion q,float range) noexcept {
    // Quaternion::zdir/ydir use the stored quaternion directly.
    const model::Vec3 z{2.0f*(q.x*q.z+q.w*q.y),2.0f*(q.y*q.z-q.w*q.x),
        1.0f-2.0f*(q.x*q.x+q.y*q.y)};
    const model::Vec3 y{2.0f*(q.x*q.y-q.w*q.z),1.0f-2.0f*(q.x*q.x+q.z*q.z),
        2.0f*(q.y*q.z+q.w*q.x)};
    return {{p.x-z.x*range,p.y-z.y*range,p.z-z.z*range},{-z.x,-z.y,-z.z},y};
}
bool engineAudioWithinRange(model::Vec3 source,model::Vec3 listener,float maximum) noexcept {
    const float x=listener.x-source.x,y=listener.y-source.y,z=listener.z-source.z;
    const float xy=x*x+y*y;return std::sqrt(xy+z*z)<=maximum;
}
void SceneAudioRuntime::load(std::vector<AreaSoundRecord> records,SceneAudioClock clock,SceneAudioRandom random,
        model::GameVersion) {
    records_=std::move(records);random_=std::move(random);states_.assign(records_.size(),{});
    lastTimeOfDay_=clock.engineTimeOfDay;
    for(std::size_t i=0;i<records_.size();++i) {
        const auto& r=records_[i];auto& s=states_[i];s.active=r.active;s.streaming=r.looping;
        s.looping=r.looping||(r.continuous&&r.sounds.size()==1&&!r.randomPosition&&r.interval==0);
        s.restartLoop=s.active&&s.looping;s.last=clock;s.sounds=r.sounds;
        updateInterval(i);
    }
    // CSWCArea::LoadArea broadcasts the already retained module phase after
    // constructing its sound objects (K1 Desktop 0x68719).
    for(std::size_t i=0;i<records_.size();++i)updateTime(i,clock);
}
void SceneAudioRuntime::clear(const SceneAudioBackend& backend) {
    for(std::size_t i=0;i<states_.size();++i)backend.stop(i+1u);
    states_.clear();records_.clear();random_={};
}
void SceneAudioRuntime::updateInterval(std::size_t i) {
    const auto& r=records_[i];auto value=r.interval;
    if(value&&r.intervalVariation) {
        const auto modulus=2u*r.intervalVariation;
        const auto random=random_.exo();
        if(!modulus)throw std::runtime_error("Engine x86 sound interval variance divides by zero");
        const auto remainder=random%modulus;
        value=value-r.intervalVariation+remainder;
        if(value&0x80000000u)value=0; // engine signed clamp after unsigned arithmetic
    }
    states_[i].interval=value;
}
void SceneAudioRuntime::updateTime(std::size_t i,SceneAudioClock clock) {
    const auto& r=records_[i];auto& s=states_[i];
    if(!r.times)s.timeEnabled=(r.hours&(1u<<(clock.hour&31u)))!=0;
    else if(clock.engineTimeOfDay<=4u) {
        if((0x0bu>>clock.engineTimeOfDay)&1u)s.timeEnabled=r.times!=2;
        else if((0x14u>>clock.engineTimeOfDay)&1u)s.timeEnabled=r.times!=1;
    }
}
std::string SceneAudioRuntime::nextSound(std::size_t i) {
    auto& s=states_[i];const auto& r=records_[i];
    if(s.first) {s.first=false;s.index=0;return s.sounds.at(0);}
    if(s.index+1<s.sounds.size())return s.sounds[++s.index];
    if(!r.continuous)return "0";
    if(r.random) {
        std::vector<std::string> shuffled;shuffled.reserve(s.sounds.size());
        while(!s.sounds.empty()) {
            const auto index=random_.crt()%s.sounds.size();
            shuffled.push_back(std::move(s.sounds[index]));s.sounds.erase(s.sounds.begin()+index);
        }
        s.sounds=std::move(shuffled);
    }
    s.index=0;return s.sounds.at(0);
}
std::uint32_t SceneAudioRuntime::start(std::size_t i,const std::string& sound,bool variations,const SceneAudioBackend& backend) {
    const auto& r=records_[i];const auto& s=states_[i];
    SceneAudioPlay p;p.source=i+1u;p.resref=engineSceneAudioResRef(sound);p.positional=r.positional;p.looping=s.looping;
    p.streaming=s.streaming;p.position=r.position;p.volume=r.volume;p.applyVariations=variations;
    p.minimumDistance=r.minimumDistance;p.maximumDistance=r.maximumDistance;
    p.priorityGroup=s.streaming?(r.positional?5u:4u):(r.positional?22u:21u);
    if(variations) {
        if(r.volumeVariation) {
            const auto volume=int(r.volume)-int(r.volumeVariation)+int(random_.crt()%(2u*r.volumeVariation));
            p.volume=std::uint8_t(std::clamp(volume,0,127));
        }
        if(!s.streaming) {p.pitchVariation=r.pitchVariation;p.fixedVariance=r.fixedVariance;}
    }
    return backend.play(p);
}
void SceneAudioRuntime::update(SceneAudioClock clock,const SceneAudioBackend& backend) {
    const bool phaseChanged=lastTimeOfDay_!=clock.engineTimeOfDay;
    for(std::size_t i=0;i<states_.size();++i) {
        auto& s=states_[i];const auto& r=records_[i];
        if(phaseChanged||!r.times)updateTime(i,clock);
        if(!s.active||!s.timeEnabled) {
            if(backend.playing(i+1u))backend.stop(i+1u);
            continue;
        }
        if(s.looping) {
            if(s.restartLoop) {
                s.restartLoop=false;s.first=true;s.duration=start(i,nextSound(i),false,backend);
                if(!s.duration) {s.active=false;s.interval=0;}
            }
            continue;
        }
        if(clock.day<s.last.day || (clock.day==s.last.day&&clock.milliseconds<s.last.milliseconds))
            throw std::runtime_error("Engine sound world-time subtraction has no result for a backward clock");
        auto elapsed=clock.milliseconds-s.last.milliseconds;
        if(elapsed>=clock.dayMilliseconds)elapsed+=clock.dayMilliseconds;
        s.last=clock;
        if(s.duration) {
            if(elapsed<s.duration) {s.duration-=elapsed;continue;}
            s.duration=0;elapsed=0;
        }
        if(s.interval) {
            if(elapsed<s.interval) {s.interval-=elapsed;continue;}
            s.interval=0;
        }
        const auto sound=nextSound(i);
        if(sound=="0") {s.interval=0;s.active=false;continue;}
        s.duration=start(i,sound,true,backend);
        if(!s.duration) {
            if(s.sounds.size()<=1) {s.interval=0;s.active=false;continue;}
            s.sounds.erase(s.sounds.begin()+s.index);if(s.index)s.index--;
        }
        if(r.continuous||s.index+1<s.sounds.size())updateInterval(i);
    }
    lastTimeOfDay_=clock.engineTimeOfDay;
}
void SceneAudioRuntime::play(std::size_t i,SceneAudioClock clock,const SceneAudioBackend& backend) {
    auto& s=states_.at(i);
    if(!backend.playing(i+1u)) {s.duration=s.interval=0;s.first=true;s.last=clock;}
    if(!s.active) {s.active=true;s.interval=0;if(s.looping)s.restartLoop=true;}
}
void SceneAudioRuntime::stop(std::size_t i,const SceneAudioBackend& backend) {
    auto& s=states_.at(i);s.active=false;s.duration=s.interval=0;backend.stop(i+1u);
}
} // namespace neomdl
