import 'package:flutter_test/flutter_test.dart';
import 'package:camera_desktop_example/main.dart';

void main() {
  testWidgets('CameraExampleApp builds', (WidgetTester tester) async {
    await tester.pumpWidget(const CameraExampleApp());
    expect(find.text('Camera Desktop Example'), findsOneWidget);
  });
}
