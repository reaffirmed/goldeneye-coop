/**
 * port_imgui_gl.cpp - Dear ImGui's OpenGL 3 backend (port/imgui/backends),
 * built on the renderer's own GL loader (fast3d's glad, loaded when the
 * window opens) instead of the backend's bundled one.
 */
#include "../fast3d/glad/glad.h"
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "../imgui/backends/imgui_impl_opengl3.cpp"
