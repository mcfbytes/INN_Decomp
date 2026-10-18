/*
   Super 2xSaI as a libretro-style GLSL shader (GLSL ES 3.00 compatible).

   Ported from libretro/slang-shaders edge-smoothing/eagle/shaders/super-2xsai.slang
   (guest(r), "Adapted by guest(r) on 20.4 and 9.5. 2007") by this project:
   the push constants and the UBO are replaced by the usual TextureSize / MVPMatrix
   uniforms, and the stage split is the VERTEX / FRAGMENT convention of the other
   shaders here. The filter code is unchanged.

   Filter code copied from the DOSBox source code,
   Copyright (C) 2002-2007  The DOSBox Team, License: GNU-GPL
   (DOSBox is GPL version 2 or later). GET_RESULT is from the original 2xSaI,
   Copyright (c) 1999-2001 by Derek Liauw Kie Fa, License: GNU-GPL.
   Used here under GPL-3.0-or-later (see README.md).
*/

#if defined(VERTEX)

in vec4 VertexCoord;
in vec4 TexCoord;
out vec4 TEX0;
uniform mat4 MVPMatrix;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    TEX0.xy = TexCoord.xy;
}

#elif defined(FRAGMENT)

precision highp float;
out vec4 FragColor;
in vec4 TEX0;
uniform vec2 TextureSize;
uniform sampler2D Texture;

const vec3 dt = vec3(65536.0,256.0,1.0);


float GET_RESULT(float A, float B, float C, float D)
{
    return (sign(abs(A-C)+abs(A-D)) - sign(abs(B-C)+abs(B-D))); 
}


float reduce(vec3 color)
{ 
    return dot(color,dt);
}

void main()
{
    vec2 OGL2Size    = TextureSize;
    vec2 OGL2InvSize = (1.0/TextureSize);

    // Calculating texel coordinates

    vec2 OGL2Pos = TEX0.xy*OGL2Size.xy;
    vec2 fp  = fract(OGL2Pos);
    vec2 dx  = vec2(OGL2InvSize.x,0.0);
    vec2 dy  = vec2(0.0,OGL2InvSize.y);
    vec2 g1  = vec2( OGL2InvSize.x,OGL2InvSize.y);
    vec2 g2  = vec2(-OGL2InvSize.x,OGL2InvSize.y);

    vec2 pC4 = floor(OGL2Pos)/OGL2Size.xy + 0.5*OGL2InvSize;
    vec2 pC8 = pC4 + g1;

    // Reading the texels

    vec3 C0 = texture(Texture,pC4-g1).xyz; 
    vec3 C1 = texture(Texture,pC4-dy).xyz;
    vec3 C2 = texture(Texture,pC4-g2).xyz;
    vec3 D3 = texture(Texture,pC4-g2+dx).xyz;
    vec3 C3 = texture(Texture,pC4-dx).xyz;
    vec3 C4 = texture(Texture,pC4).xyz;
    vec3 C5 = texture(Texture,pC4+dx).xyz;
    vec3 D4 = texture(Texture,pC8-g2).xyz;
    vec3 C6 = texture(Texture,pC4+g2).xyz;
    vec3 C7 = texture(Texture,pC4+dy).xyz;
    vec3 C8 = texture(Texture,pC8).xyz;
    vec3 D5 = texture(Texture,pC8+dx).xyz;
    vec3 D0 = texture(Texture,pC4+g2+dy).xyz;
    vec3 D1 = texture(Texture,pC8+g2).xyz;
    vec3 D2 = texture(Texture,pC8+dy).xyz;
    vec3 D6 = texture(Texture,pC8+g1).xyz;

    vec3 p00,p10,p01,p11;

    float c0 = reduce(C0);float c1 = reduce(C1);
    float c2 = reduce(C2);float c3 = reduce(C3);
    float c4 = reduce(C4);float c5 = reduce(C5);
    float c6 = reduce(C6);float c7 = reduce(C7);
    float c8 = reduce(C8);float d0 = reduce(D0);
    float d1 = reduce(D1);float d2 = reduce(D2);
    float d3 = reduce(D3);float d4 = reduce(D4);
    float d5 = reduce(D5);float d6 = reduce(D6);

    /*              Super2xSaI code               */
    /*  Copied from the Dosbox source code        */
    /*  Copyright (C) 2002-2007  The DOSBox Team  */
    /*  License: GNU-GPL                          */
    /*  Adapted by guest(r) on 16.4.2007          */

   
   if (c7 == c5 && c4 != c8) {
            p11 = p01 = C7;
    } else if (c4 == c8 && c7 != c5) {
            p11 = p01 = C4;
    } else if (c4 == c8 && c7 == c5) {
            float r;
            r = GET_RESULT(c5,c4,c6,d1);
            r+= GET_RESULT(c5,c4,c3,c1);
            r+= GET_RESULT(c5,c4,d2,d5);
            r+= GET_RESULT(c5,c4,c2,d4);

            if (r > 0.0)
                    p11 = p01 = C5;
            else if (r < 0.0)
                    p11 = p01 = C4;
            else {
                    p11 = p01 = 0.5*(C4+C5);
            }
    } else {
            if (c5 == c8 && c8 == d1 && c7 != d2 && c8 != d0)
                    p11 = 0.25*(3.0*C8+C7);
            else if (c4 == c7 && c7 == d2 && d1 != c8 && c7 != d6)
                    p11 = 0.25*(3.0*C7+C8);
            else
                    p11 = 0.5*(C7+C8);

            if (c5 == c8 && c5 == c1 && c4 != c2 && c5 != c0)
                    p01 = 0.25*(3.0*C5+C4);
            else if (c4 == c7 && c4 == c2 && c1 != c5 && c4 != d3)
                    p01 = 0.25*(3.0*C4+C5);
            else
                    p01 = 0.5*(C4+C5);
    }

    if (c4 == c8 && c7 != c5 && c3 == c4 && c4 != d2)
            p10 = 0.5*(C7+C4);
    else if (c4 == c6 && c5 == c4 && c3 != c7 && c4 != d0)
            p10 = 0.5*(C7+C4);
    else
            p10 = C7;

    if (c7 == c5 && c4 != c8 && c6 == c7 && c7 != c2)
            p00 = 0.5*(C7+C4);
    else if (c3 == c7 && c8 == c7 && c6 != c4 && c7 != c0)
            p00 = 0.5*(C7+C4);
    else
            p00 = C4;

    // Distributing the final products    

    vec3 color = vec3(0.0);

    if (fp.x < 0.50)
    { if (fp.y < 0.50) color = p00; else color = p10;}
    else
    { if (fp.y < 0.50) color = p01; else color = p11;}

    FragColor = vec4(color, 1.0);
}

#endif
