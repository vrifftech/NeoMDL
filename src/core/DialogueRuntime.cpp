#include "core/DialogueRuntime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace neomdl {
namespace {
std::uint32_t dialogueDelayMilliseconds(float seconds,model::GameVersion game) noexcept {
    const float milliseconds=1000.0f*seconds;
    if(game==model::GameVersion::Kotor1) {
        // SetDialogDelay: K1 Desktop 0x256dcd..0x256e04 saturates to DWORD.
        if(!(milliseconds>0.0f))return 0u;
        if(milliseconds>=0x1p32f)return 0xffffffffu;
        return static_cast<std::uint32_t>(milliseconds);
    }
    // K2 Desktop 0x3e42a3 uses FISTTP qword and retains its low DWORD.
    // Integer-indefinite has low bits 0.
    if(!(milliseconds>=-0x1p63f&&milliseconds<0x1p63f))return 0u;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(milliseconds));
}
void setDialogueDeadline(EngineWorldTime start,std::uint32_t delay,
                         std::uint32_t dayMilliseconds,EngineWorldTime& deadline) noexcept {
    // SetDialogDelay ignores AddWorldTimes' return: either invalid TOD operand
    // leaves BOTH retained deadline fields untouched. K1 Desktop 0x3be468,
    // K2 Desktop 0x4a218c.
    if(start.timeOfDayMilliseconds>=dayMilliseconds || delay>=dayMilliseconds)return;
    deadline.calendarDay=start.calendarDay;
    deadline.timeOfDayMilliseconds=start.timeOfDayMilliseconds+delay;
    // Engine DWORD addition followed by one carry, not a widened duration.
    if(deadline.timeOfDayMilliseconds>=dayMilliseconds) {
        ++deadline.calendarDay;
        deadline.timeOfDayMilliseconds-=dayMilliseconds;
    }
}
int compareDialogueWorldTimes(EngineWorldTime current,EngineWorldTime deadline,
                              std::uint32_t dayMilliseconds) noexcept {
    // CompareWorldTimes returns -2 for invalid TOD, before unsigned date/time
    // comparison. IsDialogDelay waits only for -1. K1 Desktop 0x3be49a,
    // K2 Desktop 0x4a21bc.
    if(current.timeOfDayMilliseconds>=dayMilliseconds ||
       deadline.timeOfDayMilliseconds>=dayMilliseconds)return -2;
    if(current.calendarDay!=deadline.calendarDay)
        return current.calendarDay<deadline.calendarDay?-1:1;
    if(current.timeOfDayMilliseconds!=deadline.timeOfDayMilliseconds)
        return current.timeOfDayMilliseconds<deadline.timeOfDayMilliseconds?-1:1;
    return 0;
}
}

