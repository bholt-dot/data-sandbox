#pragma once

// Thin RAII over SDL GPU objects. SDL defers the actual destruction of released resources until
// the GPU has finished with them, so releasing while a frame is in flight is safe.

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace viewer {

template <typename T, void (*Release)(SDL_GPUDevice*, T*)>
class GpuHandle {
public:
    GpuHandle() = default;
    GpuHandle(SDL_GPUDevice* device, T* ptr) : device_(device), ptr_(ptr) {}
    GpuHandle(GpuHandle&& o) noexcept
        : device_(std::exchange(o.device_, nullptr)), ptr_(std::exchange(o.ptr_, nullptr)) {}
    GpuHandle& operator=(GpuHandle&& o) noexcept {
        if (this != &o) {
            reset();
            device_ = std::exchange(o.device_, nullptr);
            ptr_ = std::exchange(o.ptr_, nullptr);
        }
        return *this;
    }
    GpuHandle(const GpuHandle&) = delete;
    GpuHandle& operator=(const GpuHandle&) = delete;
    ~GpuHandle() { reset(); }

    void reset() {
        if (ptr_ != nullptr) {
            Release(device_, ptr_);
        }
        ptr_ = nullptr;
    }
    T* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    SDL_GPUDevice* device_ = nullptr;
    T* ptr_ = nullptr;
};

using GpuBuffer = GpuHandle<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>;
using GpuTransferBuffer = GpuHandle<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer>;
using GpuTexture = GpuHandle<SDL_GPUTexture, SDL_ReleaseGPUTexture>;
using GpuShader = GpuHandle<SDL_GPUShader, SDL_ReleaseGPUShader>;
using GpuPipeline = GpuHandle<SDL_GPUGraphicsPipeline, SDL_ReleaseGPUGraphicsPipeline>;

// Resource counts SDL needs for a shader; they must match the shader's declared bindings.
struct ShaderResources {
    std::uint32_t samplers = 0;
    std::uint32_t storage_textures = 0;
    std::uint32_t storage_buffers = 0;
    std::uint32_t uniform_buffers = 0;
};

// Creates a shader from the embedded SPIR-V `name` ("sprite.vert"); the stage comes from the
// extension. Returns an empty handle and sets SDL's error on failure.
GpuShader load_shader(SDL_GPUDevice* device, std::string_view name, ShaderResources resources);

// A GPU buffer refilled from the CPU every time its contents change (per frame for sprites).
// Uploads cycle both the transfer buffer and the GPU buffer, so a frame still in flight keeps
// reading the old contents. Grows geometrically.
class DynamicBuffer {
public:
    explicit DynamicBuffer(SDL_GPUBufferUsageFlags usage) : usage_(usage) {}

    // Records the upload into `pass`. Returns false (SDL error set) on allocation failure.
    bool upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass, std::span<const std::byte> bytes);
    SDL_GPUBuffer* get() const { return buffer_.get(); }

private:
    SDL_GPUBufferUsageFlags usage_;
    std::uint32_t capacity_ = 0;
    GpuBuffer buffer_;
    GpuTransferBuffer transfer_;
};

} // namespace viewer
