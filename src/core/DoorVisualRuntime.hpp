#pragma once

#include "core/SceneQueries.hpp"

namespace neomdl {



struct DoorFadeSettings {
    bool enabled{};
    float minimumAlpha{0.25f};
    float fadeToZeroTime{0.25f};
};

// Gob::Render, K1 Desktop 0x34bd62 / K2 Desktop 0xddb77. Call once when the
// registered owner actually enters Render, before its Parts are drawn. This
// is not an animation tick. id must name that live owner, and the caller must
// provide the elapsed time. An absent focus means currentscene=null.
// The returned field survives skipped draws, disabled fading and SetScene(null).
inline float engineDoorRenderAlpha(SceneQueries& queries, HitOwnerId id,
    model::Vec3 cameraPosition, std::optional<model::Vec3> sceneFocus,
    float deltaT, const DoorFadeSettings& settings) {
    auto& owner=*queries.owner(id);
    if(!settings.enabled || !(owner.model->classification&8u) || !sceneFocus)
        return owner.doorAlpha;

    SceneHitInfo hit;
    hit.surfaceMask=0xbfffffffu;
    hit.checkAlpha=false;
    const bool obstructs=queries.hitOwner(id,cameraPosition,*sceneFocus,hit);
    const float step=deltaT/settings.fadeToZeroTime;
    const float alpha=obstructs ? owner.doorAlpha-step : owner.doorAlpha+step;
    // K1 preserves unordered alpha; K2 selects 1 for that upper comparison.
    // No engine caller clamps DeltaT or the division itself.
    if(owner.model->game==model::GameVersion::Kotor1) {
        const float upper=1.0f<alpha ? 1.0f : alpha;
        owner.doorAlpha=settings.minimumAlpha>upper ? settings.minimumAlpha : upper;
    } else {
        const float upper=alpha<1.0f ? alpha : 1.0f;
        owner.doorAlpha=upper>settings.minimumAlpha ? upper : settings.minimumAlpha;
    }
    return owner.doorAlpha;
}

} // namespace neomdl
