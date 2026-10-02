# ProGuard rules for Copper Oxide Renderer

# Keep native method names
-keepclasseswithmembers class * {
    native <methods>;
}

# Keep JNI bridge classes
-keep class com.oxide.mc.copperoxide.renderer.CopperOxideRenderer {
    *;
}

# Keep renderer config and data classes
-keep class com.oxide.mc.copperoxide.renderer.RendererConfig { *; }
-keep class com.oxide.mc.copperoxide.renderer.FrameStats { *; }
-keep class com.oxide.mc.copperoxide.renderer.GpuInfo { *; }
-keep class com.oxide.mc.copperoxide.renderer.RendererBackend { *; }
-keep class com.oxide.mc.copperoxide.renderer.RendererFeature { *; }
-keep class com.oxide.mc.copperoxide.renderer.GpuVendor { *; }
-keep class com.oxide.mc.copperoxide.renderer.GpuArchitecture { *; }

# Keep GUI classes
-keep class com.oxide.mc.copperoxide.gui.** { *; }

# Keep theme classes
-keep class com.oxide.mc.copperoxide.ui.theme.** { *; }

# Keep Kotlin serialization
-keep class kotlinx.serialization.** { *; }
-keep @kotlinx.serialization.Serializable class * { *; }

# Keep Room database
-keep class androidx.room.** { *; }
-keep @androidx.room.Entity class * { *; }
-keep @androidx.room.Dao interface * { *; }
-keep @androidx.room.Database class * { *; }

# Keep DataStore
-keep class androidx.datastore.** { *; }

# Keep Coroutines
-keep class kotlinx.coroutines.** { *; }

# Keep Compose runtime
-keep class androidx.compose.runtime.** { *; }

# Keep Material3
-keep class androidx.compose.material3.** { *; }

# Keep Navigation
-keep class androidx.navigation.** { *; }

# Keep Metrics
-keep class androidx.metrics.** { *; }

# Keep Timber
-keep class com.jakewharton.timber.** { *; }

# Don't obfuscate native library
-keep class ** { native <methods>; }

# Preserve annotations
-keepattributes *Annotation*,EnclosingMethod,Signature,InnerClasses

# Remove logging in release
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
    public static int w(...);
}

# Keep JNI classes
-keep class com.oxide.mc.copperoxide.** { *; }

# Optimization
-optimizationpasses 5
-allowaccessmodification
-mergeinterfacesaggressively
-overloadaggressively