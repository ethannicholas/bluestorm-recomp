// Resolves GL entry points through SDL, which knows how to ask the platform's GL
// library (wglGetProcAddress plus the opengl32.dll exports on Windows).
#include "gl.h"
#include <SDL.h>

int gl_load() {
#ifdef __APPLE__
    return 41;  // linked directly against the system framework, which is 4.1
#else
    int v = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (!v) return 0;
    return GLAD_VERSION_MAJOR(v) * 10 + GLAD_VERSION_MINOR(v);
#endif
}
