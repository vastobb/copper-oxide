#include "gpu_capabilities.h"
#include "renderer_base.h"
#include "renderer_config.h"

#include <vulkan/vulkan.h>
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cstring>

namespace copper {

GPUCapabilities::GPUCapabilities() = default;
GPUCapabilities::~GPUCapabilities() = default;

bool GPUCapabilities::detect() {
    // Idempotent: a second detection must not duplicate state.
    supported_extensions_.clear();
    driver_bugs_.clear();
    supported_features_ = RendererFeature::None;
    vendor_ = GPUVendor::Unknown;
    architecture_ = GPUArchitecture::Unknown;

    // Try Vulkan first
    queryVulkanProperties();
    
    // If Vulkan not available, try GLES
    if (vendor_ == GPUVendor::Unknown) {
        queryGLESProperties();
    }
    
    // Apply GPU-specific workarounds and optimizations
    optimization_config_ = buildOptimizationConfig();
    
    return vendor_ != GPUVendor::Unknown;
}

void GPUCapabilities::applyWorkarounds(const RendererConfig& config) const {
    // This would apply specific workarounds based on detected GPU
    // For now, just set optimization config based on vendor/architecture
}

void GPUCapabilities::setDetectedInfo(GPUVendor vendor, GPUArchitecture arch,
                                       const std::vector<std::string>& extensions,
                                       RendererFeature features) {
    vendor_ = vendor;
    architecture_ = arch;
    supported_extensions_ = extensions;
    supported_features_ = features;
    optimization_config_ = buildOptimizationConfig();
}

GPUCapabilities::GPUOptimizationConfig GPUCapabilities::buildOptimizationConfig() const {
    GPUOptimizationConfig opt;

    // Architecture comparisons must be explicit: the enum values are sparse
    // (1-3, 10-13, 20-22), so ordinal comparison gives wrong answers across
    // vendor families.
    switch (vendor_) {
        case GPUVendor::Adreno: {
            const bool is_700_or_newer = architecture_ == GPUArchitecture::Adreno_700 ||
                                         architecture_ == GPUArchitecture::Adreno_800;
            opt.use_ubwc = true;
            opt.use_image_compression = true;
            opt.prefer_compute_shaders = true;
            opt.prefer_indirect_draw = true;
            opt.use_descriptor_indexing = true;
            opt.use_timeline_semaphores = is_700_or_newer;
            opt.use_dynamic_rendering = is_700_or_newer;
            opt.max_push_constants = 256;
            opt.optimal_workgroup_size = 64;
            opt.use_subpass_merge = architecture_ == GPUArchitecture::Adreno_800;
            break;
        }
        case GPUVendor::Mali: {
            const bool is_valhall_or_newer = architecture_ == GPUArchitecture::Mali_Valhall ||
                                             architecture_ == GPUArchitecture::Mali_G715;
            opt.use_afbc = true;
            opt.use_tile_memory = true;
            opt.use_subpass_merge = true;
            opt.prefer_compute_shaders = true;
            opt.use_descriptor_indexing = true;
            opt.use_timeline_semaphores = is_valhall_or_newer;
            opt.use_dynamic_rendering = is_valhall_or_newer;
            opt.max_push_constants = 256;
            // Mali executes 4-wide on Bifrost and 8-wide from Valhall onwards.
            opt.optimal_workgroup_size = is_valhall_or_newer ? 64 : 32;
            opt.use_image_compression = is_valhall_or_newer;
            break;
        }
        case GPUVendor::PowerVR: {
            const bool is_furian = architecture_ == GPUArchitecture::PowerVR_Furian;
            opt.use_image_compression = true;
            opt.prefer_compute_shaders = true;
            opt.use_descriptor_indexing = true;
            opt.use_timeline_semaphores = is_furian;
            opt.use_dynamic_rendering = is_furian;
            opt.max_push_constants = 256;
            opt.optimal_workgroup_size = 32;
            break;
        }
        default: {
            opt.optimal_workgroup_size = 64;
            opt.max_push_constants = 128;
            break;
        }
    }

    // Never recommend a capability the device did not actually report.
    const auto has = [&](RendererFeature feature) {
        return (static_cast<uint32_t>(supported_features_) & static_cast<uint32_t>(feature)) != 0;
    };
    if (!has(RendererFeature::DescriptorIndexing)) {
        opt.use_descriptor_indexing = false;
    }
    if (!has(RendererFeature::TimelineSemaphore)) {
        opt.use_timeline_semaphores = false;
    }
    if (!has(RendererFeature::DynamicRendering)) {
        opt.use_dynamic_rendering = false;
    }

    return opt;
}

GPUCapabilities::GPUOptimizationConfig GPUCapabilities::getOptimizationConfig() const {
    return buildOptimizationConfig();
}

bool GPUCapabilities::detectAdreno() {
    // Adreno detection via Vulkan vendor ID (0x5143 = Qualcomm)
    // Or via GLES renderer string
    return false;
}

bool GPUCapabilities::detectMali() {
    // Mali detection via Vulkan vendor ID (0x13B5 = ARM)
    return false;
}

bool GPUCapabilities::detectPowerVR() {
    // PowerVR detection via Vulkan vendor ID (0x1010 = Imagination)
    return false;
}

bool GPUCapabilities::detectGeneric() {
    return false;
}

void GPUCapabilities::queryVulkanProperties() {
    VkInstance instance;
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Copper Oxide GPU Detect";
    // Probe the loader version instead of demanding 1.3: Android drivers on
    // older GPUs expose only 1.0/1.1 and would fail with
    // VK_ERROR_INCOMPATIBLE_DRIVER.
    uint32_t loader_version = VK_API_VERSION_1_0;
    auto enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_instance_version != nullptr &&
        enumerate_instance_version(&loader_version) != VK_SUCCESS) {
        loader_version = VK_API_VERSION_1_0;
    }
    app_info.apiVersion = loader_version < VK_API_VERSION_1_1 ? loader_version : VK_API_VERSION_1_1;
    
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    
    if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
        return;
    }
    
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count > 0) {
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        
        for (const auto& device : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            
            // Determine vendor from vendor ID
            switch (props.vendorID) {
                case 0x5143: // Qualcomm
                    vendor_ = GPUVendor::Adreno;
                    break;
                case 0x13B5: // ARM
                    vendor_ = GPUVendor::Mali;
                    break;
                case 0x1010: // Imagination
                    vendor_ = GPUVendor::PowerVR;
                    break;
                default:
                    vendor_ = GPUVendor::Unknown;
                    break;
            }
            
            // Determine architecture from device name
            std::string device_name = props.deviceName;
            std::transform(device_name.begin(), device_name.end(), device_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            
            if (vendor_ == GPUVendor::Adreno) {
                if (device_name.find("830") != std::string::npos || device_name.find("adreno 830") != std::string::npos) {
                    architecture_ = GPUArchitecture::Adreno_800;
                } else if (device_name.find("730") != std::string::npos || device_name.find("740") != std::string::npos || 
                           device_name.find("750") != std::string::npos || device_name.find("adreno 7") != std::string::npos) {
                    architecture_ = GPUArchitecture::Adreno_700;
                } else {
                    architecture_ = GPUArchitecture::Adreno_600;
                }
            } else if (vendor_ == GPUVendor::Mali) {
                if (device_name.find("g715") != std::string::npos || device_name.find("g720") != std::string::npos || 
                    device_name.find("immortalis") != std::string::npos) {
                    architecture_ = GPUArchitecture::Mali_G715;
                } else if (device_name.find("valhall") != std::string::npos || device_name.find("g710") != std::string::npos) {
                    architecture_ = GPUArchitecture::Mali_Valhall;
                } else if (device_name.find("bifrost") != std::string::npos || device_name.find("g76") != std::string::npos ||
                           device_name.find("g77") != std::string::npos || device_name.find("g78") != std::string::npos) {
                    architecture_ = GPUArchitecture::Mali_Bifrost;
                } else {
                    architecture_ = GPUArchitecture::Mali_Midgard;
                }
            } else if (vendor_ == GPUVendor::PowerVR) {
                if (device_name.find("bxm") != std::string::npos || device_name.find("bxm-8") != std::string::npos) {
                    architecture_ = GPUArchitecture::PowerVR_BXM;
                } else if (device_name.find("furian") != std::string::npos || device_name.find("rogue") != std::string::npos) {
                    architecture_ = GPUArchitecture::PowerVR_Furian;
                } else {
                    architecture_ = GPUArchitecture::PowerVR_Rogue;
                }
            }
            
            // Query supported extensions
            uint32_t ext_count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
            if (ext_count > 0) {
                std::vector<VkExtensionProperties> extensions(ext_count);
                vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, extensions.data());
                for (const auto& ext : extensions) {
                    supported_extensions_.push_back(ext.extensionName);
                }
            }
            
            // Query features
            VkPhysicalDeviceFeatures features;
            vkGetPhysicalDeviceFeatures(device, &features);
            
            if (features.geometryShader) supported_features_ = supported_features_ | RendererFeature::GeometryShaders;
            if (features.tessellationShader) supported_features_ = supported_features_ | RendererFeature::TessellationShaders;
            if (features.multiDrawIndirect) supported_features_ = supported_features_ | RendererFeature::MultiDrawIndirect;
            if (features.drawIndirectFirstInstance) supported_features_ = supported_features_ | RendererFeature::DrawIndirectCount;
            
            // Check for Vulkan 1.1+ features
            VkPhysicalDeviceVulkan11Features features11{};
            features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            
            VkPhysicalDeviceVulkan12Features features12{};
            features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            features12.pNext = &features11;
            
            VkPhysicalDeviceVulkan13Features features13{};
            features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            features13.pNext = &features12;
            
            VkPhysicalDeviceFeatures2 features2{};
            features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &features13;

            // Android's libvulkan does not export Vulkan 1.1+ entry points, so the
            // function is resolved dynamically at runtime.
            auto get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
            if (get_features2) {
                get_features2(device, &features2);
            } else {
                break; // Device cannot report 1.1+ features; stay on the base feature set
            }
            
            if (features11.storageBuffer16BitAccess) supported_features_ = supported_features_ | RendererFeature::StorageImageExtendedFormats;
            if (features11.uniformAndStorageBuffer16BitAccess) supported_features_ = supported_features_ | RendererFeature::UniformBufferStandardLayout;
            if (features12.descriptorIndexing) supported_features_ = supported_features_ | RendererFeature::DescriptorIndexing;
            if (features12.timelineSemaphore) supported_features_ = supported_features_ | RendererFeature::TimelineSemaphore;
            if (features12.bufferDeviceAddress) supported_features_ = supported_features_ | RendererFeature::BufferDeviceAddress;
            if (features12.hostQueryReset) supported_features_ = supported_features_ | RendererFeature::HostQueryReset;
            if (features13.dynamicRendering) supported_features_ = supported_features_ | RendererFeature::DynamicRendering;
            if (features13.synchronization2) supported_features_ = supported_features_ | RendererFeature::Synchronization2;
            if (features13.maintenance4) supported_features_ = supported_features_ | RendererFeature::MaintenanceFeatures;
            
            break; // Use first suitable device
        }
    }
    
    vkDestroyInstance(instance, nullptr);
}