DialogueNodeTiming dialogueNodeTiming(const neoshared::dlg::Dialogue& dialogue,
                                      const neoshared::dlg::Line& line,
                                      const DialogueTimingContext& context,
                                      model::GameVersion game) {
    const auto unsignedDelay=[&](float value) {
        return static_cast<std::uint32_t>(value);
    };
    // LoadDialogBase 0x6b49c6..0x6b4ca0. Explicit Delay sets bit 0x10;
    // inherited voiced delay supplies the engine wait-for-sound default.
    auto delay=line.delay.value_or(0u);
    auto flags=line.waitFlags.value_or(0u);
    const auto& voice=dialogueVoiceResRef(line,game);
    bool noVoiceWait=false;
    if(delay==0xffffffffu) {
        if(!voice.empty() || flags!=0u) {
            delay=(line.kind==neoshared::dlg::NodeKind::Entry
                ?dialogue.delayEntry:dialogue.delayReply).value_or(0u);
            if(!voice.empty() && flags==0u) flags=2u;
            noVoiceWait=voice.empty(); // K1 base +0x28 / K2 +0x74 becomes zero.
        }
    } else flags|=0x10u;

    // SendDialogEntry 0x6ba8c9 and HandleReply 0x6bcfed choose different
    // delay inputs before entering SetDialogDelay. Entry's first predicate is
    // resource existence, not successful playback or a guessed sound length.
    const bool entry=line.kind==neoshared::dlg::NodeKind::Entry;
    const bool nodeDelay=entry?(context.entryVoiceResourceAvailable||(flags&0x10u)):(flags&2u);
    if(!nodeDelay) {
        delay=0;
        if(!noVoiceWait) {
            if(dialogue.conversationType.value_or(0)!=0)return {0.0f,flags};
            if(game==model::GameVersion::Kotor1 || !entry || context.ownerTag!="info_term") {
                // K1 Desktop 0x380c0e counts the complete encoded string; K2
                // 0x7f2930 first trims ASCII spaces. Callers truncate to DWORD.
                const auto bytes=(game==model::GameVersion::Kotor1
                    ?line.rawTextByteCount:line.rawTrimmedTextByteCount).value_or(0u);
                const float spoken=bytes==0?0.0f:static_cast<float>(bytes)*0.10999999940395355f+1.0f;
                delay=unsignedDelay(spoken);
            }
        }
    }

    // SetDialogDelay 0x6b70f0: text timing is a separate engine delay, not
    // an estimate of audio duration. The shared codec preserves encoded byte
    // count and TLK sound-length metadata before converting text to UTF-8.
    float seconds=0.0f;
    if(!context.hasPlayers && !line.links.empty()) {
        seconds=line.textSoundLength.value_or(0.0f);
        if(seconds<0.15000000596046448f)
            seconds=(static_cast<float>(line.rawTextByteCount.value_or(0u))*
                0.15000000596046448f)*0.75f;
    }
    // SendDialogEntry passes the final argument as zero (0x6ba963);
    // HandleReply passes one (0x6bd017/0x6bd03a), bypassing this minimum.
    if(entry &&
       dialogue.conversationType.value_or(0)==0 && seconds<0.009999999776482582f)
        seconds=0.009999999776482582f;
    // Engine converts the current delay to DWORD before the comparison.
    if(unsignedDelay(seconds)<delay) seconds=static_cast<float>(delay);
    return {seconds,flags};
}

std::string dialogueCameraAnimationName(std::uint16_t animation) {
    // K1 Desktop 0x47af0 / K2 0x2a2e30: four sparse 128-entry switch bands.
    // All four index-28 entries actually spell cut039, not cut029.
    const auto offset=static_cast<std::uint16_t>(animation-1000u);
    if(offset>727u || offset%200u>=128u) return "none";
    const auto band=offset/200u;
    const auto number=offset%200u==28u?39u:offset%200u+1u;
    constexpr const char* suffix[]={"","w","l","wl"};
    char name[16];
    std::snprintf(name,sizeof(name),"cut%03u%s",number,suffix[band]);
    return name;
}
bool dialogueCameraAnimationLoops(std::uint16_t animation) noexcept {
    // K1 Desktop 0x4a880 / K2 0x2ac580 includes the gap in the bands.
    return static_cast<std::uint16_t>(animation-1400u)<328u;
}

