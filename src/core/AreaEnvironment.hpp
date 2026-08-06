#pragma once

#include "core/ActivePauseClock.hpp"
#include <model/Lighting.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

// ARE-owned environment inputs. Sun/Moon are names of the engine endpoint
// tuples; neither endpoint contains a direction. Diffuse/global-ambient colors
// are consumed by the scoped gidy_sun model when that resource exists.
struct AreaEnvironmentPreset {
    std::optional<model::Vec3> globalAmbient;
    std::optional<model::Vec3> diffuse;
    std::optional<model::Vec3> fogColor;
    std::optional<float> fogNear, fogFar;
    std::optional<bool> fogEnabled;
    // Compatibility boolean view of authored SunShadows/MoonShadows metadata.
    // All four clients retain these fields without consuming them in their
    // area update/phase paths; they do not produce a scene-enable value.
    std::optional<bool> shadowsEnabled;
    // LoadAreaHeader reads BYTE, then PackArea/LoadArea transport the value as
    // an unnormalized 32-bit BOOL. Preserve the authored byte, including 2..255.
    std::optional<std::uint8_t> shadowFlag;
};

struct AreaWeatherSettings {
    std::optional<std::uint8_t> chanceRain, chanceSnow, chanceLightning;
    std::optional<std::uint8_t> currentWeather;
    std::optional<bool> disabled;
    std::uint32_t areaFlags{}; // engine ARE Flags bit 0 disables the server scheduler
};

// Module IFO inputs, distinct from the area's endpoint colors. Defaults and
// field widths come from CSWSModule::LoadModuleStart in both Desktop executables.
struct ModuleTimeSettings {
    std::uint8_t minutesPerHour{},dawnHour{},duskHour{};
    std::uint32_t startYear{1340};
    std::uint8_t startMonth{6},startDay{1},startHour{23};
    std::uint16_t startMinute{},startSecond{},startMillisecond{};
    std::uint32_t transitionMilliseconds{};
};

struct AreaEnvironmentDefinition {
    // CAurScene ambient is distinct from gidy_sun_amb. Older viewer builds
    // collapsed SunAmbientColor into this channel; keep them separate here.
    std::optional<model::Vec3> sceneAmbient;
    AreaEnvironmentPreset day, night;
    bool dayNightCycle{};
    bool startsNight{};
    // CSWArea constructors use 50; SetAsDay/SetAsNight divide by 100.
    std::uint8_t shadowOpacity{50u};
    AreaWeatherSettings weather;
    std::optional<ModuleTimeSettings> moduleTime;
};

enum class AreaTimeOfDay : std::uint8_t { Day, Night, Dawn, Dusk };

struct ModuleTimeState {
    std::uint32_t calendarDay{},timeOfDayMilliseconds{};
    std::uint32_t year{},month{},day{},hour{};
    AreaTimeOfDay phase{AreaTimeOfDay::Day};
    std::uint32_t transitionMilliseconds{};
    float transitionSeconds{},transitionDurationSeconds{};
};

// The desktop K1/K2 module timer, server notification and
// client interpolation callers share this clock contract.
constexpr bool worldTimeRuntimeSupported() noexcept { return true; }

struct AreaReplayCamera {
    bool active{};
    // Rigid camera-to-world transform. The renderer derives the view matrix.
    // The camera adapter does not select a room. AreaScene separately applies
    // engine player/camera arbitration or an explicit inspection-camera query
    // to the loaded WOK surfaces. Explicit room overrides take precedence.
    model::Mat4 world{model::identityMatrix()};
    float viewAngle{45.0f};
    float nearClip{0.1f};
    float farClip{10000.0f};
    std::optional<std::uint64_t> currentRoom;
    // Explicit temporal-history cut supplied by a capture/replay adapter.
    bool cut{};
};

