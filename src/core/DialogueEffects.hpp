#pragma once

#include <dlg/Dialogue.hpp>
#include <model/Model.hpp>
#include <array>
#include <cstdint>
#include <optional>

namespace neomdl {
// CSWGuiDialogLetterbox's middle panel. Its clock advances in Draw, after the
// client's dialogue wait check, and persists between nodes until CloseDialog.
class DialogueFade {
public:
    void apply(const neoshared::dlg::FadeFields&);
    void reset();
    void advanceDraw(float frameSeconds);
    bool fading() const { return elapsed_!=0.0f && !(elapsed_>delay_+length_); }
    float alpha() const { return alpha_; }
    const std::array<float,3>& color() const { return color_; }
    // AurGUISetupViewport's packed RGBA bytes, including sentinel colours and
    // engine float-to-int conversion. No draw is represented by nullopt.
    std::optional<std::uint32_t> packedColor(model::GameVersion) const;
private:
    std::array<float,3> color_{};
    float alpha_{},delay_{},length_{},elapsed_{};
    bool fadingIn_{};
};
enum class DialogueVideoCaller : std::uint8_t {
    ActorShot, PlaceableShot, ComputerShot, ResetComputerShot, CloseDialog
};
enum class DialogueVideoOperation : std::uint8_t { Retain, Disable, Activate };
struct DialogueVideoAction {
    DialogueVideoOperation operation{DialogueVideoOperation::Retain};
    int row{-2};
};
// GUI's camera callers are distinct from EnableVideoEffect itself. Animated
// camera success does not invoke any of these shot-effect callers.
// scriptEffectEnabled is K2 GUI+0x1020, written by the VM Enable/Disable commands.
DialogueVideoAction dialogueVideoAction(model::GameVersion,DialogueVideoCaller,
    int authoredRow,bool scriptEffectEnabled);
} // namespace neomdl
