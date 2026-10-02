#include "renderer_base.h"
#include <algorithm>
#include <sstream>
#include <regex>

namespace copper {

GPUInfo detect_gpu_info() {
    GPUInfo info;
    // This will be populated by platform-specific code
    // For now, return defaults - actual detection happens in platform/android
    return info;
}

GPUVendor parse_gpu_vendor(const std::string& renderer_string, const std::string& vendor_string) {
    std::string combined = renderer_string + " " + vendor_string;
    std::transform(combined.begin(), combined.end(), combined.begin(), ::tolower);

    if (combined.find("adreno") != std::string::npos) return GPUVendor::Adreno;
    if (combined.find("mali") != std::string::npos) return GPUVendor::Mali;
    if (combined.find("powervr") != std::string::npos || combined.find("img") != std::string::npos) return GPUVendor::PowerVR;
    if (combined.find("apple") != std::string::npos) return GPUVendor::Apple;
    if (combined.find("nvidia") != std::string::npos) return GPUVendor::NVIDIA;
    if (combined.find("amd") != std::string::npos || combined.find("radeon") != std::string::npos) return GPUVendor::AMD;
    if (combined.find("intel") != std::string::npos) return GPUVendor::Intel;
    if (combined.find("broadcom") != std::string::npos || combined.find("videocore") != std::string::npos) return GPUVendor::Broadcom;
    if (combined.find("vivante") != std::string::npos) return GPUVendor::Vivante;
    if (combined.find("verisilicon") != std::string::npos) return GPUVendor::VeriSilicon;

    return GPUVendor::Unknown;
}

GPUArchitecture parse_gpu_architecture(GPUVendor vendor, const std::string& renderer_string) {
    std::string lower = renderer_string;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    switch (vendor) {
        case GPUVendor::Adreno: {
            // Snapdragon 8 Gen 1/2/3 - Adreno 730/740/750
            if (lower.find("adreno 750") != std::string::npos || lower.find("adreno 740") != std::string::npos ||
                lower.find("adreno 730") != std::string::npos || lower.find("adreno 725") != std::string::npos) return GPUArchitecture::Adreno_700;
            // Snapdragon 8 Gen 4 - Adreno 830
            if (lower.find("adreno 830") != std::string::npos || lower.find("adreno 820") != std::string::npos ||
                lower.find("adreno 8") != std::string::npos) return GPUArchitecture::Adreno_800;
            // Snapdragon 7/8 series - Adreno 640/650/660/680
            if (lower.find("adreno 6") != std::string::npos) return GPUArchitecture::Adreno_600;
            break;
        }
        case GPUVendor::Mali: {
            // Mali-G715 (Immortalis-G715) - latest flagship
            if (lower.find("g715") != std::string::npos || lower.find("g615") != std::string::npos ||
                lower.find("immortalis-g715") != std::string::npos) return GPUArchitecture::Mali_G715;
            // Mali-G710/G610 - Valhall
            if (lower.find("valhall") != std::string::npos || lower.find("g710") != std::string::npos ||
                lower.find("g610") != std::string::npos || lower.find("g71") != std::string::npos ||
                lower.find("g61") != std::string::npos || lower.find("g51") != std::string::npos) return GPUArchitecture::Mali_Valhall;
            // Mali-G78/G77/G76/G57 - Bifrost
            if (lower.find("bifrost") != std::string::npos || lower.find("g78") != std::string::npos ||
                lower.find("g77") != std::string::npos || lower.find("g76") != std::string::npos ||
                lower.find("g57") != std::string::npos || lower.find("g52") != std::string::npos ||
                lower.find("g31") != std::string::npos) return GPUArchitecture::Mali_Bifrost;
            // Mali-T880/T860/T760 - Midgard
            if (lower.find("midgard") != std::string::npos || lower.find("t880") != std::string::npos ||
                lower.find("t860") != std::string::npos || lower.find("t760") != std::string::npos ||
                lower.find("t7") != std::string::npos || lower.find("t8") != std::string::npos) return GPUArchitecture::Mali_Midgard;
            break;
        }
        case GPUVendor::PowerVR: {
            if (lower.find("bxt") != std::string::npos || lower.find("bxm") != std::string::npos ||
                lower.find("bxm-4-64") != std::string::npos) return GPUArchitecture::PowerVR_BXM;
            if (lower.find("furian") != std::string::npos || lower.find("axt") != std::string::npos) return GPUArchitecture::PowerVR_Furian;
            return GPUArchitecture::PowerVR_Rogue;
        }
        default:
            break;
    }
    return GPUArchitecture::Unknown;
}

std::vector<std::string> parse_extensions(const std::string& extensions_string) {
    std::vector<std::string> extensions;
    std::istringstream iss(extensions_string);
    std::string ext;
    while (iss >> ext) {
        extensions.push_back(ext);
    }
    return extensions;
}

std::vector<std::string> detect_driver_bugs(const GPUInfo& info) {
    std::vector<std::string> bugs;

    // Adreno-specific bugs
    if (info.vendor == GPUVendor::Adreno) {
        if (info.architecture == GPUArchitecture::Adreno_600) {
            bugs.push_back("adreno_600_ubo_corruption");
            bugs.push_back("adreno_600_ssbo_alignment");
            bugs.push_back("adreno_600_vertex_attribute_alias");
            bugs.push_back("adreno_600_shader_discard");
        }
        if (info.architecture == GPUArchitecture::Adreno_700) {
            bugs.push_back("adreno_700_subgroup_ops");
            bugs.push_back("adreno_700_descriptor_indexing_uniform");
            bugs.push_back("adreno_700_protected_memory");
        }
        if (info.architecture == GPUArchitecture::Adreno_800) {
            bugs.push_back("adreno_800_mesh_shader_early");
        }
    }

    // Mali-specific bugs
    if (info.vendor == GPUVendor::Mali) {
        if (info.architecture == GPUArchitecture::Mali_Midgard || info.architecture == GPUArchitecture::Mali_Bifrost) {
            bugs.push_back("mali_precision_mediump");
            bugs.push_back("mali_texture_border_clamp");
            bugs.push_back("mali_discard_framebuffer");
            bugs.push_back("mali_uniform_buffer_offset");
            bugs.push_back("mali_base_vertex");
        }
        if (info.architecture == GPUArchitecture::Mali_Bifrost) {
            bugs.push_back("mali_bifrost_storage_buffer_atomic");
            bugs.push_back("mali_bifrost_ray_query");
        }
        if (info.architecture == GPUArchitecture::Mali_Valhall) {
            bugs.push_back("mali_valhall_mesh_shader");
            bugs.push_back("mali_valhall_descriptor_indexing");
        }
        if (info.architecture == GPUArchitecture::Mali_G715) {
            bugs.push_back("mali_g715_ray_tracing_early");
        }
        // Common Mali Zink issue
        bugs.push_back("mali_zink_pre_1_16_5");
    }

    // PowerVR-specific bugs
    if (info.vendor == GPUVendor::PowerVR) {
        bugs.push_back("powervr_discard_framebuffer");
        bugs.push_back("powervr_texture_swizzle");
        bugs.push_back("powervr_egl_image_external");
        bugs.push_back("powervr_usc_fence");
    }

    return bugs;
}

RendererFeature get_recommended_features(const GPUInfo& info) {
    RendererFeature features = RendererFeature::None;

    // Base features available on GLES 3.1+ / Vulkan 1.0+
    features = features | RendererFeature::ComputeShaders;
    features = features | RendererFeature::IndirectDraw;
    features = features | RendererFeature::MultiDrawIndirect;
    features = features | RendererFeature::ShaderDrawParameters;

    // Vendor-specific features
    switch (info.vendor) {
        case GPUVendor::Adreno: {
            // Adreno 600 series (Snapdragon 845/855/765/778)
            if (info.architecture == GPUArchitecture::Adreno_600) {
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DescriptorIndexing; // Limited
                features = features | RendererFeature::MaintenanceFeatures;
            }
            // Adreno 700 series (Snapdragon 8 Gen 1/2/3, 7+ Gen 1/2/3)
            else if (info.architecture == GPUArchitecture::Adreno_700) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::BindlessSamplers;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::BufferDeviceAddress;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::MaintenanceFeatures;
                features = features | RendererFeature::IndirectDraw;
                features = features | RendererFeature::MultiDrawIndirect;
                features = features | RendererFeature::DrawIndirectCount;
                features = features | RendererFeature::HostQueryReset;
                features = features | RendererFeature::ImagelessFramebuffer;
            }
            // Adreno 800 series (Snapdragon 8 Gen 4)
            else if (info.architecture == GPUArchitecture::Adreno_800) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::BindlessSamplers;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::BufferDeviceAddress;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::MaintenanceFeatures;
                features = features | RendererFeature::IndirectDraw;
                features = features | RendererFeature::MultiDrawIndirect;
                features = features | RendererFeature::DrawIndirectCount;
                features = features | RendererFeature::HostQueryReset;
                features = features | RendererFeature::ImagelessFramebuffer;
                features = features | RendererFeature::MeshShaders;
                features = features | RendererFeature::TaskShaders;
                features = features | RendererFeature::RayTracing;
                features = features | RendererFeature::VariableRateShading;
            }
            break;
        }
        case GPUVendor::Mali: {
            // Mali Midgard (T880/T860/T760) - older
            if (info.architecture == GPUArchitecture::Mali_Midgard) {
                features = features | RendererFeature::SubgroupOperations; // Limited
                features = features | RendererFeature::TimelineSemaphore;
            }
            // Mali Bifrost (G78/G77/G76/G57/G52/G31)
            else if (info.architecture == GPUArchitecture::Mali_Bifrost) {
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DescriptorIndexing; // Basic
                features = features | RendererFeature::MaintenanceFeatures;
            }
            // Mali Valhall (G71/G61/G51/G710/G610)
            else if (info.architecture == GPUArchitecture::Mali_Valhall) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::BindlessSamplers;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::BufferDeviceAddress;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::MaintenanceFeatures;
                features = features | RendererFeature::MeshShaders;
                features = features | RendererFeature::TaskShaders;
            }
            // Mali G715/Immortalis-G715 (Immortalis)
            else if (info.architecture == GPUArchitecture::Mali_G715) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::BindlessSamplers;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::BufferDeviceAddress;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::MaintenanceFeatures;
                features = features | RendererFeature::MeshShaders;
                features = features | RendererFeature::TaskShaders;
                features = features | RendererFeature::RayTracing;
                features = features | RendererFeature::VariableRateShading;
                features = features | RendererFeature::ConservativeRasterization;
                features = features | RendererFeature::DepthBoundsTest;
                features = features | RendererFeature::FragmentStoresAndAtomics;
                features = features | RendererFeature::ImageWriteWithoutFormat;
            }
            break;
        }
        case GPUVendor::PowerVR: {
            // PowerVR Rogue (Series 8/9)
            if (info.architecture == GPUArchitecture::PowerVR_Rogue) {
                features = features | RendererFeature::SubgroupOperations; // Limited
                features = features | RendererFeature::TimelineSemaphore;
            }
            // PowerVR Furian (Series 10)
            else if (info.architecture == GPUArchitecture::PowerVR_Furian) {
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::DescriptorIndexing; // Basic
            }
            // PowerVR BXM (Series 11 - latest)
            else if (info.architecture == GPUArchitecture::PowerVR_BXM) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::MaintenanceFeatures;
                features = features | RendererFeature::MeshShaders;
                features = features | RendererFeature::TaskShaders;
            }
            break;
        }
        default:
            break;
    }

    // Vulkan version features
    if (info.vulkan_version >= 0x00010002) { // 1.2
        features = features | RendererFeature::TimelineSemaphore;
        features = features | RendererFeature::BufferDeviceAddress;
    }
    if (info.vulkan_version >= 0x00010003) { // 1.3
        features = features | RendererFeature::Synchronization2;
        features = features | RendererFeature::DynamicRendering;
        features = features | RendererFeature::MaintenanceFeatures;
    }

    return features;
}

