#pragma once

// Vulkan's C API as the rhi_vulkan module uses it. Internal to the module: only its sources
// include this header. The module is compiled with VK_NO_PROTOTYPES, so the headers declare
// function pointer types only; VulkanLoader.h loads the functions at run time.
//
// The window-system headers are included directly instead of through VK_USE_PLATFORM_*, so that
// no X11 or Wayland development package is needed to build: vulkan_xlib.h and vulkan_wayland.h
// only name the types declared below, exactly as Xlib and libwayland declare them.

#include <vulkan/vulkan_core.h>

// Platform code: each platform's window system has its own surface extension (ADR-0012).
#ifdef _WIN32
#include <windows.h>

// vulkan_win32.h names Win32 types, so it comes after windows.h.
#include <vulkan/vulkan_win32.h>
#else
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,modernize-use-using): Xlib's own declarations
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,modernize-use-using)
struct wl_display;
struct wl_surface;

#include <vulkan/vulkan_wayland.h>
#include <vulkan/vulkan_xlib.h>
#endif
