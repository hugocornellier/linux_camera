#include "camera_desktop_plugin.h"

#include "include/camera_desktop/camera_desktop_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <mfapi.h>
#include <objbase.h>

#include <memory>
#include <string>

#include "device_enumerator.h"

// ---------------------------------------------------------------------------
// C export — called by generated_plugin_registrant.cc
// ---------------------------------------------------------------------------

void CameraDesktopPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  CameraDesktopPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------

// static
void CameraDesktopPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  // One-time Media Foundation startup (reference-counted internally).
  MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "plugins.flutter.io/camera_desktop",
      &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<CameraDesktopPlugin>(registrar,
                                                       std::move(channel));
  plugin->channel_->SetMethodCallHandler(
      [plugin_ptr = plugin.get()](const auto& call, auto result) {
        plugin_ptr->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

CameraDesktopPlugin::CameraDesktopPlugin(
    flutter::PluginRegistrarWindows* registrar,
    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel)
    : registrar_(registrar), channel_(std::move(channel)) {}

CameraDesktopPlugin::~CameraDesktopPlugin() {
  for (auto& [id, camera] : cameras_) {
    camera->Dispose();
  }
  cameras_.clear();
  MFShutdown();
}

// ---------------------------------------------------------------------------
// Method dispatch
// ---------------------------------------------------------------------------

void CameraDesktopPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string& method = call.method_name();
  const flutter::EncodableMap* args =
      std::get_if<flutter::EncodableMap>(call.arguments());
  const flutter::EncodableMap empty_args;
  const flutter::EncodableMap& safe_args = args ? *args : empty_args;

  if (method == "availableCameras") {
    HandleAvailableCameras(std::move(result));
  } else if (method == "create") {
    HandleCreate(safe_args, std::move(result));
  } else if (method == "initialize") {
    HandleInitialize(safe_args, std::move(result));
  } else if (method == "takePicture") {
    HandleTakePicture(safe_args, std::move(result));
  } else if (method == "startVideoRecording") {
    HandleStartVideoRecording(safe_args, std::move(result));
  } else if (method == "stopVideoRecording") {
    HandleStopVideoRecording(safe_args, std::move(result));
  } else if (method == "startImageStream") {
    HandleStartImageStream(safe_args, std::move(result));
  } else if (method == "stopImageStream") {
    HandleStopImageStream(safe_args, std::move(result));
  } else if (method == "pausePreview") {
    HandlePausePreview(safe_args, std::move(result));
  } else if (method == "resumePreview") {
    HandleResumePreview(safe_args, std::move(result));
  } else if (method == "dispose") {
    HandleDispose(safe_args, std::move(result));
  } else {
    result->NotImplemented();
  }
}

// ---------------------------------------------------------------------------
// Individual handlers
// ---------------------------------------------------------------------------

void CameraDesktopPlugin::HandleAvailableCameras(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  auto devices = DeviceEnumerator::EnumerateVideoDevices();

  flutter::EncodableList list;
  for (const auto& device : devices) {
    // Convert wide strings to UTF-8.
    auto to_utf8 = [](const std::wstring& w) -> std::string {
      if (w.empty()) return {};
      int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
      std::string s(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), size,
                          nullptr, nullptr);
      return s;
    };

    // Name format: "Friendly Name (symbolic_link)" — same as Linux/macOS.
    std::string display_name =
        to_utf8(device.friendly_name) + " (" + to_utf8(device.symbolic_link) + ")";

    list.push_back(flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("name"),
         flutter::EncodableValue(display_name)},
        {flutter::EncodableValue("lensDirection"),
         flutter::EncodableValue(0)},  // front-facing (same as camera_windows)
        {flutter::EncodableValue("sensorOrientation"),
         flutter::EncodableValue(0)},
    }));
  }

  result->Success(flutter::EncodableValue(list));
}

