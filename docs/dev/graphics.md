# Graphics back end notes

What the renderer requires of GL, what the ES profile cannot do, and how the shader cache works.
The user-facing summary is in the top-level README's Graphics section.

The renderer needs an **OpenGL 3.3 core profile**, or **OpenGL ES 3.2** when built with
`-DWR_GL_ES=ON` (the default for Android). It uses nothing newer than GL 3.3 / ES 3.0:
samplers, VAOs, FBOs and explicit attribute locations are the whole requirement, so a 3.3 floor
keeps the mapping layers that stop there usable. One source serves both profiles; where they
differ, the difference is confined to a small block of helpers at the top of
`runtime/gx/render_gl.cpp`.

On macOS the system framework is linked directly; elsewhere the entry points are resolved at
runtime by a vendored [glad](https://gen.glad.sh/) loader (`runtime/gx/glad/` and
`glad_es/`, regenerated with `glad --api gl:core=3.3 --extensions ""` and
`--api gles2:core=3.2`).

Two things GL ES cannot do, both accepted rather than emulated:

- **Logic-op blending.** ES has no `GL_COLOR_LOGIC_OP`. GX logic ops are skipped, so such draws
  write through with blending off. Reproducing them would mean reading the framebuffer in the
  generated TEV shader via `GL_EXT_shader_framebuffer_fetch`.
- **Sampler LOD bias.** ES has no `GL_TEXTURE_LOD_BIAS`, so mip selection can differ slightly.

## Shader cache

Each TEV configuration becomes a generated fragment shader, compiled the first time a draw
uses it -- on the render thread, mid-frame. A race through a course meets around eighty of
them, and they do not arrive one at a time: the race start brings in nine at once (the spray,
the wake, the speed effects), and on this machine's compiler that frame took 60 ms against a
4 ms norm. A mobile driver takes tens of milliseconds per program, so the same burst is a
visible hitch at a fixed spot in the course, on every fresh launch.

So every key compiled is appended to a file (`saves/shaders.bin` on desktop, `shaders.bin` in
the app's files directory on a headset), and the next run builds all of them in `render_init`,
before the game boots. Where the driver hands back program binaries (GL ES 3.0 does; macOS
reports no binary formats) those are stored too, and the run after that loads rather than
compiles. A binary is only trusted with the same driver and the same generated source -- the
file carries the GL strings and each record a hash of its GLSL -- and anything stale falls back
to compiling and rewrites the file. That hash covers the one vertex shader as well as the
record's own fragment shader: a binary is a link of the two, and nothing in the key would
otherwise notice the vertex shader changing underneath it. Deleting the file is always safe; the first run just pays
the compiles at first use again. The startup line reports what happened:

```
[shaders] 80 programs from cache (0 from binaries, 80 compiled) in 138 ms
```
