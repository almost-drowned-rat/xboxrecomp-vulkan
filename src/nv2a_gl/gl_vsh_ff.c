/*
 * gl_vsh_ff.c -- fixed-function vertex lighting and REFLECTION_MAP texgen (with
 * the texture matrix) for the Vulkan vertex shader, and the specular exponent
 * from SET_SPECULAR_PARAMS.
 *
 * Derived from xemu (https://github.com/xemu-project/xemu, commit 36334d9):
 *   hw/xbox/nv2a/pgraph/glsl/vsh-ff.c   fixed-function light model, texgen
 *   hw/xbox/nv2a/pgraph/glsl/vsh.c      specular enable / D1 handling
 *   hw/xbox/nv2a/pgraph/pgraph.c        reconstruct_specular_power (after
 *                                       dracc/xgu xgux.h)
 *
 * Copyright (c) 2012, 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Here: rewritten as one GLSL main for nv2a_vk's uniform block (VsU, gl_vsh.c)
 * instead of xemu's per-state generated source; the light state comes per draw
 * in uniforms (nv2a_vk.c fills them). See NOTICE.
 */

#include <stddef.h>

static const char s_vsh_fixed_lit[] =
    "vec3 ff_src(int s, vec3 mat, vec4 dif, vec4 spc) { return s == 1 ? dif.rgb : s == 2 ? spc.rgb : mat; }\n"
    "void main() {\n"
    "    if (u_xform == 1) {\n"
    "        vec4 clip = vec4(dot(u_m[0], v0), dot(u_m[1], v0),\n"
    "                         dot(u_m[2], v0), dot(u_m[3], v0));\n"
    "        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;\n"
    "        vec3 sw = clip.xyz + u_vpoff.xyz * w;\n"
    "        sw.xy *= u_aa;\n"
    "        gl_Position = nv2a_clip(sw, w);\n"
    "    } else {\n"
    "        float w = (v0.w > 0.0) ? 1.0 / v0.w : 1.0;\n"
    "        gl_Position = nv2a_clip(v0.xyz * w, w);\n"
    "    }\n"
    "    vec4 dif = v3, spc = v4, d0, d1;\n"
    "    bool spec_on = (u_lflags & 8) != 0;\n"
    /* eye-space position and normal (xemu: tPosition, tNormal), for lighting and texgen */
    "    vec4 ep = vec4(0.0); vec3 n = vec3(0.0);\n"
    "    if (u_xform == 1 && (u_lit != 0 || (u_tgen & 0xFFFF) != 0)) {\n"
    "        ep = vec4(dot(u_mv[0], v0), dot(u_mv[1], v0), dot(u_mv[2], v0), dot(u_mv[3], v0));\n"
    "        n = vec3(dot(u_imv[0].xyz, v2.xyz), dot(u_imv[1].xyz, v2.xyz), dot(u_imv[2].xyz, v2.xyz));\n"
    "        if ((u_lflags & 1) != 0) n = normalize(n);\n"
    "    }\n"
    "    if (u_lit != 0 && u_xform == 1) {\n"
    "        int esrc = u_colmat & 3, asrc = (u_colmat >> 2) & 3;\n"
    "        int dsrc = (u_colmat >> 4) & 3, ssrc = (u_colmat >> 6) & 3;\n"
    "        vec3 e = ep.xyz / (abs(ep.w) < 1e-6 ? 1.0 : ep.w);\n"
    "        float alpha = dsrc == 1 ? dif.a : dsrc == 2 ? spc.a : u_memis.w;\n"
    /* xemu: ambient source first, times the emission colour, plus the emission source */
    "        d0 = vec4(asrc == 0 ? u_samb.rgb : asrc == 1 ? dif.rgb : spc.rgb, alpha);\n"
    "        d0.rgb *= u_memis.rgb;\n"
    "        d0.rgb += esrc == 0 ? u_samb.rgb : esrc == 1 ? dif.rgb : spc.rgb;\n"
    "        d1 = vec4(0.0, 0.0, 0.0, spc.a);\n"
    "        vec3 vpeye = vec3(0.0);\n"
    "        if ((u_lflags & 2) != 0) vpeye = normalize(u_eye.xyz / (u_eye.w != 0.0 ? u_eye.w : 1.0) - e);\n"
    "        for (int i = 0; i < 8; i++) {\n"
    "            int kind = (u_lmask >> (2 * i)) & 3;\n"
    "            if (kind == 0) continue;\n"
    "            int b = i * 8;\n"
    "            float att = 1.0, ndl, ndh;\n"
    "            if (kind == 1) {\n"
    "                vec3 l = normalize(u_light[b + 4].xyz);\n"
    "                ndl = max(0.0, dot(n, l));\n"
    "                ndh = (u_lflags & 2) != 0 ? max(0.0, dot(n, normalize(l + vpeye)))\n"
    "                                          : max(0.0, dot(n, u_light[b + 3].xyz));\n"
    "            } else {\n"
    "                vec3 vp = u_light[b + 5].xyz - e;\n"
    "                float d = length(vp);\n"
    "                if (d > u_light[b + 5].w) continue;\n"
    "                vp = normalize(vp);\n"
    "                vec4 k = u_light[b + 6];\n"
    "                float den = k.x + k.y * d + k.z * d * d;\n"
    "                att = den != 0.0 ? 1.0 / den : 1.0;\n"
    "                ndl = max(0.0, dot(n, vp));\n"
    "                ndh = max(0.0, dot(n, normalize(vp + vpeye)));\n"
    "                if (kind == 3) {\n"
    "                    vec4 sd = u_light[b + 7];\n"
    "                    float inv = 1.0 / max(length(sd.xyz), 1e-6);\n"
    "                    float cphi = -inv * sd.w, ctheta = inv + cphi;\n"
    "                    float sdv = dot(sd.xyz, vp), rho = inv * sdv;\n"
    "                    if (rho <= cphi) att = 0.0;\n"
    "                    else if (rho <= ctheta) att *= sdv + sd.w;\n"
    "                }\n"
    "            }\n"
    "            float pf = (ndl == 0.0 || ndh == 0.0) ? 0.0 : pow(ndh, u_specpow);\n"
    "            d0.rgb += u_light[b].rgb * att;\n"
    "            d0.rgb += ff_src(dsrc, vec3(1.0), dif, spc) * u_light[b + 1].rgb * att * ndl;\n"
    "            d1.rgb += ff_src(ssrc, vec3(1.0), dif, spc) * u_light[b + 2].rgb * att * pf;\n"
    "        }\n"
    "        if (spec_on && (u_lflags & 4) == 0) { d0.rgb += d1.rgb; d1 = spc; }\n"
    "    } else {\n"
    "        d0 = dif; d1 = spc;\n"
    "    }\n"
    /* Specular enable (xemu vsh.c): off -> (0,0,0,1); alpha 1 unless ALPHA_FROM_MATERIAL_SPECULAR. */
    "    if (!spec_on) d1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    else if ((u_lflags & 16) == 0) d1.w = 1.0;\n"
    "    vD0 = clamp(d0, 0.0, 1.0); vD1 = clamp(d1, 0.0, 1.0);\n"
    /* RECOMP_VK_FFP_MARK=1 (lflags 32): lit fixed-function draws in magenta, to see which they are */
    "    if ((u_lflags & 32) != 0 && u_lit != 0) vD0 = vec4(1.0, 0.0, 1.0, 1.0);\n"
    "    vec4 t[4] = vec4[4](v9, v10, v11, v12);\n"
    /* Texgen (xemu vsh-ff.c): REFLECTION_MAP = reflect(normalize(eye pos), eye normal) on S/T/R,
     * then the stage's texture matrix (rows as delivered, oT = oT * texMat). Fixed function only. */
    "    if (u_xform == 1 && u_tgen != 0) {\n"
    "        vec3 rf = reflect(normalize(ep.xyz), n);\n"
    "        for (int s = 0; s < 4; s++) {\n"
    "            for (int c = 0; c < 3; c++)\n"
    "                if ((u_tgen & (1 << (4 * s + c))) != 0) t[s][c] = rf[c];\n"
    "            if ((u_tgen & (1 << (16 + s))) != 0)\n"
    "                t[s] = vec4(dot(u_tmat[4 * s], t[s]), dot(u_tmat[4 * s + 1], t[s]),\n"
    "                            dot(u_tmat[4 * s + 2], t[s]), dot(u_tmat[4 * s + 3], t[s]));\n"
    "        }\n"
    "    }\n"
    "    vT0 = t[0]; vT1 = t[1]; vT2 = t[2]; vT3 = t[3];\n"
    "    vFog = u_foggen == 1 ? clamp(v4.w, 0.0, 1.0)\n"
    "         : u_foggen == 2 ? dot(u_fogv, v0)\n"
    "         : u_foggen == 3 ? abs(dot(u_fogv, v0))\n"
    "         : u_foggen == 4 ? v5.x : 0.0;\n"
    "}\n";

