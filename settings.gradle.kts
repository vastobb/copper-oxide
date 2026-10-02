pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
    plugins {
        id("com.android.application") version "8.5.0"
        id("org.jetbrains.kotlin.android") version "1.9.20"
        id("com.android.library") version "8.5.0"
        id("org.jetbrains.kotlin.multiplatform") version "1.9.20"
        id("com.diffplug.spotless") version "6.25.0"
    }
}

rootProject.name = "CopperOxide"
include(":app")