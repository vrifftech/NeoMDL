#include "material/DirtyMaterial.hpp"
#include <cmath>
namespace neomdl::material {
bool makeDirtyMaterialPlan(const DirtyMaterialInputs& in,
        bool lm,bool env,DirtyMaterialPlan& result,std::string& error,bool legacyFragmentDispatcher) {
    result={};error.clear();const auto& s=in.settings;
    const auto fail=[&](const std::string& reason){error="Dirty material: "+reason;return false;};
    if(!s.enabled)return fail("legacy caller inspection is not enabled");
    if(s.slot>=3||!std::isfinite(s.desaturation)||!std::isfinite(s.forceAlpha))return fail("invalid explicit scene inputs");
    if(in.initializedFormula<0||in.initializedFormula>2)return fail("signed-byte formula escapes the three-command Dirty group; unrelated dispatch is not synthesized");
    auto& p=result;p.lightmap=lm;p.environment=env;p.desaturate=s.forceSight;
    const unsigned base=env?(lm?47u:41u):(lm?44u:38u);
    p.command=base+static_cast<unsigned>(in.initializedFormula)+(s.forceSight?24u:s.colorShift?42u:0u);
    p.cleanupCommand=s.forceSight?74u:32u;
    if(legacyFragmentDispatcher && s.forceSight) {
        // K2 Desktop0xf9204: each ForceSight Dirty formula selects the same
        // fixed texture stages; there is no grayscale ARB fragment selection.
        p.fixedDispatch=true;p.desaturate=false;p.forceLinear=true;
        p.volumeUnit=env?(lm?3u:2u):(lm?2u:1u);
        p.binds.push_back({DirtyTextureRole::Diffuse,0});
        if(env)p.binds.push_back({DirtyTextureRole::Environment,lm?2u:1u});
        if(lm)p.binds.push_back({DirtyTextureRole::Lightmap,1});
        p.binds.push_back({DirtyTextureRole::Volume,p.volumeUnit});
        return true;
    }
    switch(p.command) {
    case 38:p.forceLinear=true;break;
    case 44:case 47:break; // subtract variants retain the texture's min/mag filters
    case 45:case 48:p.add=true;p.forceLinear=true;break;
    case 68:p.samplesVolume=false;p.forceLinear=true;break; // actual ARB string samples retained texture[2], 2D!
    case 71:p.forceLinear=true;break;
    default:return fail("engine command "+std::to_string(p.command)+" returns without selecting a program; retained arbitrary fragment state is not a volume consumer");
    }
    p.volumeUnit=env?3u:lm?2u:1u;
    p.binds.push_back({DirtyTextureRole::Diffuse,0});
    if(env)p.binds.push_back({DirtyTextureRole::Environment,2});
    if(lm)p.binds.push_back({DirtyTextureRole::Lightmap,1});
    p.binds.push_back({DirtyTextureRole::Volume,p.volumeUnit});
    return true;
}
std::array<float,3> dirtyWorldCoordinates(std::array<float,3> q) noexcept {
    for(auto& x:q)x=x*0.5f+x;
    const auto a=q;for(unsigned i=0;i<3;++i)q[i]=a[i]*0.25f+a[(i+1)%3];
    const auto b=q;for(unsigned i=0;i<3;++i)q[i]=b[i]*0.125f+b[(i+2)%3];
    for(auto& x:q)x=x*0.067500002682209015f+x;
    return q;
}
} // namespace neomdl::material
