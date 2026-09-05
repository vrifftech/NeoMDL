#pragma once
namespace neomdl::render {
// GLSL 1.20 deliberately supports the existing desktop legacy wxGLCanvas
// context. Only helper glyphs/grid still need compatibility fixed functions.
inline constexpr const char* meshVertexShader=R"GLSL(#version 120
#ifdef ENGINE_FIXED_FRAGMENT
uniform bool uFixedUv1Array;
uniform int uFixedDirtyUnit;
uniform bool uOverride;
#endif
attribute vec3 aPosition;
attribute vec3 aNormal;
attribute vec3 aRawNormal;
attribute vec2 aUv0;
attribute vec2 aUv1;
attribute vec4 aTangent;
attribute vec3 aBitangent;
attribute vec4 aBasisNormal;
attribute vec4 aColor;
uniform mat4 uModelView;
uniform mat4 uProjection;
uniform mat4 uFixedTextureMatrix;
uniform mat3 uNormalMatrix;
uniform bool uPartHasOwner;
uniform bool uFixedNormalize;
uniform float uTransformHandedness;
uniform int uSkinEffect;
uniform int uOwnerFragment;
uniform int uMaterialProgram;
uniform int uForceSightCommand;
uniform vec4 uForceSightColor;
uniform float uForceSightDesaturation;
uniform int uDirtyCommand;
uniform bool uDirtyWorld;
uniform bool uEngineFlat;
uniform bool uLighting;
uniform mat4 uWorldFromEye;
uniform mat3 uEyeToWorld;
uniform float uShellAmount;
uniform vec4 uColor;
uniform vec3 uMeshAmbient;
uniform vec3 uSceneAmbient;
uniform vec3 uSelfIllumination;
uniform int uLightCount;
uniform vec4 uLightPosition[8];
uniform vec3 uLightDiffuse[8];
uniform vec3 uLightAmbient[8];
uniform vec3 uLightAttenuation[8];
uniform bool uExactK2Skin;
uniform vec3 uSkinMotion;
uniform bool uEngineSkinBlur;
uniform vec3 uSkinBlurDirection;
uniform float uSkinBlurSpeed;
uniform float uSkinMaximumBlurLength;
uniform float uSkinBlurSideScale;
uniform vec3 uSkinCameraPosition;
uniform vec3 uSkinBlurSideDirection;
varying vec3 vDirty;
varying vec3 vPosition;
varying vec3 vNormal;
varying vec4 vTangent;
varying vec3 vBitangent;
varying vec4 vBasisNormal;
varying float vSkinBlurAlpha;
varying float vFogCoordinate;
varying vec2 vUv0;
varying vec2 vUv1;
varying vec4 vColor;
vec3 vertexSafeUnit(vec3 value, vec3 fallback) {
    float squared = dot(value, value);
    return squared > 0.000000000001 ? value * inversesqrt(squared) : fallback;
}
void main() {
    bool engineBlur = uEngineSkinBlur && uSkinEffect == 1;
    vec3 localNormal = engineBlur ? aRawNormal * inversesqrt(dot(aRawNormal,aRawNormal))
        : vertexSafeUnit(aNormal, vec3(0.0,0.0,1.0));
    vec3 localPosition = aPosition;
    vSkinBlurAlpha = 1.0;
    if (uSkinEffect == 1) {
        // cSkinMotionBlurVP after skinning. uSkinMotion is current minus
        // previous Gob position in this part's model space.
        float movement = length(uSkinMotion);
        vec3 direction = engineBlur ? uSkinBlurDirection : movement > 0.000000000001
            ? uSkinMotion / movement : vec3(0.0,0.0,1.0);
        float maximumLength = engineBlur ? uSkinMaximumBlurLength : max(0.000001, uSkinMaximumBlurLength);
        float amount = min(engineBlur ? uSkinBlurSpeed : movement, maximumLength);
        float backFacing = dot(direction, localNormal) < 0.0 ? 1.0 : 0.0;
        localPosition -= direction * amount * backFacing;

        vec3 sideDirection = engineBlur
            ? uSkinBlurSideDirection * inversesqrt(dot(uSkinBlurSideDirection,uSkinBlurSideDirection))
            : vertexSafeUnit(uSkinBlurSideDirection, vec3(1.0,0.0,0.0));
        // Engine c17.xyz is the Gob origin in Part space, not the camera.
        // The CPU-posed stream and engine constants here are in Gob space.
        vec3 sideOrigin = engineBlur ? vec3(0.0) : uSkinCameraPosition;
        float cameraSide = dot(localPosition-sideOrigin, sideDirection) < 0.0
            ? -1.0 : 1.0;
        float sideAmount = amount * uSkinBlurSideScale *
            (amount * (1.0 / maximumLength)) * 0.15;
        localPosition += sideDirection * cameraSide * sideAmount * backFacing;
        localPosition -= localNormal * 0.01 * (1.0-backFacing);
        vSkinBlurAlpha = 0.5 * (1.0-backFacing);
    }
    if (uSkinEffect == 2) {
        // szBumpedOutVPARB / szSkinBumpedOutVPARB: displacement uses the
        // authored/weighted normal BEFORE the ordinary skin normalization.
        localPosition += aRawNormal * uShellAmount;
    }
    vec4 eye = uModelView * vec4(localPosition, 1.0);
    vPosition = eye.xyz;
    vDirty=vec3(0.0);
#ifdef ENGINE_FIXED_FRAGMENT
    vec4 engineDirtyCoordinate;
#endif
    if(uDirtyCommand!=0) {
        // Exact sequential MAD/swizzles from szcStandardDirty*VPARB.
        // Local coordinates add raw UV0 (including its implicit z=0,w=1).
        vec4 q=uWorldFromEye*eye;
        if(uDirtyWorld) {
            q=q*0.5+q;
            q=q*0.25+q.yzxw;
            q=q*0.125+q.zxyw;
            q=q*0.067500002682209015+q;
        } else q=vec4(aPosition,1.0)+vec4(aUv0,0.0,1.0);
        vDirty=q.xyz;
#ifdef ENGINE_FIXED_FRAGMENT
        engineDirtyCoordinate=q;
#endif
    }
    float skinFog = length(eye.xyz);
    // Flat/Environment/EMLM ARB programs write radial distance. The fixed
    // fallback retains fixed-function eye-z fog; fragment programs without
    // an ARB_fog option bypass fog entirely.
    bool engineVertex = uMaterialProgram>0 && uMaterialProgram<5;
    vFogCoordinate = (engineVertex || uExactK2Skin || uSkinEffect == 2)
        ? (uSkinEffect == 1 ? -skinFog : skinFog)
        : (uMaterialProgram==5 ? abs(eye.z) : -eye.z);
    vNormal = uNormalMatrix * aNormal;
    vTangent = vec4(mat3(uModelView) * aTangent.xyz,
                    aTangent.w * uTransformHandedness);
    vBitangent = mat3(uModelView) * aBitangent;
    // cNewSkinBumpyVP keeps all three authored basis vectors in one common
    // object/model space. Transform all three by the same linear matrix before
    // the fragment-stage reassembly; using the inverse-transpose only for
    // TEX6 would change their relative magnitudes under uniform model scale.
    vBasisNormal = vec4(mat3(uModelView) * aBasisNormal.xyz,
                        aBasisNormal.w);
    vUv0 = aUv0; vUv1 = aUv1; vColor = aColor;
    if(uMaterialProgram==5) {
        vec4 uv=uFixedTextureMatrix*vec4(aUv0,0.0,1.0);
        vUv0=uv.xy; vUv1=vec2(uv.w,0.0);
    }
    if (uOwnerFragment == 1) {
        // cHoloVPARB/cHoloSkinVPARB: camera inverse env92..89 and
        // env16.wxwx = (1,0,1,0). The radial axes really are x/z.
        vec3 world = (uWorldFromEye * eye).xyz;
        vUv1 = vec2(length(world.xz), world.z);
    } else if (uOwnerFragment == 2 || uOwnerFragment == 3 || uMaterialProgram>0) {
        // FlatARB/EnvironmentARB and VertexProgram::Enable light snippets.
        // Rigid normals use the linear model-view, not inverse-transpose or
        // normalization. Skinned input has already had its engine normalization.
        vec3 engineNormal = mat3(uModelView) * aRawNormal;
        // szSkinVPARB/szSkinCubeMapVPARB normalize AFTER linear model-view.
        if(uExactK2Skin) engineNormal=normalize(engineNormal);
        if(uMaterialProgram==5) {
            // RenderBumpmapFallback does not enable a VP. The GL normal
            // transformation is inverse-transpose, with its separate engine
            // normalization enable, not the ARB program's linear transform.
            engineNormal=uNormalMatrix*aRawNormal;
            if(uFixedNormalize) engineNormal=normalize(engineNormal);
        }
        vec3 primary = (uPartHasOwner ? uSceneAmbient*uMeshAmbient : vec3(0.0))
            + uSelfIllumination;
        for (int i=0; i<8; ++i) {
            if (i>=uLightCount) break;
            vec3 delta=uLightPosition[i].xyz-eye.xyz*uLightPosition[i].w;
            float squared=dot(delta,delta);
            vec3 direction=delta*inversesqrt(squared);
            float attenuation=1.0;
            if (uLightPosition[i].w!=0.0) {
                vec3 factors=uLightAttenuation[i];
                attenuation=1.0/(factors.x+factors.y*sqrt(squared)+factors.z*squared);
            }
            primary+=attenuation*(uMeshAmbient*uLightAmbient[i]+
                uColor.rgb*uLightDiffuse[i]*max(dot(engineNormal,direction),0.0));
        }
        // ARB vertex primary colors are clamped before interpolation.
        vColor=clamp(vec4(primary,uColor.a),0.0,1.0);
        if (uOwnerFragment==3 || uMaterialProgram==3 || uMaterialProgram==4) {
            vec3 reflection=eye.xyz-2.0*dot(engineNormal,eye.xyz)*engineNormal;
            vBitangent=uEyeToWorld*normalize(reflection);
        }
    }
    // RenderFlat temporarily disables lighting/color arrays and sets white.
    if(uEngineFlat && uMaterialProgram==5 && !uLighting) vColor=vec4(1.0);
#ifdef ENGINE_FIXED_FRAGMENT
    // Engine rigid Holo writes env15.wwwz, while SkinHolo leaves primary
    // unwritten. Shell writes env12.w (the caller supplies1).
    if(uOwnerFragment==1) {
        if(!uExactK2Skin)gl_FrontColor=vec4(0.0,0.0,0.0,1.0);
        gl_TexCoord[0]=vec4(aUv0,0.0,1.0);
        gl_TexCoord[1].xy=vUv1;
        if(!uExactK2Skin){gl_TexCoord[2]=uFixedCoordinates[2];gl_TexCoord[3]=uFixedCoordinates[3];}
    } else if(engineBlur) {
        // cSkinMotionBlurVP owns COL0; retained fixed texture stages consume
        // its black RGB / backface alpha without a material-alpha multiply.
        gl_FrontColor=vec4(0.0,0.0,0.0,vSkinBlurAlpha);
        gl_TexCoord[0]=vec4(aUv0,0.0,1.0);
    } else if(uSkinEffect==2) {
        gl_FrontColor=vec4(1.0);
        gl_TexCoord[0]=vec4(aUv0,0.0,1.0);
        if(!uExactK2Skin)gl_TexCoord[1]=uFixedCoordinates[1];
    } else {
        gl_FrontColor=vColor;
        if(uMaterialProgram==5 && !uLighting)gl_FrontColor=vec4(1.0);
        if((uMaterialProgram>0 && uMaterialProgram<5) || uOwnerFragment==3) {
            gl_TexCoord[0]=vec4(aUv0,0.0,1.0);
            gl_TexCoord[1]=uFixedUv1Array?vec4(aUv1,0.0,1.0):uFixedCoordinates[1];
            gl_TexCoord[2]=uFixedCoordinates[2];gl_TexCoord[3]=uFixedCoordinates[3];
            if(uMaterialProgram==3 || uOwnerFragment==3)gl_TexCoord[1]=vec4(vBitangent,1.0);
            if(uMaterialProgram==4)gl_TexCoord[2]=vec4(vBitangent,1.0);
            if(uFixedDirtyUnit>=0)gl_TexCoord[uFixedDirtyUnit]=engineDirtyCoordinate;
            // Original Standard/Cube/LightmapCube only write units0..3.
        } else {
            vec3 n=uNormalMatrix*aRawNormal;
            if(uFixedNormalize)n=normalize(n);
            engineFixedTexcoords(eye,n,vec4(aUv0,0.0,1.0));
        }
    }
    if(uOverride)gl_FrontColor=uColor; // viewer selection overlay only
    gl_FogFragCoord=vFogCoordinate;
#endif
    gl_Position = uProjection * eye;
}
)GLSL";
inline constexpr const char* meshFragmentShader=R"GLSL(#version 120
uniform int uFlareProgram;
uniform float uFlareDesaturation;
uniform sampler2D uDiffuseTexture;
uniform sampler3D uDirtyTexture;
uniform vec3 uDirtyForce;
uniform sampler2D uLightmapTexture;
uniform sampler2D uHologramFilter;
uniform int uOwnerFragment;
uniform int uMaterialProgram;
uniform int uForceSightCommand;
uniform vec4 uForceSightColor;
uniform float uForceSightDesaturation;
uniform int uDirtyCommand;
uniform bool uDirtyWorld;
uniform vec3 uOwnerIllumination;
uniform sampler2D uBumpTexture;
uniform sampler2D uEnvironmentTexture;
uniform samplerCube uEnvironmentCube;
uniform int uBumpKind;
uniform int uEnvironmentMode;
uniform mat3 uEyeToWorld;
uniform float uBumpScale;
uniform bool uDiffuseBump;
uniform bool uSpecularBump;
uniform vec3 uBumpIntensities;
uniform vec3 uSpecularColor;
uniform float uEnvironmentStrength;
uniform float uEnvironmentAlpha;
uniform bool uAlphaTest;
uniform float uAlphaCutoff;
uniform bool uTextureAlphaIsOpacity;
uniform vec4 uDiffuseUv;
uniform vec3 uDiffuseScroll;
uniform vec3 uDiffuseUvInfo;
uniform vec4 uBumpUv;
uniform vec3 uBumpScroll;
uniform vec3 uBumpUvInfo;
uniform bool uUseDiffuse;
uniform bool uUseLightmap;
uniform bool uLighting;
uniform bool uOverride;
uniform vec4 uColor;
uniform vec3 uSelfIllumination;
uniform int uBasisDebug;
uniform int uLightCount;
uniform vec3 uMeshAmbient;
uniform vec3 uSceneAmbient;
uniform int uFogMode;
uniform vec3 uFogColor;
uniform vec3 uFogData; // start, end, density
uniform bool uAdditive;
uniform int uSkinEffect;
uniform bool uEngineSkinBlur;
uniform bool uExactK2Skin;
uniform vec4 uLightPosition[8];
uniform vec3 uLightDiffuse[8];
uniform vec3 uLightAmbient[8];
uniform vec3 uLightAttenuation[8];
varying vec3 vDirty;
varying vec3 vPosition;
varying vec3 vNormal;
varying vec4 vTangent;
varying vec3 vBitangent;
varying vec4 vBasisNormal;
varying float vSkinBlurAlpha;
varying float vFogCoordinate;
varying vec2 vUv0;
varying vec2 vUv1;
varying vec4 vColor;
vec3 safeUnit(vec3 v, vec3 fallback) {
    float squared = dot(v,v);
    return squared > 0.000000000001 ? v * inversesqrt(squared) : fallback;
}
vec2 materialUv(vec2 source, vec4 atlas, vec3 scroll, vec3 info) {
    vec2 uv=source+scroll.xy;
    if(scroll.z>0.5) {
        uv=info.z>0.5 ? clamp(uv,0.0,1.0) : fract(uv);
        // Clamp inside the selected frame, including height-gradient taps.
        vec2 inset=min(info.xy*0.5,atlas.xy*0.5);
        return atlas.zw+clamp(uv*atlas.xy,inset,atlas.xy-inset);
    }
    return info.z>0.5 ? clamp(uv,0.0,1.0) : uv;
}
vec3 perturbedNormal(vec3 n,vec3 t) {
    vec3 b=safeUnit(cross(n,t),vec3(0.0,1.0,0.0))*vTangent.w;
    vec2 uv=materialUv(vUv0,uBumpUv,uBumpScroll,uBumpUvInfo);
    vec3 sampleNormal=vec3(0.0,0.0,1.0);
    if(uBumpKind==2) {
        sampleNormal=texture2D(uBumpTexture,uv).xyz*2.0-1.0;
        sampleNormal.xy*=uBumpScale;
    } else {
        vec2 stepSize=max(uBumpUvInfo.xy,vec2(0.00001));
        vec2 localStep=stepSize/max(uBumpUv.xy,vec2(0.00001));
        float l=texture2D(uBumpTexture,materialUv(vUv0-vec2(localStep.x,0.0),uBumpUv,uBumpScroll,uBumpUvInfo)).r;
        float r=texture2D(uBumpTexture,materialUv(vUv0+vec2(localStep.x,0.0),uBumpUv,uBumpScroll,uBumpUvInfo)).r;
        float d=texture2D(uBumpTexture,materialUv(vUv0-vec2(0.0,localStep.y),uBumpUv,uBumpScroll,uBumpUvInfo)).r;
        float u=texture2D(uBumpTexture,materialUv(vUv0+vec2(0.0,localStep.y),uBumpUv,uBumpScroll,uBumpUvInfo)).r;
        sampleNormal=vec3(-(r-l)*uBumpScale,-(u-d)*uBumpScale,1.0);
    }
    if (vBasisNormal.w > 0.5) {
        // K2 MDX stores TEX4/TEX5/TEX6 as bitangent/tangent/normal. The
        // vectors reaching this branch are the exact weighted skin results.
        vec3 exactB = vBitangent;
        vec3 exactT = vTangent.xyz;
        vec3 exactN = vBasisNormal.xyz;
        // cNewSkinBumpyVP does not independently normalize or orthogonalize
        // TEX4/TEX5/TEX6 after weighted bone deformation. Only the final
        // perturbed result is normalized for lighting.
        return safeUnit(exactB*sampleNormal.x + exactT*sampleNormal.y +
                        exactN*sampleNormal.z, safeUnit(exactN,n));
    }
    return safeUnit(t*sampleNormal.x+b*sampleNormal.y+n*sampleNormal.z,n);
}

