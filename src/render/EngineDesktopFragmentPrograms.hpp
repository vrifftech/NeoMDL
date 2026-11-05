#pragma once
#include "render/SkinMaterialPrograms.hpp"
#include "render/DesktopMaterialPrograms.hpp"

// K2 Desktop GLRender::EnableFragmentShadersARB program objects.
// Packed text and declaration order are retained; engine shared env registers
// are relocated to private locals supplied from EngineMaterialState.
namespace neomdl::render::desktop_programs {

// Text0x52ac54, engine object0x5e9ab4.
inline constexpr char Lightmap[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
TEMP r0, r1, r2;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
ADD r2.rgb, v0, r1;
MUL r0.a, v0, r0;
MUL r0.rgb, r0, r2;
MOV result.color, r0;
END
)ARB";

// Text0x52b697, engine object0x5e9ac4.
inline constexpr char OldBump[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB v1 = fragment.color.secondary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB T3 = fragment.texcoord[3];
TEMP r0, r1, r2, r3, r4, r5;
PARAM c[2] = { program.local[0..1] };
TEMP rTemp1, rTemp2;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], CUBE;
TEX r3, T3, texture[3], CUBE;
MAD  rTemp1,  r1, 2.0, -1.0;
MAD  rTemp2,  r3, 2.0, -1.0;
DP3 r5.xyz, rTemp1, rTemp2;
MAD  rTemp2,  r2, 2.0, -1.0;
DP3 r4.xyz, rTemp1, rTemp2;
MUL r1.xyz, r5, r5;
MOV r5.xyz, r5.z;
MUL r1.xyz, r4, c[1];
MUL r5.w, r5.x, r5.x;
MUL r1.xyz, r4, v1;
MUL r2.xyz, v1, c[0];
MUL r2.w, r5.w, r5.w;
ADD_SAT r1.xyz, v0, r1;
MUL r2.w, r2.w, r2.w;
MUL r1.xyz, r1, r0;
MUL r2.xyz, r2.w, r2;
SUB rTemp1, 1, r0;
MUL r2.xyz, r2, rTemp1.w;
ADD_SAT r0.xyz, r2, r1;
MOV r0.w, v0.w;
MOV result.color, r0;
END
)ARB";

// Text0x52ba27, engine object0x5e9ac8.
inline constexpr char EnvironmentLightmap[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
PARAM c[1] = { program.local[0] };
TEMP r0, r1, r2, r3, r4;
TEMP rTemp0;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 2D;
MUL r3.rgb, v0, r0;
MAD r3.rgb, r1, r0, r3;
SUB rTemp0, 1.0, r0;
MUL r4.rgb, r2, rTemp0.a;
ADD r0.rgb, r3, r4;
MOV r0.a, c[0].a;
MOV result.color, r0;
END
)ARB";

// Text0x52bbf3, engine object0x5e9ae0.
inline constexpr char CubeEnvironmentLightmap[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
PARAM c[1] = { program.local[0] };
TEMP r0, r1, r2, r3, r4;
TEMP rTemp0;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], CUBE;
MUL r3.rgb, v0, r0;
MAD r3.rgb, r1, r0, r3;
SUB rTemp0, 1.0, r0;
MUL r4.rgb, r2, rTemp0.a;
ADD r0.rgb, r3, r4;
MOV r0.a, c[0].a;
MOV result.color, r0;
END
)ARB";

// Text0x52c54f, engine object0x5e9b00.
inline constexpr char BumpGlow[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB T3 = fragment.texcoord[3];
ATTRIB T4 = fragment.texcoord[4];
PARAM c1 = program.local[0];
PARAM c7 = program.local[7];
TEMP r0, r1, r2, r3, r4, r5;
TEMP rTemp0, rTemp1;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], CUBE;
TEX r2, T2, texture[2], CUBE;
TEX r3, T3, texture[3], CUBE;
TEX r4, T4, texture[4], CUBE;
MAD  rTemp1,  r0, 2.0, -1.0;
MAD  rTemp0,  r1, 2.0, -1.0;
DP3 r5.r, rTemp0, rTemp1;
MAD  rTemp0,  r2, 2.0, -1.0;
DP3 r5.g, rTemp0, rTemp1;
MAD  rTemp0,  r3, 2.0, -1.0;
DP3 r5.b, rTemp0, rTemp1;
MAD  rTemp0,  r4, 2.0, -1.0;
DP3 r3.rgb, r5, rTemp0;
MUL r3.rgb, r3, 2.0;
MUL r3.rgb, r5, r3;
DP3 r2.rgb, r5, r5;
MAD rTemp1, -r4, 2.0, 1.0;MAD r3.xyz, rTemp1, r2, r3;TEX r5, r3, texture[5], CUBE;
DP3 r3.rgb, r5, c7;
MUL r2.rgb, r3, c1;
MAD r0.rgb, c1, c1.a, r2;MUL r0.a, r3.a, c7.a;
MOV result.color, r0;
END
)ARB";

