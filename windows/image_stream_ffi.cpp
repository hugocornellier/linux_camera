#include "camera.h"
#include "camera_desktop_plugin.h"

#include <cstdint>

extern "C" {

__declspec(dllexport) void* camera_desktop_get_image_stream_buffer(
    int camera_id) {
  auto* plugin = CameraDesktopPlugin::instance();
  if (!plugin) return nullptr;
  Camera* camera = plugin->FindCameraById(camera_id);
  if (!camera) return nullptr;
  return camera->GetImageStreamBuffer();
}

__declspec(dllexport) void camera_desktop_register_image_stream_callback(
    int camera_id, void (*callback)(int32_t)) {
  auto* plugin = CameraDesktopPlugin::instance();
  if (!plugin) return;
  Camera* camera = plugin->FindCameraById(camera_id);
  if (camera) camera->RegisterImageStreamCallback(callback);
}

__declspec(dllexport) void camera_desktop_unregister_image_stream_callback(
    int camera_id) {
  auto* plugin = CameraDesktopPlugin::instance();
  if (!plugin) return;
  Camera* camera = plugin->FindCameraById(camera_id);
  if (camera) camera->UnregisterImageStreamCallback();
}

}  // extern "C"
