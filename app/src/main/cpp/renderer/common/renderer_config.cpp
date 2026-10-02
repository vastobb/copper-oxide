#include "renderer_base.h"
#include <unordered_map>
#include <mutex>

namespace copper {

class BufferManager {
public:
    struct Buffer {
        uint64_t handle = 0;
        uint64_t size = 0;
        uint32_t usage = 0;
        uint32_t memory_flags = 0;
        void* mapped_ptr = nullptr;
        bool is_mapped = false;
        std::string debug_name;
    };

    virtual ~BufferManager() = default;

    virtual uint64_t create_buffer(uint64_t size, uint32_t usage, uint32_t memory_flags) = 0;
    virtual void destroy_buffer(uint64_t handle) = 0;
    virtual void* map_buffer(uint64_t handle, uint64_t offset = 0, uint64_t size = 0) = 0;
    virtual void unmap_buffer(uint64_t handle) = 0;
    virtual void flush_buffer(uint64_t handle, uint64_t offset, uint64_t size) = 0;
    virtual void invalidate_buffer(uint64_t handle, uint64_t offset, uint64_t size) = 0;
    virtual void update_buffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) = 0;
    virtual void copy_buffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset = 0, uint64_t dst_offset = 0) = 0;

    virtual const Buffer* get_buffer(uint64_t handle) const = 0;
    virtual uint64_t get_buffer_size(uint64_t handle) const = 0;
    virtual void set_buffer_debug_name(uint64_t handle, const std::string& name) = 0;

    // Pool management
    virtual uint64_t allocate_from_pool(uint64_t size, uint32_t usage) = 0;
    virtual void return_to_pool(uint64_t handle) = 0;
    virtual void trim_pool() = 0;
};

class TextureManager {
public:
    struct Texture {
        uint64_t handle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t mip_levels = 1;
        uint32_t array_layers = 1;
        uint32_t format = 0;
        uint32_t usage = 0;
        uint32_t sample_count = 1;
        std::string debug_name;
    };

    virtual ~TextureManager() = default;

    virtual uint64_t create_texture_2d(uint32_t width, uint32_t height, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) = 0;
    virtual uint64_t create_texture_3d(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, uint32_t usage) = 0;
    virtual uint64_t create_texture_cube(uint32_t size, uint32_t format, uint32_t usage, uint32_t mip_levels = 1) = 0;
    virtual uint64_t create_texture_array(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t usage) = 0;
    virtual void destroy_texture(uint64_t handle) = 0;

    virtual void upload_texture_data(uint64_t handle, uint32_t mip_level, uint32_t array_layer, const void* data, uint64_t size) = 0;
    virtual void upload_compressed_texture_data(uint64_t handle, uint32_t mip_level, uint32_t array_layer, const void* data, uint64_t size, uint32_t format) = 0;
    virtual void generate_mipmaps(uint64_t handle) = 0;
    virtual void copy_texture(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth = 1) = 0;
    virtual void blit_texture(uint64_t src, uint64_t dst, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height, uint32_t filter) = 0;

    virtual const Texture* get_texture(uint64_t handle) const = 0;
    virtual void set_texture_debug_name(uint64_t handle, const std::string& name) = 0;

    // Streaming
    virtual uint64_t create_streaming_texture(uint32_t width, uint32_t height, uint32_t format) = 0;
    virtual void update_streaming_texture(uint64_t handle, const void* data, uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

    // Compression
    virtual bool supports_format(uint32_t format) const = 0;
    virtual uint32_t get_compressed_format(uint32_t base_format) const = 0;
};

class ShaderManager {
public:
    struct ShaderModule {
        uint64_t handle = 0;
        uint32_t stage = 0;
        std::vector<uint32_t> spirv_code;
        std::string entry_point;
        std::string debug_name;
    };

    struct ShaderProgram {
        uint64_t handle = 0;
        std::array<uint64_t, 5> stages = {0, 0, 0, 0, 0}; // vert, frag, comp, geom, tesc, tese
        std::unordered_map<std::string, uint32_t> uniform_locations;
        std::unordered_map<std::string, uint32_t> attribute_locations;
        std::unordered_map<std::string, uint32_t> storage_buffer_bindings;
        std::unordered_map<std::string, uint32_t> texture_bindings;
        std::unordered_map<std::string, uint32_t> sampler_bindings;
        std::string debug_name;
    };

    virtual ~ShaderManager() = default;

    virtual uint64_t create_shader_module(uint32_t stage, const uint32_t* spirv, uint32_t word_count, const std::string& entry_point = "main") = 0;
    virtual void destroy_shader_module(uint64_t handle) = 0;

