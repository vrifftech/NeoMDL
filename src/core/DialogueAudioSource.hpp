#pragma once

#include "model/Model.hpp"
#include "core/ResourceContext.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neomdl {

struct DialogueAudioSourceSpan {
    std::size_t offset{};
    std::size_t size{};
};

// Select the bytes consumed by InitializeSource before invoking a codec.
// requestedType is the Play request (8 or 4), not the returned resource's
// filename type.
// Null means engine rejection or a span outside the supplied bytes. K2 retains
// its existing decoder path.
inline std::optional<DialogueAudioSourceSpan> dialogueAudioSourceSpan(
        const std::vector<std::uint8_t>& bytes,std::uint16_t requestedType,
        model::GameVersion game) noexcept {
    if(game==model::GameVersion::Kotor2)
        return DialogueAudioSourceSpan{0u,bytes.size()};
    const auto riffAt=[&](std::size_t offset) {
        return offset<=bytes.size() && bytes.size()-offset>=4u &&
            bytes[offset]=='R' && bytes[offset+1u]=='I' &&
            bytes[offset+2u]=='F' && bytes[offset+3u]=='F';
    };
    const auto riffLength=[&](std::size_t offset)->std::optional<std::uint32_t> {
        if(offset>bytes.size() || bytes.size()-offset<8u)return std::nullopt;
        const auto length=std::uint32_t(bytes[offset+4u]) |
            (std::uint32_t(bytes[offset+5u])<<8u) |
            (std::uint32_t(bytes[offset+6u])<<16u) |
            (std::uint32_t(bytes[offset+7u])<<24u);
        return length+std::uint32_t{8u}; // Engine DWORD unsigned addition.
    };
    if(requestedType==8u) {
        // K1 0x3bff2b..0x3bff7f: a RIFF whose declared extent differs from
        // the file length selects the trailing stream at exactly that extent.
        if(riffAt(0u)) {
            const auto extent=riffLength(0u);
            if(!extent || *extent>bytes.size())return std::nullopt;
            if(*extent!=bytes.size())
                return DialogueAudioSourceSpan{*extent,bytes.size()-*extent};
        }
        // 0x3bffe3..0x3c00ae: otherwise reject a WAV header at 470, then
        // rewind and supply the complete file to the QuickTime stream class.
        if(riffAt(470u))return std::nullopt;
        return DialogueAudioSourceSpan{0u,bytes.size()};
    }
    // K1 type 4 (0x3c024d..0x3c0381) accepts RIFF at zero or at 470.
    // CWaveFile::LoadMemory 0x3c3776/0x3c3a45 stops at RIFF's own extent,
    // so appended compressed data does not become another WAVE chunk.
    const auto offset=riffAt(0u)?0u:470u;
    if(!riffAt(offset))return std::nullopt;
    const auto extent=riffLength(offset);
    if(!extent || *extent>bytes.size()-offset)return std::nullopt;
    return DialogueAudioSourceSpan{offset,*extent};
}

// The file lookup is already scoped to one engine voice root (or the user's
// explicit resource directory). It receives actual file types; the result can
// therefore be a WAV for a requested type-8 stream. Both callers use the same
// K1 InitializeSource probes before the ordinary codec boundary.
inline std::optional<neoshared::ResourceDocument> resolveK1DialogueVoice(
        const ResourceResolver& files,const std::string& name,
        std::uint16_t requestedType) {
    if(requestedType==4u)return files(name,4u);
    if(requestedType!=8u)return std::nullopt;
    return files(name,4u);
}

} // namespace neomdl
