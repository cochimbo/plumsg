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
// The shelf behind: the boxes' middle, their spacing, and its boards.
constexpr float kShelfZ = -1.6f, kSpacing = kBoxD + 0.012f;
constexpr float kBoard = 0.045f, kPlinth = 0.09f, kSide = 0.05f, kRowH = kBoxH + 0.1f + kBoard;
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
uniform sampler2D uNormals;
uniform int uHasNormals;
uniform sampler2D uLabel;
uniform int uHasLabel;
uniform vec4 uLabelArea;
uniform vec4 uLabelRect;
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
uniform vec4 uFloorFade;  // where the floor is brightest (x, z) and how far it fades out
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUv;
in vec3 vColour;
flat in int vFace;
out vec4 outColor;
void main() {
    if (uFloor == 1) {
        float r = length(vWorld.xz - uFloorFade.xy);
        outColor = vec4(mix(uColour, uBackground, smoothstep(uFloorFade.z, uFloorFade.w, r)), 0.84);
        return;
    }
    // What is still under the floor (the console rising) reflects nothing.
    if (uMirror == 1 && vWorld.y > uFloorY) discard;
    bool textured = ((uTexFaces >> vFace) & 1) != 0;
    vec3 base = textured ? texture(uImage, mix(uTexRect.xy, uTexRect.zw, vUv)).rgb * vColour : uColour * vColour;
    // A game's label printed over a cartridge model's own.
    if (uHasLabel == 1 && all(greaterThanEqual(vUv, uLabelArea.xy)) && all(lessThanEqual(vUv, uLabelArea.zw))) {
        vec2 t = (vUv - uLabelArea.xy) / (uLabelArea.zw - uLabelArea.xy);
        base = texture(uLabel, mix(uLabelRect.xy, uLabelRect.zw, vec2(t.x, 1.0 - t.y))).rgb;
    }
    vec3 n = normalize(vNormal);
    // A model's normal map, in a frame made from how its texture runs across the screen.
    if (uHasNormals == 1) {
        vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
        vec2 duv1 = dFdx(vUv), duv2 = dFdy(vUv);
        vec3 a = cross(dp2, n), b = cross(n, dp1);
        vec3 t = a * duv1.x + b * duv2.x, bt = a * duv1.y + b * duv2.y;
        float k = inversesqrt(max(max(dot(t, t), dot(bt, bt)), 1e-20));
        vec3 m = texture(uNormals, vUv).xyz * 2.0 - 1.0;
        n = normalize(mat3(t * k, -bt * k, n) * m);
    }
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

    vertexArray(v, &vao_, &vbo_);
}

void BoxShelf::vertexArray(const std::vector<float>& v, gl::GLuint* vao, gl::GLuint* vbo) {
    gl::GenVertexArrays(1, vao);
    gl::BindVertexArray(*vao);
    gl::GenBuffers(1, vbo);
    gl::BindBuffer(gl::ARRAY_BUFFER, *vbo);
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

void BoxShelf::upload(Custom& c, const GltfModel& model) {
    if (c.vao) gl::DeleteVertexArrays(1, &c.vao);
    if (c.vbo) gl::DeleteBuffers(1, &c.vbo);
    if (!c.textures.empty()) gl::DeleteTextures(static_cast<gl::GLsizei>(c.textures.size()), c.textures.data());
    c = Custom();
    vertexArray(model.vertices, &c.vao, &c.vbo);
    c.batches = model.batches;
    for (const GltfModel::Image& im : model.images) {
        gl::GLuint t = 0;
        gl::GenTextures(1, &t);
        gl::BindTexture(gl::TEXTURE_2D, t);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, im.width, im.height, 0, gl::RGBA, gl::UNSIGNED_BYTE, im.pixels.data());
        gl::GenerateMipmap(gl::TEXTURE_2D);
        c.textures.push_back(t);
    }
}