    virtual uint64_t create_shader_program(const std::vector<uint64_t>& stages) = 0;
    virtual void destroy_shader_program(uint64_t handle) = 0;

    virtual bool compile_shader_from_source(uint32_t stage, const std::string& source, const std::string& entry_point, std::vector<uint32_t>& out_spirv, std::string& out_error) = 0;
    virtual bool translate_glsl_to_spirv(uint32_t stage, const std::string& glsl, const std::string& entry_point, std::vector<uint32_t>& out_spirv, std::string& out_error) = 0;

    virtual const ShaderModule* get_shader_module(uint64_t handle) const = 0;
    virtual const ShaderProgram* get_shader_program(uint64_t handle) const = 0;

    virtual int get_uniform_location(uint64_t program, const std::string& name) = 0;
    virtual int get_attribute_location(uint64_t program, const std::string& name) = 0;
    virtual int get_storage_buffer_binding(uint64_t program, const std::string& name) = 0;
    virtual int get_texture_binding(uint64_t program, const std::string& name) = 0;
    virtual int get_sampler_binding(uint64_t program, const std::string& name) = 0;

    virtual void set_shader_debug_name(uint64_t handle, const std::string& name) = 0;

    // Caching
    virtual void cache_shader_program(const std::string& key, uint64_t program) = 0;
    virtual uint64_t get_cached_shader_program(const std::string& key) = 0;
    virtual void clear_shader_cache() = 0;

    // Precompilation
    virtual void precompile_shader_async(uint64_t program) = 0;
    virtual bool is_shader_compiled(uint64_t program) = 0;
    virtual void wait_for_shader_compilation(uint64_t program) = 0;
};

class FramebufferManager {
public:
    struct Attachment {
        uint64_t texture_handle = 0;
        uint32_t mip_level = 0;
        uint32_t array_layer = 0;
        uint32_t load_op = 0;
        uint32_t store_op = 0;
        uint32_t stencil_load_op = 0;
        uint32_t stencil_store_op = 0;
        float clear_color[4] = {0, 0, 0, 1};
        float clear_depth = 1.0f;
        uint32_t clear_stencil = 0;
    };

    struct Framebuffer {
        uint64_t handle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<Attachment> color_attachments;
        Attachment depth_stencil_attachment;
        uint32_t sample_count = 1;
        std::string debug_name;
    };

    virtual ~FramebufferManager() = default;

    virtual uint64_t create_framebuffer(uint32_t width, uint32_t height, const std::vector<Attachment>& color_attachments, const Attachment& depth_stencil = {}) = 0;
    virtual void destroy_framebuffer(uint64_t handle) = 0;
    virtual void begin_render_pass(uint64_t framebuffer) = 0;
    virtual void end_render_pass() = 0;
    virtual void set_viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float min_depth = 0.0f, float max_depth = 1.0f) = 0;
    virtual void set_scissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

    virtual const Framebuffer* get_framebuffer(uint64_t handle) const = 0;
    virtual void set_framebuffer_debug_name(uint64_t handle, const std::string& name) = 0;

    // Render target management
    virtual uint64_t create_render_target(uint32_t width, uint32_t height, uint32_t color_format, uint32_t depth_format, uint32_t samples = 1) = 0;
    virtual void destroy_render_target(uint64_t handle) = 0;
    virtual void resolve_render_target(uint64_t src, uint64_t dst) = 0;
};

class StateManager {
public:
    struct PipelineState {
        uint64_t shader_program = 0;
        uint32_t primitive_topology = 0;
        uint32_t polygon_mode = 0;
        uint32_t cull_mode = 0;
        uint32_t front_face = 0;
        bool depth_test = true;
        bool depth_write = true;
        uint32_t depth_compare = 0;
        bool stencil_test = false;
        uint32_t stencil_read_mask = 0xFF;
        uint32_t stencil_write_mask = 0xFF;
        uint32_t stencil_fail_op = 0;
        uint32_t stencil_pass_depth_fail_op = 0;
        uint32_t stencil_pass_depth_pass_op = 0;
        uint32_t stencil_compare = 0;
        uint32_t stencil_reference = 0;
        bool blend_enable = false;
        uint32_t src_blend = 0;
        uint32_t dst_blend = 0;
        uint32_t blend_op = 0;
        uint32_t src_blend_alpha = 0;
        uint32_t dst_blend_alpha = 0;
        uint32_t blend_op_alpha = 0;
        float blend_constants[4] = {0, 0, 0, 0};
        uint32_t color_write_mask = 0xF;
        bool sample_shading = false;
        float min_sample_shading = 1.0f;
        uint32_t sample_mask = 0xFFFFFFFF;
        bool alpha_to_coverage = false;
        bool alpha_to_one = false;
    };