// Text0x52ca2e, engine object0x5e9b08.
inline constexpr char LightmapDesaturate[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
PARAM c7 = program.local[7];
TEMP r0, r1, r2;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
MUL r2.rgb, r0, r1;
MAD r2.rgb, r0, v0, r2;
DP3 r2.rgb, r2, c7;
MUL r0.rgb, r2, c7.a;
MUL r2.a, r0.a, v0.a;
MOV result.color, r0;
END
)ARB";

// Text0x52cd22, engine object0x5e9b10.
inline constexpr char EnvironmentLightmapDesaturate[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
PARAM c7 = program.local[7];
PARAM c1 = program.local[1];
TEMP r0, r1, r2, r3, rTemp0;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 2D;
MUL r3.rgb, r0, r1;
MAD r3.rgb, r0, v0, r3;
MOV r3.a, 1;
SUB rTemp0, 1.0, r0;
MAD r3.rgb, r2, rTemp0.a, r3;
DP3 r3.rgb, r3, c7;
MUL r0.rgb, r3, c7.a;
MOV r0.a, c1.a;
MOV result.color, r0;
END
)ARB";

// Text0x52cf1f, engine object0x5e9b14.
inline constexpr char DirtyLightmapDesaturate[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
PARAM c7 = program.local[7];
PARAM c1 = program.local[1];
TEMP r0, r1, r2, r3, r4;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 3D;
SUB r3.rgb, r0, r2;
MUL r4.rgb, r3, r1;
MOV r0.a, 1;
MAD r0.rgb, r3, v0, r4;
DP3 r3.rgb, r0, c7;
MUL r0.rgb, r3, c7.a;
MUL r0.a, r0.a, c1.a;
MOV result.color, r0;
END
)ARB";

// Text0x52d0ff, engine object0x5e9b18.
inline constexpr char DirtyEnvironmentLightmapDesaturate[] = R"ARB(!!ARBfp1.0
ATTRIB v0 = fragment.color.primary;
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB T3 = fragment.texcoord[3];
PARAM c7 = program.local[7];
PARAM c1 = program.local[1];
TEMP r0, r1, r2, r3, r4, rTemp0;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 2D;
TEX r3, T3, texture[3], 3D;
SUB rTemp0, 1.0, r0;
MAD r4.rgb, r2, rTemp0.a, r0;
SUB r4.rgb, r4, r3;
MUL r3.rgb, r4, r1;
MAD r0.rgb, r4, v0, r3;
MOV r0.a, 1;
DP3 r3.rgb, r0, c7;
MUL r0.rgb, r3, c7.a;
MUL r0.a, r0.a, c1.a;
MOV result.color, r0;
END
)ARB";

// Text0x52d358, engine object0x5e9b1c.
inline constexpr char Hologram[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
PARAM c7 = program.local[7];
TEMP r0, r1, r2;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
DP3 r2.rgb, r0, c7;
MUL r0.rgb, r2, r1;
MOV r0.a, r1.a;
MOV result.color, r0;
END
)ARB";

// Text0x52d45e, engine object0x5e9b20.
inline constexpr char HologramDesaturate[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
PARAM c7 = program.local[7];
TEMP r0, r1, r2, r3;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
DP3 r2.rgb, r0, c7;
MUL r3.rgb, r2, r1;
DP3 r0.rgb, r3, c7;
MUL r0.a, r1.a, c7.a;
MOV result.color, r0;
END
)ARB";

// Text0x52d582, engine object0x5e9b24.
inline constexpr char Illumination[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB v0 = fragment.color.primary;
PARAM c1 = program.local[1];
TEMP temp1, r0, r1;
TEX r0, T0, texture[0], 2D;
MAD temp1, c1, 2, -1.0;
ADD r1.rgb, v0, temp1;
MUL r0.rgb, r0, r1;
MOV result.color, r0;
END
)ARB";

// Text0x52d67c, engine object0x5e9b28.
inline constexpr char EnvironmentIllumination[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB v0 = fragment.color.primary;
PARAM c1 = program.local[1];
TEMP temp1, r0, r1, r2;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], CUBE;
MAD temp1, c1, 2, -1.0;
ADD r2.rgb, v0, temp1;
MUL r0.rgb, r0, r2;
LRP r0.rgb, r0.a, r0, r1;
MOV r0.a, c1.a;
MOV result.color, r0;
END
)ARB";

inline const std::string SphereEnvironmentIllumination=[] {
    std::string source=EnvironmentIllumination;
    const std::string sampler="texture[1], CUBE";
    source.replace(source.find(sampler),sampler.size(),"texture[1], 2D");
    return source;
}();

// Text0x52d7e4, engine object0x5e9ae4.
inline constexpr char Dirty[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB v0 = fragment.color.primary;
TEMP r0, r1;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 3D;
SUB r0.rgb, r0, r1;
MUL r0.rgb, r0, v0;
MOV result.color, r0;
END
)ARB";

