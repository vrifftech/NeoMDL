#pragma once
#include "render/DesktopMaterialPrograms.hpp"
#include <stdexcept>

namespace neomdl::render {
// K2Desktop ATI postprocess has no ATI fragment implementation. These are the
// actual compatibility entry points used by its retained fixed pipeline.
struct DesktopAtiPostprocessApi : desktop_programs::ProgramCache {
#define ATI_POST_GL(X) \
 X(void,Begin,(gl::Enum)) X(void,End,()) \
 X(void,Ortho,(double,double,double,double,double,double)) \
 X(void,Color4f,(float,float,float,float)) X(void,Vertex4f,(float,float,float,float)) \
 X(void,MultiTexCoord4f,(gl::Enum,float,float,float,float)) \
 X(void,TexImage1D,(gl::Enum,gl::Int,gl::Int,gl::Size,gl::Int,gl::Enum,gl::Enum,const void*))
#define DECLARE(ret,name,args) using name##Fn=ret(NEOMDL_GL_CALL*)args;name##Fn name{};
 ATI_POST_GL(DECLARE)
#undef DECLARE
 void load(const gl::Loader& resolve) {
     if(ready)return;
     std::string error;
     if(!x.load(resolve,error))throw std::runtime_error(error);
#define LOAD(ret,name,args) name=reinterpret_cast<name##Fn>(resolve("gl" #name));if(!name)throw std::runtime_error("Engine ATI postprocess requires gl" #name);
 ATI_POST_GL(LOAD)
#undef LOAD
     ready=true;
 }
#undef ATI_POST_GL
};
// Original K2Desktop0x56cf3b, also used by the ARB branch. Only storage changes
// from global env[15] to this program's private local[15]. No fragment added.
inline constexpr char DesktopAtiForceVertex[]=R"ARB(!!ARBvp1.0
TEMP vReg0, vReg1, vReg2, vReg4, vReg3;
DP4 result.position.x, state.matrix.mvp.row[0], vertex.position;
DP4 result.position.y, state.matrix.mvp.row[1], vertex.position;
DP4 result.position.z, state.matrix.mvp.row[2], vertex.position;
DP4 result.position.w, state.matrix.mvp.row[3], vertex.position;
MOV result.texcoord[0], vertex.texcoord[0];
MOV vReg1, vertex.texcoord[1];
MOV vReg2, vertex.texcoord[2];
MOV vReg3.x, vReg1.x;
MOV vReg3.y, program.local[15].x;
MOV vReg3.z, vReg2.x;
MOV vReg4.x, program.local[15].x;
MOV vReg4.y, vReg1.y;
MOV vReg4.z, vReg2.y;
MOV result.texcoord[1].xyz, vReg3;
MOV result.texcoord[2].xyz, vReg4;
MOV result.color.primary, program.local[15].y;
END
)ARB";
}