void CameraDesktopPlugin::HandleCreate(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string* camera_name =
      std::get_if<std::string>(&args.at(flutter::EncodableValue("cameraName")));
  if (!camera_name) {
    result->Error("invalid_args", "cameraName is required");
    return;
  }

  const int* resolution_preset_ptr =
      std::get_if<int>(&args.at(flutter::EncodableValue("resolutionPreset")));
  int resolution_preset = resolution_preset_ptr ? *resolution_preset_ptr : 4;

  const bool* enable_audio_ptr = nullptr;
  auto audio_it = args.find(flutter::EncodableValue("enableAudio"));
  if (audio_it != args.end()) {
    enable_audio_ptr = std::get_if<bool>(&audio_it->second);
  }
  bool enable_audio = enable_audio_ptr ? *enable_audio_ptr : false;

  std::wstring symbolic_link = DeviceEnumerator::FindSymbolicLink(*camera_name);
  if (symbolic_link.empty()) {
    result->Error("camera_not_found",
                  "Could not find camera: " + *camera_name);
    return;
  }

  CameraConfig config;
  config.symbolic_link = symbolic_link;
  config.resolution_preset = resolution_preset;
  config.enable_audio = enable_audio;
  config.target_fps = 30;

  int camera_id = next_camera_id_++;
  auto camera = std::make_unique<Camera>(
      camera_id,
      registrar_->texture_registrar(),
      channel_.get(),
      config);

  int64_t texture_id = camera->RegisterTexture();
  if (texture_id < 0) {
    result->Error("texture_registration_failed",
                  "Failed to register Flutter texture");
    return;
  }

  cameras_[camera_id] = std::move(camera);

  result->Success(flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("cameraId"),
       flutter::EncodableValue(camera_id)},
      {flutter::EncodableValue("textureId"),
       flutter::EncodableValue(static_cast<int64_t>(texture_id))},
  }));
}

Camera* CameraDesktopPlugin::FindCamera(
    const flutter::EncodableMap& args,
    flutter::MethodResult<flutter::EncodableValue>* result) {
  auto it = args.find(flutter::EncodableValue("cameraId"));
  if (it == args.end()) {
    result->Error("invalid_args", "cameraId is required");
    return nullptr;
  }
  int camera_id = std::get<int>(it->second);
  auto cam_it = cameras_.find(camera_id);
  if (cam_it == cameras_.end()) {
    result->Error("camera_not_found", "No camera with id " +
                                          std::to_string(camera_id));
    return nullptr;
  }
  return cam_it->second.get();
}

void CameraDesktopPlugin::HandleInitialize(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->Initialize(std::move(result));
}

void CameraDesktopPlugin::HandleTakePicture(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->TakePicture(std::move(result));
}

void CameraDesktopPlugin::HandleStartVideoRecording(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->StartVideoRecording(std::move(result));
}

void CameraDesktopPlugin::HandleStopVideoRecording(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->StopVideoRecording(std::move(result));
}

void CameraDesktopPlugin::HandleStartImageStream(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->StartImageStream();
  result->Success(flutter::EncodableValue(nullptr));
}

void CameraDesktopPlugin::HandleStopImageStream(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->StopImageStream();
  result->Success(flutter::EncodableValue(nullptr));
}

void CameraDesktopPlugin::HandlePausePreview(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->PausePreview();
  result->Success(flutter::EncodableValue(nullptr));
}

void CameraDesktopPlugin::HandleResumePreview(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  Camera* camera = FindCamera(args, result.get());
  if (!camera) return;
  camera->ResumePreview();
  result->Success(flutter::EncodableValue(nullptr));
}

void CameraDesktopPlugin::HandleDispose(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  auto it = args.find(flutter::EncodableValue("cameraId"));
  if (it != args.end()) {
    int camera_id = std::get<int>(it->second);
    auto cam_it = cameras_.find(camera_id);
    if (cam_it != cameras_.end()) {
      cam_it->second->Dispose();
      cameras_.erase(cam_it);
    }
  }
  result->Success(flutter::EncodableValue(nullptr));
}
