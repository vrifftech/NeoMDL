#pragma once

#include "core/AreaVisualRecords.hpp"
#include "core/ResourceContext.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

struct TriggerAnimationData {
    std::uint16_t identity{};
    std::string name;
    bool looping{}, fireForget{};
    std::uint32_t milliseconds{};
};



struct TriggerMineState {
    std::size_t sourceIndex{};
    std::uint64_t objectId{0x7f000000u}, creator{0x7f000000u};
    std::uint32_t faction{};
    model::Vec3 position{}, clientPosition{};
    bool detectable{}, oneShot{}, flagged{}, removeKey{}, deleted{}, deleting{}, serverRemoved{};
    std::uint8_t trapType{255};
    std::int32_t detectDc{}, disarmDc{};
    float nearestPointRadius{1.0f};
    std::string keyName, onTrapTriggered, onEnter, onDisarm, explosionSound;
    std::optional<std::string> recoveredItemResref;
    std::int32_t creatorDemolitionsRank{};
    std::vector<std::uint64_t> detectedBy;
    std::vector<TriggerAnimationData> animations;
    std::uint16_t serverAnimation{10143u}, currentAnimation{10143u};
    std::uint32_t animationRemaining{}, deleteRemaining{}, deletionClockElapsed{};
    std::vector<std::uint16_t> fireForgetQueue;
    std::uint64_t lastEntered{0x7f000000u},lastDisarmed{0x7f000000u};
    std::uint8_t deleteReason{};
    bool pendingDestroy{};
    bool inDetectionArea{true};
    bool serverHostile{};
    std::optional<bool> clientHostile;
};

struct ObjectTrapState : AreaObjectTrapRecord {
    std::uint8_t flagged{};
    bool inDetectionArea{true};
    std::uint64_t lastDisarmed{0x7f000000u},lastTriggered{0x7f000000u};
    std::uint64_t lastUnlocked{0x7f000000u},lastOpened{0x7f000000u};
    std::optional<std::string> recoveredItemResref;
    std::int32_t creatorDemolitionsRank{};
    std::vector<std::uint64_t> detectedBy;
    std::optional<std::size_t> attachedMine;
};
std::uint8_t objectOpenLockDc(const AreaObjectTrapRecord&,std::uint8_t difficulty,model::GameVersion) noexcept;
std::uint8_t objectTrapDetectionDc(TrapObjectKind,std::uint8_t stored,
    std::uint8_t difficulty,model::GameVersion) noexcept;

// Captured engine inventory ownership. Repository order and nested containers
// are retained; equipment entries are slot-bit order, not repository order.
struct TrapInventoryItem {
    std::uint64_t objectId{0x7f000000u};
    std::string tag, templateResref;
    std::vector<std::uint64_t> containedItems;
    std::optional<std::uint16_t> firstActivePropertySubtype;
    std::optional<std::int32_t> stackSize;
    std::optional<std::uint64_t> possessor;
};
struct TrapActorInventory {
    std::uint64_t actor{0x7f000000u};
    bool inParty{}, availablePartyMember{};
    std::vector<std::uint64_t> personalItems;
    std::array<std::optional<std::uint64_t>,20> equippedItems{};
};
struct TrapInventoryState {
    std::vector<TrapInventoryItem> items;
    std::vector<TrapActorInventory> actors;
    std::vector<std::uint64_t> partyItems;
    // GetItemRepository(1) additional engine GUI/server inputs.
    bool serverPartyAccess{}, partyAccessPanelUp{}; // K1
    std::int32_t topMenuOverrideCharacter{-1}; // K2
};
bool trapUsesPartyInventory(const TrapActorInventory&,const TrapInventoryState&,
    model::GameVersion) noexcept;

struct TriggerObserver {
    std::uint64_t objectId{};
    std::uint32_t faction{};
};
using TriggerReputation = std::function<std::int32_t(std::uint64_t, std::uint64_t)>;

