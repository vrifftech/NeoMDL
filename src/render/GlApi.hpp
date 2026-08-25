#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Small, per-context dispatch table for the OpenGL 2.1 entry points actually
// used by this backend. No process-global loader, GLEW dependency or GL SDK is
// needed by the headless build. ABI signatures follow the Khronos API.
#if defined(_WIN32)
#define NEOMDL_GL_CALL __stdcall
#else
#define NEOMDL_GL_CALL
#endif
namespace neomdl::render::gl {
using Enum=std::uint32_t; using UInt=std::uint32_t; using Int=std::int32_t;
using Size=std::int32_t; using Boolean=std::uint8_t; using Bytes=std::ptrdiff_t;
using Proc=void (NEOMDL_GL_CALL*)();
using Loader=std::function<Proc(const char*)>;
inline constexpr Enum Version=0x1F02, Renderer=0x1F01, NoError=0;
inline constexpr Enum VertexShader=0x8B31, FragmentShader=0x8B30, CompileStatus=0x8B81,
    LinkStatus=0x8B82, InfoLogLength=0x8B84, CurrentProgram=0x8B8D;
inline constexpr Enum ArrayBuffer=0x8892, ElementArrayBuffer=0x8893,
    ArrayBufferBinding=0x8894, ElementArrayBufferBinding=0x8895,
    StaticDraw=0x88E4, DynamicDraw=0x88E8;
inline constexpr Enum Float=0x1406, UnsignedInt=0x1405, Triangles=0x0004, TriangleStrip=0x0005, TriangleFan=0x0006, QuadStrip=0x0008;
inline constexpr Enum Texture0=0x84C0, Texture1=0x84C1, ActiveTexture=0x84E0,
    Texture2D=0x0DE1, TextureBinding2D=0x8069, TextureCube=0x8513, TextureBindingCube=0x8514;
inline constexpr Enum Blend=0x0BE2, DepthTest=0x0B71, AlphaTest=0x0BC0,
    Lighting=0x0B50, ColorMaterial=0x0B57, LightModelAmbient=0x0B53,
    MaterialAmbient=0x1200, MaterialDiffuse=0x1201, MaterialEmission=0x1600, Fog=0x0B60, AlphaTestFunc=0x0BC1, AlphaTestRef=0x0BC2,
    CullFace=0x0B44, CullFaceMode=0x0B45, FrontFace=0x0B46,
    CW=0x0900, CCW=0x0901, Back=0x0405, Front=0x0404, FrontAndBack=0x0408;
inline constexpr Enum BlendSrcRgb=0x80C9, BlendDstRgb=0x80C8, BlendSrcAlpha=0x80CB,
    BlendDstAlpha=0x80CA, BlendEquationRgb=0x8009, BlendEquationAlpha=0x883D,
    Zero=0x0000, One=0x0001, SrcAlpha=0x0302, OneMinusSrcAlpha=0x0303,
    FuncAdd=0x8006;
inline constexpr Enum DepthWriteMask=0x0B72, DepthFunc=0x0B74, Lequal=0x0203,
    PolygonMode=0x0B40, LineWidth=0x0B21, Line=0x1B01, Fill=0x1B02;
inline constexpr Enum StencilTest=0x0B90, StencilBits=0x0D57, StencilBufferBit=0x0400,
    StencilClearValue=0x0B91, StencilFunc=0x0B92, StencilValueMask=0x0B93,
    StencilFail=0x0B94, StencilDepthFail=0x0B95, StencilDepthPass=0x0B96,
    StencilRef=0x0B97, StencilWriteMask=0x0B98, StencilBackFunc=0x8800,
    StencilBackFail=0x8801, StencilBackDepthFail=0x8802, StencilBackDepthPass=0x8803,
    StencilBackRef=0x8CA3, StencilBackValueMask=0x8CA4, StencilBackWriteMask=0x8CA5,
    ColorWriteMask=0x0C23, Keep=0x1E00, Replace=0x1E01, Greater=0x0204, Incr=0x1E02, Decr=0x1E03, IncrWrap=0x8507, DecrWrap=0x8508, Always=0x0207;
inline constexpr Enum AttribEnabled=0x8622, AttribSize=0x8623, AttribStride=0x8624,
    AttribType=0x8625, AttribNormalized=0x886A, AttribBufferBinding=0x889F,
    CurrentVertexAttrib=0x8626, AttribPointer=0x8645, MaxVertexAttribs=0x8869, MaxTextureUnits=0x8872;

#define NEOMDL_GL_FUNCTIONS(X) \
 X(const unsigned char*, GetString, (Enum)) \
 X(void, GetIntegerv, (Enum, Int*)) \
 X(void, GetBooleanv, (Enum, Boolean*)) \
 X(void, GetFloatv, (Enum, float*)) \
 X(Enum, GetError, ()) \
 X(Boolean, IsEnabled, (Enum)) \
 X(void, Enable, (Enum)) \
 X(void, Disable, (Enum)) \
 X(UInt, CreateShader, (Enum)) \
 X(void, ShaderSource, (UInt, Size, const char* const*, const Int*)) \
 X(void, CompileShader, (UInt)) \
 X(void, GetShaderiv, (UInt, Enum, Int*)) \
 X(void, GetShaderInfoLog, (UInt, Size, Size*, char*)) \
 X(void, DeleteShader, (UInt)) \
 X(UInt, CreateProgram, ()) \
 X(void, AttachShader, (UInt, UInt)) \
 X(void, BindAttribLocation, (UInt, UInt, const char*)) \
 X(void, LinkProgram, (UInt)) \
 X(void, GetProgramiv, (UInt, Enum, Int*)) \
 X(void, GetProgramInfoLog, (UInt, Size, Size*, char*)) \
 X(void, DeleteProgram, (UInt)) \
 X(void, UseProgram, (UInt)) \
 X(Int, GetUniformLocation, (UInt, const char*)) \
 X(void, GetActiveUniform, (UInt, UInt, Size, Size*, Size*, Enum*, char*)) \
 X(void, Uniform1i, (Int, Int)) \
 X(void, Uniform1f, (Int, float)) \
 X(void, Uniform3fv, (Int, Size, const float*)) \
 X(void, Uniform4fv, (Int, Size, const float*)) \
 X(void, UniformMatrix3fv, (Int, Size, Boolean, const float*)) \
 X(void, UniformMatrix4fv, (Int, Size, Boolean, const float*)) \
 X(void, GenBuffers, (Size, UInt*)) \
 X(void, DeleteBuffers, (Size, const UInt*)) \
 X(void, BindBuffer, (Enum, UInt)) \
 X(void, BufferData, (Enum, Bytes, const void*, Enum)) \
 X(void, BufferSubData, (Enum, Bytes, Bytes, const void*)) \
 X(void, EnableVertexAttribArray, (UInt)) \
 X(void, DisableVertexAttribArray, (UInt)) \
 X(void, VertexAttribPointer, (UInt, Int, Enum, Boolean, Size, const void*)) \
 X(void, GetVertexAttribiv, (UInt, Enum, Int*)) \
 X(void, GetVertexAttribfv, (UInt, Enum, float*)) \
 X(void, VertexAttrib4fv, (UInt, const float*)) \
 X(void, GetVertexAttribPointerv, (UInt, Enum, void**)) \
 X(void, ActiveTexture, (Enum)) \
 X(void, BindTexture, (Enum, UInt)) \
 X(void, GenTextures, (Size, UInt*)) \
 X(void, DeleteTextures, (Size, const UInt*)) \
 X(void, TexParameteri, (Enum, Enum, Int)) \
 X(void, TexImage2D, (Enum, Int, Int, Size, Size, Int, Enum, Enum, const void*)) \
 X(void, TexSubImage2D, (Enum, Int, Int, Int, Size, Size, Enum, Enum, const void*)) \
 X(void, CopyTexSubImage2D, (Enum, Int, Int, Int, Int, Int, Size, Size)) \
 X(void, PixelStorei, (Enum, Int)) \
 X(void, BlendFuncSeparate, (Enum, Enum, Enum, Enum)) \
 X(void, BlendEquationSeparate, (Enum, Enum)) \
 X(void, DepthMask, (Boolean)) \
 X(void, DepthFunc, (Enum)) \
 X(void, AlphaFunc, (Enum, float)) \
 X(void, Materialfv, (Enum, Enum, const float*)) \
 X(void, GetMaterialfv, (Enum, Enum, float*)) \
 X(void, LightModelfv, (Enum, const float*)) \
 X(void, PolygonMode, (Enum, Enum)) \
 X(void, LineWidth, (float)) \
 X(void, CullFace, (Enum)) \
 X(void, FrontFace, (Enum)) \
 X(void, Viewport, (Int, Int, Size, Size)) \
 X(void, Clear, (Enum)) \
 X(void, ClearColor, (float, float, float, float)) \
 X(void, ClearDepth, (double)) \
 X(void, ClearStencil, (Int)) \
 X(void, StencilFuncSeparate, (Enum, Enum, Int, UInt)) \
 X(void, StencilOpSeparate, (Enum, Enum, Enum, Enum)) \
 X(void, StencilMaskSeparate, (Enum, UInt)) \
 X(void, DepthRange, (double, double)) \
 X(void, ColorMask, (Boolean, Boolean, Boolean, Boolean)) \
 X(void, DrawBuffer, (Enum)) \
 X(void, ReadBuffer, (Enum)) \
 X(void, DrawElements, (Enum, Size, Enum, const void*))
struct Api {
#define NEOMDL_GL_DECLARE(ret, name, args) using name##Fn=ret (NEOMDL_GL_CALL*) args; name##Fn name{};
    NEOMDL_GL_FUNCTIONS(NEOMDL_GL_DECLARE)
#undef NEOMDL_GL_DECLARE
    bool load(const Loader& loader, std::string& error) {
        if (!loader) { error="No OpenGL procedure resolver"; return false; }
#define NEOMDL_GL_LOAD(ret, name, args) \
        name=reinterpret_cast<name##Fn>(loader("gl" #name)); \
        if (!name) { error="Required OpenGL entry point unavailable: gl" #name; return false; }
        NEOMDL_GL_FUNCTIONS(NEOMDL_GL_LOAD)
#undef NEOMDL_GL_LOAD
        return true;
    }
};
#undef NEOMDL_GL_FUNCTIONS
} // namespace neomdl::render::gl