void GPUCapabilities::queryGLESProperties() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return;
    
    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) return;
    
    const char* vendor = reinterpret_cast<const char*>(eglQueryString(display, EGL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    
    if (vendor) {
        std::string vendor_str(vendor);
        std::transform(vendor_str.begin(), vendor_str.end(), vendor_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        if (vendor_str.find("qualcomm") != std::string::npos || vendor_str.find("adreno") != std::string::npos) {
            vendor_ = GPUVendor::Adreno;
        } else if (vendor_str.find("arm") != std::string::npos || vendor_str.find("mali") != std::string::npos) {
            vendor_ = GPUVendor::Mali;
        } else if (vendor_str.find("imagination") != std::string::npos || vendor_str.find("powervr") != std::string::npos) {
            vendor_ = GPUVendor::PowerVR;
        }
    }
    
    if (renderer) {
        std::string renderer_str(renderer);
        std::transform(renderer_str.begin(), renderer_str.end(), renderer_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        if (vendor_ == GPUVendor::Adreno) {
            if (renderer_str.find("830") != std::string::npos) {
                architecture_ = GPUArchitecture::Adreno_800;
            } else if (renderer_str.find("730") != std::string::npos || renderer_str.find("740") != std::string::npos ||
                       renderer_str.find("750") != std::string::npos) {
                architecture_ = GPUArchitecture::Adreno_700;
            } else {
                architecture_ = GPUArchitecture::Adreno_600;
            }
        } else if (vendor_ == GPUVendor::Mali) {
            if (renderer_str.find("g715") != std::string::npos || renderer_str.find("g720") != std::string::npos ||
                renderer_str.find("immortalis") != std::string::npos) {
                architecture_ = GPUArchitecture::Mali_G715;
            } else if (renderer_str.find("valhall") != std::string::npos || renderer_str.find("g710") != std::string::npos) {
                architecture_ = GPUArchitecture::Mali_Valhall;
            } else if (renderer_str.find("bifrost") != std::string::npos || renderer_str.find("g76") != std::string::npos ||
                       renderer_str.find("g77") != std::string::npos || renderer_str.find("g78") != std::string::npos) {
                architecture_ = GPUArchitecture::Mali_Bifrost;
            } else {
                architecture_ = GPUArchitecture::Mali_Midgard;
            }
        } else if (vendor_ == GPUVendor::PowerVR) {
            if (renderer_str.find("bxm") != std::string::npos) {
                architecture_ = GPUArchitecture::PowerVR_BXM;
            } else if (renderer_str.find("furian") != std::string::npos) {
                architecture_ = GPUArchitecture::PowerVR_Furian;
            } else {
                architecture_ = GPUArchitecture::PowerVR_Rogue;
            }
        }
    }
    
    if (extensions) {
        std::string ext_str(extensions);
        std::istringstream iss(ext_str);
        std::string ext;
        while (iss >> ext) {
            supported_extensions_.push_back(ext);
        }
    }
    
    // Check for compute shaders (GLES 3.1+)
    if (version != nullptr &&
        (std::strstr(version, "OpenGL ES 3.1") || std::strstr(version, "OpenGL ES 3.2"))) {
        supported_features_ = supported_features_ | RendererFeature::ComputeShaders;
    }
    
    // Check for geometry shaders
    if (std::find(supported_extensions_.begin(), supported_extensions_.end(), "GL_EXT_geometry_shader") != supported_extensions_.end() ||
        std::find(supported_extensions_.begin(), supported_extensions_.end(), "GL_OES_geometry_shader") != supported_extensions_.end()) {
        supported_features_ = supported_features_ | RendererFeature::GeometryShaders;
    }
    
    // Check for tessellation
    if (std::find(supported_extensions_.begin(), supported_extensions_.end(), "GL_EXT_tessellation_shader") != supported_extensions_.end() ||
        std::find(supported_extensions_.begin(), supported_extensions_.end(), "GL_OES_tessellation_shader") != supported_extensions_.end()) {
        supported_features_ = supported_features_ | RendererFeature::TessellationShaders;
    }
    
    eglTerminate(display);
}

} // namespace copper