float fogAmount() {
    float d=uExactK2Skin ? vFogCoordinate : max(0.0,vFogCoordinate);
    if(uFogMode==1) return clamp((uFogData.y-d)/(uFogData.y-uFogData.x),0.0,1.0);
    float x=uFogData.z*d;
    if(uFogMode==2) return clamp(exp(-x),0.0,1.0);
    if(uFogMode==3) return clamp(exp(-x*x),0.0,1.0);
    return 1.0;
}
void main() {
    if (uOverride) { gl_FragColor = uColor; return; }
    if(uFlareProgram!=0) {
        vec4 c=texture2D(uDiffuseTexture,vUv0)*vColor;
        if(uFlareProgram==1) {
            c.rgb=vec3(dot(c.rgb,vec3(0.30,0.59,0.11))*uFlareDesaturation);
            c.a*=0.75;
        }
        if(uAlphaTest && !(c.a>uAlphaCutoff)) discard;
        gl_FragColor=c;return;
    }
    vec3 n = safeUnit(vNormal, vec3(0.0,0.0,1.0));
    vec3 t = safeUnit(vTangent.xyz - n * dot(n,vTangent.xyz), vec3(1.0,0.0,0.0));
    vec3 debugT = vBasisNormal.w > 0.5 ? safeUnit(vTangent.xyz,t) : t;
    vec3 debugN = vBasisNormal.w > 0.5 ? safeUnit(vBasisNormal.xyz,n) : n;
    vec3 debugB = vBasisNormal.w > 0.5
        ? safeUnit(vBitangent,vec3(0.0,1.0,0.0))
        : safeUnit(cross(n,t),vec3(0.0,1.0,0.0))*vTangent.w;
    if (uBasisDebug == 1) { gl_FragColor=vec4(debugT*0.5+0.5,1.0); return; }
    if (uBasisDebug == 2) { gl_FragColor=vec4(debugN*0.5+0.5,1.0); return; }
    if (uBasisDebug == 3) { gl_FragColor=vec4(debugB*0.5+0.5,1.0); return; }
    if(uForceSightCommand!=0) {
        // Literal K2 Desktop ARB fragments selected by SetupForceSightShader.
        // Their alpha rules differ by command and none enables ARB fog.
        vec4 base=texture2D(uDiffuseTexture,vUv0);
        vec4 primary=uSkinEffect==2?vec4(1.0):vColor;
        vec3 rgb=base.rgb*primary.rgb;
        float alpha=base.a*primary.a;
        if(uForceSightCommand==50 || uForceSightCommand==52) {
            vec3 value=base.rgb;
            if(uForceSightCommand==52)
                value+=texture2D(uEnvironmentTexture,vBitangent.xy).rgb*(1.0-base.a);
            rgb=dot(value,vec3(0.3,0.59,0.11))*uForceSightColor.rgb+
                uForceSightColor.rgb*uForceSightColor.a;
            alpha=uForceSightCommand==52?0.8:base.a*0.8;
        } else if(uForceSightCommand==76) {
            vec4 filter=texture2D(uHologramFilter,vUv1);
            rgb=vec3(dot(dot(base.rgb,vec3(0.3,0.59,0.11))*filter.rgb,vec3(0.3,0.59,0.11)));
            alpha=filter.a*0.75;
        } else if(uForceSightCommand==92 || uForceSightCommand==93) {
            rgb=base.rgb*(vColor.rgb+2.0*uOwnerIllumination-vec3(1.0));
            rgb=vec3(dot(rgb,vec3(0.3,0.59,0.11))*uForceSightDesaturation);
        } else if(uForceSightCommand>0) {
            if(uForceSightCommand==57 || uForceSightCommand==59)
                rgb+=base.rgb*texture2D(uLightmapTexture,vUv1).rgb;
            if(uForceSightCommand==58 || uForceSightCommand==59)
                rgb+=texture2D(uEnvironmentTexture,vBitangent.xy).rgb*(1.0-base.a);
            rgb=vec3(dot(rgb,vec3(0.3,0.59,0.11))*uForceSightDesaturation);
            if(uForceSightCommand==57)alpha=base.a; // original FP's r2.a is unused
            else if(uForceSightCommand==58 || uForceSightCommand==59)alpha=uForceSightColor.a;
            else alpha*=uForceSightColor.a;
        } else rgb=mix(uFogColor,rgb,fogAmount()); // dispatcher no-op: FP remains disabled
        if(uAlphaTest && !(alpha>uAlphaCutoff))discard;
        gl_FragColor=vec4(rgb,alpha);return;
    }
    if (uOwnerFragment == 1) {
        // HoloFP, selector 75: DP3(base,c7.rgb), filter RGB, filter alpha.
        // No ARB_fog option; owner diffuse/emission/alpha do not modulate it.
        vec4 base=texture2D(uDiffuseTexture,vUv0);
        vec4 filter=texture2D(uHologramFilter,vUv1);
        vec4 result=vec4(dot(base.rgb,vec3(0.3,0.59,0.11))*filter.rgb,filter.a);
        if(uAlphaTest && !(result.a>uAlphaCutoff)) discard;
        gl_FragColor=result;
        return;
    }
    if (uOwnerFragment == 2 || uOwnerFragment == 3) {
        // IllumFP (78) / IllumEnvFP (79). These replace fragment processing;
        // neither program has a fog option or multiplies primary alpha.
        vec4 base=texture2D(uDiffuseTexture,vUv0);
        vec3 rgb=base.rgb*(vColor.rgb+2.0*uOwnerIllumination-vec3(1.0));
        float alpha=base.a;
        if(uOwnerFragment==3) {
            rgb=base.a*rgb+(1.0-base.a)*textureCube(uEnvironmentCube,vBitangent).rgb;
            alpha=1.0; // getter supplies RGB; caller initializes c1.a to one
        }
        if(uAlphaTest && !(alpha>uAlphaCutoff)) discard;
        gl_FragColor=vec4(rgb,alpha);
        return;
    }
    if (uSkinEffect == 2) {
        // Both shell vertex programs emit white primary and raw texture UV0.
        vec4 shell=texture2D(uDiffuseTexture,vUv0);
        if(uAlphaTest && !(shell.a>uAlphaCutoff)) discard;
        shell.rgb=mix(uFogColor,shell.rgb,fogAmount());
        gl_FragColor=shell;
        return;
    }
    if(uDirtyCommand!=0) {
        vec4 base=texture2D(uDiffuseTexture,vUv0);
        vec3 dirt=uDirtyCommand==68 ? texture2D(uBumpTexture,vDirty.xy).rgb : texture3D(uDirtyTexture,vDirty).rgb;
        vec3 value=base.rgb;
        bool env=uDirtyCommand==47||uDirtyCommand==48||uDirtyCommand==71;
        if(env)value+=texture2D(uEnvironmentTexture,vBitangent.xy).rgb*(1.0-base.a);
        if(uDirtyCommand==45||uDirtyCommand==48)value+=dirt;else value-=dirt;
        vec3 rgb=value*vColor.rgb;
        if(uDirtyCommand!=38)rgb+=value*texture2D(uLightmapTexture,vUv1).rgb;
        float alpha=env?1.0:base.a; // ordinary Dirty FP never multiplies primary alpha
        if(uDirtyCommand==68||uDirtyCommand==71) {
            rgb=vec3(dot(rgb,vec3(0.3,0.59,0.11))*uDirtyForce.x);
            alpha=uDirtyForce.y;
        }
        if(uAlphaTest && !(alpha>uAlphaCutoff))discard;
        gl_FragColor=vec4(rgb,alpha);return; // none of these engine FP strings enables fog
    }
    if(uMaterialProgram>0) {
        // The ARB vertex programs pass the authored coordinates through. They
        // bypass fixed texture matrices/texgen. In particular, do not apply the
        // preview atlas inset, fract/clamp or elapsed-time scrolling here.
        vec4 base=uUseDiffuse ? (uMaterialProgram==5
            ? texture2DProj(uDiffuseTexture,vec3(vUv0,vUv1.x))
            : texture2D(uDiffuseTexture,vUv0)) : vec4(1.0);
        vec3 rgb=base.rgb*vColor.rgb;
        float alpha=base.a*vColor.a;
        if(uMaterialProgram==2 || uMaterialProgram==4) {
            // LightmapFP: base * (primary + lightmap), not base*primary*lightmap.
            vec3 lm=texture2D(uLightmapTexture,vUv1).rgb;
            if(uMaterialProgram==2) rgb=base.rgb*(vColor.rgb+lm);
            else rgb+=base.rgb*lm;
        }
        if(uMaterialProgram==3 || uMaterialProgram==4) {
            // Environment/EMLM VP normalize reflection per VERTEX and rotate
            // to world. Do not normalize the interpolated coordinates again.
            // 2D targets consume reflection.xy, not a sphere-map reassembly.
            vec3 env=uEnvironmentMode==2
                ? textureCube(uEnvironmentCube,vBitangent).rgb
                : texture2D(uEnvironmentTexture,vBitangent.xy).rgb;
            rgb+=(1.0-base.a)*env;
            alpha=uEnvironmentAlpha; // fragment env[0].a; neither texel nor primary alpha
        }
        if(uAlphaTest && !(alpha>uAlphaCutoff)) discard;
        if(uMaterialProgram==1 || uMaterialProgram==5)
            rgb=mix(uFogColor,rgb,fogAmount());
        // LM/Env/EMLM fragment programs replace fixed texturing AND fog. None
        // of the selected ARB strings contains a fog option.
        gl_FragColor=vec4(rgb,alpha);
        return;
    }
    vec4 texel=vec4(1.0);
    if(uUseDiffuse) texel=texture2D(uDiffuseTexture,uEngineSkinBlur && uSkinEffect == 1
        ? vUv0 : materialUv(vUv0,uDiffuseUv,uDiffuseScroll,uDiffuseUvInfo));
    // The blur vertex program writes its own COL0 values; authored
    // MDX vertex colors do not modulate those passes.
    vec4 primary = uSkinEffect == 0 ? vColor : vec4(1.0);
    float coverage=primary.a*(uTextureAlphaIsOpacity ? texel.a : 1.0);
    float fragmentAlpha=uColor.a*coverage;
    // Fixed-function glAlphaFunc compares the final fragment alpha after the
    // material/vertex/texture modulation, not texture coverage in isolation.
    if(uSkinEffect == 1) fragmentAlpha*=vSkinBlurAlpha;
    if(uAlphaTest && !(fragmentAlpha>uAlphaCutoff)) discard;
    if (uSkinEffect == 1) {
        vec4 blurColor=vec4(0.0,0.0,0.0,fragmentAlpha);
        float transmission=fogAmount();
        blurColor.rgb=mix(uFogColor,blurColor.rgb,transmission);
        gl_FragColor=blurColor;
        return;
    }
    vec4 color=vec4(uColor.rgb*primary.rgb*texel.rgb,fragmentAlpha);
    vec3 shadingNormal=uBumpKind!=0 ? perturbedNormal(n,t) : n;
    vec3 diffuseNormal=uDiffuseBump ? safeUnit(mix(n,shadingNormal,uBumpIntensities.x),n) : n;
    vec3 specular=vec3(0.0);
    if (uLighting) {
        vec3 illumination = vec3(0.0);
        vec3 ambient = uSceneAmbient*uMeshAmbient;
        for (int i=0; i<8; ++i) {
            if (i >= uLightCount) break;
            vec3 delta = uLightPosition[i].xyz - vPosition * uLightPosition[i].w;
            float distanceToLight = length(delta);
            vec3 direction = safeUnit(delta,vec3(0.0,0.0,1.0));
            float attenuation = 1.0;
            if (uLightPosition[i].w != 0.0) {
                vec3 att = uLightAttenuation[i];
                attenuation = 1.0 / max(0.0001,att.x+att.y*distanceToLight+att.z*distanceToLight*distanceToLight);
            }
            ambient += attenuation*uLightAmbient[i]*uMeshAmbient;
            illumination += attenuation*uLightDiffuse[i]*max(0.0,dot(diffuseNormal,direction));
            if(uSpecularBump && uBumpKind!=0 && dot(shadingNormal,direction)>0.0) {
                vec3 halfVector=safeUnit(direction+safeUnit(-vPosition,vec3(0.0,0.0,1.0)),shadingNormal);
                specular+=attenuation*uLightDiffuse[i]*uSpecularColor*uBumpIntensities.y*
                    pow(max(0.0,dot(shadingNormal,halfVector)),32.0);
            }
        }
        color.rgb = texel.rgb*vColor.rgb*(uColor.rgb*illumination+ambient);
    }
    if (uUseLightmap) color.rgb *= texture2D(uLightmapTexture,vUv1).rgb;
    // Texture-tinted emissive preview, independent of direct light and UV1
    // lightmap darkness. Keep straight alpha: node opacity is applied once by
    // the selected blend pass, including emission and environment reflection.
    color.rgb += texel.rgb * vColor.rgb * uSelfIllumination;
    color.rgb+=specular;
    if(uEnvironmentMode!=0) {
        vec3 reflected=reflect(safeUnit(vPosition,vec3(0.0,0.0,-1.0)),shadingNormal);
        vec3 environment;
        if(uEnvironmentMode==2) environment=textureCube(uEnvironmentCube,uEyeToWorld*reflected).rgb;
        else {
            // Legacy sphere-map preview for a non-cube environment texture.
            float divisor=max(0.00001,2.0*length(reflected+vec3(0.0,0.0,1.0)));
            environment=texture2D(uEnvironmentTexture,reflected.xy/divisor+vec2(0.5)).rgb;
        }
        color.rgb+=environment*uEnvironmentStrength*(uUseDiffuse ? 1.0-texel.a : 1.0);
    }
    float transmission=fogAmount();
    // Additive passes fade to black, not to an additional copy of the fog color.
    color.rgb=mix(uAdditive ? vec3(0.0) : uFogColor,color.rgb,transmission);
    gl_FragColor = color;
}
)GLSL";
} // namespace neomdl::render
