#include "core/TriggerRuntime.hpp"
#include "core/AreaLoadTables.hpp"
#include "core/DialogueAnimation.hpp"

#include <algorithm>
#include <cmath>

namespace neomdl {
namespace {
std::uint16_t clientAnimation(std::uint16_t id) noexcept {
    return id==10143u?347u:id==10144u?350u:id;
}
std::optional<std::int32_t> integer(detail::AreaLoadTables& tables,
    const std::string& table,std::uint32_t row,const std::string& column) {
    int value=0;
    if(!engine2da::getInt(tables.table(table),static_cast<int>(row),column,value))return {};
    return value;
}
float nearestRadius(detail::AreaLoadTables& tables) {
    // CalculateNearestPoint K1 0x2919ec / K2 0x44bd42 checks success.
    float value=0.0f;
    if(!engine2da::getFloat(tables.table("ranges"),17,"PrimaryRange",value))value=1.0f;
    return value;
}
std::uint32_t duration(float seconds,model::GameVersion game) noexcept {
    // GetAnimationLength K1 0x5859e / K2 0x21f9a2, followed by
    // AddFireAndForgetAnimation K1 0xceb22 / K2 0x1e96c4.
    const float ms=seconds>0.0f?seconds*1000.0f:0.0f;
    if(game==model::GameVersion::Kotor1) {
        if(!(ms>0.0f))return 0u;
        if(double(ms)>=4294967295.0)return 0xffffffffu;
        return static_cast<std::uint32_t>(ms);
    }
    if(!(double(ms)<9223372036854775808.0))return 0u;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(ms));
}
model::Vec3 nearestPoint(const TriggerMineState& mine,model::Vec3 actor) {
    const model::Vec3 d{actor.x-mine.position.x,actor.y-mine.position.y,actor.z-mine.position.z};
    const float length2=(d.x*d.x+d.y*d.y)+d.z*d.z;
    if(!(length2>0.0001f))return actor;
    const float inverse=1.0f/std::sqrt(length2);
    return {mine.position.x+(d.x*inverse)*mine.nearestPointRadius,
            mine.position.y+(d.y*inverse)*mine.nearestPointRadius,
            mine.position.z+(d.z*inverse)*mine.nearestPointRadius};
}
bool containsId(const std::vector<std::uint64_t>& ids,std::uint64_t id) {
    return std::find(ids.begin(),ids.end(),id)!=ids.end();
}
void addUnique(std::vector<std::uint64_t>& ids,std::uint64_t id) {
    if(!containsId(ids,id))ids.push_back(id);
}
}

bool triggerMineHostile(std::uint32_t faction,std::uint32_t observerFaction,
    std::int32_t reputation) noexcept {
    // Message producer K1 0x28748b / K2 CreateNewLastUpdateObject.
    return reputation<=89 && faction!=observerFaction;
}
std::int32_t triggerDetectionDc(std::int32_t stored,std::uint8_t difficulty,
    model::GameVersion game) noexcept {
    if(game==model::GameVersion::Kotor1)return stored;
    // K2 GetDetectDC 0x44c914; K1 reads the stored field directly.
    return std::max(1,stored+(difficulty==2u?5:difficulty==0u?-5:0));
}
bool triggerMineContains(model::Vec3 minePosition,model::Vec3 actorPosition,float radius) noexcept {
    // CSWSTrigger::InTrigger mine branch K1 0x29164b / K2 0x44bb08.
    const auto x=actorPosition.x-minePosition.x,y=actorPosition.y-minePosition.y,
               z=actorPosition.z-minePosition.z;
    // Constructor 0x294935 reads ranges[17].PrimaryRange into the shared
    // m_fTrapRadiusRange used here. It is not permanently the literal one.
    return (x*x+y*y)+z*z<=radius*radius;
}
bool triggerMineCanFire(std::uint32_t faction,const TriggerMineFireInput& input,
    model::GameVersion game) noexcept {
    // GetCanFireMineOnObject K1 0x2924ae / K2 0x44ec5a.
    if(!input.targetIsCreature)return true;
    bool allowed=input.force;
    if(!allowed) {
        if(input.creatorIsCreature)
            allowed=!input.creatorHasFaction ||
                (input.reputationToCreator<=10 && input.targetFaction!=faction);
        else allowed=input.reputationToTrigger<=10 && input.targetFaction!=faction;
    }
    if(input.mineImmune)allowed=false;
    // K2 alone adds individual feat 238 and party-leader feat 239 immunity.
    if(game==model::GameVersion::Kotor2 &&
       (input.feat238 || (input.partyMember && input.partyLeaderHasServerCreature && input.partyLeaderFeat239)))
        allowed=false;
    return allowed;
}