void BoxShelf::setConsoleModel(ConsoleModel which, const GltfModel& model, float width, float turn, const float slot[3],
                               const float* led, float ledTilt) {
    Custom& c = custom_[static_cast<int>(which)];
    upload(c, model);
    // Turned, its width measured across; then scaled, centred and stood on the floor.
    const float cx = (model.min[0] + model.max[0]) / 2, cz = (model.min[2] + model.max[2]) / 2;
    const float a = turn * kRad;
    const float across = std::fabs(std::cos(a)) * (model.max[0] - model.min[0]) + std::fabs(std::sin(a)) * (model.max[2] - model.min[2]);
    const float k = across > 0 ? width / across : 1;
    c.fit = scale(k, k, k) * rotateY(turn) * translate(-cx, -model.min[1], -cz);
    std::copy(slot, slot + 3, c.slot);
    if (led) {
        c.hasLed = true;
        std::copy(led, led + 3, c.led);
        c.ledTilt = ledTilt;
    }
}

namespace {

// The models that come with plumsg (or the user's own of the same consoles): how they are
// turned and where their slots are, once standing centred on the floor.
struct ModelFile {
    ConsoleModel model;
    const char* file;
    float width, turn;  // 13.5 cm = 1
    float slot[3];
    float led[3], ledTilt;
};
// The Mega Drive by synthetic worlds (Jon Blenky), CC BY 4.0: its slot is behind the dome,
// its power light on the plate in front of it.
const ModelFile kModelFiles[] = {
    {ConsoleModel::MegaDrive, "megadrive.glb", 278.0f / 135.0f, 0.0f, {0.206f, 0.43f, -0.306f}, {0.206f, 0.415f, 0.42f}, 12.9f},
};

// The Mega Drive cartridge by Naser, CC BY 4.0: where its texture has the label (which a
// scan replaces) and the picture and title above the "MEGA DRIVE" band of its European
// label (which a cover replaces).
constexpr const char* kCartridgeFile = "megadrive-cartridge.glb";
const float kCartridgeLabel[4] = {0.125f, 0.617f, 0.623f, 0.967f};
const float kCartridgePicture[4] = {0.125f, 0.662f, 0.623f, 0.967f};

}  // namespace

void BoxShelf::loadConsoleModels(const std::vector<std::string>& folders) {
    for (const std::string& folder : folders) {
        GltfModel model;
        std::string error;
        if (!loadGltf(folder + kCartridgeFile, model, &error)) continue;
        setCartridgeModel(model, kCartridgeLabel, kCartridgePicture);
        break;
    }
    for (const ModelFile& m : kModelFiles) {
        for (const std::string& folder : folders) {
            GltfModel model;
            std::string error;
            if (!loadGltf(folder + m.file, model, &error)) continue;
            setConsoleModel(m.model, model, m.width, m.turn, m.slot, m.led, m.ledTilt);
            break;
        }
    }
}

void BoxShelf::setCartridgeModel(const GltfModel& model, const float labelArea[4], const float pictureArea[4]) {
    Custom& c = custom_[kCartCustom];
    upload(c, model);
    // Centred and stretched to the cartridge's real size.
    const CartridgeShape shape = cartridgeShape(ConsoleSystem::MegaDrive);
    float size[3], mid[3];
    for (int i = 0; i < 3; ++i) {
        size[i] = std::max(model.max[i] - model.min[i], 1e-6f);
        mid[i] = (model.min[i] + model.max[i]) / 2;
    }
    c.fit = scale(shape.w / size[0], shape.h / size[1], shape.d / size[2]) * translate(-mid[0], -mid[1], -mid[2]);
    std::copy(labelArea, labelArea + 4, c.labelArea);
    std::copy(pictureArea, pictureArea + 4, c.pictureArea);
}

BoxShelf::~BoxShelf() {
    for (Custom& c : custom_) {
        if (c.vao) gl::DeleteVertexArrays(1, &c.vao);
        if (c.vbo) gl::DeleteBuffers(1, &c.vbo);
        if (!c.textures.empty()) gl::DeleteTextures(static_cast<gl::GLsizei>(c.textures.size()), c.textures.data());
    }
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
    take_ = open_ = eject_ = light_ = 0;
    boxYaw_ = boxPitch_ = cartYaw_ = cartPitch_ = 0;
    launch_ = launched_ = false;
    frozen_ = false;
}

void BoxShelf::pose(int step) {
    reset();
    if (step <= 0) return;
    take_ = 1;
    open_ = step >= 2 ? 1.0f : 0.0f;
    state_ = step == 1 ? State::Held : step == 2 ? State::Open : step == 3 ? State::Ejecting : State::Inserted;
    eject_ = step == 3 ? 0.6f : step == 4 ? 1.0f : 0.0f;
    frozen_ = true;
}

