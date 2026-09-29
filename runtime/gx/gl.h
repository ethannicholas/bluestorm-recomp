// OpenGL include shim. macOS ships a 4.1 core profile directly; other platforms
// will need a loader (e.g. glad) wired in here.
#pragma once
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#error "add a GL loader for this platform"
#endif
