#pragma once

#include <array>
#include <cstdint>

namespace plumsg {

// Which hardware layer produced a pixel. The pseudo-3D renderer pushes each one to a
// different depth instead of flattening them. Mars is the 32X's bitmap.
enum class MdLayer : uint8_t { Backdrop = 0, PlaneB = 1, PlaneA = 2, Window = 3, Sprite = 4, Mars = 5 };

// Shadow/highlight state of a pixel (VDP register 12 bit 3).
enum class MdShade : uint8_t { Normal = 0, Shadow = 1, Highlight = 2 };

struct MdPixel {
    uint8_t color = 0;  // 0..63: palette * 16 + index (index 0 = transparent)
    MdLayer layer = MdLayer::Backdrop;
    bool priority = false;  // priority bit of the tile or sprite
    MdShade shade = MdShade::Normal;
    uint16_t source = 0;    // tile pattern index (11 bits) or sprite number
};

// Everything the VDP produced for one frame. Not just a picture: for every pixel it
// also records the layer it came from, and it keeps the three layers as they were
// before the VDP mixed them (alpha 0 where a layer is empty), so a renderer can pull
// them apart without holes. Colours are already resolved with the palette in effect on
// that line and with shadow/highlight applied, so mid-frame palette changes survive
// in 3D too.
struct VideoFrameMd {
    // 320 plus what the widescreen option adds on each side (VdpMd::kMaxWideColumns).
    static constexpr int kMaxWidth = 432;
    static constexpr int kMaxHeight = 480;  // 240 lines, doubled in interlace mode 2
    static constexpr int kPixels = kMaxWidth * kMaxHeight;

    int width = 320;    // 256 (H32) or 320 (H40), plus `wide` on each side
    // Columns the widescreen option adds on each side: picture the console never shows,
    // drawn from the planes and sprites past its edges. 0 without the option.
    int wide = 0;
    int height = 224;   // 224 or 240; 448 or 480 in interlace mode 2
    bool interlaced = false;

    // Row stride is always kMaxWidth.
    std::array<uint32_t, kPixels> rgba{};  // final picture, 0xAARRGGBB
    std::array<MdPixel, kPixels> info{};   // where each final pixel came from

    std::array<uint32_t, kPixels> planeB{};
    std::array<uint32_t, kPixels> planeA{};  // includes the window
    std::array<uint32_t, kPixels> sprites{};
    std::array<MdPixel, kPixels> planeBInfo{};
    std::array<MdPixel, kPixels> planeAInfo{};
    std::array<MdPixel, kPixels> spriteInfo{};
    std::array<uint32_t, kMaxHeight> backdrop{};  // backdrop colour of each line
    // The 32X's picture of each line (alpha 0 where it draws nothing), whether or not
    // it is in front of the Mega Drive's. Indexed by line, not by interlaced row.
    std::array<uint32_t, kPixels> mars{};

    struct Line {
        int16_t hscrollA = 0, hscrollB = 0;
        bool displayEnabled = false;
    };
    std::array<Line, kMaxHeight> lines{};

    uint64_t frameNumber = 0;

    uint32_t pixel(int x, int y) const { return rgba[y * kMaxWidth + x]; }
};

}  // namespace plumsg
