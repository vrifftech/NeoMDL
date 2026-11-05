#pragma once
// K1 Desktop i386 engine rigid vertex programs. The ARB bodies also occur byte for
// byte in the K2 Desktop packed. These rigid bodies have no existing
// SkinMaterialPrograms definition; fragment programs and lighting snippets are
// reused from that shared renderer header by K1MaterialPass.
// Only vertex input bindings and env-to-local constant bindings are relocated.
// Engine VertexProgram::Enable appends lighting snippets as requested, then END.
namespace neomdl::render::k1_programs {
// K1 source 0x489ce8.
inline constexpr char Standard[] = R"ENGINE(!!ARBvp1.0
PARAM mvMatrix[4] = { state.matrix.modelview[0] };
PARAM mvpMatrix[4] = { state.matrix.mvp };
TEMP vTemp;
TEMP vReg0, vReg1, vReg2, vReg3;
TEMP vReg10;
TEMP vNormalWorld;
ALIAS vFinalColour = vTemp;
DP4   result.position.x, mvpMatrix[0], vertex.attrib[0];
DP4   result.position.y, mvpMatrix[1], vertex.attrib[0];
DP4   result.position.z, mvpMatrix[2], vertex.attrib[0];
DP4   result.position.w, mvpMatrix[3], vertex.attrib[0];
DP4   vReg10.x, mvMatrix[0], vertex.attrib[0];
DP4   vReg10.y, mvMatrix[1], vertex.attrib[0];
DP4   vReg10.z, mvMatrix[2], vertex.attrib[0];
DP4   vReg10.w, mvMatrix[3], vertex.attrib[0];
DP3   vNormalWorld.x, mvMatrix[0], vertex.attrib[2];
DP3   vNormalWorld.y, mvMatrix[1], vertex.attrib[2];
DP3   vNormalWorld.z, mvMatrix[2], vertex.attrib[2];
DP3	vReg0.x, vReg10, vReg10;
RSQ	vReg0.x, vReg0.x;
RCP	vReg0.x, vReg0.x;
MOV result.fogcoord, vReg0.x;
MOV   result.texcoord[0], vertex.attrib[8];
MOV   result.texcoord[1], vertex.attrib[9];
MOV   result.texcoord[2], vertex.attrib[10];
MOV   result.texcoord[3], vertex.attrib[11];
MOV result.color, program.local[15].wwwz;
)ENGINE";
// K1 source 0x488d64.
inline constexpr char StandardFog[] = R"ENGINE(!!ARBvp1.0
PARAM const0 = 0.0;
PARAM const1 = 1.0;
PARAM mvMatrix[4] = { state.matrix.modelview[0] };
PARAM mvpMatrix[4] = { state.matrix.mvp };
TEMP vTemp;
TEMP vReg0, vReg1, vReg2, vReg3;
TEMP vReg10;
TEMP vNormalWorld;
ALIAS vFinalColour = vTemp;
DP4   result.position.x, mvpMatrix[0], vertex.attrib[0];
DP4   result.position.y, mvpMatrix[1], vertex.attrib[0];
DP4   result.position.z, mvpMatrix[2], vertex.attrib[0];
DP4   result.position.w, mvpMatrix[3], vertex.attrib[0];
DP4   vReg10.x, mvMatrix[0], vertex.attrib[0];
DP4   vReg10.y, mvMatrix[1], vertex.attrib[0];
DP4   vReg10.z, mvMatrix[2], vertex.attrib[0];
DP4   vReg10.w, mvMatrix[3], vertex.attrib[0];
DP3   vNormalWorld.x, mvMatrix[0], vertex.attrib[2];
DP3   vNormalWorld.y, mvMatrix[1], vertex.attrib[2];
DP3   vNormalWorld.z, mvMatrix[2], vertex.attrib[2];
DP3	vReg0.x, vReg10, vReg10;
RSQ	vReg0.x, vReg0.x;
RCP	vReg0.x, vReg0.x;
ADD	vReg0.x, -state.fog.params.y, vReg0;
MOV    result.color.xyz, state.fog.color;
MUL	result.color.w, vReg0.x, state.fog.params.w;
MOV result.fogcoord, const1;
)ENGINE" "END\n";
// K1 source 0x48a270 + ATI suffix 0x48a130.
inline constexpr char Shiny[] = R"ENGINE(!!ARBvp1.0
ATTRIB vTex0 = vertex.attrib[8];
ATTRIB vTex4 = vertex.attrib[12];
ATTRIB vTex5 = vertex.attrib[13];
ATTRIB vTex6 = vertex.attrib[14];
PARAM mvMatrix[4] = { state.matrix.modelview };
PARAM mvpMatrix[4] = { state.matrix.mvp };
TEMP vReg1, vReg3, vReg10;
TEMP vReg0;
TEMP vFinalColour;
DP4   result.position.x, mvpMatrix[0], vertex.attrib[0];
DP4   result.position.y, mvpMatrix[1], vertex.attrib[0];
DP4   result.position.z, mvpMatrix[2], vertex.attrib[0];
DP4   result.position.w, mvpMatrix[3], vertex.attrib[0];
DP3   vReg1.x, mvMatrix[0], vTex4; DP3   vReg10.x, mvMatrix[0], vTex5; DP3   vReg3.x, mvMatrix[0], vTex6; DP3   vReg1.y, mvMatrix[1], vTex4; DP3   vReg10.y, mvMatrix[1], vTex5; DP3   vReg3.y, mvMatrix[1], vTex6; DP3   vReg1.z, mvMatrix[2], vTex4; DP3   vReg10.z, mvMatrix[2], vTex5; DP3   vReg3.z, mvMatrix[2], vTex6; DP3   result.texcoord[1].x, program.local[92], vReg1; DP3   result.texcoord[1].y, program.local[92], vReg10; DP3   result.texcoord[1].z, program.local[92], vReg3; DP3   result.texcoord[2].x, program.local[91], vReg1; DP3   result.texcoord[2].y, program.local[91], vReg10; DP3   result.texcoord[2].z, program.local[91], vReg3; DP3   result.texcoord[3].x, program.local[90], vReg1; DP3   result.texcoord[3].y, program.local[90], vReg10; DP3   result.texcoord[3].z, program.local[90], vReg3; DP4   vReg10.x, vertex.attrib[0], mvMatrix[0] ;
DP4   vReg10.y, vertex.attrib[0], mvMatrix[1] ;
DP4   vReg10.z, vertex.attrib[0], mvMatrix[2] ;
DP4   vReg10.w, vertex.attrib[0], mvMatrix[3] ;
DP3	vReg1.x, vReg10, vReg10;
RSQ	vReg1.x, vReg1.x;
RCP	vReg1.x, vReg1.x;
MOV result.fogcoord, vReg1.x;
RCP   vReg10.w, vReg10.w           ;
MUL   vReg10.xyz, vReg10, -vReg10.w    ;
MOV   result.texcoord[0], vTex0;
MOV   result.color, program.local[15].x;
DP3   result.texcoord[4].x, vReg10, program.local[92];
DP3   result.texcoord[4].y, vReg10, program.local[91];
DP3   result.texcoord[4].z, vReg10, program.local[90];
)ENGINE" "END\n";
// K1 source 0x48aff0.
inline constexpr char Bump[] = R"ENGINE(!!ARBvp1.0
PARAM mvMatrix[4] = { state.matrix.modelview[0] };
PARAM mvpMatrix[4] = { state.matrix.mvp };
TEMP vReg10, vLength, vTemp;
TEMP vReg0, vReg1, vReg2, vReg3;
ALIAS vNormalWorld = vLength;
ALIAS vFinalColour = vTemp;
PARAM fogParams = state.fog.params;
DP4   result.position.x, mvpMatrix[0], vertex.attrib[0];
DP4   result.position.y, mvpMatrix[1], vertex.attrib[0];
DP4   result.position.z, mvpMatrix[2], vertex.attrib[0];
DP4   result.position.w, mvpMatrix[3], vertex.attrib[0];
SUB  vReg10, program.local[12], vertex.attrib[0];
DP3   vReg10.w, vReg10, vReg10;
RSQ   vLength.w, vReg10.w;
DST vTemp, vReg10.wwww, vLength.wwww;
DP3 vTemp.w, vTemp, program.local[14];
RCP vTemp.w, vTemp.w;
MOV   result.color.secondary, vTemp.wwww;
MUL   vReg10, vReg10, vLength.w;
DP3   result.texcoord[1].x, vertex.attrib[12], vReg10;
DP3   result.texcoord[1].y, vertex.attrib[13], vReg10;
DP3   result.texcoord[1].z, vertex.attrib[14], vReg10;
MOV   result.texcoord[1].w, 1;
SUB vTemp, program.local[13], vertex.attrib[0];
DP3   vTemp.w, vTemp, vTemp;
RSQ   vTemp.w, vTemp.w;
MUL   vTemp, vTemp, vTemp.w;
ADD vTemp, vTemp,vReg10;
DP3   result.texcoord[2].x, vertex.attrib[12], vTemp;
DP3   result.texcoord[2].y, vertex.attrib[13], vTemp;
DP3   result.texcoord[2].z, vertex.attrib[14], vTemp;
MOV   result.texcoord[2].w, 1;
DP3   vNormalWorld.x, mvMatrix[0], vertex.attrib[2];
DP3   vNormalWorld.y, mvMatrix[1], vertex.attrib[2];
DP3   vNormalWorld.z, mvMatrix[2], vertex.attrib[2];
MOV   result.texcoord[0], vertex.attrib[8];
MOV   result.texcoord[3], vertex.attrib[8];
DP4   vReg10.x, mvMatrix[0], vertex.attrib[0];
DP4   vReg10.y, mvMatrix[1], vertex.attrib[0];
DP4   vReg10.z, mvMatrix[2], vertex.attrib[0];
DP4   vReg10.w, mvMatrix[3], vertex.attrib[0];
DP3	vReg0.x, vReg10, vReg10;
RSQ	vReg0.x, vReg0.x;
RCP	vReg0.x, vReg0.x;
MOV result.fogcoord.x, vReg0.x;
)ENGINE";
}