void BoxShelf::activate() {
    if (count_ == 0) return;
    switch (state_) {
        case State::Browse:
        case State::Returning: state_ = State::Taking; break;
        case State::Held:
        case State::Closing: state_ = State::Opening; break;
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
        case State::Taking:
        case State::Held: state_ = State::Returning; return true;
        case State::Opening:
        case State::Open: state_ = State::Closing; return true;
        default: return true;  // on its way into the console
    }
}

BoxShelf::Stage BoxShelf::stage() const {
    switch (state_) {
        case State::Browse:
        case State::Returning: return Stage::Shelf;
        case State::Taking:
        case State::Held:
        case State::Closing: return Stage::Box;
        case State::Opening:
        case State::Open: return Stage::Cartridge;
        default: return Stage::Console;
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
            selected_ = p->box;
            state_ = State::Taking;
            return p->box;
        case State::Held:
            if (p && p->box == selected_) activate();
            else back();
            return -1;
        case State::Open:
            if (p && p->cartridge) activate();
            else back();
            return -1;
        default: return -1;
    }
}

BoxShelf::Target BoxShelf::hover(float x, float y) {
    const Part* p = pick(x, y);
    switch (state_) {
        case State::Browse: return p && p->box >= 0 ? Target::Box : Target::None;
        case State::Held: return p && p->box == selected_ ? Target::Box : Target::None;
        case State::Open: return p && p->cartridge ? Target::Cartridge : Target::None;
        default: return Target::None;
    }
}

void BoxShelf::drag(float dx, float dy) {
    switch (state_) {
        case State::Held:
            boxYaw_ += dx * 360;
            boxPitch_ = std::clamp(boxPitch_ + dy * 180, -60.0f, 60.0f);
            break;
        case State::Open:
            cartYaw_ += dx * 360;
            cartPitch_ = std::clamp(cartPitch_ + dy * 180, -60.0f, 60.0f);
            break;
        case State::Browse: {
            // The shelf follows the pointer, as far as the picture is wide at its distance.
            const float across = 2 * distance_ * tanHalf_;
            pan_.x -= dx * across * aspect_;
            pan_.y += dy * across;
            break;
        }
        default: break;
    }
}

void BoxShelf::zoom(float steps) {
    if (state_ != State::Browse) return;
    distance_ *= std::pow(0.85f, steps);  // the most is set by render, once it knows the shelf
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
        if ((p.box < 0 && !p.cartridge) || (p.cartridge && p.custom >= 0)) continue;
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
    const float a = smooth(take_), o = smooth(open_);
    const float e = state_ == State::Inserted ? 1.0f : state_ == State::Ejecting ? smooth(eject_ / 0.45f) : 0.0f;
    const float x = stageX_;
    // The shelf, the box out, its cartridge out and the console: each framed for its size.
    Vec3 consoleEye{0.3f, 0.9f, 4.9f}, consoleTarget{0.1f, -0.35f, 1.5f};
    switch (system_) {
        case ConsoleSystem::Mega32X: consoleEye = {0.35f, 1.3f, 5.5f}, consoleTarget = {0.1f, -0.05f, 1.4f}; break;
        case ConsoleSystem::MegaCD: consoleEye = {0.5f, 1.2f, 5.9f}, consoleTarget = {0.0f, -0.25f, 1.9f}; break;
        case ConsoleSystem::MasterSystem: consoleEye = {0.4f, 1.0f, 5.5f}, consoleTarget = {0.2f, -0.35f, 1.4f}; break;
        case ConsoleSystem::GameGear: consoleEye = {0.15f, 0.5f, 4.3f}, consoleTarget = {0.0f, -0.12f, 1.6f}; break;
        default: break;
    }
    const Vec3 shelfTarget = look_, shelfEye{look_.x, look_.y + distance_ * 0.1f, look_.z + distance_};
    Vec3 eyes[3] = {{x, 0.3f, 3.4f}, {x - 0.2f, 0.35f, 3.6f}, {x + consoleEye.x, consoleEye.y, consoleEye.z}};
    const Vec3 targets[3] = {{x, 0.05f, 0}, {x - 0.2f, 0.0f, 0.3f}, {x + consoleTarget.x, consoleTarget.y, consoleTarget.z}};
    // A narrow view steps back from the box and the console to keep them in.
    const float back = std::max(1.0f, 1.6f / aspect_);
    for (int i = 0; i < 3; ++i)
        eyes[i] = {targets[i].x + (eyes[i].x - targets[i].x) * back, targets[i].y + (eyes[i].y - targets[i].y) * back,
                   targets[i].z + (eyes[i].z - targets[i].z) * back};
    auto mix = [&](const Vec3& shelf, const Vec3 (&k)[3]) {
        const Vec3 b{lerp(k[0].x, k[1].x, o), lerp(k[0].y, k[1].y, o), lerp(k[0].z, k[1].z, o)};
        const Vec3 c{lerp(b.x, k[2].x, e), lerp(b.y, k[2].y, e), lerp(b.z, k[2].z, e)};
        return Vec3{lerp(shelf.x, c.x, a), lerp(shelf.y, c.y, a), lerp(shelf.z, c.z, a)};
    };
    *eye = mix(shelfEye, eyes);
    *target = mix(shelfTarget, targets);
}

