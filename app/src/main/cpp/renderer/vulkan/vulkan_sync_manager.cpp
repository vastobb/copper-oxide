#include "vulkan_sync_manager.h"

// The VulkanRenderer accessors used below (device(), physicalDevice(),
// instance()) are the ones the integrator must add to VulkanRenderer; see the
// class comment in the header.
#include "vulkan_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <string>
#include <vector>

#define LOG_TAG "CopperOxide-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        default: return "VkResult";
    }
}

} // namespace

VulkanSyncManager::VulkanSyncManager(VulkanRenderer* renderer) : renderer_(renderer) {}

VulkanSyncManager::~VulkanSyncManager() {
    // SyncManager::shutdown() is NOT virtual, so this destructor is the only
    // guaranteed cleanup point. Anything still in the tables was never handed
    // back through the base API; destroying it here is the whole reason the
    // tables are members instead of an Impl the base owns.
    // Named vk_device, not device: a local called `device` would shadow the
    // device() accessor and the call on the right-hand side would resolve to the
    // variable being declared.
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        return;
    }
    for (auto& [handle, fence] : fences_) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(vk_device, fence, nullptr);
        }
    }
    for (auto& [handle, semaphore] : semaphores_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk_device, semaphore, nullptr);
        }
    }
    for (auto& [handle, event] : events_) {
        if (event != VK_NULL_HANDLE) {
            vkDestroyEvent(vk_device, event, nullptr);
        }
    }
    fences_.clear();
    semaphores_.clear();
    events_.clear();
    timeline_values_.clear();
}

// ---------------------------------------------------------------------------
// capability probe
// ---------------------------------------------------------------------------

VkDevice VulkanSyncManager::device() const {
    return renderer_ != nullptr ? renderer_->device() : VK_NULL_HANDLE;
}

void VulkanSyncManager::load_timeline_entry_points_locked() {
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE || signal_semaphore_fn_ != nullptr) {
        return;
    }
    // Extension entry points must be resolved through the loader even when the
    // header declares a prototype; core-1.2 builds resolve it straight through.
    signal_semaphore_fn_ = reinterpret_cast<PFN_vkSignalSemaphoreKHR>(
        vkGetDeviceProcAddr(vk_device, "vkSignalSemaphoreKHR"));
}

bool VulkanSyncManager::prepare() {
    std::lock_guard<std::mutex> lock(mutex_);
    return prepare_locked();
}

bool VulkanSyncManager::prepare_locked() {
    if (prepared_) {
        return true;
    }
    prepared_ = true;
    if (renderer_ == nullptr || device() == VK_NULL_HANDLE) {
        LOGW("sync manager prepared without a live VkDevice; every create call will fail");
        return false;
    }

    // Why query the extension list directly instead of
    // VulkanRenderer::isExtensionSupported(): that helper reads
    // gpu_info_.extensions, which VulkanRenderer::query_gpu_info() never fills,
    // so it answers "unsupported" for everything. vkEnumerateDeviceExtension*
    // is the only honest source until that is fixed.
    const VkPhysicalDevice physical_device = renderer_->physicalDevice();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    bool has_extension = properties.apiVersion >= VK_API_VERSION_1_2;
    if (!has_extension) {
        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (extension_count > 0) {
            vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                 extensions.data());
        }
        for (const auto& extension : extensions) {
            if (std::string(extension.extensionName) == VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) {
                has_extension = true;
                break;
            }
        }
    }
    timeline_available_ = has_extension;

    // Being advertised is not the same as being usable. Timeline semaphores
    // need the extension enabled in VkDeviceCreateInfo AND the feature bit set;
    // the feature lives in VkPhysicalDeviceTimelineSemaphoreFeatures, which is a
    // Vulkan 1.2 (extension) structure this 1.0-ABI build does not have. Since
    // VulkanRenderer::create_logical_device() enables no extension and passes a
    // zeroed VkPhysicalDeviceFeatures, timeline semaphores cannot be active on
    // this path today. Reporting that honestly is better than probing a
    // structure that is not there and guessing.
    timeline_enabled_ = false;

    if (!has_extension) {
        LOGI("sync manager: %s absent; using binary semaphores",
             VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    } else {
        LOGI("sync manager: %s present but not enabled on the device;"
             " using binary semaphores",
             VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    }
    return true;
}

bool VulkanSyncManager::timeline_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeline_enabled_;
}

