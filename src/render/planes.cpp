#include "planes.h"

#include <algorithm>

namespace plumsg {

namespace {

// Source row of plane line y: in interlace mode 2 only the even field.
int sourceRow(const VideoFrameMd& frame, int y) { return frame.interlaced ? y * 2 : y; }

}  // namespace

void splitPlanes(const VideoFrameMd& frame, const SplitOptions& options, Planes& planes) {
    const int w = frame.width;
    const int h = frame.interlaced ? frame.height / 2 : frame.height;
    planes.width = w;
    planes.height = h;
    for (auto& p : planes.layer) p.assign(static_cast<size_t>(w) * h, 0);

    auto planeOf = [&](const MdPixel& px) -> int {
        switch (px.layer) {
            case MdLayer::Backdrop: return PlaneSky;
            case MdLayer::PlaneB:
                if (options.flatB) return PlaneSky;
                return px.priority && options.highBInFront ? PlanePriority : PlaneB;
            case MdLayer::PlaneA:
            case MdLayer::Window: return px.priority ? PlanePriority : PlaneA;
            case MdLayer::Sprite: return PlaneSprites;
        }
        return PlaneSky;
    };

    for (int y = 0; y < h; ++y) {
        const int row = sourceRow(frame, y);
        uint32_t* sky = &planes[PlaneSky][static_cast<size_t>(y) * w];
        int firstSky = -1;
        for (int x = 0; x < w; ++x) {
            const int i = row * VideoFrameMd::kMaxWidth + x;
            const size_t o = static_cast<size_t>(y) * w + x;
            // Every layer into its plane.
            uint32_t b = frame.planeB[i];
            if (b >> 24) {
                planes[planeOf(frame.planeBInfo[i])][o] = b;
            } else {
                b = 0;
            }
            if (frame.planeA[i] >> 24) planes[planeOf(frame.planeAInfo[i])][o] = frame.planeA[i];
            if (frame.sprites[i] >> 24) planes[PlaneSprites][o] = frame.sprites[i];
            // What the console shows decides which plane is the visible one here.
            const int shown = planeOf(frame.info[i]);
            for (int p = shown + 1; p < kPlanes; ++p) planes[p][o] = 0;
            if (shown != PlaneSky) planes[shown][o] = frame.rgba[i] | 0xFF000000u;
            const bool bOpaque = (b >> 24) != 0;
            if (shown == PlaneSky) sky[x] = frame.rgba[i] | 0xFF000000u;
            else if (options.flatB) sky[x] = (bOpaque ? b : frame.backdrop[row]) | 0xFF000000u;
            else if (!bOpaque) sky[x] = frame.backdrop[row] | 0xFF000000u;  // under plane A or a sprite
            if (sky[x] != 0 && firstSky < 0) firstSky = x;
        }
        // Fill the sky behind plane B from the nearest sky pixel to the left (the first
        // one for the start of the line). A line with no sky at all keeps plane B's own
        // colours (or the backdrop), so tilting never shows a hole.
        if (firstSky < 0) {
            for (int x = 0; x < w; ++x) {
                const int i = row * VideoFrameMd::kMaxWidth + x;
                sky[x] = (frame.planeB[i] >> 24 ? frame.planeB[i] : frame.backdrop[row]) | 0xFF000000u;
            }
            continue;
        }
        uint32_t fill = sky[firstSky];
        for (int x = 0; x < w; ++x) {
            if (sky[x]) fill = sky[x];
            else sky[x] = fill;
        }
    }
}

void markEdgeDistance(std::vector<uint32_t>& plane, int w, int h) {
    // Two-pass chamfer distance: forward from the top left, then back from the bottom right.
    std::vector<uint8_t> d(plane.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!(plane[i] >> 24)) continue;
            const int up = y > 0 ? d[i - w] : 0;
            const int left = x > 0 ? d[i - 1] : 0;
            d[i] = static_cast<uint8_t>(std::min({up, left, kMaxEdgeDistance - 1}) + 1);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const int i = y * w + x;
            if (!d[i]) continue;
            const int down = y < h - 1 ? d[i + w] : 0;
            const int right = x < w - 1 ? d[i + 1] : 0;
            d[i] = static_cast<uint8_t>(std::min<int>({d[i], down + 1, right + 1}));
            plane[i] = (plane[i] & 0x00FFFFFFu) | static_cast<uint32_t>(255 - 16 * (kMaxEdgeDistance - d[i])) << 24;
        }
}

void markSolidTiles(const VideoFrameMd& frame, const Planes& planes, std::vector<uint32_t>& solid) {
    const int w = planes.width, h = planes.height;
    std::array<uint16_t, 2048> total{}, clear{};  // per tile pattern
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const MdPixel& a = frame.planeAInfo[sourceRow(frame, y) * VideoFrameMd::kMaxWidth + x];
            if (a.layer != MdLayer::PlaneA && a.layer != MdLayer::Window) continue;
            const int pattern = a.source & 2047;
            ++total[pattern];
            if ((a.color & 0x0F) == 0) ++clear[pattern];
        }
    solid.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const MdPixel& a = frame.planeAInfo[sourceRow(frame, y) * VideoFrameMd::kMaxWidth + x];
            if ((a.layer != MdLayer::PlaneA && a.layer != MdLayer::Window) || (a.color & 0x0F) == 0) continue;
            const int pattern = a.source & 2047;
            if (clear[pattern] * 4 <= total[pattern] && planes[PlaneA][static_cast<size_t>(y) * w + x])
                solid[static_cast<size_t>(y) * w + x] = 0xFFFFFFFFu;
        }
}

}  // namespace plumsg