std::uint8_t objectTrapDetectionDc(TrapObjectKind kind,std::uint8_t stored,
    std::uint8_t difficulty,model::GameVersion game) noexcept {
    if(game==model::GameVersion::Kotor1)return stored;
    const int adjustment=difficulty==2u?5:difficulty==0u?-5:0;
    // K2 Door GetDetectDC 0x469c46 clamps before the caller's byte cast.
    // Placeable 0x3cdb4e adds in AL, then replaces only zero with one.
    if(kind==TrapObjectKind::Door)
        return static_cast<std::uint8_t>(std::max(1,int(stored)+adjustment));
    const auto result=static_cast<std::uint8_t>(int(stored)+adjustment);
    return result?result:1u;
}
bool trapUsesPartyInventory(const TrapActorInventory& actor,const TrapInventoryState& inventory,
    model::GameVersion game) noexcept {
    if(game==model::GameVersion::Kotor1)
        return actor.inParty || (inventory.serverPartyAccess && actor.availablePartyMember && inventory.partyAccessPanelUp);
    return actor.inParty || inventory.topMenuOverrideCharacter!=-1;
}
namespace {
const TrapInventoryItem* inventoryItem(const TrapInventoryState& inventory,std::uint64_t id) {
    if(id==0x7f000000u)return nullptr;
    const auto found=std::find_if(inventory.items.begin(),inventory.items.end(),
        [&](const auto& item){return item.objectId==id;});
    return found==inventory.items.end()?nullptr:&*found;
}
std::optional<std::uint64_t> findInventoryTag(const TrapInventoryState& inventory,
    const std::vector<std::uint64_t>& repository,const std::string& tag) {
    for(const auto id:repository)if(const auto* item=inventoryItem(inventory,id)) {
        if(item->tag==tag)return item->objectId;
        if(const auto nested=findInventoryTag(inventory,item->containedItems,tag))return nested;
    }
    return {};
}
bool inventoryContains(const TrapInventoryState& inventory,const std::vector<std::uint64_t>& repository,
    std::uint64_t id) {
    for(const auto current:repository)if(const auto* item=inventoryItem(inventory,current)) {
        if(current==id||inventoryContains(inventory,item->containedItems,id))return true;
    }
    return false;
}
void removeInventoryId(std::vector<std::uint64_t>& list,std::uint64_t id) {
    const auto found=std::find(list.begin(),list.end(),id);
    if(found!=list.end())list.erase(found);
}
}
void TriggerRuntime::clear() noexcept {mines_.clear();objectTraps_.clear();events_.clear();inventory_={};mineEntry_.reset();pendingMineScript_.reset();trapAction_.reset();lastTrapAction_.reset();lastSetMine_.reset();scheduledTrapEvents_.clear();serverRequests_.clear();detectionAreaObjects_.reset();detectionCaptures_.clear();detectionResults_.clear();}
void TriggerRuntime::loadObjectTraps(const std::vector<AreaObjectTrapRecord>& records,const ResourceResolver& resolver) {
    detail::AreaLoadTables tables{resolver,{}};
    objectTraps_.clear();
    for(const auto& record:records) {
        ObjectTrapState state;static_cast<AreaObjectTrapRecord&>(state)=record;
        if(state.onTrapTriggered.empty()||state.onTrapTriggered=="default")
            state.onTrapTriggered=tables.stringValue("traps",record.trapType,"MineScript");
        state.recoveredItemResref=tables.lookup("traps",record.trapType,"ResRef");
        objectTraps_.push_back(std::move(state));
    }
}
bool TriggerRuntime::flagObjectTrap(TrapObjectTarget target,std::uint8_t value) {
    for(auto& state:objectTraps_)
        if(state.target.kind==target.kind && state.target.sourceIndex==target.sourceIndex) {
            state.flagged=value;return true;
        }
    return false;
}
bool TriggerRuntime::setObjectTrapId(TrapObjectTarget target,std::uint64_t identity) {
    for(auto& state:objectTraps_)
        if(state.target.kind==target.kind && state.target.sourceIndex==target.sourceIndex) {
            state.objectId=identity;return true;
        }
    return false;
}
namespace {
TriggerMineState prepareMineState(const AreaTriggerRecord& record,detail::AreaLoadTables& tables,
    model::GameVersion game,const TriggerRuntime::AnimationLength& length) {
    TriggerMineState state;
    state.sourceIndex=record.sourceIndex;state.objectId=record.objectId;
    state.faction=record.faction;state.position=record.position;
    const auto points=engineTriggerClientFootprint(record.worldFootprint);
    auto minimum=points.front(),maximum=minimum;
    for(const auto& p:points) {
        minimum={std::min(minimum.x,p.x),std::min(minimum.y,p.y),std::min(minimum.z,p.z)};
        maximum={std::max(maximum.x,p.x),std::max(maximum.y,p.y),std::max(maximum.z,p.z)};
    }
    state.clientPosition={minimum.x+(maximum.x-minimum.x)*0.5f,
        minimum.y+(maximum.y-minimum.y)*0.5f,minimum.z+(maximum.z-minimum.z)*0.5f};
    state.detectable=record.trapDetectable;state.oneShot=record.trapOneShot;
    state.removeKey=record.autoRemoveKey;
    state.trapType=record.trapType;state.keyName=record.keyName;
    state.onEnter=record.onEnter;state.onDisarm=record.onDisarm;
    state.onTrapTriggered=record.onTrapTriggered;
    if(state.onTrapTriggered.empty()||state.onTrapTriggered=="default")
        // LoadTrigger copies GetCExoStringEntry's output even on failure,
        // so an absent row/column retains the table's DEFAULT value.
        state.onTrapTriggered=tables.stringValue("traps",record.trapType,"TrapScript");
    state.detectDc=integer(tables,"traps",record.trapType,"DetectDCMod").value_or(0);
    state.disarmDc=integer(tables,"traps",record.trapType,"DisarmDCMod").value_or(0);
    state.nearestPointRadius=nearestRadius(tables);
    if(record.trapType!=255u)
        state.explosionSound=tables.lookup("traps",record.trapType,"ExplosionSound").value_or("");
    state.recoveredItemResref=tables.lookup("traps",record.trapType,"ResRef");
    for(const auto id:{347u,348u,349u,350u}) {
        TriggerAnimationData a;a.identity=static_cast<std::uint16_t>(id);
        a.name=tables.lookup("animations",id,"name").value_or("");
        // All four CSWCObject predicate callers return false on a failed
        // GetINTEntry. K1 Desktop 0xbfd5b branches past its special-range
        // return-one block; successful cells use nonzero truth.
        a.looping=integer(tables,"animations",id,"looping").value_or(0)!=0;
        a.fireForget=integer(tables,"animations",id,"fireforget").value_or(0)!=0;
        a.milliseconds=duration(length(record.sourceIndex,a.name),game);
        state.animations.push_back(std::move(a));
    }
    return state;
}
}
TriggerMineState TriggerRuntime::prepareMine(const AreaTriggerRecord& record,
    const ResourceResolver& resolver,model::GameVersion game,const AnimationLength& length) {
    detail::AreaLoadTables tables{resolver,{}};
    return prepareMineState(record,tables,game,length);
}
void TriggerRuntime::load(const std::vector<AreaTriggerRecord>& records,
    const ResourceResolver& resolver,model::GameVersion game,const AnimationLength& length) {
    clear();game_=game;
    detail::AreaLoadTables tables{resolver,{}};
    for(const auto& record:records)if(record.type==2 && record.runtimeInstance)
        mines_.push_back(prepareMineState(record,tables,game,length));
}
TriggerMineState* TriggerRuntime::mine(std::size_t source) noexcept {
    const auto it=std::find_if(mines_.begin(),mines_.end(),[&](const auto& m){return m.sourceIndex==source;});
    return it==mines_.end()?nullptr:&*it;
}
const TriggerMineState* TriggerRuntime::mine(std::size_t source) const noexcept {
    const auto it=std::find_if(mines_.begin(),mines_.end(),[&](const auto& m){return m.sourceIndex==source;});
    return it==mines_.end()?nullptr:&*it;
}
const TriggerAnimationData& TriggerRuntime::animation(const TriggerMineState& state,std::uint16_t id) const {
    return *std::find_if(state.animations.begin(),state.animations.end(),
        [&](const auto& a){return a.identity==clientAnimation(id);});
}
void TriggerRuntime::updateObserver(const std::optional<TriggerObserver>& observer,
    const TriggerReputation& reputation) {
    for(const auto& state:mines_)if(!state.serverRemoved)
        updateMineObserver(state.sourceIndex,observer,observer?reputation(state.objectId,observer->objectId):0);
}
bool TriggerRuntime::updateMineObserver(std::size_t source,const std::optional<TriggerObserver>& observer,
    std::int32_t reputation) {
    auto* found=mine(source);if(!found||found->serverRemoved)return false;
    auto& state=*found;
    bool displayed=state.flagged;
    if(observer) {
        state.serverHostile=triggerMineHostile(state.faction,observer->faction,reputation);
        // The Add message stores this byte. Update's color call returns
        // immediately for mines and does not replace IsHostile's byte.
        if(!state.clientHostile)state.clientHostile=state.serverHostile;
        displayed=displayed || !state.serverHostile || containsId(state.detectedBy,observer->objectId);
    }
    const std::uint16_t next=displayed?10144u:10143u;
    if(next==state.serverAnimation)return true;
    state.serverAnimation=state.currentAnimation=next;
    events_.push_back({TriggerRuntimeEvent::Kind::Animation,state.sourceIndex,next,false,
        animation(state,next).name,state.clientPosition});
    return true;
}
namespace {
bool detectionTargetIs(const TriggerDetectionInput::Object& object,TrapObjectTarget target) {
    if(object.trapTarget)return object.trapTarget->kind==target.kind&&object.trapTarget->sourceIndex==target.sourceIndex;
    return target.kind==TrapObjectKind::Mine&&object.triggerSourceIndex&&*object.triggerSourceIndex==target.sourceIndex;
}
}
void TriggerRuntime::setDetectionAreaObjects(std::vector<TriggerDetectionInput::Object> objects) {
    detectionAreaObjects_=std::move(objects);
    const auto present=[&](TrapObjectTarget target) {
        return std::any_of(detectionAreaObjects_->begin(),detectionAreaObjects_->end(),
            [&](const auto& object){return detectionTargetIs(object,target);});
    };
    for(auto& state:mines_)state.inDetectionArea=present({TrapObjectKind::Mine,state.sourceIndex});
    for(auto& state:objectTraps_)state.inDetectionArea=present(state.target);
}
void TriggerRuntime::appendDetectionTarget(TrapObjectTarget target) {
    if(detectionAreaObjects_) {
        TriggerDetectionInput::Object object;object.trapTarget=target;
        detectionAreaObjects_->push_back(std::move(object));
    }
    if(target.kind==TrapObjectKind::Mine)mine(target.sourceIndex)->inDetectionArea=true;
    else for(auto& state:objectTraps_)if(state.target.kind==target.kind&&state.target.sourceIndex==target.sourceIndex)
        state.inDetectionArea=true;
}
void TriggerRuntime::removeDetectionTarget(TrapObjectTarget target,bool requireUnique) {
    bool remaining=false;
    if(detectionAreaObjects_) {
        auto& objects=*detectionAreaObjects_;
        const auto matches=[&](const auto& object){return detectionTargetIs(object,target);};
        const auto count=std::count_if(objects.begin(),objects.end(),matches);
        if((requireUnique&&count==1)||(!requireUnique&&count!=0))
            objects.erase(std::find_if(objects.begin(),objects.end(),matches));
        remaining=std::any_of(objects.begin(),objects.end(),matches);
    }
    if(target.kind==TrapObjectKind::Mine)mine(target.sourceIndex)->inDetectionArea=remaining;
    else for(auto& state:objectTraps_)if(state.target.kind==target.kind&&state.target.sourceIndex==target.sourceIndex)
        state.inDetectionArea=remaining;
}
TriggerDetectionResult TriggerRuntime::checkDetection(const TriggerDetectionInput& input) {
    setDetectionAreaObjects(input.areaObjects);
    return checkDetectionInput(input,*detectionAreaObjects_);
}
void TriggerRuntime::captureDetection(TriggerDetectionInput input) {
    setDetectionAreaObjects(std::move(input.areaObjects));
    input.areaObjects.clear();
    const auto found=std::find_if(detectionCaptures_.begin(),detectionCaptures_.end(),
        [&](const auto& actor){return actor.objectId==input.objectId;});
    if(found==detectionCaptures_.end())detectionCaptures_.push_back(std::move(input));
    else *found=std::move(input);
}
void TriggerRuntime::advanceDetection(std::uint32_t elapsed) {
    for(auto& actor:detectionCaptures_) {
        actor.elapsedSinceCheckMilliseconds+=elapsed;
        // AIUpdate's caller reaches UpdateMineCheck for captured active actors.
        // It performs one check, not a catch-up loop, then stores current time.
        auto result=checkDetectionInput(actor,*detectionAreaObjects_);
        if(result.checked) {
            actor.elapsedSinceCheckMilliseconds=0u;
            detectionResults_.push_back(std::move(result));
        }
    }
}
std::vector<TriggerDetectionResult> TriggerRuntime::takeDetectionResults() {
    auto results=std::move(detectionResults_);detectionResults_.clear();return results;
}
TriggerDetectionResult TriggerRuntime::checkDetectionInput(const TriggerDetectionInput& input,
    const std::vector<TriggerDetectionInput::Object>& objects) {
    TriggerDetectionResult result;
    const bool slow=input.serverAnimation!=10000u && input.detectionMode==0u;
    if(input.elapsedSinceCheckMilliseconds<(slow?3000u:100u))return result;
    result.checked=true;
    if(!input.areaPresent)return result;
    for(const auto& object:objects)if(object.exists) {
        const auto target=object.trapTarget.value_or(TrapObjectTarget{TrapObjectKind::Mine,object.triggerSourceIndex.value_or(0u)});
        if(object.trapTarget || object.triggerSourceIndex) {
            if(target.kind==TrapObjectKind::Mine) {
                const auto* state=mine(target.sourceIndex);if(state&&(state->serverRemoved||!state->inDetectionArea))continue;
            } else {
                const auto found=std::find_if(objectTraps_.begin(),objectTraps_.end(),[&](const auto& trap) {
                    return trap.target.kind==target.kind&&trap.target.sourceIndex==target.sourceIndex;
                });
                if(found!=objectTraps_.end()&&!found->inDetectionArea)continue;
            }
        }
        // The draw precedes trigger casting and all trigger admission checks.
        std::int32_t roll=input.engineRand()%10+1;
        if(input.serverAnimation==10000u || input.detectionMode==1u || input.detectionMode==2u)roll+=10;
        if(input.fivePointPenalty)roll-=5;
        if(object.trapTarget && object.trapTarget->kind!=TrapObjectKind::Mine) {
            const auto target=*object.trapTarget;
            const auto found=std::find_if(objectTraps_.begin(),objectTraps_.end(),[&](const auto& trap) {
                return trap.target.kind==target.kind && trap.target.sourceIndex==target.sourceIndex;
            });
            if(found==objectTraps_.end())continue;
            auto& state=*found;
            if(containsId(state.detectedBy,input.objectId) || state.detectable!=1u)continue;
            const auto x=input.position.x-state.position.x,y=input.position.y-state.position.y,
                       z=input.position.z-state.position.z;
            if(!((x*x+y*y)+z*z<=(slow?9.0f:400.0f)))continue;
            if(input.reputation(state.objectId,input.objectId)>89 || state.faction==input.faction)continue;
            const auto dc=objectTrapDetectionDc(target.kind,state.detectDc,input.difficulty,game_);
            if(roll+input.awarenessRank<dc && state.flagged!=1u)continue;
            // These two engine branches do not broadcast mine-detection feedback.
            result.discoveries.push_back({target.sourceIndex,input.objectId,roll,input.awarenessRank,dc,false,target.kind});
            if(input.partyMember)for(const auto id:input.partyObjectIds)addUnique(state.detectedBy,id);
            else addUnique(state.detectedBy,input.objectId);
            continue;
        }
        const auto source=object.trapTarget?std::optional<std::size_t>{object.trapTarget->sourceIndex}:
            object.triggerSourceIndex;
        if(!source)continue;
        auto* state=mine(*source);
        if(!state || state->serverRemoved)continue;
        const auto nearest=nearestPoint(*state,input.position);
        if(containsId(state->detectedBy,input.objectId)||!state->detectable)continue;
        const auto x=input.position.x-nearest.x,y=input.position.y-nearest.y,z=input.position.z-nearest.z;
        if(!((x*x+y*y)+z*z<=(slow?9.0f:400.0f)))continue;
        if(input.reputation(state->objectId,input.objectId)>89 || state->faction==input.faction)continue;
        const auto dc=triggerDetectionDc(state->detectDc,input.difficulty,game_);
        if(roll+input.awarenessRank<dc && !state->flagged)continue;
        result.discoveries.push_back({state->sourceIndex,input.objectId,roll,input.awarenessRank,dc,!state->flagged});
        if(input.partyMember)for(const auto id:input.partyObjectIds)addUnique(state->detectedBy,id);
        else addUnique(state->detectedBy,input.objectId);
    }
    return result;
}
std::vector<TriggerMineRequest> TriggerRuntime::enterMine(std::size_t source,
    const TriggerMineEnterInput& input) const {
    std::vector<TriggerMineRequest> result;
    const auto* state=mine(source);
    if(!state || state->serverRemoved)return result;
    const auto emit=[&](TriggerMineRequest::Kind kind,std::string script={}) {
        TriggerMineRequest request;request.kind=kind;request.sourceIndex=source;
        request.actor=input.actor;request.item=input.keyItem.value_or(0x7f000000u);
        request.script=std::move(script);result.push_back(std::move(request));
    };
    emit(TriggerMineRequest::Kind::SetLastEntered);
    if(!input.firing.targetIsCreature || !triggerMineCanFire(state->faction,input.firing,game_)) {
        // EventHandler still calls SetFadeOutTime(1) after OnEnterMine returns.
    } else if(input.keyItem) {
        if(state->removeKey)emit(TriggerMineRequest::Kind::RemoveKey);
        emit(TriggerMineRequest::Kind::SetLastDisarmed);
        emit(TriggerMineRequest::Kind::RunScript,state->onDisarm);
        emit(TriggerMineRequest::Kind::ScheduleDestroy);
    } else {
        emit(TriggerMineRequest::Kind::Feedback82);
        emit(TriggerMineRequest::Kind::RunScript,state->onTrapTriggered);
        emit(TriggerMineRequest::Kind::RunScript,state->onEnter);
        if(state->oneShot)emit(TriggerMineRequest::Kind::ScheduleDestroy);
    }
    // K1 EventHandler 0x292f6a. This updates retained last-update records;
    // later server removal serializes their low byte as the delete reason.
    emit(TriggerMineRequest::Kind::SetDeleteReason);result.back().deleteReason=1u;
    return result;
}
bool TriggerRuntime::enterMineFromInventory(std::size_t source,const TriggerMineEnterInput& input,
    const RequestSink& sink) {
    if(mineEntry_||pendingMineScript_)throw std::runtime_error("A engine mine script request is awaiting completion");
    auto* state=mine(source);if(!state||state->serverRemoved)return true;
    state->lastEntered=input.actor;
    std::optional<std::uint64_t> key;
    const auto emit=[&](TriggerMineRequest::Kind kind,const std::string& script={}) {
        TriggerMineRequest request;request.kind=kind;request.sourceIndex=source;
        request.actor=input.actor;request.item=key.value_or(0x7f000000u);request.script=script;
        if(kind==TriggerMineRequest::Kind::SetDeleteReason)request.deleteReason=state->deleteReason;
        sink(request);
    };
    emit(TriggerMineRequest::Kind::SetLastEntered);
    if(input.firing.targetIsCreature && triggerMineCanFire(state->faction,input.firing,game_)) {
        const auto actor=std::find_if(inventory_.actors.begin(),inventory_.actors.end(),
            [&](const auto& value){return value.actor==input.actor;});
        if(actor==inventory_.actors.end())
            throw std::runtime_error("Mine entry requires the actor's engine inventory capture");
        {
            auto& repository=trapUsesPartyInventory(*actor,inventory_,game_)?inventory_.partyItems:actor->personalItems;
            key=findInventoryTag(inventory_,repository,state->keyName);
            if(!key)for(std::size_t slot=0;slot<(game_==model::GameVersion::Kotor1?18u:20u);++slot)
                if(actor->equippedItems[slot])
                    if(const auto* item=inventoryItem(inventory_,*actor->equippedItems[slot]))
                        if(item->tag==state->keyName)key=item->objectId;
            if(key && state->removeKey) {
                // RemoveItem is called on the selected top-level repository
                // and the actor's equipment, followed by the item destructor.
                // The destructor releases its repository array, not its items.
                removeInventoryId(repository,*key);
                for(auto& equipped:actor->equippedItems)if(equipped==key)equipped.reset();
                inventory_.items.erase(std::find_if(inventory_.items.begin(),inventory_.items.end(),
                    [&](const auto& item){return item.objectId==*key;}));
                emit(TriggerMineRequest::Kind::RemoveKey);
            }
        }
    }
    mineEntry_=MineEntryContinuation{source,input.actor,key,
        input.firing.targetIsCreature && triggerMineCanFire(state->faction,input.firing,game_),0u};
    return resumeMineEntry(sink);
}
bool TriggerRuntime::resumeMineEntry(const RequestSink& sink) {
    pendingMineScript_.reset();
    if(!mineEntry_)return true;
    while(mineEntry_) {
        auto& continuation=*mineEntry_;
        auto& state=*mine(continuation.sourceIndex);
        TriggerMineRequest request;
        request.sourceIndex=continuation.sourceIndex;request.actor=continuation.actor;
        request.item=continuation.key.value_or(0x7f000000u);
        const unsigned stage=continuation.stage++;
        bool script=false,done=false,emit=true;
        if(!continuation.eligible)done=true;
        else if(continuation.key) {
            switch(stage) {
            case 0:state.lastDisarmed=continuation.actor;request.kind=TriggerMineRequest::Kind::SetLastDisarmed;break;
            case 1:request.kind=TriggerMineRequest::Kind::RunScript;request.script=state.onDisarm;script=true;break;
            case 2:state.pendingDestroy=true;request.kind=TriggerMineRequest::Kind::ScheduleDestroy;break;
            default:done=true;break;
            }
        } else {
            switch(stage) {
            case 0:request.kind=TriggerMineRequest::Kind::Feedback82;break;
            case 1:request.kind=TriggerMineRequest::Kind::RunScript;request.script=state.onTrapTriggered;script=true;break;
            case 2:request.kind=TriggerMineRequest::Kind::RunScript;request.script=state.onEnter;script=true;break;
            case 3:if(state.oneShot) {state.pendingDestroy=true;request.kind=TriggerMineRequest::Kind::ScheduleDestroy;}else emit=false;break;
            default:done=true;break;
            }
        }
        if(done) {
            state.deleteReason=1u;request.kind=TriggerMineRequest::Kind::SetDeleteReason;request.deleteReason=1u;
            sink(request);mineEntry_.reset();return true;
        }
        if(script&&request.script.empty())continue; // engine RunScript returns before reading a script
        if(emit && !sink(request) && script) {pendingMineScript_=std::move(request);return false;}
    }
    return true;
}
TrapSetMinePlan TriggerRuntime::prepareSetMine(const TrapSetMineInput& input,
    const TrapInventoryState& inventory,const ResourceResolver& resolver,model::GameVersion game,
    const std::vector<ObjectTrapState>& objects) {
    TrapSetMinePlan plan;plan.input=input;
    const auto* item=inventoryItem(inventory,input.item);
    if(!item||(input.areaPresent&&!input.areaCanSetMines))return plan;
    if(!item->firstActivePropertySubtype)
        throw std::runtime_error("SetMine requires the captured first active item property subtype");
    plan.propertySubtype=*item->firstActivePropertySubtype;
    detail::AreaLoadTables tables{resolver,{}};
    const auto read=[&](const char* column) {
        int value=0;engine2da::getInt(tables.table("traps"),plan.propertySubtype,column,value);return value;
    };
    auto& result=plan.result;result.accepted=true;
    result.difficultyClass=std::max(1,read("SetDC")+(game==model::GameVersion::Kotor2?
        (input.difficulty==2u?5:input.difficulty==0u?-5:0):0));
    result.skill=input.demolitionsRank;
    if(game==model::GameVersion::Kotor1&&input.baseDemolitionsRank>=5)result.skill+=2;
    const bool takeTwenty=!input.inCombat||(game==model::GameVersion::Kotor2&&input.target);
    result.roll=takeTwenty?20:input.engineRand()%20+1;
    const auto total=result.roll+result.skill;
    result.success=total>=result.difficultyClass;
    result.catastrophic=!takeTwenty&&total<result.difficultyClass-10;
    if(!result.success&&!result.catastrophic) {result.completed=true;return plan;}
    plan.detectDc=total+read("DetectDCMod");plan.disarmDc=total+read("DisarmDCMod");
    plan.trapScript=tables.stringValue("traps",plan.propertySubtype,"TrapScript");
    plan.recoveredItemResref=tables.lookup("traps",plan.propertySubtype,"ResRef");
    model::Vec3 position=input.actorPosition;
    const ObjectTrapState* target=nullptr;
    if(input.target&&input.target->kind!=TrapObjectKind::Mine) {
        const auto found=std::find_if(objects.begin(),objects.end(),[&](const auto& object) {
            return object.target.kind==input.target->kind&&object.target.sourceIndex==input.target->sourceIndex;
        });
        if(found==objects.end())throw std::runtime_error("SetMine requires the captured target trap state");
        target=&*found;
        plan.mutateObject=game==model::GameVersion::Kotor1||target->armed==0u;
        if(game==model::GameVersion::Kotor1||!plan.mutateObject)return plan;
        plan.objectVisual=true;position=target->position;
        // K2's sparse table is an event parameter, not a trap-type remap.
        static constexpr std::int32_t parameters[]{3,9,0,3,9,0,0,0,0,0,0,3,9,0,0,9,12,9,12,0,0,9,12};
        if(plan.propertySubtype>=4u&&plan.propertySubtype<=26u)
            plan.objectEventParameter=parameters[plan.propertySubtype-4u];
        if(input.target->kind==TrapObjectKind::Door&&
           20+int(input.demolitionsRank)/2+plan.objectEventParameter>=objectOpenLockDc(*target,input.difficulty,game)) {
            if(target->objectId==0x7f000000u)
                throw std::runtime_error("Door opening requires the captured server-assigned door identity");
            if(target->linkedToFlags==1u||target->linkedToFlags==2u) {
                if(!input.linkedDoorObjectId)
                    throw std::runtime_error("Door opening requires the captured linked door identity");
                if(*input.linkedDoorObjectId!=0x7f000000u&&
                   std::none_of(objects.begin(),objects.end(),[&](const auto& object) {
                       return object.target.kind==TrapObjectKind::Door&&object.objectId==*input.linkedDoorObjectId;
                   }))throw std::runtime_error("Captured linked door is outside the loaded area");
            }
        }
    }
    AreaTriggerRecord record;record.objectId=input.newObjectId;record.type=2;
    record.position=position;record.faction=input.actorFaction;
    record.trapType=static_cast<std::uint8_t>(plan.propertySubtype);
    record.trapDetectable=!plan.objectVisual;record.trapDisarmable=!plan.objectVisual;
    record.trapOneShot=true;record.onEnter=record.onDisarm="default";
    record.onTrapTriggered=plan.objectVisual?std::string("default"):plan.trapScript;
    auto& state=plan.constructorState;state.objectId=input.newObjectId;state.creator=input.actor;
    state.position=position;state.faction=input.actorFaction;state.trapType=record.trapType;
    state.detectable=record.trapDetectable;state.oneShot=true;
    state.onEnter=state.onDisarm="default";state.onTrapTriggered=record.onTrapTriggered;
    if(!plan.objectVisual) {
        state.onTrapTriggered=plan.trapScript;state.detectDc=plan.detectDc;state.disarmDc=plan.disarmDc;
        if(game==model::GameVersion::Kotor2)state.creatorDemolitionsRank=input.demolitionsRank;
    }
    plan.nameStrref=read("TrapName");
    plan.record=std::move(record);return plan;
}
TrapSetMineResult TriggerRuntime::completeSetMine(const TrapSetMinePlan& plan,
    const CreateMine& create,const RequestSink& sink) {
    if(mineEntry_||trapAction_||pendingMineScript_)throw std::runtime_error("A engine trap operation is awaiting captured completion");
    lastTrapAction_.reset();lastSetMine_=plan.result;
    auto& result=*lastSetMine_;
    if(!result.accepted||(!result.success&&!result.catastrophic)) {result.completed=true;return result;}
    auto target=plan.input.target;
    ObjectTrapState* object=nullptr;
    if(target&&target->kind!=TrapObjectKind::Mine)for(auto& candidate:objectTraps_)
        if(candidate.target.kind==target->kind&&candidate.target.sourceIndex==target->sourceIndex) {
            object=&candidate;break;
        }
    const auto emit=[&](TriggerMineRequest::Kind kind) {
        TriggerMineRequest request;request.kind=kind;request.actor=plan.input.actor;request.item=plan.input.item;
        if(target) {request.sourceIndex=target->sourceIndex;request.targetKind=target->kind;}
        sink(request);
    };
    if(plan.mutateObject) {
        object->armed=1u;object->trapType=static_cast<std::uint8_t>(plan.propertySubtype);
        object->onTrapTriggered=plan.trapScript;object->detectDc=static_cast<std::uint8_t>(plan.detectDc);
        object->disarmDc=static_cast<std::uint8_t>(plan.disarmDc);
        object->detectable=object->oneShot=1u;object->disarmable=game_==model::GameVersion::Kotor1?1u:0u;
        object->recoveredItemResref=plan.recoveredItemResref;
        appendDetectionTarget(*target);
        if(game_==model::GameVersion::Kotor2)object->creatorDemolitionsRank=plan.input.demolitionsRank;
        else if(target->kind==TrapObjectKind::Placeable)object->faction=plan.input.actorFaction;
        result.placed=true;emit(TriggerMineRequest::Kind::AddToDetectionArea);
    }
    if(plan.record) {
        result.createdSourceIndex=create(*plan.record,plan.constructorState);
        result.placed=true;
        appendDetectionTarget({TrapObjectKind::Mine,*result.createdSourceIndex});
        if(plan.objectVisual)object->attachedMine=result.createdSourceIndex;
        else if(game_==model::GameVersion::Kotor2)target=TrapObjectTarget{TrapObjectKind::Mine,*result.createdSourceIndex};
    }
    if(plan.objectVisual&&plan.result.success) {
        ScheduledTrapEvent event;event.request.kind=TriggerMineRequest::Kind::SignalEvent;
        event.request.targetKind=plan.input.target->kind;event.request.sourceIndex=plan.input.target->sourceIndex;
        event.request.actor=plan.input.actor;event.request.event=26u;
        event.request.integers={1,0,1,plan.objectEventParameter,0};
        event.request.delayMilliseconds=event.remaining=3000u;
        event.actor.actor=plan.input.actor;event.actor.demolitionsRank=plan.input.demolitionsRank;
        event.actor.difficulty=plan.input.difficulty;
        event.actor.linkedDoorObjectId=plan.input.linkedDoorObjectId;
        event.actor.firing=plan.input.firing;
        sink(event.request);scheduledTrapEvents_.push_back(std::move(event));
    }
    TrapActionContinuation continuation;continuation.settingMine=true;
    continuation.input.actor=plan.input.actor;continuation.input.firing=plan.input.firing;
    continuation.input.firing.force=true;continuation.removeInventoryItem=plan.input.item;
    if(target)continuation.target=*target;
    continuation.fireEvent=plan.result.catastrophic&&bool(target);
    continuation.stage=5u;trapAction_=std::move(continuation);
    resumeTrapAction(sink);
    return *lastSetMine_;
}
TrapActionResult TriggerRuntime::completeTrapAction(TrapObjectTarget target,TrapAction action,
    const TrapActionInput& input,const RequestSink& sink) {
    if(mineEntry_||trapAction_||pendingMineScript_)
        throw std::runtime_error("A engine trap operation is awaiting captured completion");
    lastSetMine_.reset();
    TrapActionResult result;result.action=action;result.target=target;
    auto* trigger=target.kind==TrapObjectKind::Mine?mine(target.sourceIndex):nullptr;
    ObjectTrapState* object=nullptr;
    if(target.kind!=TrapObjectKind::Mine)for(auto& candidate:objectTraps_)
        if(candidate.target.kind==target.kind&&candidate.target.sourceIndex==target.sourceIndex) {
            object=&candidate;break;
        }
    if((!trigger&&!object)||(trigger&&trigger->serverRemoved)) {
        lastTrapAction_=result;return result;
    }
    result.accepted=true;result.skill=input.demolitionsRank;
    // The die is consumed before creator/party bypass. K1 loads libc rand;
    // K2 calls rand_wincompatible. Out-of-combat actions take twenty.
    result.roll=input.inCombat?input.engineRand()%20+1:20;
    const auto stored=trigger?triggerDetectionDc(trigger->disarmDc,input.difficulty,game_):
        int(objectTrapDetectionDc(target.kind,object->disarmDc,input.difficulty,game_));
    result.difficultyClass=std::max(1,stored+(action==TrapAction::Recover?10:action==TrapAction::Flag?-5:0));
    if(trigger) {
        result.creatorBypass=trigger->creator==input.actor;
        if(action!=TrapAction::Flag)
            result.creatorBypass=result.creatorBypass ||
                ((input.creatorPartyMember||(input.creatorIsCreature&&input.creatorPlayerFlag))&&
                 (input.actorPartyMember||input.actorPlayerFlag));
    }
    const auto total=result.roll+result.skill;
    result.success=result.creatorBypass || (total>=result.difficultyClass &&
        (action!=TrapAction::Disarm || result.difficultyClass<=(game_==model::GameVersion::Kotor1?35:65)));
    const auto emit=[&](TriggerMineRequest::Kind kind,std::uint16_t event=0u) {
        TriggerMineRequest request;request.kind=kind;request.sourceIndex=target.sourceIndex;
        request.targetKind=target.kind;request.actor=input.actor;request.event=event;
        if(kind==TriggerMineRequest::Kind::SetDeleteReason)request.deleteReason=2u;
        sink(request);
    };
    if(action==TrapAction::Flag) {
        if(result.success) {
            if(trigger)trigger->flagged=true;else object->flagged=1u;
            emit(TriggerMineRequest::Kind::SetTrapFlag);
        }
        result.completed=true;lastTrapAction_=result;return result;
    }
    TrapActionContinuation continuation;continuation.target=target;
    continuation.action=action;continuation.input=input;
    if(result.success) {
        continuation.disarmEvent=action==TrapAction::Disarm || object;
        continuation.recovery=action==TrapAction::Recover;
        // AddEventDeltaTime queues the script event before destruction. Its
        // handler runs after this action's synchronous inventory work.
        if(continuation.disarmEvent)emit(TriggerMineRequest::Kind::SignalEvent,24u);
        if(trigger) {
            trigger->pendingDestroy=true;emit(TriggerMineRequest::Kind::ScheduleDestroy);
            trigger->deleteReason=2u;emit(TriggerMineRequest::Kind::SetDeleteReason);
        }
        if(action==TrapAction::Disarm) {
            removeDetectionTarget(target,true);
            emit(TriggerMineRequest::Kind::RemoveFromDetectionArea);
        }
    } else {
        if(action==TrapAction::Disarm)emit(TriggerMineRequest::Kind::SoundSet,24u);
        continuation.fireEvent=total<result.difficultyClass-(action==TrapAction::Disarm?10:5);
        if(continuation.fireEvent)emit(TriggerMineRequest::Kind::SignalEvent,trigger?12u:26u);
    }
    lastTrapAction_=result;trapAction_=std::move(continuation);
    resumeTrapAction(sink);
    return *lastTrapAction_;
}
bool TriggerRuntime::resumeTrapAction(const RequestSink& sink) {
    pendingMineScript_.reset();
    if(!trapAction_)return true;
    // A failed disarm/recovery may be suspended inside OnEnterMine. Resume
    // that exact call before returning to the queued action continuation.
    if(mineEntry_&&!resumeMineEntry(sink))return false;
    while(trapAction_) {
        auto& continuation=*trapAction_;
        const auto target=continuation.target;
        auto* trigger=target.kind==TrapObjectKind::Mine?mine(target.sourceIndex):nullptr;
        ObjectTrapState* object=nullptr;
        if(!trigger)for(auto& candidate:objectTraps_)
            if(candidate.target.kind==target.kind&&candidate.target.sourceIndex==target.sourceIndex) {
                object=&candidate;break;
            }
        const auto request=[&](TriggerMineRequest::Kind kind) {
            TriggerMineRequest value;value.kind=kind;value.targetKind=target.kind;
            value.sourceIndex=target.sourceIndex;value.actor=continuation.input.actor;
            return value;
        };
        const auto external=[&](TriggerMineRequest value) {
            if(value.kind==TriggerMineRequest::Kind::RunScript&&value.script.empty())return true;
            if(sink(value))return true;
            pendingMineScript_=std::move(value);return false;
        };
        switch(continuation.stage++) {
        case 0:if(continuation.recovery) {
            const auto& resource=trigger?trigger->recoveredItemResref:object->recoveredItemResref;
            if(resource) {
                auto value=request(TriggerMineRequest::Kind::RecoverInventoryItem);value.resource=*resource;
                if(!external(std::move(value)))return false;
            }
            break;
        }break;
        case 1:if(continuation.recovery) {
            const auto& resource=trigger?trigger->recoveredItemResref:object->recoveredItemResref;
            // The engine removal occurs after AddItem even when its admission
            // failed, but only when GetCExoStringEntry(ResRef) succeeded.
            if(resource) {
                removeDetectionTarget(target,true);
                sink(request(TriggerMineRequest::Kind::RemoveFromDetectionArea));
            }
        }break;
        case 2:if(continuation.disarmEvent && (trigger||object->armed!=0u)) {
            if(trigger)trigger->lastDisarmed=continuation.input.actor;
            else {object->armed=0u;object->lastDisarmed=continuation.input.actor;}
            sink(request(TriggerMineRequest::Kind::SetLastDisarmed));
            auto value=request(TriggerMineRequest::Kind::RunScript);
            value.script=trigger?trigger->onDisarm:object->onDisarm;
            if(!external(std::move(value)))return false;
        }break;
        case 3:if(continuation.fireEvent) {
            if(trigger) {
                TriggerMineEnterInput input;input.actor=continuation.input.actor;
                input.firing=continuation.input.firing;
                if(!enterMineFromInventory(target.sourceIndex,input,sink))return false;
            } else {
                const auto& input=continuation.input.firing;
                bool allowed=object->armed!=0u;
                if(input.targetIsCreature&&input.mineImmune)allowed=false;
                if(!input.force&&(input.reputationToTrigger>89||input.targetFaction==object->faction))allowed=false;
                if(allowed) {
                    continuation.objectFired=true;
                    if(input.targetIsCreature)sink(request(TriggerMineRequest::Kind::Feedback82));
                    object->lastTriggered=continuation.input.actor;
                    auto value=request(TriggerMineRequest::Kind::RunScript);value.script=object->onTrapTriggered;
                    if(!external(std::move(value)))return false;
                }
            }
        }break;
        case 4:continuation.stage=7u;if(continuation.objectFired) {
            if(object->oneShot!=0u) {
                object->armed=0u;removeDetectionTarget(target,game_==model::GameVersion::Kotor1);
                sink(request(TriggerMineRequest::Kind::RemoveFromDetectionArea));
            }
            if(game_==model::GameVersion::Kotor2&&target.kind==TrapObjectKind::Placeable&&
               object->oneShot!=0u&&object->attachedMine)
                deleteTrigger(*object->attachedMine,1u);
            object->detectedBy.clear();
            if(game_==model::GameVersion::Kotor2&&target.kind==TrapObjectKind::Door&&object->attachedMine)
                deleteTrigger(*object->attachedMine,1u);
        }break;
        case 5: {
            const auto actor=std::find_if(inventory_.actors.begin(),inventory_.actors.end(),
                [&](const auto& value){return value.actor==continuation.input.actor;});
            if(actor==inventory_.actors.end())throw std::runtime_error("SetMine requires the actor's engine inventory capture");
            auto& repository=trapUsesPartyInventory(*actor,inventory_,game_)?inventory_.partyItems:actor->personalItems;
            const auto id=*continuation.removeInventoryItem;
            auto found=std::find_if(inventory_.items.begin(),inventory_.items.end(),[&](const auto& item){return item.objectId==id;});
            if(found==inventory_.items.end()||!inventoryContains(inventory_,repository,id)) {continuation.stage=3u;break;}
            if(!found->stackSize)throw std::runtime_error("SetMine requires the source item's captured stack size");
            if(*found->stackSize>1) {
                --*found->stackSize;auto value=request(TriggerMineRequest::Kind::DecrementItemStack);value.item=id;sink(value);
                continuation.stage=3u;break;
            }
            if(std::find(actor->equippedItems.begin(),actor->equippedItems.end(),std::optional<std::uint64_t>{id})!=actor->equippedItems.end()) {
                auto value=request(TriggerMineRequest::Kind::UnequipInventoryItem);value.item=id;
                if(!external(std::move(value)))return false;
            }
        }break;
        case 6: {
            const auto id=*continuation.removeInventoryItem;
            const auto actor=std::find_if(inventory_.actors.begin(),inventory_.actors.end(),
                [&](const auto& value){return value.actor==continuation.input.actor;});
            auto found=std::find_if(inventory_.items.begin(),inventory_.items.end(),[&](const auto& item){return item.objectId==id;});
            if(found==inventory_.items.end())throw std::runtime_error("SetMine source item vanished during captured RunUnequip");
            if(!found->possessor)throw std::runtime_error("SetMine requires the source item's captured possessor");
            auto& repository=trapUsesPartyInventory(*actor,inventory_,game_)?inventory_.partyItems:actor->personalItems;
            bool own=*found->possessor==actor->actor;
            if(!own&&game_==model::GameVersion::Kotor2&&actor->inParty) {
                const auto owner=std::find_if(inventory_.actors.begin(),inventory_.actors.end(),
                    [&](const auto& value){return value.actor==*found->possessor;});
                own=owner!=inventory_.actors.end()&&owner->inParty;
            }
            bool removed=own;
            if(own)removeInventoryId(repository,id);
            else {
                const auto owner=std::find_if(inventory_.items.begin(),inventory_.items.end(),
                    [&](const auto& value){return value.objectId==*found->possessor;});
                if(owner!=inventory_.items.end()) {
                    if(!owner->possessor)throw std::runtime_error("SetMine requires the container's captured possessor");
                    if(*owner->possessor==actor->actor) {removeInventoryId(owner->containedItems,id);removed=true;}
                }
            }
            if(removed) {
                found->possessor=0x7f000000u;
                auto value=request(TriggerMineRequest::Kind::RemoveInventoryItem);value.item=id;sink(value);
                // The following queued DestroyObject is before any failure
                // signal event, and deletes the item, not its contained items.
                inventory_.items.erase(found);
            }
            continuation.stage=3u;
        }break;
        default:
            if(continuation.settingMine)lastSetMine_->completed=true;
            else lastTrapAction_->completed=true;
            trapAction_.reset();return true;
        }
    }
    return true;
}
void TriggerRuntime::advanceServer(std::uint32_t elapsed) {
    if(mineEntry_||trapAction_||pendingMineScript_)return;
    // Engine events are ordered by due time, preserving insertion order for
    // equal times. Newly queued delays start at the current dispatch time.
    std::stable_sort(scheduledTrapEvents_.begin(),scheduledTrapEvents_.end(),
        [](const auto& a,const auto& b){return a.remaining<b.remaining;});
    for(auto& event:scheduledTrapEvents_)
        event.remaining=elapsed>=event.remaining?0u:event.remaining-elapsed;
    for(auto& state:mines_)if(state.pendingDestroy) {
        state.pendingDestroy=false;deleteTrigger(state.sourceIndex,state.deleteReason);
    }
    const auto sink=[&](const TriggerMineRequest& request) {
        serverRequests_.push_back(request);
        return request.kind!=TriggerMineRequest::Kind::RunScript;
    };
    while(!pendingMineScript_) {
        const auto due=std::find_if(scheduledTrapEvents_.begin(),scheduledTrapEvents_.end(),
            [](const auto& event){return event.remaining==0u;});
        if(due==scheduledTrapEvents_.end())break;
        auto event=std::move(*due);scheduledTrapEvents_.erase(due);
        dispatchScheduledTrapEvent(event,sink);
    }
    if(!pendingMineScript_)advanceDetection(elapsed);
}
std::vector<TriggerMineRequest> TriggerRuntime::takeServerRequests() {
    auto requests=std::move(serverRequests_);serverRequests_.clear();return requests;
}
bool TriggerRuntime::flagMine(std::size_t source,bool value) {
    auto* state=mine(source);if(!state||state->serverRemoved)return false;
    state->flagged=value;return true;
}
bool TriggerRuntime::setObjectId(std::size_t source,std::uint64_t identity) {
    auto* state=mine(source);if(!state||state->serverRemoved)return false;
    state->objectId=identity;return true;
}
bool TriggerRuntime::setCreator(std::size_t source,std::uint64_t creator) {
    auto* state=mine(source);if(!state)return false;
    state->creator=creator;return true;
}
bool TriggerRuntime::deleteTrigger(std::size_t source,std::uint8_t reason) {
    auto* state=mine(source);if(!state||state->deleted)return false;
    state->serverRemoved=true;removeDetectionTarget({TrapObjectKind::Mine,source},true);
    events_.push_back({TriggerRuntimeEvent::Kind::UnregisterFootprint,source,0u,false,{},state->clientPosition});
    if(reason==1u || reason==2u) {
        if(reason==1u && !state->explosionSound.empty())
            events_.push_back({TriggerRuntimeEvent::Kind::Sound,source,0u,false,state->explosionSound,state->clientPosition});
        state->fireForgetQueue.push_back(reason==1u?348u:349u);
    } else {
        state->deleted=true;
        events_.push_back({TriggerRuntimeEvent::Kind::Destroy,source,0u,false,{},state->clientPosition});
    }
    return true;
}
void TriggerRuntime::advanceClient(std::uint32_t elapsed) {
    for(auto& state:mines_)if(!state.deleted) {
        if(state.deleting) {
            // AIUpdate's deleting branch does not refresh its stored world
            // timestamp (K1 0xcf928..0xcf98e / K2 0x1e8170..0x1e8180).
            state.deletionClockElapsed+=elapsed;
            if(state.deletionClockElapsed>=state.deleteRemaining) {
                state.deleted=true;
                events_.push_back({TriggerRuntimeEvent::Kind::Destroy,state.sourceIndex,0u,false,{},state.clientPosition});
            } else state.deleteRemaining-=state.deletionClockElapsed;
            continue;
        }
        const auto& current=animation(state,state.currentAnimation);
        if(current.fireForget && advanceDialogueAnimationCounter(state.animationRemaining,elapsed)) {
            state.deleting=true;state.deleteRemaining=500u;state.deletionClockElapsed=0u;
            state.animationRemaining=0u;
            continue;
        }
        if(current.looping && !state.fireForgetQueue.empty()) {
            const auto next=state.fireForgetQueue.front();
            state.fireForgetQueue.erase(state.fireForgetQueue.begin());
            state.currentAnimation=next;
            const auto& data=animation(state,next);state.animationRemaining=data.milliseconds;
            events_.push_back({TriggerRuntimeEvent::Kind::Animation,state.sourceIndex,next,true,data.name,state.clientPosition});
        }
    }
}
std::vector<TriggerRuntimeEvent> TriggerRuntime::takeEvents() {
    auto events=std::move(events_);events_.clear();return events;
}
} // namespace neomdl