bool VulkanSyncManager::timeline_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeline_available_;
}

// ---------------------------------------------------------------------------
// handle translation
// ---------------------------------------------------------------------------

VkFence VulkanSyncManager::fence(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fences_.find(handle);
    return it != fences_.end() ? it->second : VK_NULL_HANDLE;
}

VkSemaphore VulkanSyncManager::semaphore(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = semaphores_.find(handle);
    return it != semaphores_.end() ? it->second : VK_NULL_HANDLE;
}

bool VulkanSyncManager::is_timeline_semaphore(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timeline_values_.count(handle) != 0;
}

uint64_t VulkanSyncManager::timeline_value(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = timeline_values_.find(handle);
    return it != timeline_values_.end() ? it->second : 0;
}

// ---------------------------------------------------------------------------
// fences
// ---------------------------------------------------------------------------

bool VulkanSyncManager::onCreateFence(uint64_t handle, bool signaled) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_locked();
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        LOGE("cannot create fence %llu: no VkDevice", static_cast<unsigned long long>(handle));
        return false;
    }

    VkFenceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Signalled-on-create is what makes the first wait a no-op: a fence that
    // nothing has submitted yet must not block the frame that reuses it.
    create_info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;

    VkFence fence = VK_NULL_HANDLE;
    const VkResult result = vkCreateFence(vk_device, &create_info, nullptr, &fence);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateFence failed for handle %llu: %s", static_cast<unsigned long long>(handle),
             result_name(result));
        return false;
    }
    fences_[handle] = fence;
    return true;
}

void VulkanSyncManager::onDestroyFence(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fences_.find(handle);
    if (it == fences_.end()) {
        return;
    }
    const VkDevice vk_device = device();
    // A fence with a pending wait may not be destroyed. Nothing here can know
    // whether a submission is in flight, so the caller is responsible for the
    // wait (VulkanRenderer::onBeginFrame waits on the frame fence before reuse).
    if (vk_device != VK_NULL_HANDLE && it->second != VK_NULL_HANDLE) {
        vkDestroyFence(vk_device, it->second, nullptr);
    }
    fences_.erase(it);
}

bool VulkanSyncManager::onWaitFence(uint64_t handle, uint64_t timeout_ns) {
    VkFence fence = VK_NULL_HANDLE;
    bool warn_infinite = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prepare_locked();
        const auto it = fences_.find(handle);
        if (it != fences_.end()) {
            fence = it->second;
        }
        // Not an error, but this is the call that hangs a render thread forever on
        // a lost submission, so it is worth one line. The flag is set under the
        // lock; the log happens outside it.
        warn_infinite = (timeout_ns == UINT64_MAX) && !infinite_wait_logged_;
        if (warn_infinite) {
            infinite_wait_logged_ = true;
        }
    }
    if (fence == VK_NULL_HANDLE) {
        return false;
    }
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        return false;
    }
    if (warn_infinite) {
        LOGW("fence %llu waited on with an infinite timeout",
             static_cast<unsigned long long>(handle));
    }
    // timeout_ns is already nanoseconds, which is exactly vkWaitForFences's
    // unit, so 0 becomes the non-blocking poll the base API asks for.
    const VkResult result = vkWaitForFences(vk_device, 1, &fence, VK_TRUE, timeout_ns);
    if (result == VK_SUCCESS) {
        return true;
    }
    if (result == VK_TIMEOUT) {
        return false;
    }
    LOGE("vkWaitForFences(handle=%llu) failed: %s", static_cast<unsigned long long>(handle),
         result_name(result));
    return false;
}

bool VulkanSyncManager::wait_fence_for(uint64_t handle, uint64_t timeout_ns) {
    return onWaitFence(handle, timeout_ns);
}

