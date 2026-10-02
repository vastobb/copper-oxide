// Top-level build file for Copper Oxide
plugins {
    id("com.android.application") version "8.5.0" apply false
    id("org.jetbrains.kotlin.android") version "1.9.20" apply false
    id("org.jetbrains.kotlin.kapt") version "1.9.20" apply false
    id("com.android.library") version "8.5.0" apply false
    id("dev.zacsweers.spotless") version "6.25.0" apply false
    id("io.gitlab.arturbosch.detekt") version "1.23.6" apply false
    kotlin("multiplatform") version "1.9.20" apply false
}

allprojects {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
        maven { url = uri("https://maven.pkg.jetbrains.space/public/p/compose/dev") }
    }
}

tasks.register("clean", Delete::class) {
    delete(rootProject.buildDir)
}