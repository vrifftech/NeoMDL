#pragma once
#include "core/ResourceContext.hpp"
#include "core/SceneEffects.hpp"

namespace neo2da { class TwoDAFile; }

namespace neomdl {
struct VideoEffectActivation {
    SceneEffectsSettings settings;
    // CClientExoAppInternal+0x3a4. A row with no admitted effect retains -2,
    // including a missing table or negative/out-of-range row.
    int effectId{-2};
};

// CClientExoAppInternal::EnableVideoEffect. K1 admits scan noise/saturation;
// K2 additionally admits its clairvoyance, Force Sight and fury columns.
VideoEffectActivation activateVideoEffect(const neo2da::TwoDAFile* table,
    int row,const SceneEffectsSettings& current,model::GameVersion game,
    bool desktopFrameBufferAvailable);
VideoEffectActivation loadVideoEffect(const ResourceResolver&,int row,
    const SceneEffectsSettings& current,model::GameVersion game,
    bool desktopFrameBufferAvailable);

// K2 CClientExoAppInternal::EnableVideoEffect: Desktop 0x1a68f6.
// The row is an ordinal, not the authored 2DA label. Desktop availability is the
// OR of AurFrameBufferModificationsAvailable and its ATI counterpart.
VideoEffectActivation activateK2VideoEffect(const neo2da::TwoDAFile* table,
    int row,const SceneEffectsSettings& current,
    bool desktopFrameBufferAvailable);

// Worker-side resource adapter. Uses the caller's captured resolver and active
// ResourceLoadScope; a missing VideoEffects.2da performs the engine disable
// prefix. The shared 2DA reader reports malformed-resource errors unchanged.
VideoEffectActivation loadK2VideoEffect(const ResourceResolver&,int row,
    const SceneEffectsSettings& current,
    bool desktopFrameBufferAvailable);
} // namespace neomdl
