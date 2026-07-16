#pragma once

#include "model/Model.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace neomdl {
// The engine camera and K2 alien-voice sequences have independent cursors but
// identical DLG-resref hashing and endpoint clipping (not modulo wrapping).
class DialogueSequenceCursor final {
public:
    void initialize(const std::string& engineResref,model::GameVersion) noexcept {
        std::uint32_t seed=0u,power=1u;
        // GetResRef/CopyToString supplies a terminated 16-byte CResRef.
        for(std::size_t i=0;i<engineResref.size()&&i<16u&&engineResref[i]!='\0';++i) {
            const auto character=static_cast<std::uint32_t>(static_cast<std::int32_t>(
                static_cast<std::int8_t>(engineResref[i])));
            // CVTTSD2SI returns INT_MIN on overflow.
            const auto integerPower=i<10u?power:0x80000000u;
            seed+=character+character*integerPower;
            power*=10u;
        }
        index_=static_cast<std::int32_t>(seed%19u);
        step_=static_cast<std::int32_t>(seed%5u+1u);
        forward_=(seed&1u)!=0u;
    }
    std::uint32_t next(const std::array<std::uint32_t,19>& sequence) noexcept {
        const auto result=sequence[index_];
        index_+=forward_?step_:-step_;
        if(index_>18)index_=0;
        if(index_<0)index_=18;
        return result;
    }
private:
    std::int32_t index_{},step_{1};
    bool forward_{};
};
} // namespace neomdl
