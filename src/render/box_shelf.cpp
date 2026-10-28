#include "box_shelf.h"

#include "console_models.h"

#include <algorithm>
#include <cmath>

namespace plumsg {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRad = kPi / 180.0f;

// A Mega Drive case: 13.5 x 19 x 2.5 cm, with 13.5 cm as 1.
constexpr float kBoxW = 1.0f, kBoxH = 1.4f, kBoxD = 0.2f, kWall = 0.02f;
constexpr float kFloorY = -kBoxH / 2;   // the boxes stand on the floor
constexpr float kConsoleZ = 1.6f;       // the console sits on the floor in front of them
constexpr float kLidOpen = 165.0f;      // degrees, all the way open
constexpr float kFov = 34.0f;           // degrees, top to bottom
const float kBackground[3] = {0.035f, 0.037f, 0.045f};

// Each vertex: position, normal, texture coordinates, the face it belongs to and its colour.
constexpr int kFloats = kConsoleVertexFloats;

constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in float aFace;
layout(location = 4) in vec3 aColour;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform int uMirror;
uniform float uFloorY;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vUv;
out vec3 vColour;
flat out int vFace;
void main() {
    vec4 w = uModel * vec4(aPos, 1.0);
    vec3 n = transpose(inverse(mat3(uModel))) * aNormal;
    if (uMirror == 1) {
        w.y = 2.0 * uFloorY - w.y;
        n.y = -n.y;
    }
    vWorld = w.xyz;
    vNormal = n;
    vUv = aUv;
    vColour = aColour;
    vFace = int(aFace + 0.5);
    gl_Position = uViewProj * w;
}
)";

// A key light from the top left, a weak fill from the right, a glossy highlight and a rim.
// The floor fades into the background away from the boxes; what it reflects fades
// with the distance below it.
constexpr const char* kFragment = R"(#version 330 core
uniform sampler2D uImage;
uniform vec3 uColour;
uniform int uTexFaces;
uniform vec4 uTexRect;
uniform float uGlow;
uniform float uShine;
uniform vec3 uEye;
uniform int uMirror;
uniform int uFloor;
uniform float uFloorY;
uniform vec3 uBackground;
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUv;
in vec3 vColour;
flat in int vFace;
out vec4 outColor;
void main() {
    if (uFloor == 1) {
        float r = length(vWorld.xz - vec2(0.0, 0.8));
        outColor = vec4(mix(uColour, uBackground, smoothstep(1.0, 6.5, r)), 0.84);
        return;
    }
    bool textured = ((uTexFaces >> vFace) & 1) != 0;
    vec3 base = textured ? texture(uImage, mix(uTexRect.xy, uTexRect.zw, vUv)).rgb : uColour * vColour;
    vec3 n = normalize(vNormal);
    vec3 l = normalize(vec3(-0.35, 0.8, 0.6));
    vec3 v = normalize(uEye - vWorld);
    float diffuse = max(dot(n, l), 0.0);
    float fill = max(dot(n, normalize(vec3(0.7, 0.1, 0.5))), 0.0) * 0.25;
    float spec = pow(max(dot(n, normalize(l + v)), 0.0), 48.0) * uShine;
    float ambient = textured ? 0.45 : 0.34;
    // A cool light from behind outlines the black plastics against the dark.
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * max(dot(n, normalize(vec3(0.2, 0.5, -0.8))) + 0.6, 0.0);
    vec3 c = base * (ambient + 0.75 * diffuse + fill) + vec3(spec) + base * uGlow + vec3(0.16, 0.18, 0.24) * rim;
    if (uMirror == 1) c *= 0.4 * (1.0 - smoothstep(0.0, 1.1, uFloorY - vWorld.y));
    outColor = vec4(c, 1.0);
}
)";

}  // namespace

// Column-major 4x4 matrices, as OpenGL expects them.
namespace {

using Mat4 = std::array<float, 16>;

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}

Mat4 translate(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }
Mat4 scale(float x, float y, float z) { return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}; }

// Turns +z towards +x for positive degrees.
Mat4 rotateY(float degrees) {
    const float c = std::cos(degrees * kRad), s = std::sin(degrees * kRad);
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}

// Turns +z towards -y for positive degrees (so -90 lays a face looking at the viewer on its back).
Mat4 rotateX(float degrees) {
    const float c = std::cos(degrees * kRad), s = std::sin(degrees * kRad);
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}

Mat4 perspective(float fovY, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovY / 2);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1;
    m[14] = 2 * zFar * zNear / (zNear - zFar);
    return m;
}