// Snapshot of UpdateMineCheck's actor inputs. The five-point penalty is the
// engine client/server movement predicate, supplied by the gameplay host.
// The actor timer is an active-world timer, not the renderer's frame counter.
struct TriggerDetectionInput {
    std::uint64_t objectId{};
    std::uint32_t faction{}, elapsedSinceCheckMilliseconds{};
    model::Vec3 position{};
    std::uint16_t serverAnimation{10000u};
    std::uint8_t detectionMode{}, difficulty{1};
    std::int8_t awarenessRank{};
    bool fivePointPenalty{}, partyMember{}, areaPresent{true};
    std::vector<std::uint64_t> partyObjectIds;
    // Engine area-object order, including non-trigger objects. One die is
    // consumed for every existing object before testing its concrete type.
    struct Object {
        bool exists{true};
        std::optional<std::size_t> triggerSourceIndex;
        std::optional<TrapObjectTarget> trapTarget;
    };
    std::vector<Object> areaObjects;
    TriggerReputation reputation;
    // CSWRules::RollDice(1,10) uses loaded libc rand, not rand_wincompatible.
    std::function<std::int32_t()> engineRand;
};
struct TriggerDetectionResult {
    bool checked{};
    struct Discovery {
        std::size_t sourceIndex{};
        std::uint64_t actor{};
        std::int32_t roll{}, awareness{}, difficultyClass{};
        bool feedback{};
        TrapObjectKind kind{TrapObjectKind::Mine};
    };
    std::vector<Discovery> discoveries;
};

struct TriggerMineFireInput {
    bool targetIsCreature{}, creatorIsCreature{}, creatorHasFaction{}, force{}, mineImmune{};
    std::uint32_t targetFaction{};
    std::int32_t reputationToCreator{}, reputationToTrigger{};
    bool feat238{}, partyMember{}, partyLeaderHasServerCreature{}, partyLeaderFeat239{};
};
bool triggerMineCanFire(std::uint32_t mineFaction,const TriggerMineFireInput&,
                       model::GameVersion game) noexcept;
bool triggerMineContains(model::Vec3 minePosition,model::Vec3 actorPosition,float radius) noexcept;
bool triggerMineHostile(std::uint32_t mineFaction,std::uint32_t observerFaction,
                       std::int32_t reputation) noexcept;
std::int32_t triggerDetectionDc(std::int32_t stored,std::uint8_t difficulty,
                               model::GameVersion game) noexcept;

// OnEnterMine emits engine requests in order. Captured inventory is mutated by
// the engine key-removal path; external script calls pause before the following
// state transition. Server destruction then produces the existing delete packet.
struct TriggerMineEnterInput {
    std::uint64_t actor{};
    TriggerMineFireInput firing;
    std::optional<std::uint64_t> keyItem;
};
struct TriggerMineRequest {
    enum class Kind { SetLastEntered, SetLastDisarmed, RemoveKey, Feedback82,
                      RunScript, ScheduleDestroy, SetDeleteReason, RecoverInventoryItem,
                      SignalEvent, SoundSet, SetTrapFlag, RemoveFromDetectionArea,
                      RemoveInventoryItem, DecrementItemStack, AddToDetectionArea, UnequipInventoryItem, DoorOpenState };
    Kind kind{};
    std::size_t sourceIndex{};
    std::uint64_t actor{}, item{};
    std::string script;
    std::uint8_t deleteReason{};
    TrapObjectKind targetKind{TrapObjectKind::Mine};
    std::uint16_t event{};
    std::string resource;
    std::array<std::int32_t,5> integers{};
    std::uint32_t delayMilliseconds{};
};
enum class TrapAction { Flag, Disarm, Recover };
// The engine action has already reached its in-range completion pass. Movement,
// animation/timing-bar action queues remain inputs to this server transition.
struct TrapActionInput {
    std::uint64_t actor{};
    std::int8_t demolitionsRank{};
    bool inCombat{}, actorPartyMember{}, actorPlayerFlag{}, creatorIsCreature{},
         creatorPartyMember{}, creatorPlayerFlag{};
    std::uint8_t difficulty{1};
    // Actual GetLinkedObject()->AsDoor identity; invalid token means no linked door.
    std::optional<std::uint64_t> linkedDoorObjectId;
    TriggerMineFireInput firing;
    // K1 loaded rand, K2 rand_wincompatible, consumed only in combat.
    std::function<std::int32_t()> engineRand;
};
struct TrapActionResult {
    TrapAction action{};
    TrapObjectTarget target;
    bool accepted{}, success{}, creatorBypass{}, completed{};
    std::int32_t roll{}, skill{}, difficultyClass{};
};

