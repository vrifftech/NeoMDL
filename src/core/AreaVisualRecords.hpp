#pragma once

#include "model/Model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

enum class TrapObjectKind { Mine, Door, Placeable };
struct TrapObjectTarget {
    TrapObjectKind kind{TrapObjectKind::Mine};
    std::size_t sourceIndex{};
};
// Server trap fields retained independently of the visual model. TrapFlag is
// the armed field; the runtime flagged/detected-by fields begin empty.
struct AreaObjectTrapRecord {
    TrapObjectTarget target;
    std::uint64_t objectId{0x7f000000u};
    std::uint32_t faction{};
    model::Vec3 position{};
    std::uint8_t detectable{}, disarmable{}, armed{}, oneShot{}, trapType{255};
    std::uint8_t detectDc{}, disarmDc{};
    std::uint8_t locked{},openLockDc{},openLockDifficulty{},pcLevelAtSpawn{},linkedToFlags{};
    std::int8_t openLockDifficultyModifier{};
    bool doorVisibleModel{true};
    std::string onTrapTriggered, onDisarm, onOpen, onUnlock;
};

// Visual/runtime subset of a GIT TriggerList record. Geometry is retained in
// both its serialized trigger-local form and the world-space form consumed by
// PartTrigger. Gameplay scripts and transition execution remain out of scope.
struct AreaTriggerRecord {
    std::size_t sourceIndex{};
    std::uint64_t objectId{0x7f000000u};
    std::string label;
    std::string tag;
    std::string templateResref;
    std::string templateIdentity;
    model::Vec3 position{};
    model::Vec3 orientation{};
    std::vector<model::Vec3> localFootprint;
    std::vector<model::Vec3> worldFootprint;

    // CSWSTrigger::LoadTrigger inputs which affect the client-side visual or
    // identify the trigger to an inspection/replay host.
    std::int32_t type{};
    float highlightHeight{0.1f};
    std::uint8_t cursor{};
    std::uint8_t linkedToFlags{};
    std::string linkedTo;
    std::string linkedToModule;
    std::string transitionDestination;
    std::uint32_t faction{};
    std::uint8_t trapType{};
    bool trapFlag{};
    bool trapOneShot{};
    bool trapDisarmable{};
    bool trapDetectable{};
    bool setByPlayerParty{};
    std::vector<std::string> diagnostics;
    std::string modelResref; // engine mine model; other triggers own PartTrigger
    std::optional<std::size_t> runtimeInstance;
    std::string keyName, onTrapTriggered, onEnter, onDisarm;
    bool autoRemoveKey{};
};

// Server geometry is sent with an unsigned-byte count. The client retains that
// prefix and raises its Z by 0.05 before creating either visual representation.
std::vector<model::Vec3> engineTriggerClientFootprint(const std::vector<model::Vec3>& worldFootprint);

// K2 GIT CameraList entry loaded by CSWSArea::LoadPlaceableCameras. Pitch,
// height and microphone range are retained dialogue-camera inputs; they are not
// folded into the serialized position/orientation transform.
struct AreaCameraRecord {
    std::size_t sourceIndex{};
    std::int32_t cameraId{-1};
    model::Vec3 position{};
    model::Quaternion orientation{};
    float pitch{};
    float height{};
    float fieldOfView{55.0f};
    float microphoneRange{};
    std::vector<std::string> diagnostics;
};

} // namespace neomdl
