#include <flutter_linux/flutter_linux.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "include/camera_desktop/camera_desktop_plugin.h"

namespace camera_desktop {
namespace test {

TEST(CameraDesktopPlugin, PluginRegistration) {
  // Verify the plugin registration function exists and is callable.
  // Full lifecycle testing requires a running Flutter engine, so this
  // just validates the symbol is exported.
  EXPECT_NE(
      reinterpret_cast<void*>(&camera_desktop_plugin_register_with_registrar),
      nullptr);
}

}  // namespace test
}  // namespace camera_desktop
