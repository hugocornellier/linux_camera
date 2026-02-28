#include "include/camera_desktop/camera_desktop_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gst/gst.h>


#include <map>
#include <memory>
#include <string>
#include <cstdint>

#include "camera.h"
#include "device_enumerator.h"

int64_t camera_desktop_ffi_register_stream_handle(Camera* camera);
void camera_desktop_ffi_release_stream_handle(int64_t stream_handle);
void camera_desktop_ffi_release_handles_for_camera(Camera* camera);

// Plugin data stored as an opaque C++ pointer inside the GObject struct.
struct PluginData {
  std::map<int, std::unique_ptr<Camera>> cameras;
  int next_camera_id = 1;
};

#define CAMERA_DESKTOP_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), camera_desktop_plugin_get_type(), \
                              CameraDesktopPlugin))

struct _CameraDesktopPlugin {
  GObject parent_instance;
  FlMethodChannel* channel;
  FlTextureRegistrar* texture_registrar;
  PluginData* data;
};

G_DEFINE_TYPE(CameraDesktopPlugin, camera_desktop_plugin, g_object_get_type())

// --- Method handlers ---

static void handle_available_cameras(CameraDesktopPlugin* self,
                                     FlMethodCall* method_call) {
  auto devices = DeviceEnumerator::EnumerateDevices();

  g_autoptr(FlValue) result = fl_value_new_list();
  for (const auto& device : devices) {
    g_autoptr(FlValue) entry = fl_value_new_map();
    fl_value_set_string_take(entry, "name",
                             fl_value_new_string(device.name.c_str()));
    fl_value_set_string_take(entry, "lensDirection",
                             fl_value_new_int(device.lens_direction));
    fl_value_set_string_take(entry, "sensorOrientation",
                             fl_value_new_int(device.sensor_orientation));
    // Also pass the device path so Dart can pass it back in create().
    // The CameraDescription.name field will carry this.
    // Override name to include both friendly name and path for disambiguation.
    // Format: "Friendly Name (/dev/videoN)"
    std::string display_name =
        device.name + " (" + device.device_path + ")";
    fl_value_set_string(entry, "name",
                        fl_value_new_string(display_name.c_str()));
    fl_value_append(result, entry);
  }

  fl_method_call_respond_success(method_call, result, nullptr);
}

static void handle_get_platform_capabilities(FlMethodCall* method_call) {
  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "supportsMirrorControl",
                           fl_value_new_bool(true));
  fl_value_set_string_take(result, "supportsVideoFpsControl",
                           fl_value_new_bool(true));
  fl_value_set_string_take(result, "supportsVideoBitrateControl",
                           fl_value_new_bool(true));
  fl_method_call_respond_success(method_call, result, nullptr);
}