// Ready/in-range AIActionSetMine inputs; engine movement and timing-bar queues
// have completed. target absent denotes a ground placement. New object IDs are
// assigned by the captured server, independently of viewer source indices.
struct TrapSetMineInput {
    std::uint64_t actor{}, item{}, newObjectId{0x7f000000u};
    std::optional<TrapObjectTarget> target;
    model::Vec3 actorPosition{};
    std::uint32_t actorFaction{};
    std::int8_t demolitionsRank{}, baseDemolitionsRank{};
    bool inCombat{}, areaPresent{}, areaCanSetMines{};
    std::uint8_t difficulty{1};
    // Actual GetLinkedObject()->AsDoor identity; invalid token means no linked door.
    std::optional<std::uint64_t> linkedDoorObjectId;
    TriggerMineFireInput firing;
    std::function<std::int32_t()> engineRand;
};
struct TrapSetMineResult {
    bool accepted{}, success{}, catastrophic{}, placed{}, completed{};
    std::int32_t roll{},skill{},difficultyClass{};
    std::optional<std::size_t> createdSourceIndex;
};
struct TrapSetMinePlan {
    TrapSetMineInput input;
    TrapSetMineResult result;
    std::uint16_t propertySubtype{};
    std::int32_t detectDc{},disarmDc{},nameStrref{},objectEventParameter{};
    bool mutateObject{},objectVisual{};
    std::string trapScript;
    std::optional<std::string> recoveredItemResref;
    std::optional<AreaTriggerRecord> record;
    TriggerMineState constructorState;
};

struct TriggerRuntimeEvent {
    enum class Kind { Animation, Sound, UnregisterFootprint, Destroy, DoorOpenState, MoveMine };
    Kind kind{};
    std::size_t sourceIndex{};
    std::uint16_t animation{};
    bool nonLooping{};
    std::string resource;
    model::Vec3 position{};
    std::uint8_t soundChannel{22};
};