RendererConfig create_optimal_config(const GPUInfo& info) {
    RendererConfig config;
    config.preferred_backend = info.supports_vulkan ? RendererBackend::Vulkan : RendererBackend::OpenGLES;
    config.required_features = get_recommended_features(info);
    config.optional_features = RendererFeature::All;

    // Vendor-specific optimizations
    switch (info.vendor) {
        case GPUVendor::Adreno: {
            // Snapdragon 8+ (Adreno 700/800) - high performance
            if (info.architecture == GPUArchitecture::Adreno_700 || info.architecture == GPUArchitecture::Adreno_800) {
                config.enable_multithreaded_rendering = true;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.enable_pipeline_caching = true;
                config.enable_descriptor_caching = true;
                config.enable_draw_call_batching = true;
                config.max_frames_in_flight = 3;
                config.max_command_buffers_per_frame = 32;
                config.max_descriptor_sets = 16384;
                config.max_push_constants_size = 256;
                config.texture_cache_size_mb = 1024;
                config.shader_cache_size_mb = 256;
                config.buffer_pool_size_mb = 512;
                config.frame_timeout_ms = 5000;
                config.vsync_enabled = true;
                config.target_fps = 120; // High refresh rate displays
                config.low_latency_mode = true;
                config.battery_saver_mode = false;
                config.thermal_throttling_aware = true;
                config.thermal_throttle_threshold = 0.9f;
            }
            // Snapdragon 7/8 series (Adreno 600) - mid to high
            else if (info.architecture == GPUArchitecture::Adreno_600) {
                config.enable_multithreaded_rendering = true;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.enable_pipeline_caching = true;
                config.max_frames_in_flight = 3;
                config.max_command_buffers_per_frame = 16;
                config.max_descriptor_sets = 8192;
                config.max_push_constants_size = 256;
                config.texture_cache_size_mb = 512;
                config.shader_cache_size_mb = 128;
                config.buffer_pool_size_mb = 256;
                config.target_fps = 90;
                config.low_latency_mode = true;
                config.thermal_throttle_threshold = 0.85f;
            }
            break;
        }
        case GPUVendor::Mali: {
            // Mali G715/Immortalis-G715 - flagship
            if (info.architecture == GPUArchitecture::Mali_G715) {
                config.enable_multithreaded_rendering = true;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.enable_pipeline_caching = true;
                config.enable_descriptor_caching = true;
                config.enable_draw_call_batching = true;
                config.max_frames_in_flight = 2; // Mali prefers 2
                config.max_command_buffers_per_frame = 16;
                config.max_descriptor_sets = 8192;
                config.max_push_constants_size = 256;
                config.texture_cache_size_mb = 512;
                config.shader_cache_size_mb = 128;
                config.buffer_pool_size_mb = 256;
                config.target_fps = 120;
                config.low_latency_mode = true;
                config.thermal_throttle_threshold = 0.85f;
            }
            // Mali Valhall (G71/G61/G710/G610) - high-end
            else if (info.architecture == GPUArchitecture::Mali_Valhall) {
                config.enable_multithreaded_rendering = true;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.enable_pipeline_caching = true;
                config.max_frames_in_flight = 2;
                config.max_command_buffers_per_frame = 16;
                config.max_descriptor_sets = 8192;
                config.max_push_constants_size = 256;
                config.texture_cache_size_mb = 512;
                config.shader_cache_size_mb = 128;
                config.buffer_pool_size_mb = 256;
                config.target_fps = 90;
                config.low_latency_mode = true;
                config.thermal_throttle_threshold = 0.8f;
            }
            // Mali Bifrost (G78/G77/G76/G57) - mid-range
            else if (info.architecture == GPUArchitecture::Mali_Bifrost) {
                config.enable_multithreaded_rendering = true;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.enable_pipeline_caching = true;
                config.max_frames_in_flight = 2;
                config.max_command_buffers_per_frame = 8;
                config.max_descriptor_sets = 4096;
                config.texture_cache_size_mb = 256;
                config.shader_cache_size_mb = 64;
                config.buffer_pool_size_mb = 128;
                config.target_fps = 60;
                config.thermal_throttle_threshold = 0.75f;
            }
            // Mali Midgard - older
            else {
                config.enable_multithreaded_rendering = false;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.max_frames_in_flight = 2;
                config.texture_cache_size_mb = 128;
                config.shader_cache_size_mb = 64;
                config.buffer_pool_size_mb = 64;
                config.target_fps = 60;
                config.thermal_throttle_threshold = 0.7f;
            }
            break;
        }
        case GPUVendor::PowerVR: {
            // PowerVR BXM - latest
            if (info.architecture == GPUArchitecture::PowerVR_BXM) {
                config.enable_multithreaded_rendering = false; // TBDR
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.enable_pipeline_caching = true;
                config.max_frames_in_flight = 2;
                config.texture_cache_size_mb = 256;
                config.shader_cache_size_mb = 128;
                config.buffer_pool_size_mb = 128;
                config.target_fps = 90;
                config.low_latency_mode = true;
                config.thermal_throttle_threshold = 0.85f;
            }
            // PowerVR Furian
            else if (info.architecture == GPUArchitecture::PowerVR_Furian) {
                config.enable_multithreaded_rendering = false;
                config.enable_async_shader_compilation = true;
                config.enable_command_buffer_reuse = true;
                config.max_frames_in_flight = 2;
                config.texture_cache_size_mb = 128;
                config.shader_cache_size_mb = 64;
                config.buffer_pool_size_mb = 64;
                config.target_fps = 60;
                config.thermal_throttle_threshold = 0.8f;
            }
            // PowerVR Rogue - older
            else {
                config.enable_multithreaded_rendering = false;
                config.max_frames_in_flight = 2;
                config.texture_cache_size_mb = 128;
                config.shader_cache_size_mb = 64;
                config.buffer_pool_size_mb = 64;
                config.target_fps = 60;
                config.thermal_throttle_threshold = 0.75f;
            }
            break;
        }
        default: {
            config.enable_multithreaded_rendering = true;
            config.max_frames_in_flight = 3;
            config.texture_cache_size_mb = 256;
            config.shader_cache_size_mb = 64;
            config.buffer_pool_size_mb = 128;
            config.target_fps = 60;
            break;
        }
    }

    // Thermal-aware settings
    if (info.is_tiled_renderer) {
        config.battery_saver_mode = false; // TBDR is inherently power efficient
    }

    return config;
}

} // namespace copper