#pragma once

#include "core/ModelDocument.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neogff { class GffStruct; }
namespace neomdl {

struct CreatureEquipmentItem {
    std::uint32_t slot{};
    std::string resref, identity;
    std::int32_t baseItem{30};
    std::uint8_t modelVariation{1}, bodyVariation{}, textureVariation{};
    std::uint8_t modelType{};
    std::uint8_t itemType{};
    std::string itemClass, defaultModel;
    bool genderSpecific{};
    std::optional<std::uint16_t> disguiseAppearance;
};

// CCAppearanceInfo inputs retained separately from the resolved model assets.
// PM_Appearance is the pre-disguise appearance, not the displayed row.
struct CreatureAppearanceState {
    std::string tag;
    std::uint16_t appearance{};
    std::uint8_t phenotype{}, gender{}, headVariation{1}, headOverride{255};
    bool backupHead{};
    std::int16_t alignment{};
    bool disguised{};
    std::uint16_t originalAppearance{};
    std::optional<std::uint16_t> disguiseAppearance;
    bool armorEquipped{};
    std::uint8_t bodyVariation{}, textureVariation{};
    std::vector<CreatureEquipmentItem> equipment;
    model::GameVersion game{model::GameVersion::Kotor2};
    std::uint16_t race{6}; // CSWRace::CRACE_HUMAN, retained client stats input.
};

struct CreatureAppearanceAttachment {
    std::string model, texture, environmentMap, hook;
    VisualAttachmentKind kind{VisualAttachmentKind::None};
    std::uint32_t equipmentSlot{};
    std::uint8_t equipmentItemType{};
    std::optional<std::size_t> parentAttachment;
    float scale{1.0f};
};

struct CreatureAppearance {
    std::uint16_t appearance{};
    std::uint8_t animationType{11};
    float height{0.5f}; // CSWCCreatureAppearance+0x28; assigned appearance.2da HEIGHT.
    std::string modelType, bodyModel, bodyTexture, bodyEnvironmentMap;
    std::int32_t headRow{-1};
    std::vector<CreatureAppearanceAttachment> attachments;
    std::vector<std::string> diagnostics;
    model::GameVersion game{model::GameVersion::Kotor2};
};

CreatureAppearanceState readCreatureAppearance(const neogff::GffStruct& creature,
    bool useEquipmentTemplates, const ResourceResolver& resolver,
    std::vector<std::string>& diagnostics,
    const neoshared::bif::JobControl& job = resourceLoadJob(),
    model::GameVersion game = model::GameVersion::Kotor2);
CreatureEquipmentItem readCreatureEquipment(const std::string& resref,
    std::uint32_t slot, const ResourceResolver& resolver,
    const neoshared::bif::JobControl& job = resourceLoadJob(),
    model::GameVersion game = model::GameVersion::Kotor2);
void setCreatureEquipment(CreatureAppearanceState&, std::vector<CreatureEquipmentItem>);
CreatureAppearance resolveCreatureAppearance(const CreatureAppearanceState&,
    const ResourceResolver&, const CreatureAppearance* previousAlignmentState = nullptr,
    const neoshared::bif::JobControl& job = resourceLoadJob());
bool creatureAppearanceChangesGeometry(const CreatureAppearanceState&,
                                      const CreatureAppearanceState&);
std::shared_ptr<ModelDocument> assembleCreatureAppearance(const CreatureAppearance&,
    const ResourceResolver&, const std::string& identitySuffix,
    const neoshared::bif::JobControl& job = resourceLoadJob(),
    const model::AsciiResourceResolver& asciiResources = {});

} // namespace neomdl
