#include "render/DesktopMaterialPrograms.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include "render/SkinMaterialPrograms.hpp"
#include <cstdio>
#include <stdexcept>
namespace neomdl::render::desktop_programs {
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
std::string lightProgram(const std::string& source,const std::vector<PreviewLight>& lights) {
    using namespace skin_programs;
    std::string result=source;result+=Light529592;
    for(unsigned n=0;n<lights.size();++n) {
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
    return result;
}
LegacyGuard::LegacyGuard(g::Api& a_,LegacyApi& x_,const MeshDrawParameters* p):a(a_),x(x_) {
    a.GetIntegerv(MatrixMode,&mode);x.GetProgramivARB(VP,ProgramBinding,&vp);x.GetProgramivARB(FP,ProgramBinding,&fp);
    x.PushAttrib(0x000fffff);
    if(p) {
        x.MatrixMode(Projection);x.PushMatrix();x.LoadMatrixf(p->projection.values.data());
        x.MatrixMode(ModelView);x.PushMatrix();x.LoadMatrixf(p->modelView.values.data());
        matrices=true;
    }
}
LegacyGuard::~LegacyGuard(){
    if(matrices){x.MatrixMode(ModelView);x.PopMatrix();x.MatrixMode(Projection);x.PopMatrix();}
    x.PopAttrib();x.BindProgramARB(VP,static_cast<g::UInt>(vp));x.BindProgramARB(FP,static_cast<g::UInt>(fp));
    x.MatrixMode(static_cast<g::Enum>(mode));
}
g::UInt ProgramCache::program(g::Api& a,g::Enum target,const std::string& text,std::string& error) {
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
        programs.emplace(text,id);return id;
    }

void ProgramCache::shutdown(){if(ready)for(const auto& entry:programs)x.DeleteProgramsARB(1,&entry.second);programs.clear();ready=false;}
} // namespace neomdl::render::desktop_programs