class TriggerRuntime final {
public:
    using AnimationLength = std::function<float(std::size_t,const std::string&)>;
    void load(const std::vector<AreaTriggerRecord>&,const ResourceResolver&,
              model::GameVersion,const AnimationLength&);
    static TriggerMineState prepareMine(const AreaTriggerRecord&,const ResourceResolver&,
        model::GameVersion,const AnimationLength&);
    void appendMine(TriggerMineState state,model::GameVersion game) {game_=game;mines_.push_back(std::move(state));}
    void clear() noexcept;
    void setInventory(TrapInventoryState state) {inventory_=std::move(state);}
    const TrapInventoryState& inventory() const noexcept {return inventory_;}
    // Return false for RunScript, RecoverInventoryItem or UnequipInventoryItem
    // to await the external VM/inventory capture. Resume acknowledges that call.
    using RequestSink=std::function<bool(const TriggerMineRequest&)>;
    bool enterMineFromInventory(std::size_t,const TriggerMineEnterInput&,const RequestSink&);
    bool resumeMineEntry(const RequestSink&);
    TrapActionResult completeTrapAction(TrapObjectTarget,TrapAction,const TrapActionInput&,const RequestSink&);
    bool resumeTrapAction(const RequestSink&);
    static TrapSetMinePlan prepareSetMine(const TrapSetMineInput&,const TrapInventoryState&,
        const ResourceResolver&,model::GameVersion,const std::vector<ObjectTrapState>&);
    using CreateMine=std::function<std::size_t(const AreaTriggerRecord&,const TriggerMineState&)>;
    TrapSetMineResult completeSetMine(const TrapSetMinePlan&,const CreateMine&,const RequestSink&);
    const std::optional<TrapSetMineResult>& lastSetMine() const noexcept {return lastSetMine_;}
    bool trapActionPending() const noexcept {return trapAction_.has_value();}
    const std::optional<TrapActionResult>& lastTrapAction() const noexcept {return lastTrapAction_;}
    const std::optional<TriggerMineRequest>& pendingMineScript() const noexcept {return pendingMineScript_;}
    void advanceServer(std::uint32_t elapsedMilliseconds=0u);
    std::vector<TriggerMineRequest> takeServerRequests();
    void loadObjectTraps(const std::vector<AreaObjectTrapRecord>&,const ResourceResolver&);
    bool flagObjectTrap(TrapObjectTarget,std::uint8_t value);
    bool setObjectTrapId(TrapObjectTarget,std::uint64_t objectId);
    bool captureObjectLock(TrapObjectTarget,const AreaObjectTrapRecord&);
    const std::vector<ObjectTrapState>& objectTraps() const noexcept {return objectTraps_;}
    void updateObserver(const std::optional<TriggerObserver>&,const TriggerReputation&);
    bool updateMineObserver(std::size_t,const std::optional<TriggerObserver>&,std::int32_t reputation);
    TriggerDetectionResult checkDetection(const TriggerDetectionInput&);
    // Caller admission and live actor values are captured; the runtime retains
    // the engine area array, timestamp and repeated UpdateMineCheck cadence.
    void captureDetection(TriggerDetectionInput);
    void clearDetectionCaptures() noexcept {detectionCaptures_.clear();}
    const std::vector<TriggerDetectionInput>& detectionCaptures() const noexcept {return detectionCaptures_;}
    std::vector<TriggerDetectionResult> takeDetectionResults();
    void setDetectionAreaObjects(std::vector<TriggerDetectionInput::Object>);
    const std::optional<std::vector<TriggerDetectionInput::Object>>& detectionAreaObjects() const noexcept {return detectionAreaObjects_;}
    std::vector<TriggerMineRequest> enterMine(std::size_t,const TriggerMineEnterInput&) const;
    bool flagMine(std::size_t sourceIndex,bool value);
    // Loaded ObjectId may be the invalid-object token; a capture can bind
    // the ID assigned by the engine server constructor without inventing one.
    bool setObjectId(std::size_t sourceIndex,std::uint64_t objectId);
    bool setCreator(std::size_t sourceIndex,std::uint64_t creator);
    bool deleteTrigger(std::size_t sourceIndex,std::uint8_t reason);
    void advanceClient(std::uint32_t elapsedMilliseconds);
    std::vector<TriggerRuntimeEvent> takeEvents();
    const std::vector<TriggerMineState>& mines() const noexcept {return mines_;}
private:
    TriggerDetectionResult checkDetectionInput(const TriggerDetectionInput&,
        const std::vector<TriggerDetectionInput::Object>&);
    void appendDetectionTarget(TrapObjectTarget);
    void removeDetectionTarget(TrapObjectTarget,bool requireUnique);
    void advanceDetection(std::uint32_t);
    TriggerMineState* mine(std::size_t) noexcept;
    const TriggerMineState* mine(std::size_t) const noexcept;
    const TriggerAnimationData& animation(const TriggerMineState&,std::uint16_t) const;
    model::GameVersion game_{model::GameVersion::Kotor2};
    std::vector<TriggerMineState> mines_;
    std::vector<ObjectTrapState> objectTraps_;
    TrapInventoryState inventory_;
    struct MineEntryContinuation {
        std::size_t sourceIndex{};
        std::uint64_t actor{};
        std::optional<std::uint64_t> key;
        bool eligible{};
        unsigned stage{};
    };
    std::optional<MineEntryContinuation> mineEntry_;
    std::optional<TriggerMineRequest> pendingMineScript_;
    struct TrapActionContinuation {
        TrapObjectTarget target;
        TrapAction action{};
        TrapActionInput input;
        bool disarmEvent{}, fireEvent{}, recovery{}, objectFired{};
        std::optional<std::uint64_t> removeInventoryItem;
        bool settingMine{};
        unsigned stage{};
    };
    std::optional<TrapActionContinuation> trapAction_;
    std::optional<TrapActionResult> lastTrapAction_;
    std::optional<TrapSetMineResult> lastSetMine_;
    struct ScheduledTrapEvent {
        TriggerMineRequest request;
        TrapActionInput actor;
        std::uint32_t remaining{};
    };
    void dispatchScheduledTrapEvent(const ScheduledTrapEvent&,const RequestSink&);
    std::vector<ScheduledTrapEvent> scheduledTrapEvents_;
    std::vector<TriggerMineRequest> serverRequests_;
    std::optional<std::vector<TriggerDetectionInput::Object>> detectionAreaObjects_;
    std::vector<TriggerDetectionInput> detectionCaptures_;
    std::vector<TriggerDetectionResult> detectionResults_;
    std::vector<TriggerRuntimeEvent> events_;
};
} // namespace neomdl
