#pragma once

#include "model/Model.hpp"
#include <cmath>
#include <cstdint>
#include <optional>

namespace neomdl {





struct WorldRoomInputs {
    std::uint8_t cameraMode{};
    std::optional<model::Vec3> playerRenderPosition;
    // Presence represents both the module and its current camera.
    std::optional<model::Vec3> moduleCameraPosition;
    bool dialogueActive{};
    bool shotCameraActive{};
    bool animatedCameraActive{};
};

// null means no UpdateRoom call: the engine caller retains the current room.
// GetDialogCameraPosition(0) starts with the player's rendered position and
// substitutes the module camera only while a shot/animated camera is active.
inline std::optional<model::Vec3> engineWorldRoomPosition(const WorldRoomInputs& input) noexcept {
    if(input.cameraMode==7u&&input.moduleCameraPosition)
        return input.moduleCameraPosition;
    if(!input.playerRenderPosition)return {};
    if(input.dialogueActive&&(input.shotCameraActive||input.animatedCameraActive)&&
       input.moduleCameraPosition)return input.moduleCameraPosition;
    return input.playerRenderPosition;
}

struct WorldSceneFocusPlayer {
    model::Vec3 position{};       // logical CSWCObject position
    model::Vec3 renderPosition{}; // interpolated CSWCObject render position
};
struct WorldSceneFocusCamera {
    model::Vec3 position{};
    model::Quaternion orientation{};
};
struct WorldSceneFocusInputs {
    std::optional<WorldSceneFocusPlayer> player;
    std::optional<WorldSceneFocusCamera> moduleCamera;
    bool dialogueActive{};
    bool shotCameraActive{};
    bool animatedCameraActive{};
};






inline std::optional<model::Vec3> engineWorldSceneFocus(const WorldSceneFocusInputs& input) noexcept {
    if(!input.player)return {};
    if(!input.dialogueActive)return input.player->position;
    if(!(input.shotCameraActive||input.animatedCameraActive)||!input.moduleCamera)
        return input.player->renderPosition;
    const auto& camera=*input.moduleCamera;
    const auto& q=camera.orientation;



    model::Vec3 direction{(q.x*q.z+q.w*q.y)*-2.0f,
                          (q.z*q.y-q.x*q.w)*-2.0f,
                          -(1.0f-(q.x*q.x+q.y*q.y)*2.0f)};
    const float length=std::sqrt((direction.x*direction.x+direction.y*direction.y)+
                                 direction.z*direction.z);
    if(static_cast<double>(length)<1.0e-9)direction={1,0,0};
    else {
        const float reciprocal=1.0f/length;
        direction={direction.x*reciprocal,direction.y*reciprocal,direction.z*reciprocal};
    }
    return model::Vec3{camera.position.x+direction.x*5.0f,
                       camera.position.y+direction.y*5.0f,
                       camera.position.z+direction.z*5.0f};
}

} // namespace neomdl
