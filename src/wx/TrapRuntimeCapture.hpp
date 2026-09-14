#pragma once

#include "core/TriggerRuntime.hpp"
#include <SimpleJson.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace neomdl::ui::trapcapture {
using J=neojson::Value;
template<class T> T integer(const J& value) {
    if constexpr(std::is_signed_v<T>) {
        const auto result=neojson::toInt64(value,"captured integer");
        if(result<std::numeric_limits<T>::min()||result>std::numeric_limits<T>::max())
            throw std::runtime_error("Captured integer is outside its engine field width");
        return static_cast<T>(result);
    } else {
        const auto result=neojson::toUInt64(value,"captured unsigned integer");
        if(result>std::numeric_limits<T>::max())throw std::runtime_error("Captured integer is outside its engine field width");
        return static_cast<T>(result);
    }
}
inline std::vector<std::uint64_t> ids(const J& value) {
    std::vector<std::uint64_t> result;
    for(const auto& item:value.asArray())result.push_back(integer<std::uint64_t>(item));
    return result;
}
inline TrapInventoryState inventory(const std::string& text) {
    const auto root=neojson::parse(text);TrapInventoryState result;
    result.serverPartyAccess=root.at("serverPartyAccess").asBool();
    result.partyAccessPanelUp=root.at("partyAccessPanelUp").asBool();
    result.topMenuOverrideCharacter=integer<std::int32_t>(root.at("topMenuOverrideCharacter"));
    result.partyItems=ids(root.at("partyItems"));
    for(const auto& entry:root.at("items").asArray()) {
        TrapInventoryItem item;item.objectId=integer<std::uint64_t>(entry.at("objectId"));
        item.tag=entry.at("tag").asString();item.templateResref=entry.at("templateResref").asString();
        item.containedItems=ids(entry.at("containedItems"));
        if(!entry.at("firstActivePropertySubtype").isNull())item.firstActivePropertySubtype=integer<std::uint16_t>(entry.at("firstActivePropertySubtype"));
        if(!entry.at("stackSize").isNull())item.stackSize=integer<std::int32_t>(entry.at("stackSize"));
        if(!entry.at("possessor").isNull())item.possessor=integer<std::uint64_t>(entry.at("possessor"));
        result.items.push_back(std::move(item));
    }
    for(const auto& entry:root.at("actors").asArray()) {
        TrapActorInventory actor;actor.actor=integer<std::uint64_t>(entry.at("actor"));
        actor.inParty=entry.at("inParty").asBool();actor.availablePartyMember=entry.at("availablePartyMember").asBool();
        actor.personalItems=ids(entry.at("personalItems"));
        const auto& equipment=entry.at("equippedItems").asArray();
        if(equipment.size()!=actor.equippedItems.size())throw std::runtime_error("equippedItems must contain 20 slot entries (null for an empty slot)");
        for(std::size_t i=0;i<equipment.size();++i)if(!equipment[i].isNull())
            actor.equippedItems[i]=integer<std::uint64_t>(equipment[i]);
        result.actors.push_back(std::move(actor));
    }
    return result;
}
inline J number(std::uint64_t value) {return J::number(std::to_string(value));}
inline J idArray(const std::vector<std::uint64_t>& values) {
    J::Array result;for(const auto value:values)result.push_back(number(value));return J::array(std::move(result));
}
inline std::string inventoryText(const TrapInventoryState& inventory) {
    J::Array items,actors;
    for(const auto& item:inventory.items)items.push_back(J::object({{"objectId",number(item.objectId)},
        {"tag",J::string(item.tag)},{"templateResref",J::string(item.templateResref)},
        {"containedItems",idArray(item.containedItems)},
        {"firstActivePropertySubtype",item.firstActivePropertySubtype?number(*item.firstActivePropertySubtype):J::null()},
        {"stackSize",item.stackSize?J::number(std::to_string(*item.stackSize)):J::null()},
        {"possessor",item.possessor?number(*item.possessor):J::null()}}));
    for(const auto& actor:inventory.actors) {
        J::Array equipment;for(const auto id:actor.equippedItems)equipment.push_back(id?number(*id):J::null());
        actors.push_back(J::object({{"actor",number(actor.actor)},{"inParty",J::boolean(actor.inParty)},
            {"availablePartyMember",J::boolean(actor.availablePartyMember)},{"personalItems",idArray(actor.personalItems)},
            {"equippedItems",J::array(std::move(equipment))}}));
    }
    return neojson::stringify(J::object({{"serverPartyAccess",J::boolean(inventory.serverPartyAccess)},
        {"partyAccessPanelUp",J::boolean(inventory.partyAccessPanelUp)},
        {"topMenuOverrideCharacter",J::number(std::to_string(inventory.topMenuOverrideCharacter))},
        {"partyItems",idArray(inventory.partyItems)},{"items",J::array(std::move(items))},{"actors",J::array(std::move(actors))}}));
}
inline TriggerMineFireInput firing(const J& root) {
    TriggerMineFireInput f;
    f.targetIsCreature=root.at("targetIsCreature").asBool();f.creatorIsCreature=root.at("creatorIsCreature").asBool();
    f.creatorHasFaction=root.at("creatorHasFaction").asBool();f.force=root.at("force").asBool();
    f.mineImmune=root.at("mineImmune").asBool();f.targetFaction=integer<std::uint32_t>(root.at("targetFaction"));
    f.reputationToCreator=integer<std::int32_t>(root.at("reputationToCreator"));
    f.reputationToTrigger=integer<std::int32_t>(root.at("reputationToTrigger"));
    f.feat238=root.at("feat238").asBool();f.partyMember=root.at("partyMember").asBool();
    f.partyLeaderHasServerCreature=root.at("partyLeaderHasServerCreature").asBool();
    f.partyLeaderFeat239=root.at("partyLeaderFeat239").asBool();return f;
}
inline TriggerMineEnterInput enter(const std::string& text) {
    const auto root=neojson::parse(text);TriggerMineEnterInput result;
    result.actor=integer<std::uint64_t>(root.at("actor"));result.firing=firing(root);return result;
}
inline TrapActionInput action(const std::string& text) {
    const auto root=neojson::parse(text);TrapActionInput result;
    result.actor=integer<std::uint64_t>(root.at("actor"));
    result.demolitionsRank=integer<std::int8_t>(root.at("demolitionsRank"));
    result.inCombat=root.at("inCombat").asBool();result.actorPartyMember=root.at("actorPartyMember").asBool();
    result.actorPlayerFlag=root.at("actorPlayerFlag").asBool();result.creatorIsCreature=root.at("creatorIsCreature").asBool();
    result.creatorPartyMember=root.at("creatorPartyMember").asBool();result.creatorPlayerFlag=root.at("creatorPlayerFlag").asBool();
    result.difficulty=integer<std::uint8_t>(root.at("difficulty"));result.firing=firing(root.at("firing"));
    auto draws=std::make_shared<std::vector<std::int32_t>>();
    for(const auto& draw:root.at("nativeRand").asArray())draws->push_back(integer<std::int32_t>(draw));
    if(result.inCombat&&draws->empty())throw std::runtime_error("Combat trap completion needs its captured engine random output");
    result.engineRand=[draws,index=std::size_t{}]() mutable {return draws->at(index++);};return result;
}
inline TrapObjectKind kind(const J& value) {
    const auto& name=value.asString();
    if(name=="mine")return TrapObjectKind::Mine;
    if(name=="door")return TrapObjectKind::Door;
    if(name=="placeable")return TrapObjectKind::Placeable;
    throw std::runtime_error("Trap kind must be mine, door or placeable");
}
inline TrapSetMineInput setMine(const std::string& text) {
    const auto root=neojson::parse(text);TrapSetMineInput result;
    result.actor=integer<std::uint64_t>(root.at("actor"));result.item=integer<std::uint64_t>(root.at("item"));
    result.newObjectId=integer<std::uint64_t>(root.at("newObjectId"));
    if(const auto* linked=root.find("linkedDoorObjectId");linked&&!linked->isNull())
        result.linkedDoorObjectId=integer<std::uint64_t>(*linked);
    if(!root.at("target").isNull()) {
        const auto& target=root.at("target");
        result.target=TrapObjectTarget{kind(target.at("kind")),integer<std::size_t>(target.at("sourceIndex"))};
    }
    const auto& position=root.at("actorPosition").asArray();
    if(position.size()!=3)throw std::runtime_error("actorPosition must contain x, y and z");
    result.actorPosition={static_cast<float>(neojson::toDouble(position[0],"x")),static_cast<float>(neojson::toDouble(position[1],"y")),static_cast<float>(neojson::toDouble(position[2],"z"))};
    result.actorFaction=integer<std::uint32_t>(root.at("actorFaction"));
    result.demolitionsRank=integer<std::int8_t>(root.at("demolitionsRank"));
    result.baseDemolitionsRank=integer<std::int8_t>(root.at("baseDemolitionsRank"));
    result.inCombat=root.at("inCombat").asBool();result.areaPresent=root.at("areaPresent").asBool();
    result.areaCanSetMines=root.at("areaCanSetMines").asBool();result.difficulty=integer<std::uint8_t>(root.at("difficulty"));
    result.firing=firing(root.at("firing"));
    auto draws=std::make_shared<std::vector<std::int32_t>>();
    for(const auto& draw:root.at("nativeRand").asArray())draws->push_back(integer<std::int32_t>(draw));
    result.engineRand=[draws,index=std::size_t{}]() mutable {return draws->at(index++);};return result;
}
inline TriggerDetectionInput detection(const std::string& text) {
    const auto root=neojson::parse(text);TriggerDetectionInput result;
    result.objectId=integer<std::uint64_t>(root.at("objectId"));result.faction=integer<std::uint32_t>(root.at("faction"));
    result.elapsedSinceCheckMilliseconds=integer<std::uint32_t>(root.at("elapsedSinceCheckMilliseconds"));
    const auto& position=root.at("position").asArray();
    if(position.size()!=3)throw std::runtime_error("position must contain x, y and z");
    result.position={static_cast<float>(neojson::toDouble(position[0],"x")),static_cast<float>(neojson::toDouble(position[1],"y")),static_cast<float>(neojson::toDouble(position[2],"z"))};
    result.serverAnimation=integer<std::uint16_t>(root.at("serverAnimation"));
    result.detectionMode=integer<std::uint8_t>(root.at("detectionMode"));result.difficulty=integer<std::uint8_t>(root.at("difficulty"));
    result.awarenessRank=integer<std::int8_t>(root.at("awarenessRank"));
    result.fivePointPenalty=root.at("fivePointPenalty").asBool();result.partyMember=root.at("partyMember").asBool();
    result.areaPresent=root.at("areaPresent").asBool();result.partyObjectIds=ids(root.at("partyObjectIds"));
    for(const auto& entry:root.at("areaObjects").asArray()) {
        TriggerDetectionInput::Object object;object.exists=entry.at("exists").asBool();
        if(!entry.at("trap").isNull()) {
            const auto& target=entry.at("trap");
            object.trapTarget=TrapObjectTarget{kind(target.at("kind")),integer<std::size_t>(target.at("sourceIndex"))};
        }
        result.areaObjects.push_back(object);
    }
    auto reputation=std::make_shared<std::map<std::pair<std::uint64_t,std::uint64_t>,std::int32_t>>();
    for(const auto& entry:root.at("reputations").asArray())
        reputation->emplace(std::make_pair(integer<std::uint64_t>(entry.at("from")),integer<std::uint64_t>(entry.at("toward"))),integer<std::int32_t>(entry.at("value")));
    result.reputation=[reputation](auto from,auto toward) {
        const auto found=reputation->find({from,toward});
        if(found==reputation->end())throw std::runtime_error("Detection capture has no reputation for a queried object pair");
        return found->second;
    };
    auto draws=std::make_shared<std::vector<std::int32_t>>();
    for(const auto& value:root.at("nativeRand").asArray())draws->push_back(integer<std::int32_t>(value));
    const auto needed=static_cast<std::size_t>(std::count_if(result.areaObjects.begin(),result.areaObjects.end(),[](const auto& object){return object.exists;}));
    if(draws->size()<needed)throw std::runtime_error("Supply one captured libc rand output for every existing area object, including non-traps");
    result.engineRand=[draws,index=std::size_t{}]() mutable {return draws->at(index++);};
    return result;
}
} // namespace neomdl::ui::trapcapture
