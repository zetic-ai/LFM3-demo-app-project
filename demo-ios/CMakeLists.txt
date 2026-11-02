cmake_minimum_required(VERSION 3.24)
project(ZeticOmniDemo LANGUAGES Swift CXX OBJCXX)
option(DEMO_UI_PREVIEW "Build UI-only simulator preview with all model inference disabled" OFF)
if(DEMO_UI_PREVIEW)
  set(DEMO_BRIDGE Sources/PreviewBridge.mm)
  set(DEMO_UI_PREVIEW_LABEL "true")
else()
  find_package(LFM3IOS 0.1 CONFIG REQUIRED)
  set(DEMO_BRIDGE Sources/OmniBridge.mm)
  set(DEMO_UI_PREVIEW_LABEL "false")
endif()
set(DEMO_BUNDLE_ID "ai.zetic.omni.demo" CACHE STRING "App bundle identifier")
set(DEMO_MODEL_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ModelConfiguration.json" CACHE FILEPATH "Publisher-provided model release configuration")
configure_file("${DEMO_MODEL_CONFIG}" "${CMAKE_CURRENT_BINARY_DIR}/generated/ModelConfiguration.json" COPYONLY)
add_executable(ZeticOmniDemo MACOSX_BUNDLE
  Sources/OmniDemoApp.swift Sources/DemoView.swift Sources/DemoStore.swift Sources/BoundedMediaCopy.swift Sources/DemoPreset.swift Sources/ScenarioLaunch.swift Sources/ModelCatalog.swift
  ${DEMO_BRIDGE} Sources/OmniBridge.h Sources/DemoMetrics.hpp Sources/PromptContract.hpp
  Sources/ModelStore.mm Sources/ModelStore.h
  Sources/AudioInput.mm Sources/AudioInput.h
  Resources/Samples "${CMAKE_CURRENT_BINARY_DIR}/generated/ModelConfiguration.json"
  Resources/Assets.xcassets)
set_source_files_properties(${DEMO_BRIDGE} Sources/ModelStore.mm Sources/AudioInput.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
set_source_files_properties(Resources/Samples "${CMAKE_CURRENT_BINARY_DIR}/generated/ModelConfiguration.json" Resources/Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(ZeticOmniDemo PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${DEMO_BUNDLE_ID}"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${DEMO_BUNDLE_ID}"
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "18.0"
  XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
  XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Sources/OmniBridge.h"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE "NO"
  XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
  XCODE_ATTRIBUTE_ENABLE_BITCODE "NO")
target_compile_features(ZeticOmniDemo PRIVATE cxx_std_17)
target_link_libraries(ZeticOmniDemo PRIVATE "-framework UIKit" "-framework SwiftUI" "-framework AVFoundation"
  "-framework UniformTypeIdentifiers")
if(NOT DEMO_UI_PREVIEW)
  target_link_libraries(ZeticOmniDemo PRIVATE LFM3IOS::omni LFM3IOS::audio LFM3IOS::vision)
endif()
