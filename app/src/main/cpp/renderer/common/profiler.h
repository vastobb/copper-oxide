#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace copper {

class RendererBase;

class Profiler {
public:
    struct Stats {
        uint64_t frame_count = 0;
        double avg_frame_time_ms = 0.0;
        double min_frame_time_ms = 0.0;
        double max_frame_time_ms = 0.0;
        double avg_fps = 0.0;
        double min_fps = 0.0;
        double max_fps = 0.0;
        double last_frame_time_ms = 0.0;
        double last_cpu_time_ms = 0.0;
        double last_gpu_time_ms = 0.0;
        uint32_t last_draw_calls = 0;
        uint64_t last_gpu_memory = 0;
        uint64_t last_cpu_memory = 0;
    };

    struct FrameStats {
        uint64_t frame_number = 0;
        double frame_time_ms = 0.0;
        double cpu_time_ms = 0.0;
        double gpu_time_ms = 0.0;
        uint32_t draw_calls = 0;
        uint64_t gpu_memory = 0;
        uint64_t cpu_memory = 0;
    };

    Profiler();
    ~Profiler();

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;
    Profiler(Profiler&&) noexcept = default;
    Profiler& operator=(Profiler&&) noexcept = default;

    bool initialize(RendererBase* renderer);
    void shutdown();

    virtual void beginFrame(uint64_t frame_number);
    virtual void endFrame(uint64_t frame_number, double frame_time_ms, double cpu_time_ms, double gpu_time_ms, uint32_t draw_calls);

    virtual void beginGpuTimer(const std::string& name);
    virtual double endGpuTimer(const std::string& name);

    virtual void beginCpuTimer(const std::string& name);
    virtual double endCpuTimer(const std::string& name);

    virtual void recordDrawCall();
    virtual void recordMemoryUsage(uint64_t gpu_memory, uint64_t cpu_memory);

    virtual Stats getStats() const;
    virtual std::vector<FrameStats> getFrameHistory(uint32_t count) const;

    virtual void reset();
    virtual void setEnabled(bool enabled);
    virtual bool isEnabled() const;

protected:
    virtual void onBeginFrame(uint64_t frame_number) = 0;
    virtual void onEndFrame(uint64_t frame_number, double frame_time_ms, double cpu_time_ms, double gpu_time_ms, uint32_t draw_calls) = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper