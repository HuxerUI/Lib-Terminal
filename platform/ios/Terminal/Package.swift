// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "Terminal",
    platforms: [
        .iOS(.v15),
    ],
    products: [
        .library(
            name: "Terminal",
            targets: ["Terminal"]
        ),
    ],
    targets: [
        .target(name: "Terminal"),
    ]
)
