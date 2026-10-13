#include "core/WeatherScene.hpp"
#include "core/SceneQueries.hpp"
#include "model/LightningControl.hpp"
#include <algorithm>
#include <cmath>

namespace neomdl {
namespace {
float nextDelay(model::ParticleRandom& random) {return float(random.next()%5000u)/1000.0f+5.0f;}
}
WeatherResources prepareWeatherResources(const ResourceResolver& resolver,const neoshared::bif::JobControl& job,
                                        std::optional<model::GameVersion> asciiGame,
                                        const model::AsciiResourceResolver& asciiResources) {
    ResourceLoadScope work(job);WeatherResources result;
    for(const auto* name:{"fx_rain","fx_snow","fx_lightning01"}) {
        job.update(0,0,std::string("Loading weather model ")+name);
        if(const auto resource=resolver(name,kMdlResourceType)) {
            auto document=std::make_shared<ModelDocument>();document->openResource(*resource,resolver,job,asciiGame,asciiResources,true,false);
            result.models.emplace(name,std::move(document));
        }
    }
    job.check();return result;
}
void WeatherFrameInput::setCameraWorld(const model::Mat4& world) {
    cameraFrame=engineRigidFrame(world);
}
void WeatherScene::configure(AreaWeatherSettings settings,std::uint32_t areaFlags,std::uint8_t windPower,
                             model::GameVersion game) {
    settings_=std::move(settings);areaFlags_=areaFlags;windPower_=windPower;game_=game;
    // LoadArea receives the initial server type=0, active=false state, but
    // starts its 6000 ms client transition regardless of that active bit.
    transition_=6000u;
}
void WeatherScene::start(std::uint8_t type) noexcept {desiredType_=type;transition_=6000u;}
void WeatherScene::stop() noexcept {desiredType_=0;transition_=6000u;}
bool WeatherScene::setWeather(std::uint8_t type,float density,const WeatherVisualCallbacks& host) {
    // CSWCArea::SetWeather K1 Desktop 0x62000 / K2 Desktop 0x26f698.
    const auto erase=[&](std::size_t i) {if(existing_[i])host.destroy(i);existing_[i]=false;};
    if(density==0.0f) {
        for(std::size_t i=0;i<7;++i)erase(i);
        currentType_=0;
    } else {
        if(type!=currentType_) {
            for(std::size_t i=0;i<6;++i)erase(i);
            if(type==2 || (type==1&&(game_==model::GameVersion::Kotor1||settings_.chanceRain.value_or(0)!=0))) {
                const std::string model=type==1?"fx_rain":"fx_snow";
                for(std::size_t i=0;i<6;++i) {
                    existing_[i]=host.create(i,model);
                    if(!existing_[i])return false;
                }
            } else if(type!=0&&type!=1)return false;
            for(std::size_t i=0;i<6;++i)if(existing_[i])host.attach(i,true);
            currentType_=type;
        }
        if(settings_.chanceLightning.value_or(0)!=0) {
            erase(6);
            if(type!=1&&type!=2)return false;
            existing_[6]=host.create(6,"fx_lightning01");
            if(existing_[6])host.attach(6,true);
        } else erase(6);
    }
    const std::string animation=type==1?"rain":type==2?"snow":"";
    for(std::size_t i=0;i<6;++i)if(existing_[i])host.setDensity(i,animation,density/100.0f);
    return true;
}
WeatherFrameOutput WeatherScene::advance(const WeatherFrameInput& input,model::ParticleRandom& random,
                                         const WeatherVisualCallbacks& host) {
    WeatherFrameOutput output;
    // CSWCArea::Update samples/stores its own epoch before weather activity
    // gates. Pause commands can change the world stamp between frame calls;
    // the area must retain the prior sample until this consumer runs again.
    const auto elapsedMilliseconds=engineWorldTimeRemainderMilliseconds(
        input.worldTime,previousWorldTime_,input.dayMilliseconds);
    previousWorldTime_=input.worldTime;
    if(!input.enabled)return output;
    const bool k1=game_==model::GameVersion::Kotor1;


    if(input.serverSchedule&&input.serverOverride==255u&&(areaFlags_&1u)==0u) {
        if(serverRecheck_!=0) {
            serverRecheck_=elapsedMilliseconds>=serverRecheck_?0u:serverRecheck_-elapsedMilliseconds;
        } else {
            const auto roll=random.next()%100u;
            const auto rain=settings_.chanceRain.value_or(0),snow=settings_.chanceSnow.value_or(0),lightning=settings_.chanceLightning.value_or(0);
            // K1 AIUpdate 0x19b04c stops at snow. K2 adds a lightning-only
            // rain-type schedule; that producer is absent from K1.
            const auto chance=rain?rain:snow?snow:(k1?0u:lightning);
            if(chance) {
                const bool active=roll<chance;
                if(active!=serverActive_) {
                    serverType_=rain?1u:snow?2u:1u;serverActive_=active;
                    if(active)start(serverType_);else stop();
                }
            }
            serverRecheck_=300000;
        }
    }
    const float seconds=float(elapsedMilliseconds)/1000.0f;
    const auto thunder=[&](bool distant) {
        // K1 Update's even branch is a no-op and the close strike only fetches
        // GetInGameGui. There is no Thunder call or suffix RNG in that packed.
        if(k1||!input.thunderEnabled)return;
        const auto suffix=(random.next()&1u)?"2":"1";
        output.thunderResources.push_back(std::string(distant?"as_wt_thunderds":"as_wt_thundercl")+suffix);
    };
    host.camera(!k1&&input.cameraFrame?std::optional<model::Vec3>{input.cameraFrame->position}:std::nullopt);
    if(input.playerPosition&&input.cameraFrame) {
        if(!k1) {
            const bool precipitation=std::any_of(existing_.begin(),existing_.begin()+6,[](bool exists){return exists;});
            if(input.roomWeatherDisabled) {if(precipitation)setWeather(0,0.0f,host);}
            else if(!precipitation)setWeather(desiredType_,density_,host);
        }
        // Update reads Camera::GetPosition/GetOrientation directly before
        // its raw zdir projection (K2 Desktop 0x26e0df..0x26e166).
        const auto& camera=*input.cameraFrame;const auto player=*input.playerPosition;
        auto forward=model::engineQuaternionAxis(camera.orientation,2u);forward.z=0.0f;
        const float length=std::sqrt(forward.x*forward.x+forward.y*forward.y);
        if(double(length)<1.0e-9)forward={1,0,0};else {const float reciprocal=1.0f/length;forward.x*=reciprocal;forward.y*=reciprocal;}
        const model::Vec3 right{forward.y,-forward.x,0};
        const model::Vec3 base{camera.position.x,camera.position.y,player.z+8.0f};

        const float yaw=k1
            ?float(std::atan2(-double(forward.x),double(forward.y)))
            :std::atan2(-forward.x,forward.y);
        const float yawDegrees=yaw*57.2957763671875f;
        const float halfYaw=yawDegrees*0.01745329238474369f*0.5f;
        const model::Quaternion orientation{0,0,std::sin(halfYaw),std::cos(halfYaw)};

        constexpr std::array<model::Vec3,6> offsets{{{0,4.5f,0},{-3.8971145f,2.24999976f,5},
            {-3.8971145f,-2.24999976f,5},{0,-4.5f,0},{3.8971145f,-2.24999976f,5},{3.8971145f,2.24999976f,5}}};
        if(k1) {
            // K1 Desktop 0x647c9..0x64d35: player-centered 3-by-2 grid,
            // normalize(cross(forward,zaxis))*10, with the near middle raised.
            auto stepRight=right;
            const float rightLength=std::sqrt(stepRight.x*stepRight.x+stepRight.y*stepRight.y);
            if(double(rightLength)<1.0e-9)stepRight={1,0,0};
            else {const float reciprocal=1.0f/rightLength;stepRight.x*=reciprocal;stepRight.y*=reciprocal;}
            stepRight.x*=10.0f;stepRight.y*=10.0f;
            const model::Vec3 stepForward{forward.x*10.0f,forward.y*10.0f,0};
            const model::Vec3 origin{player.x-stepRight.x,player.y-stepRight.y,player.z+15.0f};
            for(std::size_t row=0;row<2;++row)for(std::size_t column=0;column<3;++column) {
                const auto i=row*3+column;
                if(!existing_[i])continue;
                model::Vec3 position{float(column)*stepRight.x+origin.x+float(row)*stepForward.x,
                    float(column)*stepRight.y+origin.y+float(row)*stepForward.y,origin.z};
                if(row!=0||column!=1)position.z-=5.0f;
                host.setFrame(i,{position,orientation});
            }
        } else {
            for(std::size_t i=0;i<6;++i)if(existing_[i]) {
                const auto offset=offsets[i];
                const model::Vec3 position{base.x+right.x*offset.x+forward.x*offset.y,
                    base.y+right.y*offset.x+forward.y*offset.y,base.z+offset.z};
                host.setFrame(i,{position,orientation});
            }
        }
        if((k1||!input.roomWeatherDisabled)&&existing_[6]) {
            bool allowNew=true;
            if(lightningDuration_!=0.0f) {
                if(seconds<lightningDuration_) {lightningDuration_-=seconds;allowNew=false;}
                else {
                    lightningDuration_=0;output.shadowAlphaWrite=savedShadowAlpha_;
                    lightningDelay_=nextDelay(random);host.attach(6,false);
                }
            }
            bool wait=false;
            if(lightningDelay_!=0.0f) {
                if(seconds<lightningDelay_) {lightningDelay_-=seconds;wait=true;}
                else lightningDelay_=0;
            }
            if(!wait&&allowNew) {
                lightningDelay_=nextDelay(random);
                if(random.next()%100u<settings_.chanceLightning.value_or(0)) {
                    if((random.next()&1u)==0)thunder(true);
                    else {
                        model::Vec3 position=player;position.z+=10.0f;
                        const auto signX=random.next();const float x=float(random.next()%100u)/10.0f;
                        position.x+=(signX&1u)?x:-x;
                        const auto signY=random.next();const float y=float(random.next()%100u)/10.0f;
                        position.y+=(signY&1u)?y:-y;
                        host.attach(6,true);host.setFrame(6,{position,{}});
                        const std::string animation="flash0"+std::to_string(random.next()%3u+1u);
                        lightningDuration_=host.flash(6,animation);
                        savedShadowAlpha_=output.shadowAlphaWrite.value_or(input.shadowAlpha);
                        output.shadowAlphaWrite=savedShadowAlpha_*0.4f;
                        thunder(false);
                    }
                }
            }
        }
    }
    // The engine area update also advances this otherwise dormant wind timer.
    // Its discarded first draw is observable in the shared scene random stream.
    if(windPower_==1u||windPower_==2u) {
        if(seconds<windTimer_)windTimer_-=seconds;
        else {random.next();windTimer_=nextDelay(random);}
    }
    if(transition_) {
        if(transition_<=elapsedMilliseconds) {transition_=0;setWeather(desiredType_,100.0f,host);}
        else {
            density_=float(desiredType_?6000u-transition_:transition_)/6000.0f*100.0f;
            if(desiredType_<=2u)setWeather(desiredType_?desiredType_:currentType_,density_,host);
            transition_-=elapsedMilliseconds;
        }
    }
    return output;
}
} // namespace neomdl
