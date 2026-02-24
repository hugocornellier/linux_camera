
import 'linux_camera_platform_interface.dart';

class LinuxCamera {
  Future<String?> getPlatformVersion() {
    return LinuxCameraPlatform.instance.getPlatformVersion();
  }
}
