#include "text.hpp"

#include "belter_viewer_assets.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace viewer {

namespace {

constexpr std::string_view font_file = "FiraSans-Medium.ttf";
constexpr Rgba outline_color{0.0F, 0.0F, 0.0F, 0.85F};
constexpr std::uint64_t evict_after_frames = 240; // unused strings go after ~4 s at 60 Hz

TTF_Font* open_font(float px) {
    const std::span<const unsigned char> bytes = embedded_file(font_file);
    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (io == nullptr) {
        return nullptr;
    }
    TTF_Font* font = TTF_OpenFontIO(io, true, px); // closes io, also on failure
    if (font != nullptr) {
        TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    }
    return font;
}

} // namespace

std::unique_ptr<OverlayRenderer> OverlayRenderer::create(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format,
                                                         float ui_scale) {
    std::unique_ptr<OverlayRenderer> r(new OverlayRenderer(device));
    r->ui_scale_ = std::max(ui_scale, 0.5F);
    r->engine_ = TTF_CreateGPUTextEngine(device);
    if (r->engine_ == nullptr || !r->open_fonts()) {
        return nullptr;
    }
    // SDL_GPU's convention (y up) is what we flip; keep SDL_ttf's default winding, no culling.

    GpuShader vs = load_shader(device, "overlay.vert", {.uniform_buffers = 1});
    GpuShader fs = load_shader(device, "overlay.frag", {.samplers = 1});
    if (!vs || !fs) {
        return nullptr;
    }
    SDL_GPUColorTargetDescription target{};
    target.format = target_format;
    target.blend_state.enable_blend = true; // premultiplied alpha
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    const std::array<SDL_GPUVertexAttribute, 3> attributes{{
        {.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0},
        {.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 8},
        {.location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = 16},
    }};
    static_assert(sizeof(Vertex) == 32);

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs.get();
    info.fragment_shader = fs.get();
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.vertex_input_state.vertex_buffer_descriptions = &vb;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes.data();
    info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(attributes.size());
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = false;
    r->pipeline_ = GpuPipeline{device, SDL_CreateGPUGraphicsPipeline(device, &info)};
    if (!r->pipeline_) {
        return nullptr;
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    r->sampler_ = {device, SDL_CreateGPUSampler(device, &sampler_info)};

    // A 1x1 white texel: solid quads share the text pipeline.
    SDL_GPUTextureCreateInfo tex{};
    tex.type = SDL_GPU_TEXTURETYPE_2D;
    tex.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex.width = 1;
    tex.height = 1;
    tex.layer_count_or_depth = 1;
    tex.num_levels = 1;
    r->white_ = GpuTexture{device, SDL_CreateGPUTexture(device, &tex)};
    const SDL_GPUTransferBufferCreateInfo tb_info{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4, .props = 0};
    GpuTransferBuffer transfer{device, SDL_CreateGPUTransferBuffer(device, &tb_info)};
    if (!r->sampler_ || !r->white_ || !transfer) {
        return nullptr;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer.get(), false);
    if (mapped == nullptr) {
        return nullptr;
    }
    std::memset(mapped, 0xff, 4);
    SDL_UnmapGPUTransferBuffer(device, transfer.get());
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (cmd == nullptr) {
        return nullptr;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = transfer.get();
    SDL_GPUTextureRegion dst{};
    dst.texture = r->white_.get();
    dst.w = 1;
    dst.h = 1;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        return nullptr;
    }
    return r;
}

OverlayRenderer::~OverlayRenderer() {
    close_fonts();
    if (engine_ != nullptr) {
        TTF_DestroyGPUTextEngine(engine_);
    }
}

bool OverlayRenderer::open_fonts() {
    outline_px_ = std::max(1, static_cast<int>(std::lround(ui_scale_)));
    for (std::size_t i = 0; i < text_style_count; ++i) {
        const float px = std::round(text_style_px[i] * ui_scale_);
        fonts_[i] = open_font(px);
        outline_fonts_[i] = open_font(px);
        if (fonts_[i] == nullptr || outline_fonts_[i] == nullptr ||
            !TTF_SetFontOutline(outline_fonts_[i], outline_px_)) {
            return false;
        }
    }
    return true;
}

void OverlayRenderer::close_fonts() {
    for (auto& [key, cached] : cache_) {
        TTF_DestroyText(cached.text);
    }
    cache_.clear();
    for (TTF_Font*& f : fonts_) {
        if (f != nullptr) {
            TTF_CloseFont(f);
            f = nullptr;
        }
    }
    for (TTF_Font*& f : outline_fonts_) {
        if (f != nullptr) {
            TTF_CloseFont(f);
            f = nullptr;
        }
    }
}

bool OverlayRenderer::set_ui_scale(float ui_scale) {
    ui_scale = std::max(ui_scale, 0.5F);
    if (ui_scale == ui_scale_) {
        return true;
    }
    close_fonts();
    ui_scale_ = ui_scale;
    return open_fonts();
}

TextSize OverlayRenderer::measure(std::string_view text, TextStyle style) {
    TTF_Font* font = fonts_[static_cast<std::size_t>(style)];
    int w = 0;
    int h = 0;
    if (!text.empty()) {
        TTF_GetStringSize(font, text.data(), text.size(), &w, &h);
    }
    return {static_cast<float>(w), static_cast<float>(TTF_GetFontHeight(font))};
}

TTF_Text* OverlayRenderer::text_for(std::string_view text, TextStyle style, bool outline) {
    std::string key;
    key.reserve(text.size() + 2);
    key += static_cast<char>('0' + static_cast<int>(style));
    key += outline ? 'o' : 'f';
    key += text;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        const std::size_t i = static_cast<std::size_t>(style);
        TTF_Text* t = TTF_CreateText(engine_, outline ? outline_fonts_[i] : fonts_[i], text.data(), text.size());
        if (t == nullptr) {
            return nullptr;
        }
        it = cache_.emplace(std::move(key), CachedText{t, frame_}).first;
    }
    it->second.last_used = frame_;
    return it->second.text;
}

