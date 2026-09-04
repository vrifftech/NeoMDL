#pragma once
#include "render/GlApi.hpp"
#include <array>
#include <memory>

namespace neomdl::model { struct FogSettings; }
namespace neomdl::render {
class EngineMaterialState;
// A resolved engine branch, not a claim about the emulated game's hardware.
enum class EngineSoftShadowMode : unsigned char { Disabled, DesktopARB, DesktopATI };

struct SoftShadowTap { float u{}, v{}, weight{}; };
using SoftShadowFilter = std::array<std::array<SoftShadowTap,4>,7>;
// Desktop BloomFilterARB 0xa0dcc: 25 stored weights plus three zero padding taps,
// seven SRC_ALPHA/ONE draws, at 512x512. Taps 1..3 transpose the grid indices
// relative to tap 0 in the inspected instructions. Do not normalize the kernel
// or replace the seven RGBA8 accumulation passes with one convolution.
SoftShadowFilter engineSoftShadowFilter(float darkening = 0.4f);

// InitializeImageSpaceSoftShadows 0x971da: strictly greater power of two,
// including when the drawable dimension is already a power of two.
unsigned engineAtiShadowCaptureSize(unsigned drawableDimension) noexcept;

// Per-context implementation of Desktop SetUp/FinishSoftShadowsARB and the supplied
// executable's non-ARB ATI compatibility commands. DesktopATI deliberately retains
// its incomplete pbuffer texture objects; it is not the ARB weighted filter.
// Used only by ShaderMeshRenderer under its mesh-state guard. This object also
// protects framebuffer, viewport, compatibility matrices/texture state and
// clear/scissor state.
// All methods except construction/setResolver/destruction need a current context.
class SoftShadowPass final {
public:
    SoftShadowPass();
    ~SoftShadowPass();
    SoftShadowPass(const SoftShadowPass&)=delete;
    SoftShadowPass& operator=(const SoftShadowPass&)=delete;
    void setResolver(gl::Loader resolver);
    bool begin(gl::Api& api,float darkening,std::string& error);
    bool begin(gl::Api& api,float darkening,EngineSoftShadowMode mode,
               EngineMaterialState& materials,const model::FogSettings& fog,std::string& error);
    bool finish(std::string& error);
    void cancel() noexcept; // restore saved scene color on an interrupted pass
    void shutdown() noexcept;
private:
    bool beginPass(gl::Api& api,float darkening,EngineSoftShadowMode mode,std::string& error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neomdl::render
