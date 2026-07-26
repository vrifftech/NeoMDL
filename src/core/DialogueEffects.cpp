#include "core/DialogueEffects.hpp"
#include <cmath>
#include <limits>

namespace neomdl {
void DialogueFade::apply(const neoshared::dlg::FadeFields& fields) {
    const auto type=fields.type.value_or(0);
    if(!type)return;
    // HandleDialogEntry / HandleDialogSelection map 1 and 4 to direction 0.
    fadingIn_=type!=1&&type!=4;
    color_=fields.color.value_or(std::array<float,3>{});
    delay_=fields.delay.value_or(0.0f);
    length_=fields.length.value_or(0.0f);
    alpha_=fadingIn_?1.0f:0.0f;
    elapsed_=0.1f;
}
void DialogueFade::reset() {
    alpha_=delay_=length_=elapsed_=0.0f;
    fadingIn_=false;
}
void DialogueFade::advanceDraw(float frameSeconds) {
    if(elapsed_>0.0f) {
        if(frameSeconds<5.0f)elapsed_+=frameSeconds;
        if(elapsed_>delay_) {
            const float progress=(elapsed_-delay_)/length_;
            alpha_=fadingIn_?1.0f-progress:progress;
            // Engine MINSS/MAXSS choose 1 for NaN.
            if(!(alpha_<1.0f))alpha_=1.0f;
            else if(!(alpha_>0.0f))alpha_=0.0f;
        }
    }
}
namespace {
std::int32_t guiInteger(float value) {
    // CVTTSS2SI returns integer-indefinite on overflow/NaN.
    constexpr auto minimum=std::numeric_limits<std::int32_t>::min();
    if(!(value<2147483648.0f)||value<-2147483648.0f)return minimum;
    return static_cast<std::int32_t>(value);
}
}
std::optional<std::uint32_t> DialogueFade::packedColor(model::GameVersion game) const {
    if(color_==std::array<float,3>{{-1.0f,-1.0f,-1.0f}})return std::nullopt;
    const bool k1=game==model::GameVersion::Kotor1;
    const auto alpha=guiInteger(alpha_*255.0f);
    if(!k1&&alpha<=0)return std::nullopt;
    if(!k1&&color_==std::array<float,3>{{-2.0f,-2.0f,-2.0f}})return 0x96000000u;
    const auto red=static_cast<std::uint32_t>(guiInteger(color_[0]*255.0f));
    const auto green=static_cast<std::uint32_t>(guiInteger(color_[1]*255.0f));
    const auto blue=static_cast<std::uint32_t>(guiInteger(color_[2]*255.0f));
    return red|(green<<8)|(blue<<16)|(static_cast<std::uint32_t>(alpha)<<24);
}
DialogueVideoAction dialogueVideoAction(model::GameVersion game,DialogueVideoCaller caller,
        int row,bool scriptEffectEnabled) {
    using Caller=DialogueVideoCaller;using Op=DialogueVideoOperation;
    if(caller==Caller::CloseDialog)return {Op::Disable,-2};
    if(caller==Caller::ResetComputerShot)
        return {game==model::GameVersion::Kotor2&&scriptEffectEnabled?Op::Retain:Op::Disable,-2};
    if(caller==Caller::ComputerShot) {
        // Unlike UpdateShotCamera, both games map an absent (-1) computer
        // effect to row 0; K1 preserves -2 whereas K2 disables it.
        if(row==-1)row=0;
        if(row!=-2)return {Op::Activate,row};
        return {game==model::GameVersion::Kotor1?Op::Retain:Op::Disable,-2};
    }
    if(game==model::GameVersion::Kotor1) {
        if(caller==Caller::ActorShot)return {Op::Disable,-2};
        return row==-1||row==-2?DialogueVideoAction{}:DialogueVideoAction{Op::Activate,row};
    }
    if(row==-1)row=-2;
    if(row!=-2)return {Op::Activate,row};
    return {scriptEffectEnabled?Op::Retain:Op::Disable,-2};
}
} // namespace neomdl