// Text0x52d8df, engine object0x5e9ae8.
inline constexpr char DirtyLightmapMultiply[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB v0 = fragment.color.primary;
TEMP r0, r1, r2, r3, r4;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 3D;
SUB r3.rgb, r0, r2;
MUL r4.rgb, r3, r1;
MAD r0.rgb, r3, v0, r4;
MOV result.color, r0;
END
)ARB";

// Text0x52da3c, engine object0x5e9aec.
inline constexpr char DirtyLightmapAdd[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB v0 = fragment.color.primary;
TEMP r0, r1, r2, r3, r4;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 3D;
ADD r3.rgb, r0, r2;
MUL r4.rgb, r3, r1;
MAD r0.rgb, r3, v0, r4;
MOV result.color, r0;
END
)ARB";

// Text0x52db99, engine object0x5e9af0.
inline constexpr char DirtyEnvironmentLightmapMultiply[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB T3 = fragment.texcoord[3];
ATTRIB v0 = fragment.color.primary;
TEMP r0, r1, r2, r3, r4, rTemp0;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 2D;
TEX r3, T3, texture[3], 3D;
SUB rTemp0, 1.0, r0;
MAD r4.rgb, r2, rTemp0.a, r0;
SUB r4.rgb, r4, r3;
MUL r3.rgb, r4, r1;
MAD r0.rgb, r4, v0, r3;
MOV r0.a, 1;
MOV result.color, r0;
END
)ARB";

// Text0x52dd7c, engine object0x5e9af4.
inline constexpr char DirtyEnvironmentLightmapAdd[] = R"ARB(!!ARBfp1.0
ATTRIB T0 = fragment.texcoord[0];
ATTRIB T1 = fragment.texcoord[1];
ATTRIB T2 = fragment.texcoord[2];
ATTRIB T3 = fragment.texcoord[3];
ATTRIB v0 = fragment.color.primary;
TEMP r0, r1, r2, r3, r4, rTemp0;
TEX r0, T0, texture[0], 2D;
TEX r1, T1, texture[1], 2D;
TEX r2, T2, texture[2], 2D;
TEX r3, T3, texture[3], 3D;
SUB rTemp0, 1.0, r0;
MAD r4.rgb, r2, rTemp0.a, r0;
ADD r4.rgb, r4, r3;
MUL r3.rgb, r4, r1;
MAD r0.rgb, r4, v0, r3;
MOV r0.a, 1;
MOV result.color, r0;
END
)ARB";

// Selection only: callers retain the current program on nullptr and apply
// the separate engine disable/unbind commands. The cube selector follows
// the environment texture for both ordinary and lightmapped reflections.
// Engine null-texture admission (e.g.38) belongs to the calling dispatcher.
inline const char* k2FragmentProgram(unsigned command,bool cube=false) {
    switch(command) {
    case 2:return skin_programs::FogFragment;
    case 3:return skin_programs::StandardFogFragment;
    case 6:return skin_programs::BumpFragment;
    case 7:return OldBump;
    case 24:return skin_programs::ShinyFragment;
    case 26:return Lightmap;
    case 38:return Dirty;
    case 44:return DirtyLightmapMultiply;
    case 45:return DirtyLightmapAdd;
    case 47:return cube?cubeEnvironmentFragment(DirtyEnvironmentLightmapMultiply,2):DirtyEnvironmentLightmapMultiply;
    case 48:return cube?cubeEnvironmentFragment(DirtyEnvironmentLightmapAdd,2):DirtyEnvironmentLightmapAdd;
    case 50:return skin_programs::ForceGlow;
    case 52:return cube?cubeEnvironmentFragment(skin_programs::ForceEnvironmentGlow,1):skin_programs::ForceEnvironmentGlow;
    case 55:return BumpGlow;
    case 56:return skin_programs::ForceDesaturate;
    case 57:return LightmapDesaturate;
    case 58:return cube?cubeEnvironmentFragment(skin_programs::ForceEnvironmentDesaturate,1):skin_programs::ForceEnvironmentDesaturate;
    case 59:return cube?cubeEnvironmentFragment(EnvironmentLightmapDesaturate,2):EnvironmentLightmapDesaturate;
    case 68:return DirtyLightmapDesaturate;
    case 71:return cube?cubeEnvironmentFragment(DirtyEnvironmentLightmapDesaturate,2):DirtyEnvironmentLightmapDesaturate;
    case 75:return Hologram;
    case 76:return HologramDesaturate;
    case 78:return Illumination;
    case 79:return cube?EnvironmentIllumination:SphereEnvironmentIllumination.c_str();
    case 92:return skin_programs::ForceIlluminationDesaturate;
    case 93:return skin_programs::ForceIlluminationDesaturate;
    case 19:return cube?skin_programs::CubeEnvironmentFragment:skin_programs::EnvironmentFragment;
    case 20:return cube?skin_programs::CubeEnvironmentFogFragment:skin_programs::EnvironmentFogFragment;
    case 22:return cube?CubeEnvironmentLightmap:EnvironmentLightmap;
    default:return nullptr;
    }
}
} // namespace neomdl::render::desktop_programs