void BoxShelf::shelfSize(float* width, float* height, float* bottom) const {
    *width = static_cast<float>(columns_) * kSpacing + 2 * kSide + 0.06f;
    *height = kPlinth + static_cast<float>(rows_) * kRowH + kBoard;
    *bottom = kFloorY;
}

BoxShelf::Vec3 BoxShelf::shelfSpot(int index) const {
    const int c = index % columns_, r = index / columns_;
    return {(static_cast<float>(c) - static_cast<float>(columns_ - 1) / 2) * kSpacing,
            kFloorY + kPlinth + static_cast<float>(rows_ - 1 - r) * kRowH + kBoxH / 2 + 0.002f, kShelfZ};
}

BoxShelf::Mat4 BoxShelf::heldBox() const {
    // Out of the shelf towards the viewer, then over to the front, turning to show its
    // cover; aside to the left once its cartridge is out.
    const Vec3 s = shelfSpot(selected_);
    const float t1 = smooth(take_ / 0.4f), t2 = smooth((take_ - 0.3f) / 0.7f);
    const Vec3 out{s.x, s.y, kShelfZ + 1.1f}, front{stageX_ - 0.35f * smooth(open_), 0.1f, 0};
    const Vec3 a{lerp(s.x, out.x, t1), lerp(s.y, out.y, t1), lerp(s.z, out.z, t1)};
    const Vec3 p{lerp(a.x, front.x, t2), lerp(a.y, front.y, t2), lerp(a.z, front.z, t2)};
    return translate(p.x, p.y, p.z) * rotateY(90 * (1 - t2) + boxYaw_) * rotateX(boxPitch_);
}