const std::string& dialogueVoiceResRef(const neoshared::dlg::Line& line,
                                      model::GameVersion game) noexcept {
    // K1 LoadDialogBase 0x205502..0x20564e retains Sound if valid, then
    // assigns VO_ResRef only to an empty slot. K2 Desktop 0x363890 clears it.
    return game==model::GameVersion::Kotor1 && !line.soundResRef.empty()
        ?line.soundResRef:line.voiceResRef;
}
bool dialogueVoiceUsesOverridePaths(const std::string& resref,model::GameVersion game) {
    // K1 InitializeSource 0x3bfdae has no K2 override-source probes.
    if(game==model::GameVersion::Kotor1)return false;
    auto prefix=resref.substr(0u,3u);
    for(auto& c:prefix) if(c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return prefix=="gbl" || prefix=="avo" || prefix=="000" || std::atoi(prefix.c_str())>0;
}
std::string dialogueVoicePath(const std::string& resref,model::GameVersion game) {
    // K1 Play 0x3c0ef6..0x3c0fbd nests only 16-byte n/N names whose
    // second byte is not '_': five bytes after n, then the next six bytes.
    if(game==model::GameVersion::Kotor1) {
        if(resref.size()==16u && (resref[0]=='n'||resref[0]=='N') && resref[1]!='_')
            return resref.substr(1u,5u)+"/"+resref.substr(6u,6u)+"/"+resref;
        return resref;
    }
    // CExoStreamingSoundSourceInternal::Play 0x831088..0x831314. Its
    // middle folder excludes the first three and final three characters.
    if(!dialogueVoiceUsesOverridePaths(resref,game)) return resref;
    const auto middle=resref.size()>6u?resref.substr(3u,resref.size()-6u):std::string{};
    return resref.substr(0u,3u)+"/"+middle+"/"+resref;
}

void DialogueRuntime::setDialogue(neoshared::dlg::Dialogue dialogue,
                                  std::vector<DialogueNodeTiming> timing,
                                  model::GameVersion game) {
    stop();
    events_.clear();
    dialogue_=std::move(dialogue);
    timing_=std::move(timing);
    game_=game;
}

void DialogueRuntime::setWorldTime(EngineWorldTime time,std::uint32_t dayMilliseconds) noexcept {
    worldTime_=time;
    dayMilliseconds_=dayMilliseconds;
}

void DialogueRuntime::setActiveNodeTiming(DialogueNodeTiming timing) {
    timing_.at(*active_)=timing;
    setDialogueDeadline(delayStart_,dialogueDelayMilliseconds(timing.delaySeconds,game_),
                        dayMilliseconds_,deadline_);
}

std::size_t DialogueRuntime::find(neoshared::dlg::NodeKind kind,std::size_t index) const {
    const auto it=std::find_if(dialogue_.lines.begin(),dialogue_.lines.end(),
        [=](const auto& line){return line.kind==kind && line.index==index;});
    if(it==dialogue_.lines.end()) throw std::out_of_range("Dialogue link names an absent node");
    return static_cast<std::size_t>(it-dialogue_.lines.begin());
}

void DialogueRuntime::activate(std::size_t line) {
    active_=line;
    // Successful SendDialogEntry keeps the owner's deadline; only the new
    // start stamp is sampled before the selected voice commits SetDialogDelay.
    delayStart_=worldTime_;
    waitingForReply_=false;
    needsCondition_=false;
    needsAction_=false;
    replies_.clear();
    pendingLip_=!dialogueVoiceResRef(dialogue_.lines[line],game_).empty();
    events_.push_back({DialoguePlaybackEvent::Kind::BeginNode,line});
    const auto& node=dialogue_.lines[line];
    if(!node.script.empty() || (game_==model::GameVersion::Kotor2 && !node.script2.empty())) {
        needsAction_=true;
        events_.push_back({DialoguePlaybackEvent::Kind::NeedsAction,line});
    }
}
void DialogueRuntime::begin(std::size_t entryIndex) {
    stop();
    events_.clear();
    activate(find(neoshared::dlg::NodeKind::Entry,entryIndex));
}

void DialogueRuntime::advance(const DialoguePlaybackFeedback& feedback) {
    if(!active_) return;
    if(pendingLip_ && (feedback.audioFailed ||
        (feedback.audioPlaying && feedback.audioStarted))) {
        events_.push_back({DialoguePlaybackEvent::Kind::StartLip,*active_});
        pendingLip_=false;
    }
    // SetDialogDelay precedes authored scripts (K2 Desktop 0x368b4a before
    // 0x368c07): the absolute deadline ages while the host supplies the result.
    // The client sound/LIP update continues while the server waits for a
    // reply or an external script result; node completion remains blocked.
    if(waitingForReply_ || needsCondition_ || needsAction_) return;
    const auto& timing=timing_[*active_];
    const auto flags=timing.waitFlags;
    // CSWSObject::IsDialogDelay 0x641a30: enabled live predicates OR together;
    // the world-time deadline still has to expire after those predicates clear.
    if(((flags&1u)!=0u && feedback.cameraPlaying) ||
       ((flags&2u)!=0u && feedback.audioPlaying) ||
       ((flags&4u)!=0u && feedback.animationPlaying) ||
       ((flags&8u)!=0u && feedback.fading) ||
       compareDialogueWorldTimes(worldTime_,deadline_,dayMilliseconds_)==-1) return;
    completeNode();
}

void DialogueRuntime::completeNode() {
    const auto current=*active_;
    const auto& line=dialogue_.lines[current];
    replies_.clear();
    const auto targetKind=line.kind==neoshared::dlg::NodeKind::Entry
        ?neoshared::dlg::NodeKind::Reply:neoshared::dlg::NodeKind::Entry;
    for(const auto& link:line.links) {
        // CheckScript owns the game VM. A visual replay must wait for that
        // caller's result instead of skipping or accepting a conditional link.
        if(!link.active.empty() || (game_==model::GameVersion::Kotor2 && !link.active2.empty())) {
            // K1 LoadDialogLinkedNode 0x203c28 reads only Active and Index;
            // its CheckScript 0x20477c has no K2 logic/negation/parameters.
            auto condition=link;
            if(game_==model::GameVersion::Kotor1) {
                condition={};condition.index=link.index;condition.active=link.active;
            }
            const auto result=conditionEvaluator_?conditionEvaluator_(condition):std::nullopt;
            if(!result) {
                needsCondition_=true;
                events_.push_back({DialoguePlaybackEvent::Kind::NeedsCondition,current});
                return;
            }
            if(!*result) continue;
        }
        const auto next=find(targetKind,link.index.value_or(0u));
        if(targetKind==neoshared::dlg::NodeKind::Entry) {
            activate(next);
            return;
        }
        replies_.push_back(next);
    }
    if(replies_.empty()) {stop();return;}
    // SendReplyNode assigns one pseudo-camera angle to the complete admitted
    // reply list before the client may automatically select its sole blank row.
    events_.push_back({DialoguePlaybackEvent::Kind::PrepareReplies,current,replies_});
    // K1 HandleDialogReplies 0x54f06 preserves spaces, while K2 trims them.
    // Both ShowDialogReplies callers automatically select a sole empty row.
    if(replies_.size()==1u) {
        const auto& text=dialogue_.lines[replies_.front()].text;
        const bool blank=game_==model::GameVersion::Kotor1?text.empty():
            text.find_first_not_of(' ')==std::string::npos;
        if(blank) {activate(replies_.front());return;}
    }
    waitingForReply_=true;
    events_.push_back({DialoguePlaybackEvent::Kind::Replies,current});
}

void DialogueRuntime::completeExternalActions() { needsAction_=false; }

void DialogueRuntime::resumeConditions() {
    if(!active_ || !needsCondition_) return;
    needsCondition_=false;
    completeNode();
}

void DialogueRuntime::chooseReply(std::size_t replyIndex) {
    const auto line=find(neoshared::dlg::NodeKind::Reply,replyIndex);
    if(!waitingForReply_ || std::find(replies_.begin(),replies_.end(),line)==replies_.end())
        throw std::invalid_argument("Reply is not in the current engine reply list");
    activate(line);
}
void DialogueRuntime::stop() {
    if(active_) events_.push_back({DialoguePlaybackEvent::Kind::Finished,*active_});
    // End-conversation clears the owner deadline; successful node/reply
    // continuation does not. K1 Desktop 0x259b50, K2 Desktop 0x3e4b40.
    deadline_={};
    active_.reset();
    replies_.clear();
    pendingLip_=false;
    waitingForReply_=false;
    needsCondition_=false;
    needsAction_=false;
}
std::vector<DialoguePlaybackEvent> DialogueRuntime::takeEvents() {
    auto result=std::move(events_);
    events_.clear();
    return result;
}

} // namespace neomdl
