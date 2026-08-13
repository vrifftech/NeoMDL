#pragma once

#include "core/AreaEnvironment.hpp"
#include "core/ModelDocument.hpp"
#include <model/Emitter.hpp>
#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace neomdl {

struct WeatherResources {
    std::map<std::string,std::shared_ptr<ModelDocument>> models;
};
WeatherResources prepareWeatherResources(const ResourceResolver&,
    const neoshared::bif::JobControl& job=resourceLoadJob(),
    std::optional<model::GameVersion> asciiGame=std::nullopt,
    const model::AsciiResourceResolver& asciiResources = {});

// Desktop client/server weather callers are supported for both games.
constexpr bool weatherRuntimeSupported(model::GameVersion) noexcept {
    return true;
}

// Explicit inputs to the checked server/client area weather callers. A viewer
// camera is used only when its host explicitly supplies it as module camera.
struct WeatherFrameInput {
    bool enabled{};
    bool serverSchedule{true};
    EngineWorldTime worldTime;
    std::uint32_t dayMilliseconds{7200000u};
    std::optional<model::Vec3> playerPosition;
    std::optional<model::EngineFrame> cameraFrame;
    // Explicit matrix-authored inspection input; engine cameras supply their
    // stored position/quaternion directly through cameraFrame.
    void setCameraWorld(const model::Mat4&);
    bool roomWeatherDisabled{}; // K2 GetRoomDisableWeather result; K1 has no caller.
    std::uint8_t serverOverride{255};
    bool thunderEnabled{true};
    float shadowAlpha{0.5f};
};
struct WeatherVisualCallbacks {
    // Slots 0..5 are precipitation Gobs; slot 6 is the lightning Gob.
    std::function<bool(std::size_t,const std::string&)> create;
    std::function<void(std::size_t)> destroy;
    std::function<void(std::size_t,bool)> attach;
    std::function<void(std::size_t,const model::EngineFrame&)> setFrame;
    std::function<void(std::size_t,const std::string&,float)> setDensity;
    std::function<float(std::size_t,const std::string&)> flash;
    std::function<void(std::optional<model::Vec3>)> camera;
};
struct WeatherFrameOutput {
    std::optional<float> shadowAlphaWrite;
    std::vector<std::string> thunderResources;
};

// Engine state, scheduling and producer commands. Model ownership/rendering is
// supplied by AreaScene; this class never invents precipitation geometry.
class WeatherScene final {
public:
    // A raw layout identifies a game but has no ARE/client LoadArea packet.
    // Selecting its caller family does not start a weather transition.
    void setGame(model::GameVersion game) noexcept {game_=game;}

    void configure(AreaWeatherSettings settings,std::uint32_t areaFlags,std::uint8_t windPower,
                   model::GameVersion game=model::GameVersion::Kotor2);
    // Explicit inspector load/reset rebases the retained area update stamp.
    void resetWorldTime(EngineWorldTime current) noexcept {previousWorldTime_=current;}
    void start(std::uint8_t type) noexcept;
    void stop() noexcept;
    bool setWeather(std::uint8_t type,float density,const WeatherVisualCallbacks&);
    WeatherFrameOutput advance(const WeatherFrameInput&,model::ParticleRandom&,
                               const WeatherVisualCallbacks&);
    std::uint8_t currentType() const noexcept {return currentType_;}
    std::uint8_t desiredType() const noexcept {return desiredType_;}
    float density() const noexcept {return density_;}
    std::uint32_t transitionMilliseconds() const noexcept {return transition_;}
    float lightningDelay() const noexcept {return lightningDelay_;}
    float lightningDuration() const noexcept {return lightningDuration_;}
    const std::array<bool,7>& existingGobs() const noexcept {return existing_;}
private:
    AreaWeatherSettings settings_;
    EngineWorldTime previousWorldTime_;
    model::GameVersion game_{model::GameVersion::Kotor2};

    std::uint32_t areaFlags_{},transition_{};
    std::uint8_t windPower_{},currentType_{},desiredType_{},serverType_{};
    bool serverActive_{};
    std::uint32_t serverRecheck_{};
    float density_{},lightningDelay_{},lightningDuration_{},windTimer_{},savedShadowAlpha_{};
    std::array<bool,7> existing_{};
};
} // namespace neomdl