void VulkanSyncManager::onSignalFence(uint64_t handle) {
    // DOCUMENTED NO-OP. A VkFence cannot be signalled from the host: only a
    // queue submission with that fence can do it (or vkResetFences can take it
    // back down). The base class has already flipped its own host-side flag
    // before calling this hook, so isFenceSignaled() keeps reporting what the
    // caller expects -- that host flag, not this VkFence, is the source of truth
    // for "signalled". A host-side flag that disagrees with the GPU is inherent
    // to the GL-shaped base API; onWaitFence() below is the honest query.
    if (!signal_fence_logged_) {
        signal_fence_logged_ = true;
        LOGI("SyncManager::signalFence on the Vulkan backend is host-side only;"
             " the VkFence stays untouched and is driven by vkQueueSubmit");
    }
}

void VulkanSyncManager::onResetFence(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fences_.find(handle);
    if (it == fences_.end() || it->second == VK_NULL_HANDLE) {
        return;
    }
    // Named vk_device, not device: a local called `device` would shadow the
    // device() accessor and the call on the right-hand side would resolve to the
    // variable being declared.
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        return;
    }
    // Legal on an unsignalled fence (it just stays unsignalled). Illegal while
    // the fence is in use by a pending vkWaitForFences/vkQueueSubmit, which is
    // why the reset happens right before the submit that will re-signal it.
    const VkResult result = vkResetFences(vk_device, 1, &it->second);
    if (result != VK_SUCCESS) {
        LOGE("vkResetFences(handle=%llu) failed: %s", static_cast<unsigned long long>(handle),
             result_name(result));
    }
}

// ---------------------------------------------------------------------------
// semaphores
// ---------------------------------------------------------------------------

bool VulkanSyncManager::onCreateSemaphore(uint64_t handle, uint64_t initial_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_locked();
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        LOGE("cannot create semaphore %llu: no VkDevice",
             static_cast<unsigned long long>(handle));
        return false;
    }

    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;

    VkSemaphoreCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (timeline_enabled_) {
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR;
        type_info.initialValue = initial_value;
        create_info.pNext = &type_info;
    } else if (initial_value > 1 && !binary_fallback_logged_) {
        // A binary semaphore has no value, so a caller asking for value N > 1
        // cannot get what it asked for. Say so once instead of silently
        // handing back a semaphore that behaves differently.
        binary_fallback_logged_ = true;
        LOGW("createSemaphore(%llu) requested timeline value %llu but timeline semaphores are"
             " unavailable; got a binary semaphore instead",
             static_cast<unsigned long long>(handle),
             static_cast<unsigned long long>(initial_value));
    }

    VkSemaphore semaphore = VK_NULL_HANDLE;
    const VkResult result = vkCreateSemaphore(vk_device, &create_info, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateSemaphore failed for handle %llu: %s",
             static_cast<unsigned long long>(handle), result_name(result));
        return false;
    }
    semaphores_[handle] = semaphore;
    if (timeline_enabled_) {
        timeline_values_[handle] = initial_value;
    }
    return true;
}

void VulkanSyncManager::onDestroySemaphore(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = semaphores_.find(handle);
    if (it == semaphores_.end()) {
        return;
    }
    const VkDevice vk_device = device();
    if (vk_device != VK_NULL_HANDLE && it->second != VK_NULL_HANDLE) {
        // A semaphore that is still in use by a submitted batch may not be
        // destroyed; the caller has to have waited on the fence first.
        vkDestroySemaphore(vk_device, it->second, nullptr);
    }
    semaphores_.erase(it);
    timeline_values_.erase(handle);
}