// The inverse of a matrix that only turns, scales and moves.
Mat4 affineInverse(const Mat4& m) {
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) return identity();
    const float k = 1.0f / det;
    // Rows of the inverse of the 3x3 part.
    const float r00 = A * k, r01 = -(b * i - c * h) * k, r02 = (b * f - c * e) * k;
    const float r10 = B * k, r11 = (a * i - c * g) * k, r12 = -(a * f - c * d) * k;
    const float r20 = C * k, r21 = -(a * h - b * g) * k, r22 = (a * e - b * d) * k;
    const float tx = m[12], ty = m[13], tz = m[14];
    return {r00, r10, r20, 0, r01, r11, r21, 0, r02, r12, r22, 0,
            -(r00 * tx + r01 * ty + r02 * tz), -(r10 * tx + r11 * ty + r12 * tz), -(r20 * tx + r21 * ty + r22 * tz), 1};
}

float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

BoxShelf::BoxShelf() {
    program_ = gl::buildProgram(kVertex, kFragment);
    // A unit cube, then a cylinder of radius 0.5 along z, both centred on the origin.
    // Faces: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z (the front), 5 -z; texture coordinates run as
    // the face is seen from outside, with v from its top.
    std::vector<float> v;
    auto vertex = [&v](float px, float py, float pz, float nx, float ny, float nz, float u, float t, int face) {
        v.insert(v.end(), {px, py, pz, nx, ny, nz, u, t, static_cast<float>(face), 1, 1, 1});
    };
    const float faces[6][9] = {{1, 0, 0, 0, 0, -1, 0, 1, 0}, {-1, 0, 0, 0, 0, 1, 0, 1, 0}, {0, 1, 0, 1, 0, 0, 0, 0, -1},
                               {0, -1, 0, 1, 0, 0, 0, 0, 1}, {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, -1, 0, 0, 0, 1, 0}};
    const float corners[6][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (int f = 0; f < 6; ++f) {
        const float* n = faces[f];
        for (const auto& c : corners) {
            const float a = c[0], b = c[1];
            vertex(n[0] * 0.5f + n[3] * a + n[6] * b, n[1] * 0.5f + n[4] * a + n[7] * b, n[2] * 0.5f + n[5] * a + n[8] * b,
                   n[0], n[1], n[2], a + 0.5f, 0.5f - b, f);
        }
    }
    cubeFirst_ = 0;
    cubeCount_ = static_cast<int>(v.size()) / kFloats;
    constexpr int kSides = 48;
    for (int s = 0; s < kSides; ++s) {
        const float a0 = 2 * kPi * s / kSides, a1 = 2 * kPi * (s + 1) / kSides;
        const float x0 = 0.5f * std::cos(a0), y0 = 0.5f * std::sin(a0), x1 = 0.5f * std::cos(a1), y1 = 0.5f * std::sin(a1);
        // The caps, mapped like the cube's front and back.
        vertex(0, 0, 0.5f, 0, 0, 1, 0.5f, 0.5f, 4);
        vertex(x0, y0, 0.5f, 0, 0, 1, x0 + 0.5f, 0.5f - y0, 4);
        vertex(x1, y1, 0.5f, 0, 0, 1, x1 + 0.5f, 0.5f - y1, 4);
        vertex(0, 0, -0.5f, 0, 0, -1, 0.5f, 0.5f, 5);
        vertex(x1, y1, -0.5f, 0, 0, -1, 0.5f - x1, 0.5f - y1, 5);
        vertex(x0, y0, -0.5f, 0, 0, -1, 0.5f - x0, 0.5f - y0, 5);
        // The side.
        const float nx0 = std::cos(a0), ny0 = std::sin(a0), nx1 = std::cos(a1), ny1 = std::sin(a1);
        vertex(x0, y0, -0.5f, nx0, ny0, 0, 0, 1, 0);
        vertex(x1, y1, -0.5f, nx1, ny1, 0, 0, 1, 0);
        vertex(x1, y1, 0.5f, nx1, ny1, 0, 0, 0, 0);
        vertex(x0, y0, -0.5f, nx0, ny0, 0, 0, 1, 0);
        vertex(x1, y1, 0.5f, nx1, ny1, 0, 0, 0, 0);
        vertex(x0, y0, 0.5f, nx0, ny0, 0, 0, 0, 0);
    }
    cylinderFirst_ = cubeCount_;
    cylinderCount_ = static_cast<int>(v.size()) / kFloats - cubeCount_;
    // The consoles, then the cartridges' shells.
    for (int m = 0; m < kMeshes; ++m) {
        meshFirst_[m] = static_cast<int>(v.size()) / kFloats;
        if (m < static_cast<int>(ConsoleModel::Count)) buildConsoleModel(static_cast<ConsoleModel>(m), v);
        else if (m - kCartMeshes != static_cast<int>(ConsoleSystem::MegaCD))
            buildCartridgeModel(static_cast<ConsoleSystem>(m - kCartMeshes), v);
        meshCount_[m] = static_cast<int>(v.size()) / kFloats - meshFirst_[m];
    }

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
    gl::BufferData(gl::ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(v.size() * sizeof(float)), v.data(), gl::STATIC_DRAW);
    const int stride = kFloats * sizeof(float);
    const int sizes[5] = {3, 3, 2, 1, 3};
    for (int a = 0, offset = 0; a < 5; offset += sizes[a], ++a) {
        gl::EnableVertexAttribArray(static_cast<gl::GLuint>(a));
        gl::VertexAttribPointer(static_cast<gl::GLuint>(a), sizes[a], gl::FLOAT, gl::FALSE_, stride,
                                reinterpret_cast<const void*>(static_cast<size_t>(offset) * sizeof(float)));
    }
    gl::BindVertexArray(0);
}

BoxShelf::~BoxShelf() {
    if (fbo_) gl::DeleteFramebuffers(1, &fbo_);
    if (colour_) gl::DeleteTextures(1, &colour_);
    if (depth_) gl::DeleteTextures(1, &depth_);
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    if (program_) gl::DeleteProgram(program_);
}

void BoxShelf::ensureTarget(int width, int height) {
    if (fbo_ && width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    if (!fbo_) {
        gl::GenFramebuffers(1, &fbo_);
        gl::GenTextures(1, &colour_);
        gl::GenTextures(1, &depth_);
    }
    gl::BindTexture(gl::TEXTURE_2D, colour_);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
    // Drawn at twice the size it is shown at: the linear filter averages it down smoothly.
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::BindTexture(gl::TEXTURE_2D, depth_);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::DEPTH_COMPONENT24, width, height, 0, gl::DEPTH_COMPONENT, gl::UNSIGNED_INT, nullptr);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::BindFramebuffer(gl::FRAMEBUFFER, fbo_);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, colour_, 0);
    gl::FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, depth_, 0);
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
}