static void handle_create(CameraDesktopPlugin* self,
                          FlMethodCall* method_call) {
  FlValue* args = fl_method_call_get_args(method_call);
  const char* camera_name =
      fl_value_get_string(fl_value_lookup_string(args, "cameraName"));
  int resolution_preset =
      fl_value_get_int(fl_value_lookup_string(args, "resolutionPreset"));
  FlValue* audio_val = fl_value_lookup_string(args, "enableAudio");
  bool enable_audio = audio_val ? fl_value_get_bool(audio_val) : false;

  int target_fps = 30;
  FlValue* fps_val = fl_value_lookup_string(args, "fps");
  if (fps_val && fl_value_get_type(fps_val) == FL_VALUE_TYPE_INT) {
    target_fps = fl_value_get_int(fps_val);
  } else if (fps_val && fl_value_get_type(fps_val) == FL_VALUE_TYPE_FLOAT) {
    target_fps = static_cast<int>(fl_value_get_float(fps_val));
  }
  if (target_fps < 5) target_fps = 5;
  if (target_fps > 60) target_fps = 60;

  int target_bitrate = 0;
  FlValue* bitrate_val = fl_value_lookup_string(args, "videoBitrate");
  if (bitrate_val && fl_value_get_type(bitrate_val) == FL_VALUE_TYPE_INT) {
    target_bitrate = fl_value_get_int(bitrate_val);
  } else if (bitrate_val &&
             fl_value_get_type(bitrate_val) == FL_VALUE_TYPE_FLOAT) {
    target_bitrate = static_cast<int>(fl_value_get_float(bitrate_val));
  }
  if (target_bitrate < 0) target_bitrate = 0;

  int audio_bitrate = 0;
  FlValue* audio_bitrate_val = fl_value_lookup_string(args, "audioBitrate");
  if (audio_bitrate_val &&
      fl_value_get_type(audio_bitrate_val) == FL_VALUE_TYPE_INT) {
    audio_bitrate = fl_value_get_int(audio_bitrate_val);
  } else if (audio_bitrate_val &&
             fl_value_get_type(audio_bitrate_val) == FL_VALUE_TYPE_FLOAT) {
    audio_bitrate = static_cast<int>(fl_value_get_float(audio_bitrate_val));
  }
  if (audio_bitrate < 0) audio_bitrate = 0;

  // Extract device path from the camera name.
  // Format: "Friendly Name (/dev/videoN)" — extract the path in parentheses.
  std::string name_str(camera_name);
  std::string device_path;
  size_t paren_start = name_str.rfind('(');
  size_t paren_end = name_str.rfind(')');
  if (paren_start != std::string::npos && paren_end != std::string::npos &&
      paren_end > paren_start) {
    device_path = name_str.substr(paren_start + 1, paren_end - paren_start - 1);
  } else {
    // Fallback: treat the whole name as a device path.
    device_path = name_str;
  }

  if (device_path.empty() || device_path.find("/dev/") != 0) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "invalid_camera_name",
                                 "Could not extract device path from camera name",
                                 details, nullptr);
    return;
  }

  // Enumerate resolutions and select the best match for the preset.
  auto resolutions = DeviceEnumerator::EnumerateResolutions(device_path);
  auto selected = DeviceEnumerator::SelectResolution(resolutions,
                                                     resolution_preset);

  CameraConfig config;
  config.device_path = device_path;
  config.resolution_preset = resolution_preset;
  config.enable_audio = enable_audio;
  config.target_width = selected.width;
  config.target_height = selected.height;
  config.target_fps = target_fps > 0 ? target_fps : selected.max_fps;
  config.target_bitrate = target_bitrate;
  config.audio_bitrate = audio_bitrate;

  int camera_id = self->data->next_camera_id++;
  auto camera = std::make_unique<Camera>(
      camera_id, self->texture_registrar, self->channel, config);

  int64_t texture_id = camera->RegisterTexture();
  if (texture_id < 0) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "texture_registration_failed",
                                 "Failed to register Flutter texture",
                                 details, nullptr);
    return;
  }

  self->data->cameras[camera_id] = std::move(camera);

  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "cameraId",
                           fl_value_new_int(camera_id));
  fl_value_set_string_take(result, "textureId",
                           fl_value_new_int(texture_id));
  fl_method_call_respond_success(method_call, result, nullptr);
}

static Camera* find_camera(CameraDesktopPlugin* self,
                           FlMethodCall* method_call) {
  FlValue* args = fl_method_call_get_args(method_call);
  int camera_id = fl_value_get_int(fl_value_lookup_string(args, "cameraId"));
  auto it = self->data->cameras.find(camera_id);
  if (it == self->data->cameras.end()) {
    g_autoptr(FlValue) details = fl_value_new_null();
    fl_method_call_respond_error(method_call, "camera_not_found",
                                 "No camera found with the given ID",
                                 details, nullptr);
    return nullptr;
  }
  return it->second.get();
}

static void handle_initialize(CameraDesktopPlugin* self,
                              FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  // Camera::Initialize responds asynchronously.
  camera->Initialize(method_call);
}

static void handle_take_picture(CameraDesktopPlugin* self,
                                FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  camera->TakePicture(method_call);
}

static void handle_start_video_recording(CameraDesktopPlugin* self,
                                         FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  camera->StartVideoRecording(method_call);
}

static void handle_stop_video_recording(CameraDesktopPlugin* self,
                                        FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  camera->StopVideoRecording(method_call);
}

static void handle_start_image_stream(CameraDesktopPlugin* self,
                                      FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  camera->StartImageStream();
  const int64_t stream_handle = camera_desktop_ffi_register_stream_handle(camera);
  g_autoptr(FlValue) result = fl_value_new_map();
  fl_value_set_string_take(result, "streamHandle",
                           fl_value_new_int(stream_handle));
  fl_method_call_respond_success(method_call, result, nullptr);
}

static void handle_stop_image_stream(CameraDesktopPlugin* self,
                                     FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  FlValue* args = fl_method_call_get_args(method_call);
  FlValue* handle_val = fl_value_lookup_string(args, "streamHandle");
  if (handle_val && fl_value_get_type(handle_val) == FL_VALUE_TYPE_INT) {
    camera_desktop_ffi_release_stream_handle(fl_value_get_int(handle_val));
  }
  camera->StopImageStream();
  fl_method_call_respond_success(method_call, fl_value_new_null(), nullptr);
}

