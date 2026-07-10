#pragma once

#include <array>
#include <cstdint>

namespace neomdl {

// CExoRandInternal's independent four-component generator. This is not the
// CRT stream used by particles. The engine use uint32 words.
// The argument is a captured AFTER-construction state; no implicit host seed.
class ExoRandom final {
public:
    using State=std::array<std::uint32_t,4>;
    explicit ExoRandom(State capturedState) noexcept : state_(capturedState) {}
    const State& state() const noexcept {return state_;}
    std::uint32_t next() noexcept {


        constexpr State shifts{6u,2u,13u,3u};
        constexpr State right{11u,17u,21u,10u};
        constexpr State left{20u,12u,7u,15u};
        constexpr State masks{0xfffffffeu,0xfffffff8u,0xfffffff0u,0xffffff80u};
        for(std::size_t i=0;i<4;++i) {
            const auto value=state_[i];
            state_[i]=(((value<<shifts[i])^value)>>right[i])^((value&masks[i])<<left[i]);
        }
        return state_[0]^state_[1]^state_[2]^state_[3];
    }
    void reSeed(State inputs) noexcept {
        // ReSeed retains each prior component unless input > 1<<(p-s),
        // and consumes one complete Rand before returning, even if all fail.
        constexpr State minimum{1u<<11u,1u<<17u,1u<<21u,1u<<10u};
        for(std::size_t i=0;i<4;++i)if(inputs[i]>minimum[i])state_[i]=inputs[i];
        (void)next();
    }
    static ExoRandom fromSeedWords(State inputs) noexcept {
        // The engine constructor writes these words before its entropy-derived
        // GetString(16) seed. Inputs here are those four captured seed words;
        // this factory does not replace GenSeed's external/uninitialized data.
        ExoRandom result({390451501u,613566701u,858993401u,943651322u});
        result.reSeed(inputs);return result;
    }
private:
    State state_;
};

} // namespace neomdl
