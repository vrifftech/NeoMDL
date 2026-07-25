#include "core/DialogueSpeech.hpp"
#include "core/DialogueRuntime.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace neomdl {
namespace {
std::string resref(std::string value) {
    // CResRef assignment/append: lower-case, copy at most 16 bytes, and
    // IsValid tests the first byte only. No whitespace/asterisk sentinel.
    value.resize(std::min(value.size(),std::size_t{16}));
    if(const auto zero=value.find('\0');zero!=std::string::npos)value.resize(zero);
    for(auto& c:value)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
    return value;
}
std::string append(const std::string& base,std::uint32_t variation) {
    return resref(base+std::to_string(variation));
}
}
std::vector<DialogueVoice> loadDialogueVoices(const neoshared::dlg::Dialogue& dialogue,
    const ResourceResolver& resources,model::GameVersion game,
    const neoshared::bif::JobControl& job) {
    std::map<std::string,neo2da::TwoDAFile> tables;
    const auto table=[&](const std::string& name)->const neo2da::TwoDAFile& {
        auto found=tables.find(name);
        if(found==tables.end()) {
            job.check();
            const auto resource=resources?resources(name,0x7e1u):std::nullopt;
            if(!resource)throw std::runtime_error("Alien speech requires scoped "+name+".2da");
            neo2da::TwoDAFile file;file.loadBytes(resource->bytes);job.check();
            found=tables.emplace(name,std::move(file)).first;
        }
        return found->second;
    };
    std::vector<DialogueVoice> result;result.reserve(dialogue.lines.size());
    for(const auto& line:dialogue.lines) {
        job.check();
        DialogueVoice voice{dialogueVoiceResRef(line,game),line.soundFlags.value_or(0x80u)};
        if(game==model::GameVersion::Kotor2) {
            std::string alien;
            int race=line.alienRaceNode.value_or(0);
            if(line.speaker.empty()&&race<=0)race=dialogue.alienRaceOwner.value_or(0);
            if(race>0) {
                std::string emotion;
                if(engine2da::getString(table("emotion"),line.emotion.value_or(0),"LABEL",emotion)
                    &&engine2da::getString(table("alienvo"),race,emotion,alien)) {
                    alien=resref(std::move(alien));
                    if(!alien.empty())voice.flags|=3u;
                } else alien.clear();
            }
            voice.resref=alien.empty()?resref(std::move(voice.resref)):std::move(alien);
            if(voice.resref.empty())voice.flags=0u;
        }
        result.push_back(std::move(voice));
    }
    return result;
}
std::vector<std::string> dialogueVoiceCandidates(const DialogueVoice& voice,
    neoshared::dlg::NodeKind kind,model::GameVersion game) {
    if(game==model::GameVersion::Kotor2&&kind==neoshared::dlg::NodeKind::Entry
        &&(voice.flags&6u)==2u)
        return {append(voice.resref,1u),append(voice.resref,2u),append(voice.resref,3u)};
    return {voice.resref};
}
void DialogueSpeechState::initialize(const std::string& engineResref,
    std::vector<DialogueVoice> voices,model::GameVersion game) {
    voices_=std::move(voices);game_=game;
    cursor_.initialize(engineResref,game);
}
void DialogueSpeechState::beginEntry(std::size_t lineIndex) {
    if(game_!=model::GameVersion::Kotor2)return;
    // K2 ctor 0x360fae, SendDialogEntryNode 0x3661fc,
    // and SendDialogEntry 0x367dda.
    constexpr std::array<std::uint32_t,19> sequence={1,1,2,3,1,2,1,2,2,3,3,3,1,3,2,1,3,2,2};
    const auto variation=cursor_.next(sequence);
    auto& voice=voices_.at(lineIndex);
    if((voice.flags&6u)==2u) {
        voice.resref=append(voice.resref,variation);
        voice.flags|=4u;
    }
}
std::string dialogueBarkVoice(const DialogueVoice& voice,model::GameVersion game,
                             const std::function<std::int32_t()>& engineRandom) {
    if(game==model::GameVersion::Kotor2&&(voice.flags&2u))
        return append(voice.resref,static_cast<std::uint32_t>(engineRandom()%3+1));
    return voice.resref;
}
} // namespace neomdl