void VulkanSyncManager::onSignalSemaphore(uint64_t handle, uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_locked();
    const auto it = semaphores_.find(handle);
    if (it == semaphores_.end()) {
        return;
    }

    const bool timeline = timeline_values_.count(handle) != 0;
    if (!timeline) {
        // Honest no-op: a binary semaphore is unsignalled by a queue operation
        // (vkQueueSubmit / vkQueuePresent with it in pSignalSemaphores), never
        // from the host. SyncManager::signalSemaphore has already bumped its
        // own counter, so getSemaphoreValue keeps advancing for callers; what
        // the GPU sees is whatever the next submit signals.
        if (!binary_signal_logged_) {
            binary_signal_logged_ = true;
            LOGW("signalSemaphore on binary semaphore %llu is host-side only;"
                 " signal it through a VkSubmitInfo::pSignalSemaphores entry",
                 static_cast<unsigned long long>(handle));
        }
        return;
    }

    if (signal_semaphore_fn_ == nullptr) {
        load_timeline_entry_points_locked();
    }
    if (signal_semaphore_fn_ == nullptr) {
        LOGE("cannot signal timeline semaphore %llu: vkSignalSemaphoreKHR is unreachable",
             static_cast<unsigned long long>(handle));
        return;
    }

    // The base adds to the counter before calling the hook (SyncManager::
    // signalSemaphore does `value += value`), so the target is current + value.
    // vkSignalSemaphoreKHR sets an ABSOLUTE value, hence the shadow counter --
    // passing the delta straight through would rewind the timeline on every
    // signal after the first.
    const uint64_t current = timeline_values_[handle];
    const uint64_t target = current + value;

    VkSemaphoreSignalInfo signal_info{};
    signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal_info.semaphore = it->second;
    signal_info.value = target;

    const VkResult result = signal_semaphore_fn_(device(), &signal_info);
    if (result != VK_SUCCESS) {
        LOGE("vkSignalSemaphoreKHR(handle=%llu, value=%llu) failed: %s",
             static_cast<unsigned long long>(handle),
             static_cast<unsigned long long>(target), result_name(result));
        // Drop it from the timeline table: the shadow counter no longer matches
        // the GPU, and a value array pointing at a broken semaphore would turn
        // every later submit into an error.
        timeline_values_.erase(handle);
        return;
    }
    timeline_values_[handle] = target;
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

bool VulkanSyncManager::onCreateEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_locked();
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        LOGE("cannot create event %llu: no VkDevice", static_cast<unsigned long long>(handle));
        return false;
    }
    VkEventCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    VkEvent event = VK_NULL_HANDLE;
    const VkResult result = vkCreateEvent(vk_device, &create_info, nullptr, &event);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateEvent failed for handle %llu: %s", static_cast<unsigned long long>(handle),
             result_name(result));
        return false;
    }
    events_[handle] = event;
    return true;
}

void VulkanSyncManager::onDestroyEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = events_.find(handle);
    if (it == events_.end()) {
        return;
    }
    const VkDevice vk_device = device();
    if (vk_device != VK_NULL_HANDLE && it->second != VK_NULL_HANDLE) {
        vkDestroyEvent(vk_device, it->second, nullptr);
    }
    events_.erase(it);
}

void VulkanSyncManager::onSetEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = events_.find(handle);
    if (it == events_.end() || it->second == VK_NULL_HANDLE) {
        return;
    }
    // Named vk_device, not device: a local called `device` would shadow the
    // device() accessor and the call on the right-hand side would resolve to the
    // variable being declared.
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        return;
    }
    // WHY this is real Vulkan and still only host bookkeeping: a VkEvent only
    // reaches VK_EVENT_SET through vkCmdSetEvent in a submitted batch. Calling
    // vkSetEvent before that first batch leaves the device-side state untouched
    // and is a validation error. The base has already flipped its own flag, so
    // isEventSet() stays consistent with what callers expect.
    const VkResult result = vkSetEvent(vk_device, it->second);
    if (result != VK_SUCCESS) {
        LOGE("vkSetEvent(handle=%llu) failed: %s", static_cast<unsigned long long>(handle),
             result_name(result));
    }
}

void VulkanSyncManager::onResetEvent(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = events_.find(handle);
    if (it == events_.end() || it->second == VK_NULL_HANDLE) {
        return;
    }
    // Named vk_device, not device: a local called `device` would shadow the
    // device() accessor and the call on the right-hand side would resolve to the
    // variable being declared.
    const VkDevice vk_device = device();
    if (vk_device == VK_NULL_HANDLE) {
        return;
    }
    // Unlike vkSetEvent this is always legal: the reset host-side state is the
    // only state a host can change, and it is what the pending
    // vkWaitForFences-style host waits in the base read.
    const VkResult result = vkResetEvent(vk_device, it->second);
    if (result != VK_SUCCESS) {
        LOGE("vkResetEvent(handle=%llu) failed: %s", static_cast<unsigned long long>(handle),
             result_name(result));
    }
}

