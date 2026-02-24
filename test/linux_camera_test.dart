import 'package:flutter_test/flutter_test.dart';
import 'package:linux_camera/linux_camera.dart';
import 'package:linux_camera/linux_camera_platform_interface.dart';
import 'package:linux_camera/linux_camera_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockLinuxCameraPlatform
    with MockPlatformInterfaceMixin
    implements LinuxCameraPlatform {
  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final LinuxCameraPlatform initialPlatform = LinuxCameraPlatform.instance;

  test('$MethodChannelLinuxCamera is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelLinuxCamera>());
  });

  test('getPlatformVersion', () async {
    LinuxCamera linuxCameraPlugin = LinuxCamera();
    MockLinuxCameraPlatform fakePlatform = MockLinuxCameraPlatform();
    LinuxCameraPlatform.instance = fakePlatform;

    expect(await linuxCameraPlugin.getPlatformVersion(), '42');
  });
}