void BoxShelf::reset() {
    state_ = State::Browse;
    open_ = eject_ = light_ = 0;
    launch_ = launched_ = false;
    scrolled_ = false;
    frozen_ = false;
}

void BoxShelf::pose(int step) {
    reset();
    if (step <= 0) return;
    open_ = 1;
    state_ = step == 1 ? State::Open : step == 2 ? State::Ejecting : State::Inserted;
    eject_ = step == 2 ? 0.6f : 1.0f;
    frozen_ = true;
}

void BoxShelf::activate() {
    if (count_ == 0) return;
    switch (state_) {
        case State::Browse:
        case State::Closing:
            state_ = State::Opening;
            scroll_ = static_cast<float>(selected_);
            break;
        case State::Open:
            state_ = State::Ejecting;
            eject_ = 0;
            break;
        default: break;
    }
}

bool BoxShelf::back() {
    switch (state_) {
        case State::Browse: return false;
        case State::Opening:
        case State::Open: state_ = State::Closing; return true;
        default: return true;  // on its way into the console
    }
}

bool BoxShelf::takeLaunch() {
    if (!launch_ || launched_) return false;
    launched_ = true;
    return true;
}

int BoxShelf::click(float x, float y) {
    const Part* p = pick(x, y);
    switch (state_) {
        case State::Browse:
            if (!p || p->box < 0) return -1;
            if (p->box == selected_) activate();
            else return p->box;
            return -1;
        case State::Open:
            if (p && p->cartridge) activate();
            else state_ = State::Closing;
            return -1;
        default: return -1;
    }
}

BoxShelf::Target BoxShelf::hover(float x, float y) {
    const Part* p = pick(x, y);
    if (state_ == State::Open) return p && p->cartridge ? Target::Cartridge : Target::Box;
    if (state_ == State::Browse && p && p->box >= 0) return Target::Box;
    return Target::None;
}

