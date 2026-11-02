plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "ai.zetic.lfm3demo"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "ai.zetic.lfm3demo"
        minSdk = 31
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            // ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES: the JNI library is linked 16 KB page-size aligned (NDK r27)
            cmake { arguments += listOf("-DANDROID_STL=c++_shared", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON") }
        }
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }

    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt") }
    }

    // The SDK loads the Hexagon DSP library (libhexrt_v*_skel.so) as a file from the app's native library
    // directory, so the libraries must be extracted at install time, not mapped from the APK.
    packaging {
        jniLibs {
            useLegacyPackaging = true
            pickFirsts += "**/libc++_shared.so"   // the SDK ships its own copy; either one works
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    // The sample scenarios are shared with the IQ9 CLI: one copy in <repo>/samples, packaged as assets.
    sourceSets["main"].assets.srcDir("../../samples")
}
