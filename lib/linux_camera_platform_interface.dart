import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'linux_camera_method_channel.dart';

abstract class LinuxCameraPlatform extends PlatformInterface {
  /// Constructs a LinuxCameraPlatform.
  LinuxCameraPlatform() : super(token: _token);

  static final Object _token = Object();

  static LinuxCameraPlatform _instance = MethodChannelLinuxCamera();

  /// The default instance of [LinuxCameraPlatform] to use.
  ///
  /// Defaults to [MethodChannelLinuxCamera].
  static LinuxCameraPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [LinuxCameraPlatform] when
  /// they register themselves.
  static set instance(LinuxCameraPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }
}
