#include "core/VideoEffects.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include <algorithm>

namespace neomdl {
namespace {
struct IntEntry { int value{}; bool present{}; };
IntEntry integer(const neo2da::TwoDAFile& table,int row,const char* column) {
    IntEntry out;out.present=engine2da::getInt(table,row,column,out.value);return out;
}
float real(const neo2da::TwoDAFile& table,int row,const char* column) {
    // EnableVideoEffect deliberately ignores GetFLOATEntry's success result.
    float value=0;engine2da::getFloat(table,row,column,value);return value;
}
} // namespace

VideoEffectActivation activateVideoEffect(const neo2da::TwoDAFile* table,int row,
        const SceneEffectsSettings& current,model::GameVersion game,bool desktopFrameBufferAvailable) {
    VideoEffectActivation result{current,-2};
    auto& s=result.settings;
    // DisableVideoEffect is unconditional and precedes the table/row checks.
    // Other scene effects and the retained saturation parameters survive it.
    s.saturationEnabled=false;
    s.scanNoise=false;
    if(game==model::GameVersion::Kotor2) {
        s.dream=false;
        s.forceSight=false;
        s.furyLevel=0;s.furyTexture.clear();
    }
    s.engineVideoEffect=-2;
    if(!table||row<0)return result;
    const auto enabled=[&](const char* column) {
        const auto value=integer(*table,row,column);
        if(value.present&&value.value!=0){result.effectId=row;s.engineVideoEffect=row;}
        return value.present?value.value:0;
    };
    if(enabled("EnableScanNoise"))s.scanNoise=true;
    if(enabled("EnableSaturation")) {
        s.saturationMultiplier={{real(*table,row,"ModulationRed_PC"),real(*table,row,"ModulationGreen_PC"),
            real(*table,row,"ModulationBlue_PC")}};
        s.saturation=real(*table,row,"Saturation_PC");
        s.saturationEnabled=true;
    }
    if(game==model::GameVersion::Kotor1)return result;
    if(enabled("EnableClairvoyance")){s.dream=true;s.dreamLimited=true;}
    if(enabled("EnableForceSight"))s.forceSight=desktopFrameBufferAvailable;
    if(enabled("EnableClairvoyanceFull")){s.dream=true;s.dreamLimited=false;}
    if(const auto value=enabled("EnableFury")) {
        // cwtl / movswl before EnableFury's signed-short clamp. The row ID is
        // retained even when narrowing yields zero or a negative level.
        const auto low=static_cast<unsigned>(value)&0xffffu;
        const auto level=low<0x8000u?static_cast<int>(low):static_cast<int>(low)-0x10000;
        s.furyLevel=static_cast<unsigned>(std::clamp(level,0,3));
        if(s.furyLevel)s.furyTexture="fx_Fury0"+std::to_string(s.furyLevel);
    }
    return result;
}

VideoEffectActivation loadVideoEffect(const ResourceResolver& resolver,int row,
        const SceneEffectsSettings& current,model::GameVersion game,bool desktopFrameBufferAvailable) {
    const auto& job=resourceLoadJob();job.check();
    // A host without a resource scope has no loaded engine 2DA table.
    if(row<0||!resolver)return activateVideoEffect(nullptr,row,current,game,desktopFrameBufferAvailable);
    const auto resource=resolver("VideoEffects",0x7e1);
    if(!resource)return activateVideoEffect(nullptr,row,current,game,desktopFrameBufferAvailable);
    job.update(0,0,"Decoding VideoEffects.2da");
    neo2da::TwoDAFile table;table.loadBytes(resource->bytes);job.check();
    return activateVideoEffect(&table,row,current,game,desktopFrameBufferAvailable);
}
VideoEffectActivation activateK2VideoEffect(const neo2da::TwoDAFile* table,int row,
        const SceneEffectsSettings& current,bool desktopFrameBufferAvailable) {
    return activateVideoEffect(table,row,current,model::GameVersion::Kotor2,desktopFrameBufferAvailable);
}
VideoEffectActivation loadK2VideoEffect(const ResourceResolver& resolver,int row,
        const SceneEffectsSettings& current,bool desktopFrameBufferAvailable) {
    return loadVideoEffect(resolver,row,current,model::GameVersion::Kotor2,desktopFrameBufferAvailable);
}
} // namespace neomdl
