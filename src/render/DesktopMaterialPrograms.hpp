#pragma once
#include "render/GlApi.hpp"
#include <map>
#include <string>
#include <vector>
namespace neomdl::render { struct MeshDrawParameters; struct PreviewLight; }
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
    LegacyGuard(g::Api&,LegacyApi&,const MeshDrawParameters* = nullptr);
    ~LegacyGuard();
};
const char* standardFogFragment(bool kotor1);
std::string lightProgram(const std::string&,const std::vector<PreviewLight>&);
struct ProgramCache {
    g::Loader loader; LegacyApi x; bool ready{};
    std::map<std::string,g::UInt> programs;
    g::UInt program(g::Api&,g::Enum,const std::string&,std::string&);
    void shutdown();
};
} // namespace neomdl::render::desktop_programs
