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
void (*PixelStorei)(Enum, int);
void (*GetIntegerv)(Enum, int*);

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
    ok &= L(PixelStorei, "glPixelStorei");
    ok &= L(GetIntegerv, "glGetIntegerv");
    return ok;
}
}  // namespace glf
