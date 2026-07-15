#pragma once

#include "core/DialogueAudioSource.hpp"

namespace neomdl {

// Scene sounds always request type 4. Ordinary CResWave and streaming source
// initialization have different byte admission rules, so streaming belongs in
// the prepared-resource identity even when both requests resolve the same WAV.
inline std::optional<DialogueAudioSourceSpan> engineSceneAudioSourceSpan(
        const std::vector<std::uint8_t>& bytes,bool streaming,
        model::GameVersion game) noexcept {
    (void)game; // These type-4 boundaries agree in the K1 and K2.
    const auto matches=[&](std::size_t offset,const char* value,std::size_t count) {
        if(offset>bytes.size() || count>bytes.size()-offset)return false;
        for(std::size_t i=0;i<count;++i)
            if(bytes[offset+i]!=static_cast<std::uint8_t>(value[i]))return false;
        return true;
    };
    if(!streaming && matches(0u,"BMU V1.0",8u)) {
        // Desktop CResWave (K1 0x39a8bc / K2 0x2fffad) invokes the MP3
        // decompressor on the bytes following this exact eight-byte header.
        return DialogueAudioSourceSpan{8u,bytes.size()-8u};
    }
    // Only streaming initialization probes the 470-byte prefix on the supplied
    // little-endian clients. Desktop ordinary CResWave passes byte zero to WAV_info.
    const auto offset=streaming && !matches(0u,"RIFF",4u)?470u:0u;
    if(!matches(offset,"RIFF",4u))return std::nullopt;
    // Desktop's loaded AIL wrapper uses CWaveFile::LoadMemory. Both ordinary and
    // streamed WAV decoding stop at the DWORD RIFF extent, not physical EOF.
    if(bytes.size()-offset<8u)return std::nullopt;
    const auto length=std::uint32_t(bytes[offset+4u]) |
        (std::uint32_t(bytes[offset+5u])<<8u) |
        (std::uint32_t(bytes[offset+6u])<<16u) |
        (std::uint32_t(bytes[offset+7u])<<24u);
    const auto extent=length+std::uint32_t{8u};
    if(extent>bytes.size()-offset)return std::nullopt;
    return DialogueAudioSourceSpan{offset,extent};
}

} // namespace neomdl
