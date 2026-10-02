#pragma once

#include "renderer_base.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

namespace copper {

class GPUCapabilities {
public:
    struct GPUOptimizationConfig {
        bool use_afbc = false;
        bool use_ubwc = false;
        bool use_tile_memory = false;
        bool use_subpass_merge = false;
        bool use_image_compression = false;
        bool prefer_compute_shaders = false;
        bool prefer_indirect_draw = false;
        uint32_t optimal_workgroup_size = 64;
        uint32_t max_push_constants = 256;
        bool use_descriptor_indexing = false;
        bool use_timeline_semaphores = false;
        bool use_dynamic_rendering = false;
    };

    GPUCapabilities();
    ~GPUCapabilities();

    GPUCapabilities(const GPUCapabilities&) = delete;
    GPUCapabilities& operator=(const GPUCapabilities&) = delete;
    GPUCapabilities(GPUCapabilities&&) noexcept = default;
    GPUCapabilities& operator=(GPUCapabilities&&) noexcept = default;

    bool detect();

    // Seed the capability layer with information already gathered by a backend
    // so that vendor workarounds can run without re-probing the driver.
    void setDetectedInfo(GPUVendor vendor, GPUArchitecture arch,
                         const std::vector<std::string>& extensions,
                         RendererFeature features);
    void applyWorkarounds(const RendererConfig& config) const;
    GPUOptimizationConfig getOptimizationConfig() const;

    GPUOptimizationConfig buildOptimizationConfig() const;

    GPUVendor getVendor() const { return vendor_; }
    GPUArchitecture getArchitecture() const { return architecture_; }
    RendererFeature getSupportedFeatures() const { return supported_features_; }
    const std::vector<std::string>& getSupportedExtensions() const { return supported_extensions_; }
    const std::vector<std::string>& getDriverBugs() const { return driver_bugs_; }

private:
    bool detectAdreno();
    bool detectMali();
    bool detectPowerVR();
    bool detectGeneric();

    void queryVulkanProperties();
    void queryGLESProperties();

    GPUVendor vendor_ = GPUVendor::Unknown;
    GPUArchitecture architecture_ = GPUArchitecture::Unknown;
    RendererFeature supported_features_ = RendererFeature::None;
    std::vector<std::string> supported_extensions_;
    std::vector<std::string> driver_bugs_;
    GPUOptimizationConfig optimization_config_;
};

} // namespace copper