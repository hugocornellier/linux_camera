#include "include/linux_camera/linux_camera_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <sys/utsname.h>

#include <cstring>

#include "linux_camera_plugin_private.h"

#define LINUX_CAMERA_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), linux_camera_plugin_get_type(), \
                              LinuxCameraPlugin))

struct _LinuxCameraPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(LinuxCameraPlugin, linux_camera_plugin, g_object_get_type())

// Called when a method call is received from Flutter.
static void linux_camera_plugin_handle_method_call(
    LinuxCameraPlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getPlatformVersion") == 0) {
    response = get_platform_version();
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse* get_platform_version() {
  struct utsname uname_data = {};
  uname(&uname_data);
  g_autofree gchar *version = g_strdup_printf("Linux %s", uname_data.version);
  g_autoptr(FlValue) result = fl_value_new_string(version);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void linux_camera_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(linux_camera_plugin_parent_class)->dispose(object);
}

static void linux_camera_plugin_class_init(LinuxCameraPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = linux_camera_plugin_dispose;
}

static void linux_camera_plugin_init(LinuxCameraPlugin* self) {}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  LinuxCameraPlugin* plugin = LINUX_CAMERA_PLUGIN(user_data);
  linux_camera_plugin_handle_method_call(plugin, method_call);
}

void linux_camera_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  LinuxCameraPlugin* plugin = LINUX_CAMERA_PLUGIN(
      g_object_new(linux_camera_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "linux_camera",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
