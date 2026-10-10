#pragma once
namespace neomdl::render {
// GLSL realization of the compatibility vertex texture-coordinate stage.
// Enabled modes come from the engine Desktop sphere/reflection TexGen calls.
inline constexpr const char* desktopFixedCoordinatesGlsl=R"GLSL(
varying vec4 gl_TexCoord[gl_MaxTextureCoords];
uniform vec4 uFixedCoordinates[6];
uniform vec4 uFixedTexgenEnabled[6];
uniform vec4 uFixedTexgenModes[6];
uniform mat4 uFixedMatrices[6];
void engineFixedTexcoords(vec4 eye,vec3 normal,vec4 coordinate0,vec4 coordinate1,bool hasCoordinate1) {
    vec3 reflection=reflect(normalize(eye.xyz),normal);
    float m=2.0*sqrt(reflection.x*reflection.x+reflection.y*reflection.y+
        (reflection.z+1.0)*(reflection.z+1.0));
    vec2 sphere=reflection.xy/m+vec2(0.5);
    for(int unit=0;unit<6;++unit) {
        vec4 coordinate=unit==0?coordinate0:(unit==1&&hasCoordinate1?coordinate1:uFixedCoordinates[unit]);
        for(int axis=0;axis<3;++axis)if(uFixedTexgenEnabled[unit][axis]!=0.0)
            coordinate[axis]=uFixedTexgenModes[unit][axis]==34066.0?reflection[axis]:
                (axis==0?sphere.x:sphere.y);
        gl_TexCoord[unit]=uFixedMatrices[unit]*coordinate;
    }
}
void engineFixedTexcoords(vec4 eye,vec3 normal,vec4 coordinate0) {
    engineFixedTexcoords(eye,normal,coordinate0,uFixedCoordinates[1],false);
}
)GLSL";
} // namespace neomdl::render
