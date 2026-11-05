#pragma once
#include "render/GlApi.hpp"
#include <model/Lighting.hpp>
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace neomdl::render { struct PreviewLight; }
namespace neomdl::render::desktop_programs {
namespace g=gl;
constexpr g::Enum VP=0x8620, FP=0x8804, ProgramBinding=0x8677;
constexpr g::Enum ModelView=0x1700, Projection=0x1701, MatrixMode=0x0ba0, Fog=0x0b60;
#define DESKTOP_PROGRAM_GL(X) \
 X(void,GenProgramsARB,(g::Size,g::UInt*)) \
 X(void,DeleteProgramsARB,(g::Size,const g::UInt*)) \
 X(void,BindProgramARB,(g::Enum,g::UInt)) \
 X(void,ProgramStringARB,(g::Enum,g::Enum,g::Size,const void*)) \
 X(void,ProgramLocalParameter4fvARB,(g::Enum,g::UInt,const float*)) \
 X(void,GetProgramivARB,(g::Enum,g::Enum,g::Int*)) \
 X(void,PushAttrib,(g::UInt)) X(void,PopAttrib,()) \
 X(void,MatrixMode,(g::Enum)) X(void,PushMatrix,()) X(void,PopMatrix,()) \
 X(void,LoadMatrixf,(const float*)) \
 X(void,TexEnvi,(g::Enum,g::Enum,g::Int)) \
 X(void,TexEnvfv,(g::Enum,g::Enum,const float*)) \
 X(void,TexGeni,(g::Enum,g::Enum,g::Int)) \
 X(void,Fogf,(g::Enum,float)) X(void,Fogfv,(g::Enum,const float*)) \
 X(void,AlphaFunc,(g::Enum,float))
struct LegacyApi {
#define DECLARE(ret,name,args) using name##Fn=ret (NEOMDL_GL_CALL*)args;name##Fn name{};
 DESKTOP_PROGRAM_GL(DECLARE)
#undef DECLARE
 bool load(const g::Loader& loader,std::string& error) {
#define LOAD(ret,name,args) name=reinterpret_cast<name##Fn>(loader("gl" #name));if(!name){error="Engine Desktop material program needs gl" #name;return false;}
 DESKTOP_PROGRAM_GL(LOAD)
#undef LOAD
 return true;
 }
};

#undef DESKTOP_PROGRAM_GL
struct LegacyGuard {
    g::Api& a; LegacyApi& x; g::Int mode{},vp{},fp{}; bool matrices{};
    LegacyGuard(g::Api&,LegacyApi&,const float* projection=nullptr,
                const float* modelView=nullptr);
    ~LegacyGuard();
};
const char* standardFogFragment(bool kotor1);
const std::string& fogFragment(std::string_view source,model::FogMode);
struct ProgramCache {
    g::Loader loader; LegacyApi x; bool ready{};
    std::map<std::string,g::UInt,std::less<>> programs;
    std::map<std::string,std::map<std::string,std::string>,std::less<>> lightSources;
    struct LocalParameter {std::array<float,4> value{};bool initialized{};};
    std::map<std::pair<g::Enum,g::UInt>,std::vector<LocalParameter>> localParameters;
    const std::string& lightProgram(std::string_view,const PreviewLight*,std::size_t);
    g::UInt program(g::Api&,g::Enum,std::string_view,std::string&);
    void setLocalParameters(g::Enum,g::UInt,g::UInt,const std::array<float,4>*,std::size_t);
    void shutdown();
};
} // namespace neomdl::render::desktop_programs
