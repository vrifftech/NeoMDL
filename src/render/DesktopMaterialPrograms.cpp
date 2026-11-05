#include "render/DesktopMaterialPrograms.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include "render/SkinMaterialPrograms.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace neomdl::render::desktop_programs {
const char* cubeEnvironmentFragment(const char* source,unsigned unit) {
    static std::map<std::string,std::string,std::less<>> sources;
    const auto found=sources.find(std::string_view(source));
    if(found!=sources.end())return found->second.c_str();
    std::string text(source);
    const auto sampler="texture["+std::to_string(unit)+"], ";
    const auto from=sampler+"2D",to=sampler+"CUBE";
    const auto at=text.find(from);text.replace(at,from.size(),to);
    return sources.emplace(source,std::move(text)).first->second.c_str();
}
const std::string& fogFragment(std::string_view source,model::FogMode mode) {
    static std::map<std::string,std::array<std::string,4>,std::less<>> sources;
    auto found=sources.find(source);
    if(found==sources.end())found=sources.emplace(std::string(source),std::array<std::string,4>{}).first;
    auto& text=found->second[static_cast<unsigned>(mode)];
    if(!text.empty())return text;
    text=source;
    if(mode!=model::FogMode::Disabled) {
        for(std::size_t at=0;(at=text.find("result.color",at))!=std::string::npos;at+=9)
            text.replace(at,12,"fogOutput");
        text.insert(text.find('\n')+1,"TEMP fogOutput, fogValue;\n"
            "PARAM fogSettings = state.fog.params;\nPARAM fogTint = state.fog.color;\n");
        text.erase(text.rfind("END"));
        text+="ABS fogValue.x, fragment.fogcoord.x;\n";
        if(mode==model::FogMode::Linear)
            text+="SUB fogValue.x, fogSettings.z, fogValue.x;\n"
                  "MUL_SAT fogValue.x, fogValue.x, fogSettings.w;\n";
        else {
            text+="MUL fogValue.x, fogSettings.x, fogValue.x;\n";
            if(mode==model::FogMode::ExponentialSquared)
                text+="MUL fogValue.x, fogValue.x, fogValue.x;\n";
            text+="MUL fogValue.x, fogValue.x, -1.442695041;\nEX2_SAT fogValue.x, fogValue.x;\n";
        }
        text+="LRP result.color.rgb, fogValue.x, fogOutput, fogTint;\n"
              "MOV result.color.a, fogOutput.a;\nEND\n";
    }
    return text;
}
const char* standardFogFragment(bool kotor1) {
    // K1 ATI initialization0x2e3abd invokes MOV through the two-argument
    // wrapper; shim0xc77e delegates it to Op1 and ignores argument2.
    static const std::string k1=[](){std::string result=skin_programs::StandardFogFragment;const auto at=result.find("LRP r0.rgb");result.insert(at,"MOV r0.a, v0.a;\n");return result;}();
    return kotor1?k1.c_str():skin_programs::StandardFogFragment;
}
std::string format(const char* text,unsigned a,unsigned b=0) {
    std::array<char,2048> bytes{};const int count=std::snprintf(bytes.data(),bytes.size(),text,a,b);
    if(count<0 || static_cast<std::size_t>(count)>=bytes.size())throw std::runtime_error("Engine light snippet overflow");
    return std::string(bytes.data(),static_cast<std::size_t>(count));
}
const std::string& ProgramCache::lightProgram(std::string_view source,const PreviewLight* lights,std::size_t count) {
    std::string pattern;pattern.reserve(count);
    for(std::size_t n=0;n<count;++n)pattern+=lights[n].position[3]==0?'d':'p';
    auto body=lightSources.find(source);
    if(body==lightSources.end())body=lightSources.emplace(std::string(source),std::map<std::string,std::string>{}).first;
    auto& variants=body->second;
    const auto old=variants.find(pattern);if(old!=variants.end())return old->second;
    using namespace skin_programs;
    std::string result(source);result+=Light529592;
    for(unsigned n=0;n<count;++n) {
        const unsigned slot=70+n*4;
        if(lights[n].position[3]==0) {
            // Explicit inspection headlight, not an authored K2 light. The
            // ordinary engine point-light snippets below are unchanged.
            result+="MOV vReg0, program.local["+std::to_string(slot)+"];\n"
                "DP3 vReg1.w, vReg0, vReg0;\nRSQ vReg1.w, vReg1.w;\n"
                "MUL vReg2, vReg0, vReg1.w;\nDP3 vReg0.x, vNormalWorld, vReg2;\nLIT vReg0, vReg0;\n";
        } else result+=format(Light5297c7,slot,slot+1);
        result+=format(Light5298e0,slot+2);result+=format(Light529943,slot+3);
    }
    result+=Light529ab0;result+=Light529ae2;result+=Light529b08;result+="END\n";
    return variants.emplace(std::move(pattern),std::move(result)).first->second;
}
LegacyGuard::LegacyGuard(g::Api& a_,LegacyApi& x_,const float* projection,
    const float* modelView):a(a_),x(x_) {
    a.GetIntegerv(MatrixMode,&mode);x.GetProgramivARB(VP,ProgramBinding,&vp);x.GetProgramivARB(FP,ProgramBinding,&fp);
    x.PushAttrib(0x000fffff);
    if(projection) {
        x.MatrixMode(Projection);x.PushMatrix();x.LoadMatrixf(projection);
        x.MatrixMode(ModelView);x.PushMatrix();x.LoadMatrixf(modelView);
        matrices=true;
    }
}
LegacyGuard::~LegacyGuard(){
    if(matrices){x.MatrixMode(ModelView);x.PopMatrix();x.MatrixMode(Projection);x.PopMatrix();}
    x.PopAttrib();x.BindProgramARB(VP,static_cast<g::UInt>(vp));x.BindProgramARB(FP,static_cast<g::UInt>(fp));
    x.MatrixMode(static_cast<g::Enum>(mode));
}
g::UInt ProgramCache::program(g::Api& a,g::Enum target,std::string_view text,std::string& error) {
        const auto old=programs.find(text);if(old!=programs.end())return old->second;
        g::UInt id{};x.GenProgramsARB(1,&id);if(!id){error="Could not allocate engine Desktop material program";return 0;}
        g::Int previous{};x.GetProgramivARB(target,ProgramBinding,&previous);
        struct RestoreBinding {
            LegacyApi& api;g::Enum target;g::UInt program;
            ~RestoreBinding(){api.BindProgramARB(target,program);}
        } restore{x,target,static_cast<g::UInt>(previous)};
        x.BindProgramARB(target,id);x.ProgramStringARB(target,0x8875,static_cast<g::Size>(text.size()),text.data());
        const auto code=a.GetError();g::Int location=-1;a.GetIntegerv(0x864b,&location);
        if(code!=g::NoError || location!=-1){
            const auto* log=a.GetString(0x8874);error="Engine Desktop ARB program rejected at "+std::to_string(location)+": "+(log?reinterpret_cast<const char*>(log):"no driver log");
            x.DeleteProgramsARB(1,&id);return 0;
        }
        programs.emplace(std::string(text),id);return id;
    }

void ProgramCache::setLocalParameters(g::Enum target,g::UInt program,g::UInt first,
        const std::array<float,4>* values,std::size_t count) {
    auto& cached=localParameters[{target,program}];
    if(cached.size()<first+count)cached.resize(first+count);
    for(std::size_t n=0;n<count;++n) {
        auto& slot=cached[first+n];
        if(slot.initialized&&std::memcmp(slot.value.data(),values[n].data(),sizeof(slot.value))==0)continue;
        x.ProgramLocalParameter4fvARB(target,static_cast<g::UInt>(first+n),values[n].data());
        std::memcpy(slot.value.data(),values[n].data(),sizeof(slot.value));slot.initialized=true;
    }
}
void ProgramCache::shutdown(){if(ready)for(const auto& entry:programs)x.DeleteProgramsARB(1,&entry.second);programs.clear();lightSources.clear();localParameters.clear();ready=false;}
} // namespace neomdl::render::desktop_programs
