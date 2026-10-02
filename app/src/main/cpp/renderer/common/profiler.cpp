#include "profiler.h"
#include "renderer_base.h"

#include <vector>
#include <mutex>
#include <chrono>
#include <string>
#include <unordered_map>

namespace copper {

class Profiler::Impl {
public:
    struct FrameData {
        uint64_t frame_number = 0;
        double frame_time_ms = 0.0;
        double cpu_time_ms = 0.0;
        double gpu_time_ms = 0.0;
        uint32_t draw_calls = 0;
        uint64_t gpu_memory = 0;
        uint64_t cpu_memory = 0;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point end_time;
    };

    struct GpuTimer {
        uint64_t handle = 0;
        std::string name;
        std::chrono::steady_clock::time_point start;
        double elapsed_ms = 0.0;
        bool active = false;
    };

    struct CpuTimer {
        std::string name;
        std::chrono::steady_clock::time_point start;
        double elapsed_ms = 0.0;
        bool active = false;
    };

    std::vector<FrameData> frame_history;
    std::unordered_map<std::string, GpuTimer> gpu_timers;
    std::unordered_map<std::string, CpuTimer> cpu_timers;
    std::mutex mutex;
    RendererBase* renderer = nullptr;
    bool enabled = true;
    uint32_t max_frames_history = 1000;
    uint32_t frame_count = 0;
    double total_frame_time = 0.0;
    double min_frame_time = std::numeric_limits<double>::max();
    double max_frame_time = 0.0;
};

Profiler::Profiler() : pImpl(std::make_unique<Impl>()) {}
Profiler::~Profiler() = default;

bool Profiler::initialize(RendererBase* renderer) {
    pImpl->renderer = renderer;
    if (renderer) {
        const auto& config = renderer->getConfig();
        pImpl->enabled = config.enableProfiling;
    }
    return true;
}

void Profiler::shutdown() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->frame_history.clear();
    pImpl->gpu_timers.clear();
    pImpl->cpu_timers.clear();
}

void Profiler::beginFrame(uint64_t frame_number) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled) return;

    Impl::FrameData frame;
    frame.frame_number = frame_number;
    frame.start_time = std::chrono::steady_clock::now();
    
    if (pImpl->frame_history.size() >= pImpl->max_frames_history) {
        pImpl->frame_history.erase(pImpl->frame_history.begin());
    }
    pImpl->frame_history.push_back(std::move(frame));
}

void Profiler::endFrame(uint64_t frame_number, double frame_time_ms, double cpu_time_ms, double gpu_time_ms, uint32_t draw_calls) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled) return;

    if (!pImpl->frame_history.empty()) {
        auto& frame = pImpl->frame_history.back();
        frame.frame_time_ms = frame_time_ms;
        frame.cpu_time_ms = cpu_time_ms;
        frame.gpu_time_ms = gpu_time_ms;
        frame.draw_calls = draw_calls;
        frame.end_time = std::chrono::steady_clock::now();

        pImpl->frame_count++;
        pImpl->total_frame_time += frame_time_ms;
        pImpl->min_frame_time = std::min(pImpl->min_frame_time, frame_time_ms);
        pImpl->max_frame_time = std::max(pImpl->max_frame_time, frame_time_ms);
    }
}

void Profiler::beginGpuTimer(const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled) return;

    Impl::GpuTimer timer;
    timer.name = name;
    timer.start = std::chrono::steady_clock::now();
    timer.active = true;
    pImpl->gpu_timers[name] = std::move(timer);
}

double Profiler::endGpuTimer(const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled) return 0.0;

    auto it = pImpl->gpu_timers.find(name);
    if (it == pImpl->gpu_timers.end() || !it->second.active) {
        return 0.0;
    }

    auto end = std::chrono::steady_clock::now();
    it->second.elapsed_ms = std::chrono::duration<double, std::milli>(end - it->second.start).count();
    it->second.active = false;
    return it->second.elapsed_ms;
}