// ---------------------------------------------------------------------------
// pending submit synchronisation
// ---------------------------------------------------------------------------

void VulkanSyncManager::onSubmitWaitSemaphores(const std::vector<uint64_t>& semaphores,
                                               const std::vector<uint64_t>& values,
                                               uint32_t stage_flags) {
    if (semaphores.empty()) {
        return;
    }
    if (values.size() != semaphores.size()) {
        // Reject the whole batch rather than half-applying it: a submit whose
        // value array does not line up with its semaphore array is invalid, and
        // silently dropping entries would hide the caller's bug until the queue
        // starts returning errors.
        LOGE("submitWaitSemaphores: %zu semaphores but %zu values; batch rejected",
             semaphores.size(), values.size());
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    prepare_locked();
    if (pending_waits_.size() + semaphores.size() > k_max_pending_semaphores) {
        LOGE("submitWaitSemaphores: more than %u pending waits; batch rejected",
             k_max_pending_semaphores);
        return;
    }

    // A zero destination stage mask means "no wait" in Vulkan, which is never
    // what a caller that passed 0 meant. ALL_COMMANDS is the safe reading.
    const VkPipelineStageFlags stage_mask =
        (stage_flags == 0) ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                           : static_cast<VkPipelineStageFlags>(stage_flags);

    for (size_t i = 0; i < semaphores.size(); ++i) {
        const auto it = semaphores_.find(semaphores[i]);
        if (it == semaphores_.end()) {
            LOGW("submitWaitSemaphores: unknown semaphore handle %llu; skipped",
                 static_cast<unsigned long long>(semaphores[i]));
            continue;
        }
        PendingSemaphore entry;
        entry.semaphore = it->second;
        entry.stage_mask = stage_mask;
        entry.timeline = timeline_values_.count(semaphores[i]) != 0;
        entry.value = entry.timeline ? values[i] : 0;
        pending_waits_.push_back(entry);
    }
}

void VulkanSyncManager::onSubmitSignalSemaphores(const std::vector<uint64_t>& semaphores,
                                                 const std::vector<uint64_t>& values) {
    if (semaphores.empty()) {
        return;
    }
    if (values.size() != semaphores.size()) {
        LOGE("submitSignalSemaphores: %zu semaphores but %zu values; batch rejected",
             semaphores.size(), values.size());
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    prepare_locked();
    if (pending_signals_.size() + semaphores.size() > k_max_pending_semaphores) {
        LOGE("submitSignalSemaphores: more than %u pending signals; batch rejected",
             k_max_pending_semaphores);
        return;
    }

    for (size_t i = 0; i < semaphores.size(); ++i) {
        const auto it = semaphores_.find(semaphores[i]);
        if (it == semaphores_.end()) {
            LOGW("submitSignalSemaphores: unknown semaphore handle %llu; skipped",
                 static_cast<unsigned long long>(semaphores[i]));
            continue;
        }
        PendingSemaphore entry;
        entry.semaphore = it->second;
        entry.timeline = timeline_values_.count(semaphores[i]) != 0;
        entry.value = entry.timeline ? values[i] : 0;
        pending_signals_.push_back(entry);
    }
}

VkSubmitInfo VulkanSyncManager::build_submit_info(VkCommandBuffer command_buffer) {
    std::lock_guard<std::mutex> lock(mutex_);

    // VkTimelineSemaphoreSubmitInfo requires the first semaphoreValueCount
    // entries of the wait and signal arrays to be timeline semaphores, so the
    // lists get a stable partition (timeline first). Order between two waits
    // carries no meaning -- each has its own destination stage mask -- so this
    // is free.
    const auto hoist_timeline_first = [](std::vector<PendingSemaphore>& list) {
        std::stable_partition(list.begin(), list.end(),
                              [](const PendingSemaphore& entry) { return entry.timeline; });
    };
    hoist_timeline_first(pending_waits_);
    hoist_timeline_first(pending_signals_);

    if (!submit_order_logged_) {
        const auto is_timeline = [](const PendingSemaphore& entry) { return entry.timeline; };
        const bool any_timeline = std::any_of(pending_waits_.begin(), pending_waits_.end(),
                                              is_timeline) ||
                                  std::any_of(pending_signals_.begin(), pending_signals_.end(),
                                              is_timeline);
        submit_order_logged_ = true;
        if (any_timeline) {
            LOGI("submit info carries timeline semaphores; entries reordered timeline-first as"
                 " VkTimelineSemaphoreSubmitInfo requires");
        }
    }

    submit_.wait_semaphores.clear();
    submit_.wait_stages.clear();
    submit_.wait_values.clear();
    submit_.signal_semaphores.clear();
    submit_.signal_values.clear();
    submit_.wait_value_count = 0;
    submit_.signal_value_count = 0;

    for (const auto& entry : pending_waits_) {
        submit_.wait_semaphores.push_back(entry.semaphore);
        submit_.wait_stages.push_back(entry.stage_mask);
        if (entry.timeline) {
            submit_.wait_values.push_back(entry.value);
            ++submit_.wait_value_count;
        }
    }
    for (const auto& entry : pending_signals_) {
        submit_.signal_semaphores.push_back(entry.semaphore);
        if (entry.timeline) {
            submit_.signal_values.push_back(entry.value);
            ++submit_.signal_value_count;
        }
    }

    // WHY a member and not the argument: pCommandBuffers must stay valid after
    // this function returns, and a by-value parameter does not.
    submit_.command_buffer = command_buffer;

    submit_.info = VkSubmitInfo{};
    submit_.info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_.info.commandBufferCount = (command_buffer != VK_NULL_HANDLE) ? 1u : 0u;
    submit_.info.pCommandBuffers = &submit_.command_buffer;
    submit_.info.waitSemaphoreCount = static_cast<uint32_t>(submit_.wait_semaphores.size());
    submit_.info.pWaitSemaphores = submit_.wait_semaphores.empty()
                                      ? nullptr
                                      : submit_.wait_semaphores.data();
    submit_.info.pWaitDstStageMask = submit_.wait_stages.empty() ? nullptr : submit_.wait_stages.data();
    submit_.info.signalSemaphoreCount = static_cast<uint32_t>(submit_.signal_semaphores.size());
    submit_.info.pSignalSemaphores = submit_.signal_semaphores.empty()
                                        ? nullptr
                                        : submit_.signal_semaphores.data();

    submit_.timeline = VkTimelineSemaphoreSubmitInfo{};
    if (submit_.wait_value_count > 0 || submit_.signal_value_count > 0) {
        submit_.timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        submit_.timeline.pWaitSemaphoreValues = submit_.wait_values.empty()
                                                   ? nullptr
                                                   : submit_.wait_values.data();
        submit_.timeline.waitSemaphoreValueCount = submit_.wait_value_count;
        submit_.timeline.pSignalSemaphoreValues = submit_.signal_values.empty()
                                                     ? nullptr
                                                     : submit_.signal_values.data();
        submit_.timeline.signalSemaphoreValueCount = submit_.signal_value_count;
        submit_.info.pNext = &submit_.timeline;
    }

    return submit_.info;
}

bool VulkanSyncManager::has_pending_submit_sync() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_waits_.empty() || !pending_signals_.empty();
}

void VulkanSyncManager::clear_pending_submit() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_waits_.clear();
    pending_signals_.clear();
    submit_.wait_semaphores.clear();
    submit_.wait_stages.clear();
    submit_.wait_values.clear();
    submit_.signal_semaphores.clear();
    submit_.signal_values.clear();
    submit_.command_buffer = VK_NULL_HANDLE;
    submit_.info = VkSubmitInfo{};
    submit_.timeline = VkTimelineSemaphoreSubmitInfo{};
    submit_.wait_value_count = 0;
    submit_.signal_value_count = 0;
}

uint32_t VulkanSyncManager::pending_wait_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(pending_waits_.size());
}

uint32_t VulkanSyncManager::pending_signal_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(pending_signals_.size());
}

} // namespace copper