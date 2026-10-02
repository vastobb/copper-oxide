plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.kapt")
    id("com.diffplug.spotless") version "6.25.0"
    id("io.gitlab.arturbosch.detekt") version "1.23.6"
}

android {
    namespace = "com.oxide.mc.copperoxide"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.oxide.mc.copperoxide"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables.useSupportLibrary = true

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-fexceptions", "-frtti", "-Wall", "-Wextra", "-O3")
                arguments += listOf("-DANDROID_STL=c++_shared", "-DCMAKE_CXX_STANDARD=20", "-DCMAKE_CXX_STANDARD_REQUIRED=ON")
                // No abiFilters here on purpose. The AGP docs have this on the
                // cmake block, but it constrains the NATIVE BUILD while
                // ndk.abiFilters constrains PACKAGING, and setting only the
                // former is what left the debug APK with arm64 only - so the CI
                // emulator had no library to load. The ABI set is declared once,
                // on ndk.abiFilters below plus the debug build type.
            }
        }

        // Release ships arm64 only, which is every Android device Copper Oxide
        // targets. The debug build type adds x86_64 below for CI emulators.
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildFeatures {
        compose = true
        viewBinding = true
        buildConfig = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.11"
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packagingOptions {
        jniLibs {
            useLegacyPackaging = true
        }
        resources {
            excludes.addAll(listOf("META-INF/*"))
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            isDebuggable = false
        }
        debug {
            isMinifyEnabled = false
            isShrinkResources = false
            isDebuggable = true
            // x86_64 lets the debug APK run on CI emulators; release stays arm64.
            ndk {
                abiFilters += listOf("x86_64")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // Core Android
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")

    // Material3 & Compose
    implementation("androidx.compose.ui:ui:1.7.5")
    implementation("androidx.compose.material:material-icons-extended:1.7.5")
    implementation("androidx.compose.ui:ui-graphics:1.7.5")
    implementation("androidx.compose.ui:ui-tooling-preview:1.7.5")
    implementation("androidx.compose.material3:material3:1.3.1")
    implementation("androidx.compose.material3:material3-window-size-class:1.3.1")
    implementation("androidx.compose.runtime:runtime-livedata:1.7.5")
    implementation("androidx.compose.runtime:runtime-rxjava3:1.7.5")

    // Navigation
    implementation("androidx.navigation:navigation-compose:2.8.4")

    // Coroutines & Flow
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.1")

    // Serialization & Config
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.6.3")

    // Room Database for config/caching
    implementation("androidx.room:room-runtime:2.6.1")
    implementation("androidx.room:room-ktx:2.6.1")
    kapt("androidx.room:room-compiler:2.6.1")

    // DataStore for preferences
    implementation("androidx.datastore:datastore-preferences:1.1.1")

    // Logging
    implementation("com.jakewharton.timber:timber:5.0.1")

    // Metrics & Profiling
    implementation("androidx.metrics:metrics-performance:1.0.0-beta01")

    // Testing
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
    androidTestImplementation("androidx.compose.ui:ui-test-junit4:1.7.5")
    debugImplementation("androidx.compose.ui:ui-tooling:1.7.5")
    debugImplementation("androidx.compose.ui:ui-test-manifest:1.7.5")
}

spotless {
    kotlin {
        target("**/*.kt")
        ktlint("1.2.1")
    }
    kotlinGradle {
        target("*.gradle.kts")
        ktlint("1.2.1")
    }
}

detekt {
    toolVersion = "1.23.6"
    config = files("$rootDir/config/detekt/detekt.yml")
    buildUponDefaultConfig = true
    ignoreFailures = true
    reports {
        html.required.set(true)
        xml.required.set(true)
        txt.required.set(false)
        sarif.required.set(false)
    }
}

android {
    lint {
        abortOnError = false
        warningsAsErrors = false
    }
}