void Profiler::beginCpuTimer(const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled) return;

    Impl::CpuTimer timer;
    timer.name = name;
    timer.start = std::chrono::steady_clock::now();
    timer.active = true;
    pImpl->cpu_timers[name] = std::move(timer);
}

double Profiler::endCpuTimer(const std::string& name) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled) return 0.0;

    auto it = pImpl->cpu_timers.find(name);
    if (it == pImpl->cpu_timers.end() || !it->second.active) {
        return 0.0;
    }

    auto end = std::chrono::steady_clock::now();
    it->second.elapsed_ms = std::chrono::duration<double, std::milli>(end - it->second.start).count();
    it->second.active = false;
    return it->second.elapsed_ms;
}

void Profiler::recordDrawCall() {
    // Draw calls are counted in endFrame
}

void Profiler::recordMemoryUsage(uint64_t gpu_memory, uint64_t cpu_memory) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    if (!pImpl->enabled || pImpl->frame_history.empty()) return;

    pImpl->frame_history.back().gpu_memory = gpu_memory;
    pImpl->frame_history.back().cpu_memory = cpu_memory;
}

Profiler::Stats Profiler::getStats() const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    Stats stats;
    stats.frame_count = pImpl->frame_count;
    stats.avg_frame_time_ms = pImpl->frame_count > 0 ? pImpl->total_frame_time / pImpl->frame_count : 0.0;
    stats.min_frame_time_ms = pImpl->min_frame_time == std::numeric_limits<double>::max() ? 0.0 : pImpl->min_frame_time;
    stats.max_frame_time_ms = pImpl->max_frame_time;
    stats.avg_fps = stats.avg_frame_time_ms > 0 ? 1000.0 / stats.avg_frame_time_ms : 0.0;
    stats.min_fps = stats.max_frame_time_ms > 0 ? 1000.0 / stats.max_frame_time_ms : 0.0;
    stats.max_fps = stats.min_frame_time_ms > 0 ? 1000.0 / stats.min_frame_time_ms : 0.0;

    if (!pImpl->frame_history.empty()) {
        const auto& last = pImpl->frame_history.back();
        stats.last_frame_time_ms = last.frame_time_ms;
        stats.last_cpu_time_ms = last.cpu_time_ms;
        stats.last_gpu_time_ms = last.gpu_time_ms;
        stats.last_draw_calls = last.draw_calls;
        stats.last_gpu_memory = last.gpu_memory;
        stats.last_cpu_memory = last.cpu_memory;
    }

    return stats;
}

std::vector<Profiler::FrameStats> Profiler::getFrameHistory(uint32_t count) const {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    std::vector<FrameStats> history;
    size_t start = pImpl->frame_history.size() > count ? pImpl->frame_history.size() - count : 0;
    
    for (size_t i = start; i < pImpl->frame_history.size(); ++i) {
        const auto& frame = pImpl->frame_history[i];
        FrameStats fs;
        fs.frame_number = frame.frame_number;
        fs.frame_time_ms = frame.frame_time_ms;
        fs.cpu_time_ms = frame.cpu_time_ms;
        fs.gpu_time_ms = frame.gpu_time_ms;
        fs.draw_calls = frame.draw_calls;
        fs.gpu_memory = frame.gpu_memory;
        fs.cpu_memory = frame.cpu_memory;
        history.push_back(fs);
    }
    
    return history;
}

void Profiler::reset() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->frame_history.clear();
    pImpl->gpu_timers.clear();
    pImpl->cpu_timers.clear();
    pImpl->frame_count = 0;
    pImpl->total_frame_time = 0.0;
    pImpl->min_frame_time = std::numeric_limits<double>::max();
    pImpl->max_frame_time = 0.0;
}

void Profiler::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(pImpl->mutex);
    pImpl->enabled = enabled;
}

bool Profiler::isEnabled() const {
    return pImpl->enabled;
}

} // namespace copper