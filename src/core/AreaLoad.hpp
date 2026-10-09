#pragma once
#include "core/AreaObject.hpp"
#include "core/AreaVisualRecords.hpp"
#include "core/AreaEnvironment.hpp"
#include "core/ObjectVisualState.hpp"
#include "core/DirtyArea.hpp"
#include "core/GrassScene.hpp"
#include "core/ResourceContext.hpp"
#include "core/CreatureAppearance.hpp"
#include "core/TrackScene.hpp"
#include "core/SceneAudio.hpp"
#include <optional>

namespace neomdl {
struct AreaLoadOptions {
    // Strict mode aborts on an unresolved template, appearance mapping or model.
    // Existing model texture warnings and unsupported gameplay/actor states
    // remain explicit diagnostics in both modes. Neither makes placeholders.
    bool allowPartialVisuals{};
    bool loadGrass{true};
    model::GameVersion game{model::GameVersion::Kotor2};
};
struct AreaVisualSettings {
    DirtyAreaState dirty;
    AreaEnvironmentDefinition environment;
    // Compatibility endpoint aliases retained for hosts built against the
    // earlier area API. New scene code consumes environment/sample channels.
    std::optional<model::Vec3> ambient, diffuse;
    std::optional<std::uint8_t> wind;
};
struct AreaPlacement {
    AreaObjectKind kind{AreaObjectKind::Placeable};
    std::size_t sourceIndex{};
    std::string label, templateResref, templateIdentity;
    std::uint32_t appearance{};
    model::Mat4 world{model::identityMatrix()};
    model::EngineFrame frame{};
    std::string modelResref, texture, head, headTexture;
    std::optional<CreatureAppearanceState> creatureAuthoredState, creatureState;
    std::optional<CreatureAppearance> creatureAppearance;
    std::optional<AreaObjectTrapRecord> trap;
    bool visible{true};
    bool staticFlag{}; // server metadata, not permission to flatten a Gob
    std::optional<std::uint8_t> animationState, openState;
    ObjectVisualStateLayer authoredVisualState;
    std::optional<std::size_t> runtimeInstance;
    std::vector<std::string> diagnostics;
};
struct AreaLoadPlan {
    std::string area, scopeIdentity;
    neoshared::ResourceDocument layout;
    ResourceResolver resolver;
    std::shared_ptr<ResourceTrace> trace;
    AreaVisualSettings settings;
    std::optional<GrassSettings> grass;
    std::vector<AreaPlacement> placements;
    std::vector<AreaTriggerRecord> triggers;
    std::vector<AreaCameraRecord> cameras;
    std::vector<AreaSoundRecord> sounds;
    std::optional<MiniGameDefinition> miniGame;
    std::vector<std::string> diagnostics;
    model::GameVersion game{model::GameVersion::Kotor2};
};
// Shared GFF/2DA codecs; game schema, visual mapping and mounting policy stay
// here. No gameplay scripts, spawn simulation, inventory mutation or extraction.
AreaLoadPlan readAreaLoad(const std::string& area, const std::string& scopeIdentity,
                             ResourceResolver resolver, const AreaLoadOptions& options = {},
                             const neoshared::bif::JobControl& job = resourceLoadJob());
model::Mat4 gitCreatureTransform(model::Vec3 position, model::Vec3 orientation);
model::Mat4 gitBearingTransform(model::Vec3 position, float bearing);
model::EngineFrame gitCreatureFrame(model::Vec3 position, model::Vec3 orientation);
model::EngineFrame gitBearingFrame(model::Vec3 position, float bearing);
} // namespace neomdl
