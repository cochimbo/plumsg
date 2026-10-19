#pragma once

#include "../core/video_frame_md.h"

namespace plumsg {

enum class ScaleMode { Integer, Fit, Stretch };

// Camera limits for the pseudo-3D view, shared by the mouse, the pad and the menus.
constexpr float kMinTilt = -10.0f, kMaxTilt = 80.0f, kMaxYaw = 75.0f, kMinZoom = 0.5f, kMaxZoom = 3.0f;

// How the pseudo-3D view builds each plane: flat cut-outs, or one block per pixel.
enum class Style3D { Layers, Voxels };

// Look of the 2D picture: plain, with the dark gaps between a TV's lines, or a whole
// CRT (scanlines, a slightly curved screen, the phosphor mask and darker corners).
enum class Filter2D { None, Scanlines, Crt };

// Display options chosen in the video menu.
struct RenderOptions {
    ScaleMode scale = ScaleMode::Integer;
    bool smooth = false;     // bilinear filtering instead of sharp pixels
    Filter2D filter = Filter2D::None;  // only the 2D view uses it
    float filterStrength = 0.6f;       // 0..1, how strong the scanlines and the rest are
    bool pseudo3d = false;   // toggled by the "efecto 3D" hotkey: draw with the pseudo-3D renderer
    // Pseudo-3D look, each 0..1 except the tilt.
    Style3D style = Style3D::Voxels;
    float depth = 0.6f;      // how far apart the layers are
    bool tiltCamera = true;  // off: the camera looks straight on, whatever tilt and yaw say
    float tilt = 25.0f;      // camera tilt in degrees, -10..80; at 0 the picture looks flat
    float yaw = 0.0f;        // camera turn around the vertical axis in degrees, -75..75
    float zoom = 1.0f;       // 0.5..3
    float extrusion = 0.5f;  // voxels: how far solid background tiles reach back
    float thickness = 1.0f;  // how much of the gap below a plane its sides (or blocks) fill
    bool shadows = true;     // sprites cast a shadow on the planes behind them
    bool lighting = true;    // side light on the blocks and soft shadows of every plane on those behind
    bool liveCamera = false; // the camera sways a few degrees following the action
    bool squarePixels = false;  // show pixels square instead of the TV's 4:3 picture
    bool highBInFront = false;  // high-priority plane B tiles stand with the priority plane
    bool flatB = false;         // plane B stays flat, as part of the sky
};

// Width on screen of one pixel, for a line height of 1. The TV shows 224 lines at 4:3
// whatever the horizontal resolution (PAL's 240 lines just take more of the screen);
// the widescreen columns are as wide as the console's own. The Game Gear's screen
// shows its 144 lines at 4:3.
inline float pixelAspect(const VideoFrameMd& frame, bool squarePixels) {
    return squarePixels ? 1.0f
                        : static_cast<float>(frame.aspectLines) * 4.0f / 3.0f / static_cast<float>(frame.consoleWidth());
}

// Where to draw, in OpenGL window coordinates (origin at the bottom left).
struct Viewport {
    int x = 0, y = 0, width = 0, height = 0;
};

// A renderer turns a VideoFrameMd into pixels on screen. The emulator core never calls
// into a renderer; the frontend hands it each finished frame. A pseudo-3D renderer
// is just another implementation of this interface. Renderers draw with OpenGL 3.3 into
// the current context.
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual const char* name() const = 0;
    virtual void draw(const VideoFrameMd& frame, const Viewport& area) = 0;
    void setOptions(const RenderOptions& options) { options_ = options; }

protected:
    RenderOptions options_;
};

}  // namespace plumsg