    virtual ~StateManager() = default;

    virtual void bind_pipeline(uint64_t pipeline) = 0;
    virtual void set_pipeline_state(const PipelineState& state) = 0;
    virtual void reset_pipeline_state() = 0;

    // Vertex input
    virtual void bind_vertex_buffer(uint32_t binding, uint64_t buffer, uint64_t offset, uint32_t stride) = 0;
    virtual void bind_index_buffer(uint64_t buffer, uint64_t offset, uint32_t index_type) = 0;
    virtual void set_vertex_attribute(uint32_t location, uint32_t binding, uint32_t format, uint64_t offset) = 0;
    virtual void enable_vertex_attribute(uint32_t location) = 0;
    virtual void disable_vertex_attribute(uint32_t location) = 0;

    // Resources
    virtual void bind_uniform_buffer(uint32_t binding, uint64_t buffer, uint64_t offset, uint64_t range) = 0;
    virtual void bind_storage_buffer(uint32_t binding, uint64_t buffer, uint64_t offset, uint64_t range) = 0;
    virtual void bind_texture(uint32_t binding, uint64_t texture, uint64_t sampler = 0) = 0;
    virtual void bind_sampler(uint32_t binding, uint64_t sampler) = 0;
    virtual void bind_texture_array(uint32_t binding, const std::vector<uint64_t>& textures, const std::vector<uint64_t>& samplers) = 0;

    // Push constants
    virtual void push_constants(uint32_t stage_flags, uint32_t offset, uint32_t size, const void* data) = 0;

    // Draw commands
    virtual void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) = 0;
    virtual void draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) = 0;
    virtual void draw_indirect(uint64_t buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) = 0;
    virtual void draw_indexed_indirect(uint64_t buffer, uint64_t offset, uint32_t draw_count, uint32_t stride) = 0;
    virtual void draw_indirect_count(uint64_t buffer, uint64_t offset, uint64_t count_buffer, uint64_t count_offset, uint32_t max_draw_count, uint32_t stride) = 0;
    virtual void draw_indexed_indirect_count(uint64_t buffer, uint64_t offset, uint64_t count_buffer, uint64_t count_offset, uint32_t max_draw_count, uint32_t stride) = 0;

    // Dispatch
    virtual void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) = 0;
    virtual void dispatch_indirect(uint64_t buffer, uint64_t offset) = 0;

    // State caching
    virtual void enable_state_caching(bool enable) = 0;
    virtual void flush_state_cache() = 0;
    virtual uint32_t get_state_change_count() const = 0;
};

class CommandBuffer {
public:
    enum class Level { Primary, Secondary };
    enum class Usage { OneTimeSubmit, RenderPassContinue, SimultaneousUse };

    virtual ~CommandBuffer() = default;

    virtual bool begin(Usage usage = Usage::OneTimeSubmit) = 0;
    virtual void end() = 0;
    virtual void reset() = 0;

    virtual void begin_render_pass(uint64_t framebuffer, const std::vector<uint32_t>& clear_values) = 0;
    virtual void end_render_pass() = 0;
    virtual void next_subpass(uint32_t contents) = 0;

    virtual void execute_commands(const std::vector<uint64_t>& secondary_buffers) = 0;

    virtual void pipeline_barrier(uint32_t src_stage, uint32_t dst_stage, uint32_t deps, const std::vector<MemoryBarrier>& memory_barriers, const std::vector<BufferBarrier>& buffer_barriers, const std::vector<ImageBarrier>& image_barriers) = 0;

    virtual void copy_buffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) = 0;
    virtual void copy_image(uint64_t src, uint64_t dst, uint32_t width, uint32_t height, uint32_t depth) = 0;
    virtual void blit_image(uint64_t src, uint64_t dst, uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height, uint32_t filter) = 0;

    virtual void insert_debug_marker(const std::string& marker) = 0;
    virtual void push_debug_group(const std::string& name) = 0;
    virtual void pop_debug_group() = 0;
};

struct MemoryBarrier {
    uint32_t src_access = 0;
    uint32_t dst_access = 0;
};

struct BufferBarrier {
    uint64_t buffer = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t src_access = 0;
    uint32_t dst_access = 0;
    uint32_t src_queue = 0;
    uint32_t dst_queue = 0;
};

