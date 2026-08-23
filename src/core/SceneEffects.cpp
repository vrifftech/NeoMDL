#include "core/SceneEffects.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace neomdl {
unsigned engineDesktopAtiCaptureSize(unsigned value) noexcept {
    value|=value>>1u;value|=value>>2u;value|=value>>4u;
    value|=value>>8u;value|=value>>16u;
    unsigned bits=0;
    while(value) {++bits;value>>=1u;}
    return 1u<<(bits&31u);
}
SceneEffectDimensions sceneEffectDimensions(int w,int h,model::GameVersion game,bool desktopArbShaders) {
    if(w<2 || h<2 || w>32768 || h>32768)throw std::invalid_argument("Invalid scene-effects viewport dimensions (2..32768 required)");
    // K1 FindSmallestPowerOf2WidthHeight0x311c18 and K2 ATI init0x98e09
    // (log2integer0x97bb6 plus1) round strictly up,
    // including an input that is already a power of two.
    return {w,h,128,128,(game==model::GameVersion::Kotor1||!desktopArbShaders)?static_cast<int>(engineDesktopAtiCaptureSize(w)):w,
        (game==model::GameVersion::Kotor1||!desktopArbShaders)?static_cast<int>(engineDesktopAtiCaptureSize(h)):h};
}
bool validateSceneEffects(const SceneEffectsSettings& s,std::string& error,model::GameVersion game) {
    error.clear();
    if((!s.engineVideoEffect&&(!std::isfinite(s.saturation)||s.saturation<0||s.saturation>1))||
       !std::isfinite(s.accumulationRatio)||
       s.accumulationRatio<0 || s.accumulationRatio>1) {
        error="Scene saturation and accumulation must be finite and in [0,1]";return false;
    }
    if(game==model::GameVersion::Kotor2 && s.desktopDreamLetterbox && ((*s.desktopDreamLetterbox)[0]<0 || (*s.desktopDreamLetterbox)[1]<=(*s.desktopDreamLetterbox)[0] || (*s.desktopDreamLetterbox)[1]>32768)) {
        error="Desktop dream letterbox requires 0 <= top < bottom <= 32768";return false;
    }
    if(!s.engineVideoEffect)for(float v:s.saturationMultiplier)if(!std::isfinite(v)||v<0) {error="Scene saturation multipliers must be finite and nonnegative";return false;}
    return true;
}
std::vector<SceneEffectTexture> requiredSceneEffectTextures(const SceneEffectsSettings& s,
        bool distortion,model::GameVersion game) {
    std::vector<SceneEffectTexture> out;
    if(!s.enabled)return out;
    if(s.filmNoise || s.scanNoise)out.push_back(SceneEffectTexture::FilmNoise);
    if(distortion || (game==model::GameVersion::Kotor2&&(s.dream || s.furyLevel)))out.push_back(SceneEffectTexture::ForceDistortion);
    if(game==model::GameVersion::Kotor2&&s.dream)out.push_back(SceneEffectTexture::Dream);
    if(game==model::GameVersion::Kotor2&&s.furyLevel)out.push_back(SceneEffectTexture::Fury);
    return out;
}
const std::string& sceneEffectTextureName(const SceneEffectsSettings& s,SceneEffectTexture t) {
    switch(t) {case SceneEffectTexture::FilmNoise:return s.filmNoiseTexture;
    case SceneEffectTexture::ForceDistortion:return s.forceDistortionTexture;
    case SceneEffectTexture::Dream:return s.dreamTexture;
    case SceneEffectTexture::Fury:return s.furyTexture;}
    throw std::invalid_argument("Unknown scene effect texture");
}
const char* sceneEffectTextureLabel(SceneEffectTexture t) noexcept {
    switch(t) {case SceneEffectTexture::FilmNoise:return "film/scan noise";
    case SceneEffectTexture::ForceDistortion:return "force distortion";
    case SceneEffectTexture::Dream:return "dream outline";
    case SceneEffectTexture::Fury:return "fury outline";}return "unknown";
}
std::array<std::array<float,3>,3> desktopSceneSaturationMatrix(const SceneEffectsSettings& s) {
    constexpr float weights[3]={.3086f,.6094f,.082f};
    std::array<std::array<float,3>,3> result{};
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col) {
        float v=((1-s.saturation)*weights[col]+(row==col?s.saturation:0))*s.saturationMultiplier[row];
        if(row!=2 || col!=2)v=std::min(1.0f,v);
        result[row][col]=v;
    }
    return result;
}
} // namespace neomdl
