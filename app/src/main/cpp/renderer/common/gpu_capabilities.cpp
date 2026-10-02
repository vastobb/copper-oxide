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

namespace copper {

GPUCapabilities::GPUCapabilities() = default;
GPUCapabilities::~GPUCapabilities() = default;

bool GPUCapabilities::detect() {
    // Try Vulkan first
    queryVulkanProperties();
    
    // If Vulkan not available, try GLES
    if (vendor_ == GPUVendor::Unknown) {
        queryGLESProperties();
    }
    
    // Apply GPU-specific workarounds and optimizations
    applyWorkarounds(RendererConfig()); // Dummy config to trigger optimization config setup
    
    return vendor_ != GPUVendor::Unknown;
}

void GPUCapabilities::applyWorkarounds(const RendererConfig& config) const {
    // This would apply specific workarounds based on detected GPU
    // For now, just set optimization config based on vendor/architecture
}

GPUCapabilities::GPUOptimizationConfig GPUCapabilities::getOptimizationConfig() const {
    GPUOptimizationConfig opt;
    
    switch (vendor_) {
        case GPUVendor::Adreno: {
            opt.use_ubwc = true;
            opt.use_image_compression = true;
            opt.prefer_compute_shaders = true;
            opt.prefer_indirect_draw = true;
            opt.use_descriptor_indexing = true;
            opt.use_timeline_semaphores = (architecture_ >= GPUArchitecture::Adreno_700);
            opt.use_dynamic_rendering = (architecture_ >= GPUArchitecture::Adreno_700);
            opt.max_push_constants = 256;
            opt.optimal_workgroup_size = 64;
            
            if (architecture_ == GPUArchitecture::Adreno_800) {
                opt.use_subpass_merge = true;
            }
            break;
        }
        case GPUVendor::Mali: {
            opt.use_afbc = true;
            opt.use_tile_memory = true;
            opt.use_subpass_merge = true;
            opt.prefer_compute_shaders = true;
            opt.use_descriptor_indexing = true;
            opt.use_timeline_semaphores = (architecture_ >= GPUArchitecture::Mali_Valhall);
            opt.use_dynamic_rendering = (architecture_ >= GPUArchitecture::Mali_Valhall);
            opt.max_push_constants = 256;
            opt.optimal_workgroup_size = 64;
            
            if (architecture_ == GPUArchitecture::Mali_G715 || architecture_ == GPUArchitecture::Mali_Valhall) {
                opt.use_image_compression = true;
            }
            break;
        }
        case GPUVendor::PowerVR: {
            opt.use_image_compression = true;
            opt.prefer_compute_shaders = true;
            opt.use_descriptor_indexing = true;
            opt.use_timeline_semaphores = (architecture_ >= GPUArchitecture::PowerVR_Furian);
            opt.use_dynamic_rendering = (architecture_ >= GPUArchitecture::PowerVR_Furian);
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
    
    return opt;
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
    app_info.apiVersion = VK_API_VERSION_1_3;
    
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
            std::transform(device_name.begin(), device_name.end(), device_name.begin(), ::tolower);
            
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
        std::transform(vendor_str.begin(), vendor_str.end(), vendor_str.begin(), ::tolower);
        
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
        std::transform(renderer_str.begin(), renderer_str.end(), renderer_str.begin(), ::tolower);
        
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
    if (strstr(version, "OpenGL ES 3.1") || strstr(version, "OpenGL ES 3.2")) {
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