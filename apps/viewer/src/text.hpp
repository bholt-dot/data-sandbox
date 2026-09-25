#pragma once

// Draws an OverlayDrawList (overlay.hpp) with SDL_ttf's GPU text engine: SDL_ttf rasterises
// glyphs with FreeType into its atlas textures and returns positioned, textured triangles per
// string (TTF_GetGPUTextDrawData); this class batches those and the overlay's solid quads into
// one vertex buffer and draws them with its own pipeline in a pass after the scene.
//
// Legibility: text origins are snapped to whole pixels and glyphs drawn 1:1 from the atlas;
// labels over the sky get a dark outline (a second font with TTF_SetFontOutline, drawn first),
// the usual halo of map labels and HUD readouts over bright, varying backgrounds.

#include "gpu.hpp"
#include "overlay.hpp"

#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace viewer {

class OverlayRenderer {
public:
    // Null on failure (SDL error set). Needs TTF_Init() to have succeeded.
    static std::unique_ptr<OverlayRenderer> create(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format,
                                                   float ui_scale);
    ~OverlayRenderer();
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    float ui_scale() const { return ui_scale_; }
    // Reopens the fonts at a new scale (window moved to a display of another density).
    bool set_ui_scale(float ui_scale);

    TextSize measure(std::string_view text, TextStyle style);

    // Turns the draw list into vertices, creating or reusing SDL_ttf text objects. New glyphs
    // reach the atlas through SDL_ttf's own command buffers, so call this before acquiring the
    // frame's command buffer.
    void prepare(const OverlayDrawList& list);
    // Records the vertex and index upload of the last prepare(). Call outside any render pass.
    bool upload(SDL_GPUCopyPass* pass);
    // Records a render pass that draws the overlay over `target` (loaded, not cleared).
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, float width, float height) const;

private:
    struct Vertex {
        float x, y, u, v;
        Rgba color;
    };
    struct Batch {
        SDL_GPUTexture* texture;
        std::uint32_t first_index;
        std::uint32_t index_count;
    };
    struct CachedText {
        TTF_Text* text = nullptr;
        std::uint64_t last_used = 0;
    };

    explicit OverlayRenderer(SDL_GPUDevice* device) : device_(device) {}
    bool open_fonts();
    void close_fonts();
    TTF_Text* text_for(std::string_view text, TextStyle style, bool outline);
    void append_quad(const OverlayQuad& quad);
    void append_text(const OverlayText& text, bool outline);
    void add_batch(SDL_GPUTexture* texture, std::uint32_t first_index);

    SDL_GPUDevice* device_;
    float ui_scale_ = 1.0F;
    TTF_TextEngine* engine_ = nullptr;
    std::array<TTF_Font*, text_style_count> fonts_{};
    std::array<TTF_Font*, text_style_count> outline_fonts_{};
    int outline_px_ = 1;
    std::unordered_map<std::string, CachedText> cache_; // key: style, outline flag, text
    std::uint64_t frame_ = 0;

    GpuPipeline pipeline_;
    GpuHandle<SDL_GPUSampler, SDL_ReleaseGPUSampler> sampler_;
    GpuTexture white_;
    DynamicBuffer vertex_buffer_{SDL_GPU_BUFFERUSAGE_VERTEX};
    DynamicBuffer index_buffer_{SDL_GPU_BUFFERUSAGE_INDEX};
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<Batch> batches_;
};

} // namespace viewer
