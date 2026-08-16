#include "core/TriggerRuntime.hpp"

#include <algorithm>
#include <stdexcept>

namespace neomdl {
std::uint8_t objectOpenLockDc(const AreaObjectTrapRecord& object,std::uint8_t difficulty,
    model::GameVersion game) noexcept {
    if(game==model::GameVersion::Kotor1)return object.openLockDc;
    const int adjustment=difficulty==0u?-5:difficulty==2u?5:0;
    if(object.openLockDifficulty==4u)
        return static_cast<std::uint8_t>(std::max(0,int(object.openLockDc)+adjustment));
    const int level=object.openLockDifficulty==3u?std::uint8_t(object.pcLevelAtSpawn+7u):0;
    return static_cast<std::uint8_t>(21+adjustment+object.openLockDifficultyModifier+level);
}

bool TriggerRuntime::captureObjectLock(TrapObjectTarget target,const AreaObjectTrapRecord& captured) {
    for(auto& object:objectTraps_)if(object.target.kind==target.kind&&object.target.sourceIndex==target.sourceIndex) {
        object.locked=captured.locked;object.openLockDc=captured.openLockDc;
        object.openLockDifficulty=captured.openLockDifficulty;
        object.openLockDifficultyModifier=captured.openLockDifficultyModifier;
        object.pcLevelAtSpawn=captured.pcLevelAtSpawn;return true;
    }
    return false;
}

void TriggerRuntime::dispatchScheduledTrapEvent(const ScheduledTrapEvent& scheduled,const RequestSink& sink) {
    const auto& request=scheduled.request;
    const TrapObjectTarget target{request.targetKind,request.sourceIndex};
    const auto found=std::find_if(objectTraps_.begin(),objectTraps_.end(),[&](const auto& object) {
        return object.target.kind==target.kind&&object.target.sourceIndex==target.sourceIndex;
    });
    if(found==objectTraps_.end())return; // the server event's target no longer exists
    auto& object=*found;
    const auto emit=[&](TriggerMineRequest::Kind kind) {
        auto value=request;value.kind=kind;sink(value);
    };
    const auto script=[&](const std::string& name,std::uint64_t actor) {
        if(name.empty())return;
        auto value=request;value.kind=TriggerMineRequest::Kind::RunScript;value.actor=actor;value.script=name;
        if(!sink(value))pendingMineScript_=std::move(value);
    };
    if(request.event==29u) {
        object.lastUnlocked=request.actor;
        script(object.onUnlock,request.actor);return;
    }
    if(request.event!=26u)return;
    if(target.kind==TrapObjectKind::Door&&request.integers[4]) {
        if(object.objectId==0x7f000000u)
            throw std::runtime_error("Door opening requires the captured allocated door identity");
        ObjectTrapState* linkedDoor=nullptr;
        if(object.linkedToFlags==1u||object.linkedToFlags==2u) {
            // GetLinkedObject uses a cached identity then module tag-ordinal
            // lookup. Its actual AsDoor result is a captured host input.
            if(!scheduled.actor.linkedDoorObjectId)
                throw std::runtime_error("Door opening requires the captured linked door identity");
            if(*scheduled.actor.linkedDoorObjectId!=0x7f000000u) {
                const auto linked=std::find_if(objectTraps_.begin(),objectTraps_.end(),[&](const auto& value) {
                    return value.target.kind==TrapObjectKind::Door&&value.objectId==*scheduled.actor.linkedDoorObjectId;
                });
                if(linked==objectTraps_.end())
                    throw std::runtime_error("Captured linked door is outside the loaded area");
                linkedDoor=&*linked;
            }
        }
        object.locked=0u;object.lastOpened=object.objectId;
        const auto open=[&](ObjectTrapState& door) {
            TriggerRuntimeEvent event;event.kind=TriggerRuntimeEvent::Kind::DoorOpenState;
            event.sourceIndex=door.target.sourceIndex;event.animation=door.doorVisibleModel?2u:3u;
            events_.push_back(std::move(event));
        };
        open(object);
        if(linkedDoor)open(*linkedDoor);
        // OpenDoor is called with this door's own identity; its direction
        // dot product is zero and therefore selects state two.
        script(object.onOpen,object.objectId);return;
    }
    if(!request.integers[2]||request.integers[1]||!object.armed)return;
    const int divisor=target.kind==TrapObjectKind::Door?2:20;
    const int total=20+int(scheduled.actor.demolitionsRank)/divisor+request.integers[3];
    if(total>=objectOpenLockDc(object,scheduled.actor.difficulty,game_)) {
        if(target.kind==TrapObjectKind::Door) {
            auto final=scheduled;final.request.integers={0,0,0,0,1};
            final.request.delayMilliseconds=1000u;final.remaining=1000u;
            scheduledTrapEvents_.push_back(std::move(final));
        } else object.locked=0u;
        auto unlock=scheduled;unlock.request.event=29u;unlock.request.integers={};
        unlock.request.delayMilliseconds=0u;unlock.remaining=0u;
        scheduledTrapEvents_.push_back(std::move(unlock));
    }
    if(target.kind==TrapObjectKind::Placeable)emit(TriggerMineRequest::Kind::Feedback82);
    object.lastTriggered=request.actor;object.armed=0u;
    removeDetectionTarget(target,false);emit(TriggerMineRequest::Kind::RemoveFromDetectionArea);
    object.detectedBy.clear();
    if(object.attachedMine) {
        auto* attached=mine(*object.attachedMine);
        if(target.kind==TrapObjectKind::Placeable&&attached->trapType>=3u&&attached->trapType<=5u) {
            attached->clientPosition.z+=-2.0f;
            TriggerRuntimeEvent moved;moved.kind=TriggerRuntimeEvent::Kind::MoveMine;
            moved.sourceIndex=attached->sourceIndex;moved.position=attached->clientPosition;
            events_.push_back(std::move(moved));
        }
        deleteTrigger(*object.attachedMine,1u);object.attachedMine.reset();
    }
}
} // namespace neomdl
