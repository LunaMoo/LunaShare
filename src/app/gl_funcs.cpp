#include "gl_funcs.h"

#include <SDL.h>

namespace glf {
void (*Viewport)(int, int, int, int);
void (*ClearColor)(float, float, float, float);
void (*Clear)(unsigned int);
void (*ReadPixels)(int, int, int, int, Enum, Enum, void*);
void (*GenTextures)(int, unsigned int*);
void (*DeleteTextures)(int, const unsigned int*);
void (*BindTexture)(Enum, unsigned int);
void (*TexParameteri)(Enum, Enum, int);
void (*TexImage2D)(Enum, int, int, int, int, int, Enum, Enum, const void*);
void (*TexSubImage2D)(Enum, int, int, int, int, int, Enum, Enum, const void*);
void (*PixelStorei)(Enum, int);
void (*GetIntegerv)(Enum, int*);

unsigned int (*CreateShader)(Enum);
void (*ShaderSource)(unsigned int, int, const char* const*, const int*);
void (*CompileShader)(unsigned int);
void (*GetShaderiv)(unsigned int, Enum, int*);
void (*GetShaderInfoLog)(unsigned int, int, int*, char*);
void (*DeleteShader)(unsigned int);
unsigned int (*CreateProgram)();
void (*AttachShader)(unsigned int, unsigned int);
void (*BindAttribLocation)(unsigned int, unsigned int, const char*);
void (*LinkProgram)(unsigned int);
void (*GetProgramiv)(unsigned int, Enum, int*);
void (*UseProgram)(unsigned int);
int (*GetUniformLocation)(unsigned int, const char*);
int (*GetAttribLocation)(unsigned int, const char*);
void (*Uniform1i)(int, int);
void (*Uniform4f)(int, float, float, float, float);
void (*UniformMatrix4fv)(int, int, unsigned char, const float*);
void (*GetUniformfv)(unsigned int, int, float*);
void (*ActiveTexture)(Enum);

template <class T>
static bool L(T& fn, const char* name) {
    fn = (T)SDL_GL_GetProcAddress(name);
    return fn != nullptr;
}

bool Load() {
    bool ok = true;
    ok &= L(Viewport, "glViewport");
    ok &= L(ClearColor, "glClearColor");
    ok &= L(Clear, "glClear");
    ok &= L(ReadPixels, "glReadPixels");
    ok &= L(GenTextures, "glGenTextures");
    ok &= L(DeleteTextures, "glDeleteTextures");
    ok &= L(BindTexture, "glBindTexture");
    ok &= L(TexParameteri, "glTexParameteri");
    ok &= L(TexImage2D, "glTexImage2D");
    ok &= L(TexSubImage2D, "glTexSubImage2D");
    ok &= L(PixelStorei, "glPixelStorei");
    ok &= L(GetIntegerv, "glGetIntegerv");
    return ok;
}
bool LoadShaders() {
    bool ok = true;
#define L(fn, name) (L(fn, name) || (SDL_Log("missing %s", name), false))
    ok &= L(CreateShader, "glCreateShader");
    ok &= L(ShaderSource, "glShaderSource");
    ok &= L(CompileShader, "glCompileShader");
    ok &= L(GetShaderiv, "glGetShaderiv");
    ok &= L(GetShaderInfoLog, "glGetShaderInfoLog");
    ok &= L(DeleteShader, "glDeleteShader");
    ok &= L(CreateProgram, "glCreateProgram");
    ok &= L(AttachShader, "glAttachShader");
    ok &= L(BindAttribLocation, "glBindAttribLocation");
    ok &= L(LinkProgram, "glLinkProgram");
    ok &= L(GetProgramiv, "glGetProgramiv");
    ok &= L(UseProgram, "glUseProgram");
    ok &= L(GetUniformLocation, "glGetUniformLocation");
    ok &= L(GetAttribLocation, "glGetAttribLocation");
    ok &= L(Uniform1i, "glUniform1i");
    ok &= L(Uniform4f, "glUniform4f");
    ok &= L(UniformMatrix4fv, "glUniformMatrix4fv");
    ok &= L(GetUniformfv, "glGetUniformfv");
    ok &= L(ActiveTexture, "glActiveTexture");
#undef L
    return ok;
}
}  // namespace glf