void OverlayRenderer::add_batch(SDL_GPUTexture* texture, std::uint32_t first_index) {
    if (batches_.empty() || batches_.back().texture != texture) {
        batches_.push_back({texture, first_index, 0});
    }
}

void OverlayRenderer::append_quad(const OverlayQuad& quad) {
    const auto base = static_cast<std::uint32_t>(vertices_.size());
    add_batch(white_.get(), static_cast<std::uint32_t>(indices_.size()));
    for (std::size_t c = 0; c < 4; ++c) {
        vertices_.push_back({quad.xy[2 * c], quad.xy[2 * c + 1], 0.5F, 0.5F, quad.color});
    }
    for (const std::uint32_t i : {0U, 1U, 2U, 0U, 2U, 3U}) {
        indices_.push_back(base + i);
    }
    batches_.back().index_count += 6;
}

void OverlayRenderer::append_text(const OverlayText& text, bool outline) {
    if (text.text.empty()) {
        return;
    }
    TTF_Text* t = text_for(text.text, text.style, outline);
    if (t == nullptr) {
        return;
    }
    // The outlined glyphs grow by the outline on every side; shift them back over the fill.
    const float shift = outline ? static_cast<float>(outline_px_) : 0.0F;
    const Rgba color = outline ? Rgba{outline_color.r, outline_color.g, outline_color.b, outline_color.a * text.color.a}
                               : text.color;
    for (const TTF_GPUAtlasDrawSequence* seq = TTF_GetGPUTextDrawData(t); seq != nullptr; seq = seq->next) {
        const auto base = static_cast<std::uint32_t>(vertices_.size());
        add_batch(seq->atlas_texture, static_cast<std::uint32_t>(indices_.size()));
        for (int v = 0; v < seq->num_vertices; ++v) {
            // SDL_ttf's y axis points up from the top of the line; ours points down.
            vertices_.push_back({text.x - shift + seq->xy[v].x, text.y - shift - seq->xy[v].y, seq->uv[v].x,
                                 seq->uv[v].y, color});
        }
        for (int i = 0; i < seq->num_indices; ++i) {
            indices_.push_back(base + static_cast<std::uint32_t>(seq->indices[i]));
        }
        batches_.back().index_count += static_cast<std::uint32_t>(seq->num_indices);
    }
}

void OverlayRenderer::prepare(const OverlayDrawList& list) {
    ++frame_;
    vertices_.clear();
    indices_.clear();
    batches_.clear();
    for (const OverlayLayer& layer : list.layers) {
        for (const OverlayQuad& q : layer.quads) {
            append_quad(q);
        }
        for (const OverlayText& t : layer.texts) {
            if (t.outline) {
                append_text(t, true);
            }
        }
        for (const OverlayText& t : layer.texts) {
            append_text(t, false);
        }
    }
    std::erase_if(cache_, [this](const auto& entry) {
        if (frame_ - entry.second.last_used > evict_after_frames) {
            TTF_DestroyText(entry.second.text);
            return true;
        }
        return false;
    });
}

bool OverlayRenderer::upload(SDL_GPUCopyPass* pass) {
    return vertex_buffer_.upload(device_, pass, std::as_bytes(std::span(vertices_))) &&
           index_buffer_.upload(device_, pass, std::as_bytes(std::span(indices_)));
}

void OverlayRenderer::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, float width, float height) const {
    if (batches_.empty() || vertex_buffer_.get() == nullptr || index_buffer_.get() == nullptr) {
        return;
    }
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    const std::array<float, 4> viewport{width, height, 0.0F, 0.0F};
    SDL_PushGPUVertexUniformData(cmd, 0, viewport.data(), sizeof(viewport));
    const SDL_GPUBufferBinding vb{.buffer = vertex_buffer_.get(), .offset = 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    const SDL_GPUBufferBinding ib{.buffer = index_buffer_.get(), .offset = 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for (const Batch& b : batches_) {
        const SDL_GPUTextureSamplerBinding binding{.texture = b.texture, .sampler = sampler_.get()};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUIndexedPrimitives(pass, b.index_count, 1, b.first_index, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
}

} // namespace viewer
