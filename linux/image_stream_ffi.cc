#include "camera.h"

#include <cstdint>

// Forward declaration of the plugin struct from camera_desktop_plugin.cc.
// We access it via a global pointer set during plugin registration.
struct _CameraDesktopPlugin;
typedef struct _CameraDesktopPlugin CameraDesktopPlugin;

// Global plugin instance — set during registration.
static CameraDesktopPlugin* g_plugin_instance = nullptr;

// Camera lookup function — implemented in camera_desktop_plugin.cc.
extern Camera* camera_desktop_find_camera_by_id(CameraDesktopPlugin* plugin,
                                                 int camera_id);

extern "C" {

__attribute__((visibility("default")))
void camera_desktop_ffi_set_plugin(void* plugin) {
  g_plugin_instance = static_cast<CameraDesktopPlugin*>(plugin);
}

__attribute__((visibility("default")))
void* camera_desktop_get_image_stream_buffer(int camera_id) {
  if (!g_plugin_instance) return nullptr;
  Camera* camera =
      camera_desktop_find_camera_by_id(g_plugin_instance, camera_id);
  if (!camera) return nullptr;
  return camera->GetImageStreamBuffer();
}

__attribute__((visibility("default")))
void camera_desktop_register_image_stream_callback(
    int camera_id, void (*callback)(int32_t)) {
  if (!g_plugin_instance) return;
  Camera* camera =
      camera_desktop_find_camera_by_id(g_plugin_instance, camera_id);
  if (camera) camera->RegisterImageStreamCallback(callback);
}

__attribute__((visibility("default")))
void camera_desktop_unregister_image_stream_callback(int camera_id) {
  if (!g_plugin_instance) return;
  Camera* camera =
      camera_desktop_find_camera_by_id(g_plugin_instance, camera_id);
  if (camera) camera->UnregisterImageStreamCallback();
}

}  // extern "C"
