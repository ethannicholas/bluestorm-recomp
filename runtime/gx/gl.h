// OpenGL include shim. macOS ships a core profile directly; everywhere else the entry
// points are resolved at runtime through a generated glad loader -- see glad/ and
// gl_load.cpp.
//
// The renderer targets OpenGL 3.3 core: samplers, VAOs, FBOs and explicit attribute
// locations are all that the GX pipeline needs, and nothing here uses a 4.x feature.
// Keeping the floor at 3.3 means mapping layers that stop there (Mesa's D3D12 driver,
// as shipped in Microsoft's OpenGL compatibility pack) can run it.
#pragma once

#define WR_GL_MAJOR 3
#define WR_GL_MINOR 3
// Shader preamble; must stay in step with the version requested above.
#define WR_GLSL_VERSION "#version 330 core\n"
// Minimum acceptable version, encoded as major * 10 + minor, as gl_load() returns it.
#define WR_GL_VERSION_MIN (WR_GL_MAJOR * 10 + WR_GL_MINOR)

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif

// Resolve the GL entry points for the current context. Must be called once after the
// context is made current, before any other gx:: call. Returns the version actually
// resolved as major * 10 + minor, or 0 if no usable GL was found.
//
// This has to be checked against WR_GL_VERSION_MIN: a legacy driver (Windows' "GDI
// Generic" 1.1, say) resolves successfully but leaves every GL 2.0+ entry point null,
// so calling into the renderer afterwards would dereference a null function pointer.
int gl_load();
