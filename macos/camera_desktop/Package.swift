// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "camera_desktop",
    platforms: [
        .macOS("10.15")
    ],
    products: [
        .library(name: "camera-desktop", targets: ["camera_desktop"])
    ],
    dependencies: [],
    targets: [
        .target(
            name: "camera_desktop",
            dependencies: [],
            resources: [
                .process("PrivacyInfo.xcprivacy"),
            ]
        )
    ]
)
