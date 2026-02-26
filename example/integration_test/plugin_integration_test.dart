import 'package:camera_platform_interface/camera_platform_interface.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:camera_desktop/camera_desktop.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('CameraDesktopPlugin is registered as CameraPlatform', (
    WidgetTester tester,
  ) async {
    // On Linux, CameraDesktopPlugin should be auto-registered via registerWith().
    expect(CameraPlatform.instance, isA<CameraDesktopPlugin>());
  });

  testWidgets('availableCameras returns a list', (WidgetTester tester) async {
    final cameras = await CameraPlatform.instance.availableCameras();
    // On a machine with no cameras this may be empty, but it shouldn't throw.
    expect(cameras, isA<List<CameraDescription>>());
  });
}