BoxShelf::Mat4 BoxShelf::heldCartridge() const {
    // Lifted out of the box and up in front of it, facing the viewer.
    const Mat4 in = cartridgeInBox(heldBox(), system_);
    const float s = smooth((open_ - 0.35f) / 0.65f);
    if (s <= 0) return in;
    const Vec3 out{stageX_ + 0.45f, 0.1f, 1.3f};
    const Vec3 p{lerp(in[12], out.x, s), lerp(in[13], out.y, s) + std::sin(s * kPi) * 0.25f, lerp(in[14], out.z, s)};
    return translate(p.x, p.y, p.z) * rotateY(cartYaw_ * s) * rotateX(cartPitch_ * s);
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
    // Its own label, or else the cover's front.
    const unsigned int art = item.label ? item.label : item.texture;
    p.texRect = item.label ? std::array<float, 4>{0, 0, 1, 1} : sleeve(item.width, item.height).front;
    if (item.system == ConsoleSystem::MegaCD) {
        // A 12 cm disc, 1.2 mm thick, its label printed with the cover; the hole in the middle.
        p.model = place * scale(0.889f, 0.889f, 0.009f);
        p.colour = {0.78f, 0.79f, 0.84f};
        p.shine = 1.0f;
        p.cylinder = true;
        p.texture = art;
        p.texFaces = art ? 1 << 4 : 0;
        parts_.push_back(p);
        p.model = place * scale(0.111f, 0.111f, 0.0095f);
        p.colour = {kBackground[0], kBackground[1], kBackground[2]};
        p.texFaces = 0;
        p.shine = 0;
        parts_.push_back(p);
        return;
    }
    const CartridgeShape c = cartridgeShape(item.system);
    // A Mega Drive cartridge's own model, with the game's label over the one it has.
    const Custom& own = custom_[kCartCustom];
    if (item.system == ConsoleSystem::MegaDrive && own.vao) {
        p.model = place * own.fit;
        p.colour = {1, 1, 1};
        p.shine = 0.35f;
        p.custom = kCartCustom;
        p.label = art;
        p.labelRect = p.texRect;
        p.labelWhole = item.label != 0;
        // A cover's middle, cut to the picture's shape (about 95 x 53 mm on the cartridge).
        if (!p.labelWhole && item.width > 0 && item.height > 0) {
            std::array<float, 4>& r = p.labelRect;
            const float w = (r[2] - r[0]) * static_cast<float>(item.width);
            const float h = std::min(w / 1.8f / static_cast<float>(item.height), r[3] - r[1]);
            const float mid = r[1] + (r[3] - r[1]) * 0.45f;
            r[1] = std::clamp(mid - h / 2, r[1], r[3] - h);
            r[3] = r[1] + h;
        }
        p.texRect = {0, 0, 1, 1};
        p.reflect = true;
        parts_.push_back(p);
        Part hit;
        hit.model = place * scale(c.w, c.h, c.d);
        hit.cartridge = true;
        hit.hidden = true;
        parts_.push_back(hit);
        return;
    }
    p.model = place;
    p.colour = {1, 1, 1};
    p.shine = 0.35f;
    p.mesh = kCartMeshes + static_cast<int>(item.system);
    parts_.push_back(p);
    // The label (the cover) in its recess.
    p.mesh = -1;
    p.model = place * translate(0, c.labelY, c.d / 2 + 0.0025f) * scale(c.labelW, c.labelH, 0.001f);
    p.colour = art ? Vec3{1, 1, 1} : Vec3{0.75f, 0.72f, 0.66f};
    p.shine = 0.15f;
    p.texture = art;
    p.texFaces = art ? 1 << 4 : 0;
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
}

BoxShelf::Mat4 BoxShelf::addConsole(const Item& item, float light, float rise, float tray) {
    using L = ConsoleLayout;
    const ConsoleSystem s = item.system;
    const Mat4 base = translate(stageX_, kFloorY + rise, kConsoleZ);
    // The console's own model when it was given one, else the built-in.
    auto mesh = [&](ConsoleModel m, const Mat4& at) {
        Part p;
        p.colour = {1, 1, 1};
        p.shine = 0.45f;
        const Custom& c = custom_[static_cast<int>(m)];
        if (c.vao) {
            p.model = base * at * c.fit;
            p.custom = static_cast<int>(m);
        } else {
            p.model = base * at;
            p.mesh = static_cast<int>(m);
        }
        parts_.push_back(p);
        // Its own power light, lit (the model shows it off).
        if (c.vao && c.hasLed && light > 0) {
            Part l;
            l.model = base * at * translate(c.led[0], c.led[1], c.led[2]) * rotateX(c.ledTilt - 90) * scale(0.022f, 0.022f, 0.002f);
            l.colour = {1.0f, 0.12f, 0.08f};
            l.glow = 1.6f;
            l.cylinder = true;
            parts_.push_back(l);
        }
    };
    // Where its slot is: the given model's, or the built-in's.
    auto slotOf = [&](ConsoleModel m, float x, float y, float z) {
        const Custom& c = custom_[static_cast<int>(m)];
        return c.vao ? Vec3{c.slot[0], c.slot[1], c.slot[2]} : Vec3{x, y, z};
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
        case ConsoleSystem::Mega32X: {
            mesh(ConsoleModel::MegaDrive, identity());
            const Vec3 md = slotOf(ConsoleModel::MegaDrive, L::kMdSlotX, L::kMdSlotY, L::kMdSlotZ);
            mesh(ConsoleModel::Mega32X, translate(md.x, md.y, md.z));
            if (!custom_[static_cast<int>(ConsoleModel::MegaDrive)].vao) led(L::kMdLed, false);
            const Vec3 x = slotOf(ConsoleModel::Mega32X, 0, L::k32xSlotY, 0);
            return translate(md.x + x.x, kFloorY + md.y + x.y + c.h / 2 - c.insert, kConsoleZ + md.z + x.z);
        }
        default: {
            mesh(ConsoleModel::MegaDrive, identity());
            if (!custom_[static_cast<int>(ConsoleModel::MegaDrive)].vao) led(L::kMdLed, false);
            const Vec3 md = slotOf(ConsoleModel::MegaDrive, L::kMdSlotX, L::kMdSlotY, L::kMdSlotZ);
            return translate(md.x, kFloorY + md.y + c.h / 2 - c.insert, kConsoleZ + md.z);
        }
    }
}

