#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "../core/video_frame_md.h"

namespace plumsg {

// The planes the pseudo-3D view stands apart, back to front.
enum Plane { PlaneSky, PlaneB, PlaneA, PlaneSprites, PlanePriority, kPlanes };

// One picture per plane (0xAARRGGBB, alpha 0 where a plane is empty), all the same size.
struct Planes {
    int width = 0, height = 0;
    std::array<std::vector<uint32_t>, kPlanes> layer;
    std::vector<uint32_t>& operator[](int p) { return layer[p]; }
    const std::vector<uint32_t>& operator[](int p) const { return layer[p]; }
};

struct SplitOptions {
    bool highBInFront = false;  // high-priority plane B tiles go to the Priority plane
    bool flatB = false;         // plane B is drawn flat, as part of the sky
};

// Splits the frame into planes. In interlace mode 2 only the even field is used, so
// the planes keep the normal height. The sky is always full: the backdrop (plus all of
// plane B with flatB), with the gaps behind plane B filled from the nearest sky pixel
// on the line. Plane A takes the low-priority tiles of plane A and the window, Priority
// the high-priority ones, and Sprites every sprite (operator sprites of shadow/highlight
// mode only change colours, so they are not in any plane).
//
// The hardware's priority order does not match the planes' depth order exactly (a high
// priority sprite covers high priority tiles; a high priority plane B tile covers low
// priority sprites and plane A). So, for every pixel, the plane holding what the
// console actually shows gets its final colour, and the planes in front of it lose
// that pixel; the planes behind keep theirs, so tilting the camera shows what is
// hidden. With the camera straight on, the 3D view is exactly the 2D picture.
void splitPlanes(const VideoFrameMd& frame, const SplitOptions& options, Planes& planes);

// Writes into the alpha of every solid pixel how far it is from the plane's edge, in
// pixels (city-block distance to the nearest empty pixel or the picture border), capped
// at kMaxEdgeDistance: alpha = 255 - 16 * (kMaxEdgeDistance - distance). Empty pixels
// keep alpha 0, so anything testing alpha >= 128 still sees the same shape. The voxel
// view uses it to round off the edges of every shape.
constexpr int kMaxEdgeDistance = 4;
void markEdgeDistance(std::vector<uint32_t>& plane, int width, int height);

// Marks the pixels of plane A (0xFFFFFFFF or 0, planes' size) that belong to solid
// tiles: tile patterns of which at most a quarter of the pixels on screen are
// transparent. Ground, walls and platforms are usually solid; foliage, clouds and
// outlines are not. The voxel view extrudes solid tiles far back, like 3DNES does.
void markSolidTiles(const VideoFrameMd& frame, const Planes& planes, std::vector<uint32_t>& solid);

}  // namespace plumsg
