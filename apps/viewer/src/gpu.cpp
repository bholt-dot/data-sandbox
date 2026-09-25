#include "gpu.hpp"

#include "belter_viewer_shaders.hpp"

#include <SDL3/SDL_error.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace viewer {

GpuShader load_shader(SDL_GPUDevice* device, std::string_view name, ShaderResources resources) {
    const std::span<const std::uint32_t> words = embedded_shader(name);
    if (words.empty()) {
        SDL_SetError("no embedded shader '%s'", std::string(name).c_str());
        return {};
    }
    SDL_GPUShaderStage stage{};
    if (name.ends_with(".vert")) {
        stage = SDL_GPU_SHADERSTAGE_VERTEX;
    } else if (name.ends_with(".frag")) {
        stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    } else {
        SDL_SetError("unknown shader stage for '%s'", std::string(name).c_str());
        return {};
    }
    const SDL_GPUShaderCreateInfo info{
        .code_size = words.size_bytes(),
        .code = reinterpret_cast<const Uint8*>(words.data()),
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = stage,
        .num_samplers = resources.samplers,
        .num_storage_textures = resources.storage_textures,
        .num_storage_buffers = resources.storage_buffers,
        .num_uniform_buffers = resources.uniform_buffers,
        .props = 0,
    };
    return {device, SDL_CreateGPUShader(device, &info)};
}

bool DynamicBuffer::upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass, std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return true;
    }
    const auto size = static_cast<std::uint32_t>(bytes.size());
    if (size > capacity_) {
        const std::uint32_t capacity = std::max({size, capacity_ * 2, std::uint32_t{4096}});
        const SDL_GPUBufferCreateInfo buffer_info{.usage = usage_, .size = capacity, .props = 0};
        const SDL_GPUTransferBufferCreateInfo transfer_info{
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = capacity, .props = 0};
        GpuBuffer buffer{device, SDL_CreateGPUBuffer(device, &buffer_info)};
        GpuTransferBuffer transfer{device, SDL_CreateGPUTransferBuffer(device, &transfer_info)};
        if (!buffer || !transfer) {
            return false;
        }
        buffer_ = std::move(buffer);
        transfer_ = std::move(transfer);
        capacity_ = capacity;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer_.get(), true);
    if (mapped == nullptr) {
        return false;
    }
    std::memcpy(mapped, bytes.data(), bytes.size());
    SDL_UnmapGPUTransferBuffer(device, transfer_.get());

    const SDL_GPUTransferBufferLocation src{.transfer_buffer = transfer_.get(), .offset = 0};
    const SDL_GPUBufferRegion dst{.buffer = buffer_.get(), .offset = 0, .size = size};
    SDL_UploadToGPUBuffer(pass, &src, &dst, true);
    return true;
}

} // namespace viewer