void BoxShelf::addShelfBox(const Item& item, int index, const Mat4& place) {
    // Closed, only its spine shows: the sleeve's, or the edge of the cover next to it.
    Part p;
    p.model = place * scale(kBoxW, kBoxH, kBoxD);
    p.colour = {0.045f, 0.045f, 0.055f};
    p.shine = 0.6f;
    p.box = index;
    if (item.texture) {
        const Sleeve cover = sleeve(item.width, item.height);
        p.texture = item.texture;
        p.texFaces = 1 << 1;
        if (cover.spine[2] > cover.spine[0]) p.texRect = cover.spine;
        else p.texRect = {cover.front[0], cover.front[1], cover.front[0] + (cover.front[2] - cover.front[0]) * kBoxD / kBoxW,
                          cover.front[3]};
    }
    parts_.push_back(p);
}

void BoxShelf::addShelf() {
    // Dark wood: a board under each row and over the top one, the sides and the back.
    float w, h, bottom;
    shelfSize(&w, &h, &bottom);
    const float depth = kBoxW + 0.12f, z = kShelfZ - 0.03f;
    auto board = [&](float x, float y, float zz, float sx, float sy, float sz, Vec3 colour) {
        Part p;
        p.model = translate(x, y, zz) * scale(sx, sy, sz);
        p.colour = colour;
        p.shine = 0.25f;
        parts_.push_back(p);
    };
    const Vec3 wood{0.17f, 0.1f, 0.06f}, inside{0.11f, 0.065f, 0.04f};
    board(0, bottom + kPlinth / 2, z, w, kPlinth, depth, wood);
    for (int r = 1; r <= rows_; ++r)
        board(0, bottom + kPlinth + static_cast<float>(r) * kRowH - kBoard / 2, z, w, kBoard, depth, wood);
    board(-w / 2 + kSide / 2, bottom + h / 2, z, kSide, h, depth, wood);
    board(w / 2 - kSide / 2, bottom + h / 2, z, kSide, h, depth, wood);
    board(0, bottom + h / 2, z - depth / 2 + 0.01f, w - 2 * kSide, h, 0.02f, inside);
}

void BoxShelf::build(const std::vector<Item>& items, int selected) {
    parts_.clear();
    addShelf();
    for (int i = 0; i < count_; ++i) {
        if (i == selected && state_ != State::Browse) continue;
        Vec3 s = shelfSpot(i);
        if (i == pulled_) s.z += 0.14f * smooth(pull_);
        addShelfBox(items[static_cast<size_t>(i)], i, translate(s.x, s.y, s.z) * rotateY(90));
    }
    if (state_ == State::Browse || selected < 0 || selected >= count_) return;
    const Item& item = items[static_cast<size_t>(selected)];
    const Mat4 box = heldBox();
    const float lid = smooth(open_ / 0.6f);
    addBox(items, selected, box, lid);
    const bool ejecting = state_ == State::Ejecting || state_ == State::Inserted;
    if (!ejecting) {
        if (lid > 0.001f) addCartridge(item, heldCartridge());
        return;
    }
    // The console comes up through the floor while the cartridge leaves the box.
    const float light = state_ == State::Inserted ? 1.0f : 0.0f;
    const float t = state_ == State::Inserted ? 1.0f : eject_;
    const float rise = -1.6f * (1 - smooth(t / 0.3f));
    // The Mega-CD's tray opens for the disc and closes with it.
    const float tray = smooth((t - 0.1f) / 0.2f) * (1 - smooth((t - 0.84f) / 0.14f));
    const Mat4 inConsole = translate(stageX_, 0, 0) * addConsole(item, light, rise, tray);
    // The cartridge's way: up from where it was held, over to above the slot in an arc,
    // then down into it. A disc turns to lie flat on the way.
    const Mat4 start = heldCartridge();
    const Vec3 p0{start[12], start[13], start[14]}, p1{p0.x, p0.y + 0.15f, p0.z};
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
    addCartridge(item, translate(p.x, p.y, p.z) * rotateY(cartYaw_) * rotateX(cartPitch_ + turn));
}

