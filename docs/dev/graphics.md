# Graphics back end notes

What the renderer requires of GL, what the ES profile cannot do, and how the shader cache works.
The user-facing summary is in the top-level README's Graphics section.

The renderer needs an **OpenGL 3.3 core profile**, or **OpenGL ES 3.2** when built with
`-DGCN_GL_ES=ON` (the default for Android). It uses nothing newer than GL 3.3 / ES 3.0:
samplers, VAOs, FBOs and explicit attribute locations are the whole requirement, so a 3.3 floor
keeps the mapping layers that stop there usable. One source serves both profiles; where they
differ, the difference is confined to a small block of helpers at the top of
`gcn-recomp/runtime/gx/render_gl.cpp`.

On macOS the system framework is linked directly; elsewhere the entry points are resolved at
runtime by a vendored [glad](https://gen.glad.sh/) loader (`gcn-recomp/runtime/gx/glad/` and
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

**The game ships its keys.** `runtime/shader_keys.bin` is the list gcn-recomp compiles on a
fresh install before the game starts (`SHADER_KEYS`; see "Shader cache" in
`gcn-recomp/docs/graphics.md`), so a first play-through on a new headset does not hitch either.
It was made on 2026-10-19 from the headset's two caches, both written on 2026-10-11: the
app's (751 records) and the harness's (246). Those are in format 1, from before gcn-recomp
a55d711, which `tools/shader_keys` does not read. A format-1 key is the current key's first 71
words (`genmode` to `efb_tex_mask`, with the same raw register values). The five fields added
since (`efb_pal_mask`, `efb_conv`, `ztex`, `dst_alpha`, `fg_depth`) follow those words, so each
record was widened with those five set to zero, written as a key list, and run through
`shader_keys`. That made the keys canonical and left 81 of the 997, 24 KB. A draw that now sets
one of the new fields would be missing from the list. A check found none: a harness run of the
current build in the eye path (`--eye --fast`, through the menus, a race at Southern Island and
its results, no cache) asked for 40 keys, and all 40 were already in the list. On the Quest 3
the first start with no cache compiles the 81 in 3.0 s. The next start restores them from
binaries in 33 ms:

```
[shaders] 81 programs from cache (0 from binaries, 81 compiled, 81 of them from the game's key list) in 2985 ms
[shaders] 81 programs from cache (81 from binaries, 0 compiled, 0 of them from the game's key list) in 33 ms
```

To extend the list, put it first so its order holds: `shader_keys runtime/shader_keys.bin
runtime/shader_keys.bin <new caches>`. With no desktop toolchain, build the tool with the NDK's
`aarch64-linux-android29-clang++` from `tools/shader_keys.cpp` and `runtime/gx/shadergen.cpp`
(`-std=c++20 -DGCN_GL_ES -DGCN_GL_ES30 -Iruntime -Iruntime/gx/glad_es/include`), then run it
on the headset.

**95 keys from the Vision Pro (2026-10-22).** The list was 81 keys when the Vision Pro first
ran it. Two sessions there, the second a whole championship, left a cache of 95. The first
session compiled the shipped 81 and 9 more. The championship added another 5. Only one of
those shows in the log, a `[stall]` line on a course-loading theater frame with one draw left
out. Those lines are written only for frames over 50 ms, so the other four were built during
normal frames. Any of them could be the frame or two in mid-race where the water vanished,
since a draw whose program is still being built is left out. That was not proven. Merged in
as above (`shader_keys` from the desktop build reads the headset's GL ES cache), the list is
95 keys. On the headset with its cache emptied, every one came from the list:

```
[shaders] 95 programs from cache (0 from binaries, 95 compiled, 95 of them from the game's key list) in 318 ms
```