struct AreaEnvironmentReplayState {
    std::optional<AreaTimeOfDay> phase;
    // Dawn: night -> day. Dusk: day -> night. Ignored for endpoint phases.
    float transition{0.0f};
    // Direct outputs of the checked engine setters. These are explicit
    // capture/replay values, not additional ARE fields. They override the
    // selected/interpolated endpoint for this replay snapshot.
    std::optional<model::Vec3> sceneAmbient;
    std::optional<model::Vec3> globalAmbient;
    std::optional<model::Vec3> diffuse;
    std::optional<model::Vec3> fogColor;
    std::optional<float> fogNear, fogFar;
    std::optional<bool> fogEnabled;
    std::optional<bool> shadowsEnabled;
    std::optional<float> shadowOpacity;
    // CSWCArea::ForceBrightSunAndMoon writes both endpoint diffuse tuples to
    // zero and both global-ambient tuples to one before applying the current
    // endpoint. The unrelated engine byte that it also clears is not given a
    // guessed meaning here.
    bool forceBrightSunAndMoon{};
    std::optional<std::uint64_t> currentRoom;
    std::optional<AreaReplayCamera> camera;
};

struct AreaEnvironmentSample {
    AreaTimeOfDay phase{AreaTimeOfDay::Day};
    float transition{};
    std::optional<model::Vec3> sceneAmbient;
    std::optional<model::Vec3> globalAmbient;
    std::optional<model::Vec3> diffuse;
    std::optional<model::Vec3> fogColor;
    std::optional<float> fogNear, fogFar;
    bool fogEnabled{};
    bool shadowsEnabled{true};
    bool hasFogState{};
    bool hasShadowState{};
    float shadowOpacity{1.0f};
    bool forceBrightSunAndMoon{};
};

struct AreaEnvironmentTraceEntry {
    std::uint64_t sequence{};
    std::string operation;
    std::string detail;
    bool applied{};
};

const char* areaTimeOfDayName(AreaTimeOfDay) noexcept;
model::Mat4 inverseAreaReplayCamera(const model::Mat4&) noexcept;
bool validAreaReplayCamera(const AreaReplayCamera&, std::string&) noexcept;

// Module/area state adapter. A host supplies the absolute world epoch and the
// client frame delta; it does not read the wall clock or execute scripts. Explicit
// replay outputs take precedence over the module's automatic phase producer.
class AreaEnvironmentRuntime final {
public:
    void setDefinition(AreaEnvironmentDefinition definition,bool automaticWorldTime=true);
    const AreaEnvironmentDefinition& definition() const noexcept {return definition_;}
    const AreaEnvironmentReplayState& replay() const noexcept {return replay_;}
    const AreaEnvironmentSample& sample() const noexcept {return sample_;}
    const std::vector<AreaEnvironmentTraceEntry>& trace() const noexcept {return trace_;}
    const AreaReplayCamera* activeCamera() const noexcept;
    std::optional<std::uint64_t> replayCurrentRoom() const noexcept;
    std::uint64_t cameraHistoryEpoch() const noexcept {return cameraHistoryEpoch_;}

    const std::optional<ModuleTimeState>& worldTime() const noexcept {return worldTime_;}
    std::optional<std::uint32_t> worldDayMilliseconds() const noexcept;
    // Server UpdateTime receives absolute date/time plus the DWORD elapsed
    // remainder from its own prior sample. Client Module::Update follows it.
    void advanceWorldTime(EngineWorldTime current,float clientSeconds,bool paused=false);
    void resetWorldTime();
    // Host profile changes rebuild only the clock; explicit replay survives.
    void setAutomaticWorldTime(bool enabled);

    bool applyReplay(AreaEnvironmentReplayState state, std::string& error);
    void clearReplay();

private:
    void rebuildSample();
    void record(std::string operation,std::string detail,bool applied);
    AreaEnvironmentDefinition definition_;
    AreaEnvironmentReplayState replay_;
    AreaEnvironmentSample sample_;
    std::optional<ModuleTimeState> worldTime_;
    bool automaticWorldTime_{};
    std::vector<AreaEnvironmentTraceEntry> trace_;
    std::uint64_t sequence_{};
    std::uint64_t cameraHistoryEpoch_{};
};

} // namespace neomdl
