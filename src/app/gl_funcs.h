// The few OpenGL entry points the app calls directly, loaded through SDL so the same code works with
// desktop GL and GLES and nothing has to be linked against libGL at build time.
#pragma once
#include <cstdint>

namespace glf {
using Enum = unsigned int;
extern void (*Viewport)(int, int, int, int);
extern void (*ClearColor)(float, float, float, float);
extern void (*Clear)(unsigned int);
extern void (*ReadPixels)(int, int, int, int, Enum, Enum, void*);
extern void (*GenTextures)(int, unsigned int*);
extern void (*DeleteTextures)(int, const unsigned int*);
extern void (*BindTexture)(Enum, unsigned int);
extern void (*TexParameteri)(Enum, Enum, int);
extern void (*TexImage2D)(Enum, int, int, int, int, int, Enum, Enum, const void*);
extern void (*TexSubImage2D)(Enum, int, int, int, int, int, Enum, Enum, const void*);
extern void (*PixelStorei)(Enum, int);
extern void (*GetIntegerv)(Enum, int*);
// shaders (video: YUV -> RGB on the GPU); loaded separately so a missing one only disables that
extern unsigned int (*CreateShader)(Enum);
extern void (*ShaderSource)(unsigned int, int, const char* const*, const int*);
extern void (*CompileShader)(unsigned int);
extern void (*GetShaderiv)(unsigned int, Enum, int*);
extern void (*GetShaderInfoLog)(unsigned int, int, int*, char*);
extern void (*DeleteShader)(unsigned int);
extern unsigned int (*CreateProgram)();
extern void (*AttachShader)(unsigned int, unsigned int);
extern void (*BindAttribLocation)(unsigned int, unsigned int, const char*);
extern void (*LinkProgram)(unsigned int);
extern void (*GetProgramiv)(unsigned int, Enum, int*);
extern void (*UseProgram)(unsigned int);
extern int (*GetUniformLocation)(unsigned int, const char*);
extern int (*GetAttribLocation)(unsigned int, const char*);
extern void (*Uniform1i)(int, int);
extern void (*UniformMatrix4fv)(int, int, unsigned char, const float*);
extern void (*GetUniformfv)(unsigned int, int, float*);
extern void (*ActiveTexture)(Enum);
bool Load();  // call after the GL context is current
bool LoadShaders();

constexpr Enum COLOR_BUFFER_BIT = 0x4000, RGB = 0x1907, RGBA = 0x1908, UNSIGNED_BYTE = 0x1401, TEXTURE_2D = 0x0DE1,
               TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, LINEAR = 0x2601, TEXTURE_WRAP_S = 0x2802,
               TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F, UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05,
               MAX_TEXTURE_SIZE = 0x0D33, TEXTURE_BINDING_2D = 0x8069, RED = 0x1903, RG = 0x8227, R8 = 0x8229, RG8 = 0x822B,
               TEXTURE0 = 0x84C0, TEXTURE1 = 0x84C1, CURRENT_PROGRAM = 0x8B8D, VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30,
               COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82;
}  // namespace glf
