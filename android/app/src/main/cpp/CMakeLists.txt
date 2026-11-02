cmake_minimum_required(VERSION 3.22)
project(lfm3demo CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK (headers: android/sdk/include; library: jniLibs/arm64-v8a, packaged into the APK next to this one).
set(SDK_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../sdk/include)
set(SDK_LIB ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libzetic_mlange_hexagon.so)

add_library(zetic_mlange_hexagon SHARED IMPORTED)
set_target_properties(zetic_mlange_hexagon PROPERTIES
  IMPORTED_LOCATION ${SDK_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${SDK_INCLUDE})

add_library(lfm3demo SHARED lfm3_jni.cpp)
target_link_libraries(lfm3demo PRIVATE zetic_mlange_hexagon log)
