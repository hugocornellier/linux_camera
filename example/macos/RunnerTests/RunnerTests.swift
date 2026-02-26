import Cocoa
import FlutterMacOS
import XCTest
@testable import camera_desktop

class RunnerTests: XCTestCase {

  func testDeviceEnumeration() {
    // Should return a list (may be empty on CI/headless machines).
    let devices = DeviceEnumerator.enumerateDevices()
    XCTAssertNotNil(devices)
  }

  func testDeviceIdExtraction() {
    let name = "FaceTime HD Camera (0x1234567890)"
    let deviceId = DeviceEnumerator.extractDeviceId(from: name)
    XCTAssertEqual(deviceId, "0x1234567890")
  }

  func testDeviceIdExtractionNoParens() {
    let name = "NoParen"
    let deviceId = DeviceEnumerator.extractDeviceId(from: name)
    XCTAssertNil(deviceId)
  }

  func testDeviceIdExtractionNestedParens() {
    let name = "Camera (Model X) (ABC123)"
    let deviceId = DeviceEnumerator.extractDeviceId(from: name)
    XCTAssertEqual(deviceId, "ABC123")
  }

  func testSessionPresetMapping() {
    XCTAssertEqual(DeviceEnumerator.sessionPreset(for: 0), .low)
    XCTAssertEqual(DeviceEnumerator.sessionPreset(for: 1), .medium)
    XCTAssertEqual(DeviceEnumerator.sessionPreset(for: 2), .high)
    XCTAssertEqual(DeviceEnumerator.sessionPreset(for: 3), .hd1280x720)
    XCTAssertEqual(DeviceEnumerator.sessionPreset(for: 4), .hd1920x1080)
    XCTAssertEqual(DeviceEnumerator.sessionPreset(for: 5), .hd1920x1080)
  }

  func testPhotoPathGeneration() {
    let path = PhotoHandler.generatePath(cameraId: 42)
    XCTAssertTrue(path.contains("camera_desktop_42_"))
    XCTAssertTrue(path.hasSuffix(".jpg"))
  }

  func testRecordPathGeneration() {
    let path = RecordHandler.generatePath()
    XCTAssertTrue(path.contains("camera_desktop_video_"))
    XCTAssertTrue(path.hasSuffix(".mp4"))
  }
}
