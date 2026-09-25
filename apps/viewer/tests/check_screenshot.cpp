// check_screenshot FILE WIDTH HEIGHT [MODE]: sanity checks on a belter-view screenshot. Exits
// non-zero with a reason if the image is the wrong size, blank or one colour, or misses what the
// view must show:
//   system   the default view: a bright Sun at the centre
//   labels   the default view with its overlay: the Sun, its label beside it, the date in the
//            top-left corner and the hint line along the bottom
//   info     an info panel: the top-right corner, empty sky in the other views, full of text
//   plot     a plotted course: a long dashed amber line
//   sphere   a planet close-up lit from the side: a bright lit half and a dark night half
//            either side of the centre
//   cluster  the Ceres cluster: the green ring of the player's ship at the centre and amber
//            Belt stations around it

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
#include <string_view>

namespace {

int fail(const std::string& why) {
    std::cerr << "check_screenshot: " << why << '\n';
    return 1;
}

using Rgb = std::array<int, 3>;

int luma(const Rgb& c) { return (2 * c[0] + 5 * c[1] + c[2]) / 8; }

bool is_player_green(const Rgb& c) { return c[1] > 170 && c[1] - c[0] > 60 && c[1] - c[2] > 40; }
bool is_belt_amber(const Rgb& c) { return c[0] > 190 && c[1] > 110 && c[1] < 230 && c[0] - c[2] > 100; }
bool is_text_light(const Rgb& c) { return luma(c) > 170; }

} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        return fail("usage: check_screenshot FILE WIDTH HEIGHT [system|sphere|cluster|labels|info|plot]");
    }
    const int want_w = std::atoi(argv[2]);
    const int want_h = std::atoi(argv[3]);
    const std::string_view mode = argc == 5 ? argv[4] : "system";

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
        return Rgb{p[0], p[1], p[2]};
    };

    int status = 0;
    if (img->w != want_w || img->h != want_h) {
        status = fail(std::format("size {}x{}, expected {}x{}", img->w, img->h, want_w, want_h));
    } else {
        const int cx = img->w / 2;
        const int cy = img->h / 2;
        std::set<std::uint32_t> colours;
        for (int y = 0; y < img->h; y += 2) {
            for (int x = 0; x < img->w; x += 2) {
                const Rgb c = pixel(x, y);
                colours.insert(static_cast<std::uint32_t>(c[0] << 16 | c[1] << 8 | c[2]));
            }
        }
        int centre = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                centre = std::max(centre, luma(pixel(cx + dx, cy + dy)));
            }
        }
        const int corner = luma(pixel(2, img->h - 3));
        auto count = [&](int x0, int y0, int x1, int y1, auto pred) {
            int n = 0;
            for (int y = std::max(y0, 0); y < std::min(y1, img->h); ++y) {
                for (int x = std::max(x0, 0); x < std::min(x1, img->w); ++x) {
                    n += pred(pixel(x, y)) ? 1 : 0;
                }
            }
            return n;
        };
        std::cerr << std::format("check_screenshot: {} {}x{}, {} colours, centre luma {}, corner luma {}\n", mode,
                                 img->w, img->h, colours.size(), centre, corner);
        if (colours.size() < 16) {
            status = fail("image is (nearly) one colour");
        } else if (corner > 40) {
            status = fail("background is not dark");
        } else if (mode == "system") {
            if (centre < 200) {
                status = fail("no bright Sun at the centre");
            }
        } else if (mode == "labels") {
            const int date = count(0, 0, img->w / 4, img->h / 10, is_text_light);
            const int sun_label = count(cx + 10, cy - 10, cx + 60, cy + 10, is_text_light);
            const int hints = count(0, img->h - 30, img->w, img->h, [](const Rgb& c) { return luma(c) > 100; });
            std::cerr << std::format("check_screenshot: date {} px, Sun label {} px, hints {} px\n", date, sun_label,
                                     hints);
            if (centre < 200) {
                status = fail("no bright Sun at the centre");
            } else if (date < 60) {
                status = fail("no date in the top-left corner");
            } else if (sun_label < 15) {
                status = fail("no label beside the Sun");
            } else if (hints < 100) {
                status = fail("no hint line along the bottom");
            }
        } else if (mode == "info") {
            const int x0 = img->w * 7 / 10;
            const int y1 = img->h * 3 / 10;
            const int text = count(x0, 14, img->w - 14, y1, is_text_light);
            std::cerr << std::format("check_screenshot: {} px of text in the panel corner\n", text);
            if (text < 600) {
                status = fail("no info panel text in the top-right corner");
            }
        } else if (mode == "plot") {
            const int amber = count(0, 0, img->w, img->h, [](const Rgb& c) {
                return c[0] > 200 && c[1] > 140 && c[2] < 120 && c[0] - c[2] > 120;
            });
            std::cerr << std::format("check_screenshot: {} amber course pixels\n", amber);
            if (amber < 150) {
                status = fail("no plotted course");
            }
        } else if (mode == "sphere") {
            // The brightest point on the centre row is the sub-solar limb; halfway between it and
            // the centre the disc is lit, and at the mirror image of that point it is night.
            int bright_x = cx;
            for (int x = 0; x < img->w; ++x) {
                if (luma(pixel(x, cy)) > luma(pixel(bright_x, cy))) {
                    bright_x = x;
                }
            }
            const int lit_x = (cx + bright_x) / 2;
            const int night_x = 2 * cx - lit_x;
            const int peak = luma(pixel(bright_x, cy));
            const int lit = luma(pixel(lit_x, cy));
            const int night = luma(pixel(night_x, cy));
            std::cerr << std::format("check_screenshot: limb x {} luma {}, lit x {} luma {}, night x {} luma {}\n",
                                     bright_x, peak, lit_x, lit, night_x, night);
            if (peak < 120 || std::abs(bright_x - cx) < img->w / 10) {
                status = fail("no brightly lit limb off the centre");
            } else if (lit < 50) {
                status = fail("the day side is not lit");
            } else if (night > 20) {
                status = fail("the night side is not dark");
            }
        } else if (mode == "cluster") {
            int ring = 0;
            int amber = 0;
            for (int y = 0; y < img->h; ++y) {
                for (int x = 0; x < img->w; ++x) {
                    const Rgb c = pixel(x, y);
                    const bool near_centre = std::abs(x - cx) <= 12 && std::abs(y - cy) <= 12;
                    ring += near_centre && is_player_green(c) ? 1 : 0;
                    amber += !near_centre && is_belt_amber(c) ? 1 : 0;
                }
            }
            std::cerr << std::format("check_screenshot: {} ring pixels, {} amber pixels\n", ring, amber);
            if (ring < 10) {
                status = fail("no player ship ring at the centre");
            } else if (amber < 6) {
                status = fail("no Belt stations around it");
            }
        } else {
            status = fail(std::format("unknown mode '{}'", mode));
        }
    }
    SDL_DestroySurface(img);
    SDL_Quit();
    return status;
}
