#include <material/ForceSight.hpp>
#include <cmath>

namespace neomdl::material {
ForceSightOwnerState creatureForceSightOwner(std::int16_t alignment,std::uint16_t race) noexcept {
    ForceSightOwnerState result;
    result.glow=race!=5u; // CSWRace::CRACE_DROID
    result.nonForceBlending=true;
    if(alignment>=61) {
        const float t=static_cast<float>(alignment-60)/40.0f;
        const float mix=(1.0f-t)*0.65f;
        result.color={{t*0.01f+mix,t*0.05f+mix,1.0f,t}};
    } else if(alignment>=50) {
        // The engine central branch is signed: 51..60 produce negative alpha.
        const float t=static_cast<float>(50-alignment)/10.0f;
        const float c=(1.0f-t)*0.65f+t;
        result.color={{c,c,0.9f,t}};
    } else if(alignment>=41) {
        const float t=static_cast<float>(alignment-40)/10.0f;
        const float c=(1.0f-t)*0.65f+t;
        result.color={{1.0f,c,c,t}};
    } else {
        const float t=static_cast<float>(40-alignment)/40.0f;
        const float mix=(1.0f-t)*0.65f;
        result.color={{1.0f,t*0.05f+mix,t*0.01f+mix,t}};
    }
    return result;
}

ForceSightPlan setupForceSight(const ForceSightSettings& settings,const ForceSightOwnerState* owner,
    unsigned glowCommand,unsigned desaturateCommand,bool preserveNonForceBlend,
    bool illumination) {


    ForceSightPlan result;result.command=desaturateCommand;
    if(!owner)return result;
    if(owner->glow) {
        const auto phase=static_cast<std::uint8_t>(static_cast<std::uint64_t>(
            static_cast<float>(settings.elapsedMilliseconds)*0.125f));
        const float angle=static_cast<float>(phase)*0.02463994361460209f;
        const float pulse=std::sin(angle)*0.25f+0.75f;
        result.command=glowCommand;result.color=owner->color;
        result.color[3]*=pulse;result.replaceBlend=true;result.oneSource=true;
        return result;
    }
    result.color[3]=owner->nonForceBlending?0.75f:1.0f;
    result.replaceBlend=owner->nonForceBlending&&!preserveNonForceBlend;
    if(illumination) {
        result.command+=36u;
        if(result.command>109u)result.command=92u;
        result.illumination=true;
    }
    return result;
}
} // namespace neomdl::material
