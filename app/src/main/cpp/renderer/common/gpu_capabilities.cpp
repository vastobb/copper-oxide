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
            if (lower.find("adreno 8") != std::string::npos) return GPUArchitecture::Adreno_800;
            if (lower.find("adreno 7") != std::string::npos) return GPUArchitecture::Adreno_700;
            if (lower.find("adreno 6") != std::string::npos) return GPUArchitecture::Adreno_600;
            break;
        }
        case GPUVendor::Mali: {
            if (lower.find("g715") != std::string::npos || lower.find("g615") != std::string::npos) return GPUArchitecture::Mali_G715;
            if (lower.find("valhall") != std::string::npos || lower.find("g71") != std::string::npos ||
                lower.find("g61") != std::string::npos || lower.find("g51") != std::string::npos) return GPUArchitecture::Mali_Valhall;
            if (lower.find("bifrost") != std::string::npos || lower.find("g72") != std::string::npos ||
                lower.find("g52") != std::string::npos || lower.find("g31") != std::string::npos) return GPUArchitecture::Mali_Bifrost;
            if (lower.find("midgard") != std::string::npos || lower.find("t8") != std::string::npos ||
                lower.find("t7") != std::string::npos) return GPUArchitecture::Mali_Midgard;
            break;
        }
        case GPUVendor::PowerVR: {
            if (lower.find("bxt") != std::string::npos || lower.find("bxm") != std::string::npos) return GPUArchitecture::PowerVR_BXM;
            if (lower.find("furian") != std::string::npos) return GPUArchitecture::PowerVR_Furian;
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
        // Check for specific Adreno driver versions with known issues
        if (info.architecture == GPUArchitecture::Adreno_600) {
            bugs.push_back("adreno_600_ubo_corruption");
            bugs.push_back("adreno_600_ssbo_alignment");
        }
        if (info.architecture == GPUArchitecture::Adreno_700) {
            bugs.push_back("adreno_700_subgroup_ops");
        }
    }

    // Mali-specific bugs
    if (info.vendor == GPUVendor::Mali) {
        if (info.architecture == GPUArchitecture::Mali_Midgard || info.architecture == GPUArchitecture::Mali_Bifrost) {
            bugs.push_back("mali_precision_mediump");
            bugs.push_back("mali_texture_border_clamp");
            bugs.push_back("mali_discard_framebuffer");
        }
        if (info.architecture == GPUArchitecture::Mali_Valhall) {
            bugs.push_back("mali_valhall_mesh_shader");
        }
        // Common Mali Zink issue
        bugs.push_back("mali_zink_pre_1_16_5");
    }

    // PowerVR-specific bugs
    if (info.vendor == GPUVendor::PowerVR) {
        bugs.push_back("powervr_discard_framebuffer");
        bugs.push_back("powervr_texture_swizzle");
        bugs.push_back("powervr_egl_image_external");
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
            if (info.architecture == GPUArchitecture::Adreno_700 || info.architecture == GPUArchitecture::Adreno_800) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::BindlessSamplers;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::BufferDeviceAddress;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::MaintenanceFeatures;
            } else if (info.architecture == GPUArchitecture::Adreno_600) {
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
            }
            break;
        }
        case GPUVendor::Mali: {
            if (info.architecture == GPUArchitecture::Mali_Valhall || info.architecture == GPUArchitecture::Mali_G715) {
                features = features | RendererFeature::DescriptorIndexing;
                features = features | RendererFeature::BindlessTextures;
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
                features = features | RendererFeature::Synchronization2;
                features = features | RendererFeature::MeshShaders;
                features = features | RendererFeature::TaskShaders;
            } else if (info.architecture == GPUArchitecture::Mali_Bifrost) {
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
            }
            break;
        }
        case GPUVendor::PowerVR: {
            if (info.architecture == GPUArchitecture::PowerVR_BXM || info.architecture == GPUArchitecture::PowerVR_Furian) {
                features = features | RendererFeature::SubgroupOperations;
                features = features | RendererFeature::TimelineSemaphore;
                features = features | RendererFeature::DynamicRendering;
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
            config.enable_multithreaded_rendering = true;
            config.enable_async_shader_compilation = true;
            config.enable_command_buffer_reuse = true;
            config.enable_pipeline_caching = true;
            config.max_frames_in_flight = 3;
            config.texture_cache_size_mb = 512;
            config.shader_cache_size_mb = 128;
            config.buffer_pool_size_mb = 256;
            break;
        }
        case GPUVendor::Mali: {
            config.enable_multithreaded_rendering = true;
            config.enable_async_shader_compilation = true;
            config.enable_command_buffer_reuse = true;
            config.enable_pipeline_caching = true;
            config.max_frames_in_flight = 2; // Mali prefers fewer frames in flight
            config.texture_cache_size_mb = 256;
            config.shader_cache_size_mb = 64;
            config.buffer_pool_size_mb = 128;
            break;
        }
        case GPUVendor::PowerVR: {
            config.enable_multithreaded_rendering = false; // TBDR prefers single-threaded
            config.enable_async_shader_compilation = true;
            config.enable_command_buffer_reuse = true;
            config.max_frames_in_flight = 2;
            config.texture_cache_size_mb = 128;
            config.shader_cache_size_mb = 64;
            config.buffer_pool_size_mb = 64;
            break;
        }
        default: {
            config.enable_multithreaded_rendering = true;
            config.max_frames_in_flight = 3;
            config.texture_cache_size_mb = 256;
            config.shader_cache_size_mb = 64;
            config.buffer_pool_size_mb = 128;
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