const char *nv2a_gl_vsh_fixed_lit(void)
{
    return s_vsh_fixed_lit;
}

/* Specular exponent: the XDK sends a curve (SET_SPECULAR_PARAMS, 6 floats), not the power. */
float nv2a_gl_specular_power(const float *params)
{
    static const float co[9][3] = {
        {1.000108475163f, -9.838607076280f, 54.829089549713f},
        {1.199164441703f, -3.292603784852f, 7.799987995214f},
        {8.653441252033f, 29.189473787191f, 43.586027561823f},
        {-531.307758450301f, 117.398468683934f, 113.155490738338f},
        {-4.662713151292f, 1.221108944572f, 1.217360986939f},
        {-124.435242105211f, 35.401219563514f, 35.408114377045f},
        {10672560.259502287954f, 21565843.555823743343f, 10894794.336297152564f},
        {-51973801.463933646679f, -104199997.554352939129f, -52225454.356278456748f},
        {972270.324080004124f, 2025882.096547174733f, 1054898.052467488218f},
    };
    static const float step[9] = {
        -0.022553957999f, -0.421539008617f, -0.678715527058f, -0.838916420937f, -0.961754500866f,
        -0.990773200989f, -0.994858562946f, -0.996561050415f, -0.999547004700f,
    };
    float c0 = params[0], c3 = params[3], pw = 0.0f, half = 0.0f;
    int i;
    if (params[1] > 0.0f && params[2] < 1.0f)
        return params[2] - params[0] * 2.0f;
    if (c0 > 0.0f || c3 > 0.0f)
        return 0.0001f;
    for (i = 0; i < 9; i++)
        if (c0 > step[i]) {
            pw = (i >= 3 && i <= 5) ? co[i][0] * c0 / (co[i][1] + co[i][2] * c0)
                                    : co[i][0] + co[i][1] * c0 + co[i][2] * c0 * c0;
            break;
        }
    for (i = 0; i < 9; i++)
        if (c3 > step[i]) {
            half = (i >= 3 && i <= 5) ? co[i][0] * c3 / (co[i][1] + co[i][2] * c3)
                                      : co[i][0] + co[i][1] * c3 + co[i][2] * c3 * c3;
            break;
        }
    if (pw == 0.0f || (half > pw && c0 < -0.1f))
        return half * 2.0f;
    return pw;
}