static void handle_pause_preview(CameraDesktopPlugin* self,
                                 FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  camera->PausePreview();
  fl_method_call_respond_success(method_call, fl_value_new_null(), nullptr);
}

static void handle_resume_preview(CameraDesktopPlugin* self,
                                  FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;
  camera->ResumePreview();
  fl_method_call_respond_success(method_call, fl_value_new_null(), nullptr);
}

static void handle_set_mirror(CameraDesktopPlugin* self,
                              FlMethodCall* method_call) {
  Camera* camera = find_camera(self, method_call);
  if (!camera) return;

  FlValue* args = fl_method_call_get_args(method_call);
  FlValue* mirrored_val = fl_value_lookup_string(args, "mirrored");
  bool mirrored = mirrored_val ? fl_value_get_bool(mirrored_val) : true;

  camera->SetMirror(mirrored);
  fl_method_call_respond_success(method_call, fl_value_new_null(), nullptr);
}

static void handle_dispose(CameraDesktopPlugin* self,
                           FlMethodCall* method_call) {
  FlValue* args = fl_method_call_get_args(method_call);
  int camera_id = fl_value_get_int(fl_value_lookup_string(args, "cameraId"));

  auto it = self->data->cameras.find(camera_id);
  if (it != self->data->cameras.end()) {
    camera_desktop_ffi_release_handles_for_camera(it->second.get());
    it->second->Dispose();
    self->data->cameras.erase(it);
  }
  fl_method_call_respond_success(method_call, fl_value_new_null(), nullptr);
}

// --- Plugin lifecycle ---

static void camera_desktop_plugin_handle_method_call(
    CameraDesktopPlugin* self,
    FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "availableCameras") == 0) {
    handle_available_cameras(self, method_call);
  } else if (strcmp(method, "getPlatformCapabilities") == 0) {
    handle_get_platform_capabilities(method_call);
  } else if (strcmp(method, "create") == 0) {
    handle_create(self, method_call);
  } else if (strcmp(method, "initialize") == 0) {
    handle_initialize(self, method_call);
  } else if (strcmp(method, "takePicture") == 0) {
    handle_take_picture(self, method_call);
  } else if (strcmp(method, "startVideoRecording") == 0) {
    handle_start_video_recording(self, method_call);
  } else if (strcmp(method, "stopVideoRecording") == 0) {
    handle_stop_video_recording(self, method_call);
  } else if (strcmp(method, "startImageStream") == 0) {
    handle_start_image_stream(self, method_call);
  } else if (strcmp(method, "stopImageStream") == 0) {
    handle_stop_image_stream(self, method_call);
  } else if (strcmp(method, "pausePreview") == 0) {
    handle_pause_preview(self, method_call);
  } else if (strcmp(method, "resumePreview") == 0) {
    handle_resume_preview(self, method_call);
  } else if (strcmp(method, "setMirror") == 0) {
    handle_set_mirror(self, method_call);
  } else if (strcmp(method, "dispose") == 0) {
    handle_dispose(self, method_call);
  } else {
    fl_method_call_respond_not_implemented(method_call, nullptr);
  }
}

static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  CameraDesktopPlugin* plugin = CAMERA_DESKTOP_PLUGIN(user_data);
  camera_desktop_plugin_handle_method_call(plugin, method_call);
}

static void camera_desktop_plugin_dispose(GObject* object) {
  CameraDesktopPlugin* self = CAMERA_DESKTOP_PLUGIN(object);

  // Dispose all cameras.
  if (self->data) {
    for (auto& pair : self->data->cameras) {
      camera_desktop_ffi_release_handles_for_camera(pair.second.get());
      pair.second->Dispose();
    }
    delete self->data;
    self->data = nullptr;
  }

  g_clear_object(&self->channel);

  G_OBJECT_CLASS(camera_desktop_plugin_parent_class)->dispose(object);
}

static void camera_desktop_plugin_class_init(CameraDesktopPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = camera_desktop_plugin_dispose;
}

static void camera_desktop_plugin_init(CameraDesktopPlugin* self) {
  self->data = new PluginData();
}

void camera_desktop_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  // Initialize GStreamer (safe to call multiple times).
  gst_init(nullptr, nullptr);

  CameraDesktopPlugin* plugin = CAMERA_DESKTOP_PLUGIN(
      g_object_new(camera_desktop_plugin_get_type(), nullptr));

  plugin->texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  plugin->channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "plugins.flutter.io/camera_desktop", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      plugin->channel, method_call_cb, g_object_ref(plugin), g_object_unref);

  g_object_unref(plugin);
}
