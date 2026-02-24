import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'linux_camera_platform_interface.dart';

/// An implementation of [LinuxCameraPlatform] that uses method channels.
class MethodChannelLinuxCamera extends LinuxCameraPlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('linux_camera');

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>(
      'getPlatformVersion',
    );
    return version;
  }
}