struct ImageBarrier {
    uint64_t image = 0;
    uint32_t src_layout = 0;
    uint32_t dst_layout = 0;
    uint32_t src_access = 0;
    uint32_t dst_access = 0;
    uint32_t aspect_mask = 0;
    uint32_t base_mip = 0;
    uint32_t level_count = 1;
    uint32_t base_array = 0;
    uint32_t layer_count = 1;
    uint32_t src_queue = 0;
    uint32_t dst_queue = 0;
};

class SyncManager {
public:
    struct Fence {
        uint64_t handle = 0;
        bool signaled = false;
    };

    struct Semaphore {
        uint64_t handle = 0;
    };

    struct TimelineSemaphore {
        uint64_t handle = 0;
        uint64_t current_value = 0;
    };

    virtual ~SyncManager() = default;

    virtual Fence create_fence(bool signaled = false) = 0;
    virtual void destroy_fence(Fence& fence) = 0;
    virtual bool wait_fence(const Fence& fence, uint64_t timeout_ns) = 0;
    virtual void reset_fence(Fence& fence) = 0;
    virtual bool is_fence_signaled(const Fence& fence) = 0;

    virtual Semaphore create_semaphore() = 0;
    virtual void destroy_semaphore(Semaphore& semaphore) = 0;

    virtual TimelineSemaphore create_timeline_semaphore(uint64_t initial_value = 0) = 0;
    virtual void destroy_timeline_semaphore(TimelineSemaphore& semaphore) = 0;
    virtual bool wait_timeline_semaphore(TimelineSemaphore& semaphore, uint64_t value, uint64_t timeout_ns) = 0;
    virtual void signal_timeline_semaphore(TimelineSemaphore& semaphore, uint64_t value) = 0;
    virtual uint64_t get_timeline_semaphore_value(const TimelineSemaphore& semaphore) = 0;

    virtual void submit_command_buffer(uint64_t command_buffer, const std::vector<Semaphore>& wait_semaphores, const std::vector<uint32_t>& wait_stages, const std::vector<Semaphore>& signal_semaphores, Fence* fence = nullptr) = 0;
    virtual void submit_command_buffers(const std::vector<uint64_t>& command_buffers, const std::vector<Semaphore>& wait_semaphores, const std::vector<uint32_t>& wait_stages, const std::vector<Semaphore>& signal_semaphores, Fence* fence = nullptr) = 0;

    virtual void wait_idle() = 0;
};

class ResourcePool {
public:
    struct PoolStats {
        uint64_t total_allocated = 0;
        uint64_t total_used = 0;
        uint32_t active_objects = 0;
        uint32_t pooled_objects = 0;
        uint32_t allocations = 0;
        uint32_t deallocations = 0;
        uint32_t cache_hits = 0;
        uint32_t cache_misses = 0;
    };

    virtual ~ResourcePool() = default;

    virtual void initialize(uint64_t buffer_pool_size, uint64_t texture_pool_size, uint64_t shader_pool_size) = 0;
    virtual void shutdown() = 0;

    virtual uint64_t acquire_buffer(uint64_t size, uint32_t usage) = 0;
    virtual void release_buffer(uint64_t handle) = 0;
    virtual uint64_t acquire_texture(uint32_t width, uint32_t height, uint32_t format, uint32_t usage) = 0;
    virtual void release_texture(uint64_t handle) = 0;
    virtual uint64_t acquire_shader_program(const std::string& key) = 0;
    virtual void release_shader_program(uint64_t handle) = 0;

    virtual void trim() = 0;
    virtual void clear() = 0;
    virtual PoolStats get_stats() const = 0;
    virtual void set_max_pool_size(uint64_t buffer_size, uint64_t texture_size, uint64_t shader_size) = 0;
};

class Profiler {
public:
    struct ProfileScope {
        const char* name;
        uint64_t start_time;
        uint32_t depth;
    };

    struct GPUTimer {
        uint64_t handle = 0;
        double elapsed_ms = 0.0;
        bool resolved = false;
    };

    virtual ~Profiler() = default;

    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    virtual void begin_scope(const char* name) = 0;
    virtual void end_scope() = 0;

    virtual GPUTimer create_gpu_timer() = 0;
    virtual void destroy_gpu_timer(GPUTimer& timer) = 0;
    virtual void begin_gpu_timer(GPUTimer& timer) = 0;
    virtual void end_gpu_timer(GPUTimer& timer) = 0;
    virtual double get_gpu_timer_elapsed(const GPUTimer& timer) = 0;

    virtual void record_metric(const char* name, double value) = 0;
    virtual void record_counter(const char* name, uint64_t value) = 0;

    virtual std::string get_frame_report() = 0;
    virtual std::string get_summary_report() = 0;

    virtual void set_enabled(bool enabled) = 0;
    virtual bool is_enabled() const = 0;
};

} // namespace copper