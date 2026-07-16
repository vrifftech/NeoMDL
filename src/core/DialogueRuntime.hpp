#pragma once

#include <dlg/Dialogue.hpp>
#include "core/ActivePauseClock.hpp"
#include "model/Model.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace neomdl {

// Engine CSWSObject::IsDialogDelay inputs. These are actual playback states,
// not deadlines estimated from the decoded audio or the LIP duration.
struct DialoguePlaybackFeedback {
    bool audioPlaying{};
    bool audioStarted{};
    bool audioFailed{};
    bool cameraPlaying{};
    bool animationPlaying{};
    bool fading{};
};
struct DialogueNodeTiming {
    float delaySeconds{};
    std::uint32_t waitFlags{};
};
struct DialogueTimingContext {
    bool hasPlayers;
    bool entryVoiceResourceAvailable;
    std::string ownerTag;
};
DialogueNodeTiming dialogueNodeTiming(const neoshared::dlg::Dialogue&,
                                      const neoshared::dlg::Line&,
                                      const DialogueTimingContext&,
                                      model::GameVersion game = model::GameVersion::Kotor2);
std::string dialogueCameraAnimationName(std::uint16_t animation);
bool dialogueCameraAnimationLoops(std::uint16_t animation) noexcept;
// Engine active voice name is also the LIP name. K1 first reads Sound; K2
// clears that slot before its VO_ResRef/alien-voice producer.
const std::string& dialogueVoiceResRef(const neoshared::dlg::Line&,
    model::GameVersion game = model::GameVersion::Kotor2) noexcept;
// Relative stem beneath K1 streamwaves or K2 streamvoice, without extension.
std::string dialogueVoicePath(const std::string& resref,
    model::GameVersion game = model::GameVersion::Kotor2);
bool dialogueVoiceUsesOverridePaths(const std::string& resref,
    model::GameVersion game = model::GameVersion::Kotor2);

struct DialoguePlaybackEvent {
    enum class Kind { BeginNode, StartLip, PrepareReplies, Replies, Finished, NeedsCondition, NeedsAction };
    Kind kind{};
    // Index in Dialogue.lines, not the physical EntryList/ReplyList index.
    std::size_t lineIndex{};
    std::vector<std::size_t> lineIndices{};
};

// Client visual replay of the engine dialogue graph. It does not execute game
// scripts: conditional links are exposed to the host instead of guessed.
// A new node replaces the single client voice source and pending LIP request.
class DialogueRuntime final {
public:
    void setDialogue(neoshared::dlg::Dialogue dialogue,
                     std::vector<DialogueNodeTiming> timing,
                     model::GameVersion game = model::GameVersion::Kotor2);
    void setWorldTime(EngineWorldTime time,std::uint32_t dayMilliseconds) noexcept;
    // BeginNode selects the actual voice before committing its delay. Call
    // this for that selected timing before the next advance; provisional
    // voice timings must not overwrite the owner's retained engine deadline.
    void setActiveNodeTiming(DialogueNodeTiming timing);
    void begin(std::size_t entryIndex);
    void advance(const DialoguePlaybackFeedback& feedback);
    void chooseReply(std::size_t replyIndex);
    using ConditionEvaluator=std::function<std::optional<bool>(const neoshared::dlg::Link&)>;
    void setConditionEvaluator(ConditionEvaluator evaluator) {conditionEvaluator_=std::move(evaluator);}
    void resumeConditions();
    // The host calls this after supplying Script (K1) or Script/Script2 (K2).
    void completeExternalActions();
    void stop();
    std::vector<DialoguePlaybackEvent> takeEvents();
    const neoshared::dlg::Dialogue& dialogue() const noexcept {return dialogue_;}
    const std::optional<std::size_t>& activeLineIndex() const noexcept {return active_;}
    const std::vector<std::size_t>& replies() const noexcept {return replies_;}
    bool running() const noexcept {return active_.has_value();}
    bool waitingForReply() const noexcept {return waitingForReply_;}
    bool needsCondition() const noexcept {return needsCondition_;}
    bool needsAction() const noexcept {return needsAction_;}
private:
    std::size_t find(neoshared::dlg::NodeKind kind,std::size_t index) const;
    void activate(std::size_t line);
    void completeNode();
    neoshared::dlg::Dialogue dialogue_;
    std::vector<DialogueNodeTiming> timing_;
    std::optional<std::size_t> active_;
    std::vector<std::size_t> replies_;
    std::vector<DialoguePlaybackEvent> events_;
    ConditionEvaluator conditionEvaluator_;
    model::GameVersion game_{model::GameVersion::Kotor2};
    EngineWorldTime worldTime_{},delayStart_{},deadline_{};
    std::uint32_t dayMilliseconds_{7200000u};
    bool pendingLip_{};
    bool waitingForReply_{};
    bool needsCondition_{};
    bool needsAction_{};
};

} // namespace neomdl
