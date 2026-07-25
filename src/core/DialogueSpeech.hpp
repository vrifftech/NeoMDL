#pragma once

#include "core/DialogueSequence.hpp"
#include "core/ModelDocument.hpp"
#include <dlg/Dialogue.hpp>
#include <functional>
#include <string>
#include <vector>

namespace neomdl {
struct DialogueVoice {
    std::string resref;
    std::uint8_t flags{};
};
// LoadDialogBase's active sound slot, before SendDialogEntry appends a variant.
// Table I/O stays in the loading job; playback never resolves resources.
std::vector<DialogueVoice> loadDialogueVoices(const neoshared::dlg::Dialogue&,
    const ResourceResolver&,model::GameVersion,const neoshared::bif::JobControl&);
// The worker decodes this finite engine candidate set before playback.
std::vector<std::string> dialogueVoiceCandidates(const DialogueVoice&,
    neoshared::dlg::NodeKind,model::GameVersion);
class DialogueSpeechState final {
public:
    void initialize(const std::string& engineResref,std::vector<DialogueVoice>,
        model::GameVersion);
    // Call once for every SendDialogEntryNode, including non-alien entries.
    // Revisiting a node consumes the stream but retains its appended name.
    void beginEntry(std::size_t lineIndex);
    const DialogueVoice& voice(std::size_t lineIndex) const {return voices_.at(lineIndex);}
private:
    std::vector<DialogueVoice> voices_;
    DialogueSequenceCursor cursor_;
    model::GameVersion game_{model::GameVersion::Kotor2};
};
// BarkOneLine copies the loaded base and consumes the shared engine RNG only
// when bit 2 is set. This does not mutate an entry or advance the DLG sequence.
std::string dialogueBarkVoice(const DialogueVoice&,model::GameVersion,
                             const std::function<std::int32_t()>& engineRandom);
} // namespace neomdl