const BoxShelf::Part* BoxShelf::pick(float x, float y) const {
    if (width_ == 0) return nullptr;
    // The ray from the eye through the point, then against each part's box in its own space.
    const float nx = (2 * x - 1) * tanHalf_ * aspect_, ny = (1 - 2 * y) * tanHalf_;
    const Vec3 d{forward_.x + nx * right_.x + ny * up_.x, forward_.y + nx * right_.y + ny * up_.y,
                 forward_.z + nx * right_.z + ny * up_.z};
    const Part* best = nullptr;
    float bestT = 1e30f;
    for (const Part& p : parts_) {
        if (p.box < 0 && !p.cartridge) continue;
        const Mat4 inv = affineInverse(p.model);
        const float o[3] = {inv[0] * eye_.x + inv[4] * eye_.y + inv[8] * eye_.z + inv[12],
                            inv[1] * eye_.x + inv[5] * eye_.y + inv[9] * eye_.z + inv[13],
                            inv[2] * eye_.x + inv[6] * eye_.y + inv[10] * eye_.z + inv[14]};
        const float dir[3] = {inv[0] * d.x + inv[4] * d.y + inv[8] * d.z, inv[1] * d.x + inv[5] * d.y + inv[9] * d.z,
                              inv[2] * d.x + inv[6] * d.y + inv[10] * d.z};
        float t0 = 0, t1 = 1e30f;
        bool hit = true;
        for (int a = 0; a < 3 && hit; ++a) {
            if (std::fabs(dir[a]) < 1e-9f) {
                hit = std::fabs(o[a]) <= 0.5f;
                continue;
            }
            float ta = (-0.5f - o[a]) / dir[a], tb = (0.5f - o[a]) / dir[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            hit = t0 <= t1;
        }
        // The cartridge wins over the box it lies in.
        const float t = p.cartridge ? t0 - 0.05f : t0;
        if (hit && t < bestT) {
            bestT = t;
            best = &p;
        }
    }
    return best;
}

void BoxShelf::camera(Vec3* eye, Vec3* target) const {
    const bool ejecting = state_ == State::Ejecting || state_ == State::Inserted;
    const float o = ejecting ? 1.0f : smooth(open_);
    const float e = state_ == State::Inserted ? 1.0f : ejecting ? smooth(eject_ / 0.45f) : 0.0f;
    // Browsing, the box open, and the console: each framed for its size.
    Vec3 consoleEye{0.3f, 0.9f, 4.9f}, consoleTarget{0.1f, -0.35f, 1.5f};
    switch (system_) {
        case ConsoleSystem::Mega32X: consoleEye = {0.35f, 1.3f, 5.5f}, consoleTarget = {0.1f, -0.05f, 1.4f}; break;
        case ConsoleSystem::MegaCD: consoleEye = {0.5f, 1.2f, 5.9f}, consoleTarget = {0.0f, -0.25f, 1.9f}; break;
        case ConsoleSystem::MasterSystem: consoleEye = {0.4f, 1.0f, 5.5f}, consoleTarget = {0.2f, -0.35f, 1.4f}; break;
        case ConsoleSystem::GameGear: consoleEye = {0.15f, 0.5f, 4.3f}, consoleTarget = {0.0f, -0.12f, 1.6f}; break;
        default: break;
    }
    const Vec3 eyes[3] = {{0, 0.25f, 3.6f}, {-0.45f, 0.25f, 3.5f}, consoleEye};
    const Vec3 targets[3] = {{0, -0.05f, 0}, {-0.45f, 0.0f, 0}, consoleTarget};
    auto mix = [&](const Vec3 (&k)[3]) {
        const Vec3 a{lerp(k[0].x, k[1].x, o), lerp(k[0].y, k[1].y, o), lerp(k[0].z, k[1].z, o)};
        return Vec3{lerp(a.x, k[2].x, e), lerp(a.y, k[2].y, e), lerp(a.z, k[2].z, e)};
    };
    *eye = mix(eyes);
    *target = mix(targets);
    // A narrow view steps back to keep it all in.
    const float back = std::max(1.0f, 1.6f / aspect_);
    eye->x = target->x + (eye->x - target->x) * back;
    eye->y = target->y + (eye->y - target->y) * back;
    eye->z = target->z + (eye->z - target->z) * back;
}

BoxShelf::Mat4 BoxShelf::cartridgeInBox(const Mat4& box, ConsoleSystem s) const {
    // The disc on its hub in the upper half; a cartridge lies in the lower half, its label
    // to the front and its contacts down, as in the console.
    if (s == ConsoleSystem::MegaCD) return box * translate(0, 0.16f, -kBoxD / 2 + kWall + 0.02f);
    const CartridgeShape c = cartridgeShape(s);
    return box * translate(0, -kBoxH / 2 + kWall + 0.1f + c.h / 2, -kBoxD / 2 + kWall + 0.006f + c.d / 2);
}

float BoxShelf::coverFront(int width, int height) {
    if (width <= 0 || height <= 0 || width < height * 6 / 5) return 0;
    return std::clamp(1 - kBoxW / kBoxH * static_cast<float>(height) / static_cast<float>(width), 0.0f, 0.9f);
}

namespace {

// The front, spine and back of a cover as texture rectangles; the back and spine are
// empty (u1 <= u0) on a cover that is only the front.
struct Sleeve {
    std::array<float, 4> front{0, 0, 1, 1}, spine{0, 0, 0, 1}, back{0, 0, 0, 1};
};

Sleeve sleeve(int width, int height) {
    Sleeve s;
    const float f = BoxShelf::coverFront(width, height);
    if (f <= 0) return s;
    const float side = 1 - f;  // the front's width, and the back's
    s.front = {f, 0, 1, 1};
    s.back = {0, 0, side, 1};
    s.spine = {side, 0, f, 1};
    return s;
}

}  // namespace

void BoxShelf::addCartridge(const Item& item, const Mat4& place) {
    Part p;
    p.cartridge = true;
    p.texRect = sleeve(item.width, item.height).front;
    if (item.system == ConsoleSystem::MegaCD) {
        // A 12 cm disc, 1.2 mm thick, its label printed with the cover; the hole in the middle.
        p.model = place * scale(0.889f, 0.889f, 0.009f);
        p.colour = {0.78f, 0.79f, 0.84f};
        p.shine = 1.0f;
        p.cylinder = true;
        p.texture = item.texture;
        p.texFaces = item.texture ? 1 << 4 : 0;
        parts_.push_back(p);
        p.model = place * scale(0.111f, 0.111f, 0.0095f);
        p.colour = {kBackground[0], kBackground[1], kBackground[2]};
        p.texFaces = 0;
        p.shine = 0;
        parts_.push_back(p);
        return;
    }
    const CartridgeShape c = cartridgeShape(item.system);
    p.model = place;
    p.colour = {1, 1, 1};
    p.shine = 0.35f;
    p.mesh = kCartMeshes + static_cast<int>(item.system);
    parts_.push_back(p);
    // The label (the cover) in its recess.
    p.mesh = -1;
    p.model = place * translate(0, c.labelY, c.d / 2 + 0.0025f) * scale(c.labelW, c.labelH, 0.001f);
    p.colour = item.texture ? Vec3{1, 1, 1} : Vec3{0.75f, 0.72f, 0.66f};
    p.shine = 0.15f;
    p.texture = item.texture;
    p.texFaces = item.texture ? 1 << 4 : 0;
    parts_.push_back(p);
}

void BoxShelf::addBox(const std::vector<Item>& items, int index, const Mat4& place, float lid) {
    const Item& item = items[static_cast<size_t>(index)];
    auto add = [&](const Mat4& m, Vec3 colour, float shine, int texFaces = 0) {
        Part p;
        p.model = place * m;
        p.colour = colour;
        p.shine = shine;
        p.texture = item.texture;
        p.texFaces = item.texture ? texFaces : 0;
        p.box = index;
        parts_.push_back(p);
    };
    const Vec3 plastic{0.045f, 0.045f, 0.055f};
    const float inner = kBoxD - kWall;
    // With a whole sleeve its back and spine go round the tray.
    const Sleeve cover = sleeve(item.width, item.height);
    const bool wrap = cover.spine[2] > cover.spine[0];
    // The tray: its back and four walls, open at the front.
    add(translate(0, 0, -kBoxD / 2 + kWall / 2) * scale(kBoxW, kBoxH, kWall), plastic, 0.6f, wrap ? 1 << 5 : 0);
    parts_.back().texRect = cover.back;
    add(translate(-kBoxW / 2 + kWall / 2, 0, -kWall / 2) * scale(kWall, kBoxH, inner), plastic, 0.6f, wrap ? 1 << 1 : 0);
    parts_.back().texRect = cover.spine;
    add(translate(kBoxW / 2 - kWall / 2, 0, -kWall / 2) * scale(kWall, kBoxH, inner), plastic, 0.6f);
    add(translate(0, kBoxH / 2 - kWall / 2, -kWall / 2) * scale(kBoxW - 2 * kWall, kWall, inner), plastic, 0.6f);
    add(translate(0, -kBoxH / 2 + kWall / 2, -kWall / 2) * scale(kBoxW - 2 * kWall, kWall, inner), plastic, 0.6f);
    // Without a cover the lid takes the console's colour.
    Vec3 lidColour{0.16f, 0.17f, 0.2f};
    switch (item.system) {
        case ConsoleSystem::Mega32X: lidColour = {0.45f, 0.1f, 0.08f}; break;
        case ConsoleSystem::MasterSystem: lidColour = {0.85f, 0.85f, 0.85f}; break;
        case ConsoleSystem::GameGear: lidColour = {0.25f, 0.12f, 0.35f}; break;
        case ConsoleSystem::MegaCD: lidColour = {0.1f, 0.2f, 0.4f}; break;
        default: break;
    }
    const Mat4 hinge = translate(-kBoxW / 2, 0, kBoxD / 2 - kWall / 2) * rotateY(-lid * kLidOpen);
    add(hinge * translate(kBoxW / 2, 0, 0) * scale(kBoxW, kBoxH, kWall), item.texture ? plastic : lidColour, 0.7f, 1 << 4);
    parts_.back().texRect = cover.front;
    if (lid <= 0.001f) return;
    // Inside: the manual on the lid, the holder and the game.
    add(hinge * translate(kBoxW / 2, 0, -kWall / 2 - 0.004f) * scale(kBoxW * 0.86f, kBoxH * 0.88f, 0.006f),
        {0.86f, 0.84f, 0.78f}, 0.1f, 1 << 5);
    parts_.back().texRect = cover.front;
    add(translate(0, 0, -kBoxD / 2 + kWall + 0.003f) * scale(kBoxW - 2 * kWall, kBoxH - 2 * kWall, 0.006f),
        {0.11f, 0.11f, 0.13f}, 0.3f);
    if (index == selected_ && (state_ == State::Ejecting || state_ == State::Inserted)) return;
    addCartridge(item, cartridgeInBox(place, item.system));
}

BoxShelf::Mat4 BoxShelf::addConsole(const Item& item, float light, float rise, float tray) {
    using L = ConsoleLayout;
    const ConsoleSystem s = item.system;
    const Mat4 base = translate(0, kFloorY + rise, kConsoleZ);
    auto mesh = [&](ConsoleModel m, const Mat4& at) {
        Part p;
        p.model = base * at;
        p.colour = {1, 1, 1};
        p.shine = 0.45f;
        p.mesh = static_cast<int>(m);
        parts_.push_back(p);
    };
    // The power light: dark red, lit once the game is in.
    auto led = [&](const float* at, bool front) {
        Part p;
        p.model = base * translate(at[0], at[1], at[2]) * (front ? scale(0.03f, 0.03f, 0.006f) : scale(0.035f, 0.006f, 0.035f));
        p.colour = light > 0 ? Vec3{1.0f, 0.12f, 0.08f} : Vec3{0.22f, 0.03f, 0.03f};
        p.glow = light > 0 ? 1.6f : 0.0f;
        p.shine = 0.6f;
        parts_.push_back(p);
    };
    const CartridgeShape c = cartridgeShape(s);
    switch (s) {
        case ConsoleSystem::GameGear: {
            mesh(ConsoleModel::GameGear, identity());
            led(L::kGgLed, true);
            // The screen shows the game's cover once it is on.
            Part screen;
            screen.model = base * translate(0, L::kGgScreenY, L::kGgScreenZ) * scale(L::kGgScreenW, L::kGgScreenH, 0.002f);
            screen.colour = {0.05f, 0.07f, 0.06f};
            screen.shine = 1.0f;
            if (light > 0 && item.texture) {
                screen.texture = item.texture;
                screen.texFaces = 1 << 4;
                screen.texRect = sleeve(item.width, item.height).front;
                screen.glow = 0.6f;
            }
            parts_.push_back(screen);
            return translate(0, kFloorY + L::kGgSlotY + c.h / 2 - c.insert, kConsoleZ + L::kGgSlotZ);
        }
        case ConsoleSystem::MasterSystem:
            mesh(ConsoleModel::MasterSystem, identity());
            led(L::kSmsLed, false);
            return translate(L::kSmsSlotX, kFloorY + L::kSmsSlotY + c.h / 2 - c.insert, kConsoleZ + L::kSmsSlotZ);
        case ConsoleSystem::MegaCD: {
            // The Mega-CD under the Mega Drive, its tray sliding out of the front.
            mesh(ConsoleModel::MegaCD, identity());
            mesh(ConsoleModel::MegaDrive, translate(L::kCdMdX, L::kCdHeight, L::kCdMdZ));
            led(L::kCdLed, true);
            const float z = L::kTrayZ + L::kTrayTravel * tray;
            mesh(ConsoleModel::MegaCDTray, translate(L::kTrayX, L::kTrayY, z));
            return translate(L::kTrayX, kFloorY + L::kTrayY + 0.006f, kConsoleZ + z) * rotateX(-90);
        }
        case ConsoleSystem::Mega32X:
            mesh(ConsoleModel::MegaDrive, identity());
            mesh(ConsoleModel::Mega32X, translate(L::kMdSlotX, L::kMdSlotY, L::kMdSlotZ));
            led(L::kMdLed, false);
            return translate(L::kMdSlotX, kFloorY + L::kMdSlotY + L::k32xSlotY + c.h / 2 - c.insert, kConsoleZ + L::kMdSlotZ);
        default:
            mesh(ConsoleModel::MegaDrive, identity());
            led(L::kMdLed, false);
            return translate(L::kMdSlotX, kFloorY + L::kMdSlotY + c.h / 2 - c.insert, kConsoleZ + L::kMdSlotZ);
    }
}

void BoxShelf::build(const std::vector<Item>& items, int selected) {
    parts_.clear();
    const bool ejecting = state_ == State::Ejecting || state_ == State::Inserted;
    const float spread = ejecting ? 1.0f : smooth(open_);
    // The row: the chosen box in the middle, the others turned towards it, further aside
    // while one is open so its lid has room.
    const int first = std::max(0, static_cast<int>(std::floor(scroll_)) - 9);
    const int last = std::min(count_ - 1, static_cast<int>(std::ceil(scroll_)) + 9);
    Mat4 chosen = identity();
    for (int i = first; i <= last; ++i) {
        const float d = static_cast<float>(i) - scroll_;
        const float f = std::clamp(d, -1.0f, 1.0f);
        float x = f * 1.0f + (d - f) * 0.42f;
        if (i != selected) x += (d < 0 ? -1.5f : 1.3f) * spread;
        const Mat4 place = translate(x, 0, -std::fabs(f) * 0.5f) * rotateY(-f * 62.0f);
        if (i == selected) chosen = place;
        addBox(items, i, place, i == selected ? smooth(open_) : 0.0f);
    }
    if (!ejecting || selected < 0 || selected >= count_) return;
    // The console comes up through the floor while the cartridge leaves the box.
    const float light = state_ == State::Inserted ? 1.0f : 0.0f;
    const float t = state_ == State::Inserted ? 1.0f : eject_;
    const float rise = -1.6f * (1 - smooth(t / 0.3f));
    // The Mega-CD's tray opens for the disc and closes with it.
    const float tray = smooth((t - 0.1f) / 0.2f) * (1 - smooth((t - 0.84f) / 0.14f));
    const Mat4 inConsole = addConsole(items[static_cast<size_t>(selected)], light, rise, tray);
    // The cartridge's way: out of the box towards the viewer, over to above the slot in
    // an arc, then down into it. A disc turns to lie flat on the way.
    const Mat4 start = cartridgeInBox(chosen, system_);
    const Vec3 p0{start[12], start[13], start[14]}, p1{p0.x, p0.y + 0.1f, p0.z + 0.75f};
    const float above = system_ == ConsoleSystem::MegaCD ? 0.3f : cartridgeShape(system_).insert + 0.12f;
    const Vec3 p3{inConsole[12], inConsole[13], inConsole[14]}, p2{p3.x, p3.y + above, p3.z};
    Vec3 p;
    if (t < 0.25f) {
        const float s = smooth(t / 0.25f);
        p = {lerp(p0.x, p1.x, s), lerp(p0.y, p1.y, s), lerp(p0.z, p1.z, s)};
    } else if (t < 0.75f) {
        const float s = smooth((t - 0.25f) / 0.5f);
        p = {lerp(p1.x, p2.x, s), lerp(p1.y, p2.y, s) + std::sin(s * kPi) * 0.2f, lerp(p1.z, p2.z, s)};
    } else {
        // Into the slot; the disc drops onto the tray and rides in on it.
        const float s = smooth((t - 0.75f) / (system_ == ConsoleSystem::MegaCD ? 0.08f : 0.25f));
        p = {p3.x, lerp(p2.y, p3.y, s), p3.z};
    }
    const float turn = system_ == ConsoleSystem::MegaCD ? -90.0f * smooth((t - 0.25f) / 0.5f) : 0.0f;
    addCartridge(items[static_cast<size_t>(selected)], translate(p.x, p.y, p.z) * rotateX(turn));
}

unsigned int BoxShelf::render(const std::vector<Item>& items, int selected, int width, int height, float dt) {
    if (!program_ || width <= 0 || height <= 0) return 0;
    dt = std::clamp(dt, 0.0f, 0.1f);
    count_ = static_cast<int>(items.size());
    if (selected != selected_ && state_ != State::Browse && state_ != State::Closing && !frozen_) reset();
    selected_ = std::clamp(selected, 0, std::max(0, count_ - 1));
    if (count_ > 0) system_ = items[static_cast<size_t>(selected_)].system;
    else state_ = State::Browse;
    const float goal = static_cast<float>(selected_);
    if (!scrolled_) scroll_ = goal;
    scrolled_ = true;
    scroll_ += (goal - scroll_) * (1 - std::exp(-12 * dt));
    if (std::fabs(goal - scroll_) < 0.001f) scroll_ = goal;
    switch (frozen_ ? State::Browse : state_) {
        case State::Opening:
            scroll_ = goal;
            open_ = std::min(1.0f, open_ + dt / 0.7f);
            if (open_ >= 1) state_ = State::Open;
            break;
        case State::Closing:
            open_ = std::max(0.0f, open_ - dt / 0.5f);
            if (open_ <= 0) state_ = State::Browse;
            break;
        case State::Ejecting:
            eject_ = std::min(1.0f, eject_ + dt / 2.2f);
            if (eject_ >= 1) {
                state_ = State::Inserted;
                light_ = 0;
            }
            break;
        case State::Inserted:
            light_ += dt;
            if (light_ > 0.8f) launch_ = true;
            break;
        default: break;
    }

    ensureTarget(width, height);
    aspect_ = static_cast<float>(width) / static_cast<float>(height);
    tanHalf_ = std::tan(kFov * kRad / 2);
    build(items, selected_);
    Vec3 target;
    camera(&eye_, &target);
    // The camera's axes, for clicks, and its matrix.
    forward_ = {target.x - eye_.x, target.y - eye_.y, target.z - eye_.z};
    const float fl = std::sqrt(forward_.x * forward_.x + forward_.y * forward_.y + forward_.z * forward_.z);
    forward_ = {forward_.x / fl, forward_.y / fl, forward_.z / fl};
    right_ = {-forward_.z, 0, forward_.x};  // forward x (0, 1, 0)
    const float rl = std::sqrt(right_.x * right_.x + right_.z * right_.z);
    right_ = {right_.x / rl, 0, right_.z / rl};
    up_ = {right_.y * forward_.z - right_.z * forward_.y, right_.z * forward_.x - right_.x * forward_.z,
           right_.x * forward_.y - right_.y * forward_.x};
    Mat4 view = identity();
    view[0] = right_.x, view[4] = right_.y, view[8] = right_.z;
    view[1] = up_.x, view[5] = up_.y, view[9] = up_.z;
    view[2] = -forward_.x, view[6] = -forward_.y, view[10] = -forward_.z;
    view[12] = -(right_.x * eye_.x + right_.y * eye_.y + right_.z * eye_.z);
    view[13] = -(up_.x * eye_.x + up_.y * eye_.y + up_.z * eye_.z);
    view[14] = forward_.x * eye_.x + forward_.y * eye_.y + forward_.z * eye_.z;
    viewProj_ = perspective(kFov * kRad, aspect_, 0.1f, 60.0f) * view;

    gl::BindFramebuffer(gl::FRAMEBUFFER, fbo_);
    gl::Viewport(0, 0, width, height);
    gl::Disable(gl::SCISSOR_TEST);
    gl::Disable(gl::CULL_FACE);
    gl::Disable(gl::BLEND);
    gl::Enable(gl::DEPTH_TEST);
    gl::DepthFunc(gl::LEQUAL);
    gl::DepthMask(gl::TRUE_);
    gl::ClearColor(kBackground[0], kBackground[1], kBackground[2], 1);
    gl::Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
    gl::UseProgram(program_);
    gl::BindVertexArray(vao_);
    gl::ActiveTexture(gl::TEXTURE0);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uImage"), 0);
    gl::UniformMatrix4fv(gl::GetUniformLocation(program_, "uViewProj"), 1, gl::FALSE_, viewProj_.data());
    gl::Uniform3f(gl::GetUniformLocation(program_, "uEye"), eye_.x, eye_.y, eye_.z);
    gl::Uniform1f(gl::GetUniformLocation(program_, "uFloorY"), kFloorY);
    const gl::GLint uModel = gl::GetUniformLocation(program_, "uModel");
    const gl::GLint uColour = gl::GetUniformLocation(program_, "uColour");
    const gl::GLint uTexFaces = gl::GetUniformLocation(program_, "uTexFaces");
    const gl::GLint uTexRect = gl::GetUniformLocation(program_, "uTexRect");
    const gl::GLint uGlow = gl::GetUniformLocation(program_, "uGlow");
    const gl::GLint uShine = gl::GetUniformLocation(program_, "uShine");
    const gl::GLint uMirror = gl::GetUniformLocation(program_, "uMirror");
    const gl::GLint uFloor = gl::GetUniformLocation(program_, "uFloor");
    gl::Uniform3f(gl::GetUniformLocation(program_, "uBackground"), kBackground[0], kBackground[1], kBackground[2]);
    auto drawPart = [&](const Part& p) {
        gl::UniformMatrix4fv(uModel, 1, gl::FALSE_, p.model.data());
        gl::Uniform3f(uColour, p.colour.x, p.colour.y, p.colour.z);
        gl::Uniform1i(uTexFaces, p.texFaces);
        gl::Uniform4f(uTexRect, p.texRect[0], p.texRect[1], p.texRect[2], p.texRect[3]);
        gl::Uniform1f(uGlow, p.glow);
        gl::Uniform1f(uShine, p.shine);
        if (p.texFaces) gl::BindTexture(gl::TEXTURE_2D, p.texture);
        if (p.mesh >= 0) gl::DrawArrays(gl::TRIANGLES, meshFirst_[p.mesh], meshCount_[p.mesh]);
        else if (p.cylinder) gl::DrawArrays(gl::TRIANGLES, cylinderFirst_, cylinderCount_);
        else gl::DrawArrays(gl::TRIANGLES, cubeFirst_, cubeCount_);
    };
    // What the floor reflects, the floor over it, then the scene.
    gl::Uniform1i(uFloor, 0);
    gl::Uniform1i(uMirror, 1);
    for (const Part& p : parts_)
        if (p.reflect) drawPart(p);
    gl::Uniform1i(uMirror, 0);
    gl::Uniform1i(uFloor, 1);
    gl::Enable(gl::BLEND);
    gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
    Part floor;
    floor.model = translate(0, kFloorY - 0.005f, 0) * scale(40.0f, 0.01f, 40.0f);
    floor.colour = {0.1f, 0.1f, 0.115f};
    drawPart(floor);
    gl::Disable(gl::BLEND);
    gl::Uniform1i(uFloor, 0);
    for (const Part& p : parts_) drawPart(p);

    gl::BindVertexArray(0);
    gl::Disable(gl::DEPTH_TEST);
    gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
    return colour_;
}

}  // namespace plumsg
