// check_screenshot FILE WIDTH HEIGHT: sanity checks on a belter-view screenshot of the default
// view. Exits non-zero with a reason if the image is the wrong size, blank or one colour, or the
// Sun (always at the centre of the default camera) isn't bright there.

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <set>
#include <string>

namespace {

int fail(const std::string& why) {
    std::cerr << "check_screenshot: " << why << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return fail("usage: check_screenshot FILE WIDTH HEIGHT");
    }
    const int want_w = std::atoi(argv[2]);
    const int want_h = std::atoi(argv[3]);

    SDL_Surface* loaded = SDL_LoadPNG(argv[1]);
    if (loaded == nullptr) {
        return fail(std::format("cannot load {}: {}", argv[1], SDL_GetError()));
    }
    SDL_Surface* img = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (img == nullptr) {
        return fail(SDL_GetError());
    }
    auto pixel = [img](int x, int y) {
        const auto* row = static_cast<const std::uint8_t*>(img->pixels) + static_cast<std::ptrdiff_t>(y) * img->pitch;
        const std::uint8_t* p = row + static_cast<std::ptrdiff_t>(x) * 4;
        return std::array<int, 3>{p[0], p[1], p[2]};
    };
    auto luma = [](const std::array<int, 3>& c) { return (2 * c[0] + 5 * c[1] + c[2]) / 8; };

    int status = 0;
    if (img->w != want_w || img->h != want_h) {
        status = fail(std::format("size {}x{}, expected {}x{}", img->w, img->h, want_w, want_h));
    } else {
        std::set<std::uint32_t> colours;
        for (int y = 0; y < img->h; y += 2) {
            for (int x = 0; x < img->w; x += 2) {
                const auto c = pixel(x, y);
                colours.insert(static_cast<std::uint32_t>(c[0] << 16 | c[1] << 8 | c[2]));
            }
        }
        int centre = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                centre = std::max(centre, luma(pixel(img->w / 2 + dx, img->h / 2 + dy)));
            }
        }
        const int corner = luma(pixel(2, 2));
        std::cerr << std::format("check_screenshot: {}x{}, {} colours, centre luma {}, corner luma {}\n", img->w,
                                 img->h, colours.size(), centre, corner);
        if (colours.size() < 16) {
            status = fail("image is (nearly) one colour");
        } else if (centre < 200) {
            status = fail("no bright Sun at the centre");
        } else if (corner > 40) {
            status = fail("background is not dark");
        }
    }
    SDL_DestroySurface(img);
    SDL_Quit();
    return status;
}
