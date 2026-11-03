# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) 1.91.9b (MIT, `LICENSE.txt`),
for the co-op colour picker (`port/src/port_imgui.cpp`). The files are the
release's own, unchanged: the core (`imgui*.cpp`, `imgui*.h`, `imstb_*.h`,
`imconfig.h`) and the OpenGL 3 backend (`backends/imgui_impl_opengl3.*`),
which `port/src/port_imgui_gl.cpp` builds on fast3d's glad loader instead
of its bundled one.

To update, copy the same files from a newer release over these. 1.92
changed how font textures reach the backend, which `port_imgui.cpp`
relies on (the atlas is built once on the sim thread and uploaded once on
the GL thread), so check that before moving past 1.91.
