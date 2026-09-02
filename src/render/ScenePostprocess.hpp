#pragma once
#include "core/SceneEffects.hpp"
#include "model/Lighting.hpp"
#include "render/GlApi.hpp"
#include <functional>
#include <memory>
namespace neomdl::render {
class EngineMaterialState;
using SceneEffectTextureBinding=std::function<gl::UInt(SceneEffectTexture,std::string&)>;
// Per-context targets and effect graph. This is used only under the renderer's
// scene state guard; it additionally protects framebuffer/viewport/clear and
// pixel-unpack state. No OpenGL calls from its destructor.
class ScenePostprocess final {
public:
    ScenePostprocess();
    ~ScenePostprocess();
    ScenePostprocess(const ScenePostprocess&)=delete;
    ScenePostprocess& operator=(const ScenePostprocess&)=delete;
    void setResolver(gl::Loader);
    bool begin(gl::Api&,const SceneEffectsSettings&,
        std::size_t distortionProducers,const SceneEffectTextureBinding&,
        EngineMaterialState&,std::string&,
        model::GameVersion game=model::GameVersion::Kotor2,const model::FogSettings& fog={});
    // Bracket the real Gob bucket, writing only stencil bit zero.
    bool beginDistortion(std::string&);
    void endDistortion() noexcept;
    bool desktopEffects1(std::string&);
    bool desktopEffects2(std::string&);
    // Temporal writes become visible to the next frame only after success.
    bool finish(std::string&);
    void cancel() noexcept;
    void invalidateHistory() noexcept;
    void shutdown() noexcept;
    bool active() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neomdl::render