unsigned int BoxShelf::render(const std::vector<Item>& items, int selected, int width, int height, float dt) {
    if (!program_ || width <= 0 || height <= 0) return 0;
    dt = std::clamp(dt, 0.0f, 0.1f);
    count_ = static_cast<int>(items.size());
    if (selected != selected_ && state_ != State::Browse && !frozen_) reset();
    if (selected != selected_) pan_ = {};  // chosen with the keys: the camera goes to it
    selected_ = std::clamp(selected, 0, std::max(0, count_ - 1));
    if (count_ > 0) system_ = items[static_cast<size_t>(selected_)].system;
    else state_ = State::Browse;
    // The shelf about half again as wide as tall, but never fewer than 8 boxes a row.
    const float n = static_cast<float>(std::max(1, count_));
    columns_ = std::clamp(static_cast<int>(std::lround(std::sqrt(n * kRowH * 1.6f / kSpacing))), std::min(count_, 8), std::max(1, count_));
    columns_ = std::max(1, columns_);
    rows_ = std::max(1, (count_ + columns_ - 1) / columns_);
    if (state_ == State::Browse) stageX_ = shelfSpot(selected_).x;
    if (pulled_ != selected_) pulled_ = selected_, pull_ = 0;
    pull_ = std::min(1.0f, pull_ + dt / 0.25f);
    switch (frozen_ ? State::Browse : state_) {
        case State::Taking:
            take_ = std::min(1.0f, take_ + dt / 0.9f);
            if (take_ >= 1) state_ = State::Held;
            break;
        case State::Returning:
            take_ = std::max(0.0f, take_ - dt / 0.7f);
            if (take_ <= 0) state_ = State::Browse;
            break;
        case State::Opening:
            open_ = std::min(1.0f, open_ + dt / 0.9f);
            if (open_ >= 1) state_ = State::Open;
            break;
        case State::Closing:
            open_ = std::max(0.0f, open_ - dt / 0.7f);
            if (open_ <= 0) state_ = State::Held;
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
    // What is no longer held turns back to face the viewer.
    auto settle = [&](float& yaw, float& pitch) {
        yaw = std::remainder(yaw, 360.0f);
        const float k = std::exp(-7 * dt);
        yaw *= k;
        pitch *= k;
    };
    if (state_ != State::Held && !frozen_) settle(boxYaw_, boxPitch_);
    if (state_ != State::Open && !frozen_) settle(cartYaw_, cartPitch_);

    ensureTarget(width, height);
    aspect_ = static_cast<float>(width) / static_cast<float>(height);
    tanHalf_ = std::tan(kFov * kRad / 2);
    // The shelf's camera: on the chosen box (and as far as it was dragged), going over to
    // the whole shelf as it steps back, no further than to see all of it.
    float w, h, bottom;
    shelfSize(&w, &h, &bottom);
    const float fit = std::max(w / aspect_, h) / (2 * tanHalf_) * 1.08f + 0.6f;
    const float nearest = 1.6f;
    distance_ = std::clamp(distance_, nearest, std::max(fit, 3.0f));
    const Vec3 spot = shelfSpot(selected_), middle{0, bottom + h / 2, kShelfZ};
    const float all = smooth((distance_ - nearest) / std::max(0.01f, std::max(fit, 3.0f) - nearest));
    Vec3 goal{lerp(spot.x + pan_.x, middle.x, all), lerp(spot.y + pan_.y, middle.y, all), kShelfZ + 0.5f};
    goal.x = std::clamp(goal.x, -w / 2, w / 2);
    goal.y = std::clamp(goal.y, bottom + 0.5f, bottom + std::max(h - 0.5f, 0.5f));
    if (!placed_) look_ = goal;
    placed_ = true;
    const float follow = 1 - std::exp(-10 * dt);
    look_ = {look_.x + (goal.x - look_.x) * follow, look_.y + (goal.y - look_.y) * follow, goal.z};
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
    viewProj_ = perspective(kFov * kRad, aspect_, 0.1f, 150.0f) * view;

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
    gl::Uniform1i(gl::GetUniformLocation(program_, "uNormals"), 1);
    gl::Uniform1i(gl::GetUniformLocation(program_, "uLabel"), 2);
    const gl::GLint uHasLabel = gl::GetUniformLocation(program_, "uHasLabel");
    const gl::GLint uLabelArea = gl::GetUniformLocation(program_, "uLabelArea");
    const gl::GLint uLabelRect = gl::GetUniformLocation(program_, "uLabelRect");
    const gl::GLint uHasNormals = gl::GetUniformLocation(program_, "uHasNormals");
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
    // The floor lit around what is looked at, further the further back the camera is.
    const float reach = std::max(1.0f, std::sqrt((target.x - eye_.x) * (target.x - eye_.x) + (target.y - eye_.y) * (target.y - eye_.y) +
                                                 (target.z - eye_.z) * (target.z - eye_.z)) / 3.6f);
    gl::Uniform4f(gl::GetUniformLocation(program_, "uFloorFade"), target.x, target.z + 0.8f, 1.0f * reach, 6.5f * reach);
    auto drawPart = [&](const Part& p) {
        gl::UniformMatrix4fv(uModel, 1, gl::FALSE_, p.model.data());
        gl::Uniform3f(uColour, p.colour.x, p.colour.y, p.colour.z);
        gl::Uniform1i(uTexFaces, p.texFaces);
        gl::Uniform4f(uTexRect, p.texRect[0], p.texRect[1], p.texRect[2], p.texRect[3]);
        gl::Uniform1f(uGlow, p.glow);
        gl::Uniform1f(uShine, p.shine);
        if (p.texFaces) gl::BindTexture(gl::TEXTURE_2D, p.texture);
        if (p.hidden) return;
        if (p.custom >= 0) {
            const Custom& c = custom_[p.custom];
            if (p.label) {
                const float* area = p.labelWhole ? c.labelArea : c.pictureArea;
                gl::Uniform1i(uHasLabel, 1);
                gl::Uniform4f(uLabelArea, area[0], area[1], area[2], area[3]);
                gl::Uniform4f(uLabelRect, p.labelRect[0], p.labelRect[1], p.labelRect[2], p.labelRect[3]);
                gl::ActiveTexture(gl::TEXTURE0 + 2);
                gl::BindTexture(gl::TEXTURE_2D, p.label);
                gl::ActiveTexture(gl::TEXTURE0);
            }
            gl::BindVertexArray(c.vao);
            const int images = static_cast<int>(c.textures.size());
            for (const GltfModel::Batch& b : c.batches) {
                const bool textured = b.image >= 0 && b.image < images, bumped = b.normals >= 0 && b.normals < images;
                gl::Uniform1i(uTexFaces, textured ? 1 << kGltfFace : 0);
                if (textured) gl::BindTexture(gl::TEXTURE_2D, c.textures[static_cast<size_t>(b.image)]);
                gl::Uniform1i(uHasNormals, bumped ? 1 : 0);
                if (bumped) {
                    gl::ActiveTexture(gl::TEXTURE0 + 1);
                    gl::BindTexture(gl::TEXTURE_2D, c.textures[static_cast<size_t>(b.normals)]);
                    gl::ActiveTexture(gl::TEXTURE0);
                }
                gl::DrawArrays(gl::TRIANGLES, b.first, b.count);
            }
            gl::Uniform1i(uHasNormals, 0);
            gl::Uniform1i(uHasLabel, 0);
            gl::BindVertexArray(vao_);
        } else if (p.mesh >= 0) gl::DrawArrays(gl::TRIANGLES, meshFirst_[p.mesh], meshCount_[p.mesh]);
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
    floor.model = translate(target.x, kFloorY - 0.005f, 0) * scale(120.0f, 0.01f, 120.0f);
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
