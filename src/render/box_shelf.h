#pragma once

#include <array>
#include <string>
#include <vector>

#include "../core/system.h"
#include "console_models.h"
#include "gl.h"
#include "gltf_model.h"

namespace plumsg {

// The library's 3D view: the games' boxes standing in a row, the chosen one in the
// middle facing the viewer and the rest turned aside on both sides, over a floor that
// reflects them. Opening the chosen box swings its lid on the hinge at the left, like a
// Mega Drive case, and shows the cartridge (the disc for the Mega CD) and the manual
// inside. Taking the cartridge out flies it into the slot of the game's console, whose
// light comes on, and then the game is played (takeLaunch()).
//
// It draws with OpenGL 3.3 into a texture of its own, which the library shows as an
// image; clicks on that image are passed back as points from 0 to 1.
class BoxShelf {
public:
    struct Item {
        // The cover, 0 if there is none. One much wider than tall is a whole sleeve, as
        // The Cover Project's are: the back, the spine and the front side by side.
        unsigned int texture = 0;
        int width = 0, height = 0;
        ConsoleSystem system = ConsoleSystem::MegaDrive;
        // The cartridge's label (the disc's on the Mega CD); without one the cover's front.
        unsigned int label = 0;
        int labelWidth = 0, labelHeight = 0;
    };
    // What the mouse is over.
    enum class Target { None, Box, Cartridge };

    // Where the front starts across a cover (0 to 1): 0 unless it is a whole sleeve.
    static float coverFront(int width, int height);

    BoxShelf();
    ~BoxShelf();

    // Draws `model` (a glTF model of the console) in place of the built-in one. It is
    // scaled to `width` (its real width, 13.5 cm = 1), turned `turn` degrees around its
    // vertical axis first, and stood on the floor centred; `slot` is the top of the middle
    // of its cartridge slot (from the middle of its footprint, same units), and `led`, if
    // given, the middle of its power light, which lights up there, tilted `ledTilt`
    // degrees towards the front.
    void setConsoleModel(ConsoleModel which, const GltfModel& model, float width, float turn, const float slot[3],
                         const float* led = nullptr, float ledTilt = 0);
    // Draws the Mega Drive's cartridges with `model` (a glTF model of one, front towards
    // +z, top up), stretched to their real size; `labelArea` and `pictureArea` (u0, v0, u1,
    // v1) are where its texture has the label and the picture in it, which a game's label
    // scan or cover replaces.
    void setCartridgeModel(const GltfModel& model, const float labelArea[4], const float pictureArea[4]);
    // Loads the consoles' models that plumsg knows how to place (megadrive.glb), from the
    // first of `folders` that has each. Those it cannot read keep the built-in model.
    void loadConsoleModels(const std::vector<std::string>& folders);
    BoxShelf(const BoxShelf&) = delete;
    BoxShelf& operator=(const BoxShelf&) = delete;

    // Moves the animations on by `dt` seconds and draws the shelf with `selected` in the
    // middle into a width x height texture, which it returns (OpenGL's rows, bottom up).
    // Leaves framebuffer 0 bound.
    unsigned int render(const std::vector<Item>& items, int selected, int width, int height, float dt);

    // A click at (x, y), from 0 to 1 across the last picture drawn, from its top left. On a
    // closed box at the side returns its index, to choose it. On the chosen box opens it;
    // with it open takes the cartridge out when it is clicked, and closes it anywhere else.
    int click(float x, float y);
    // What is under (x, y), for the pointer: something there can be clicked.
    Target hover(float x, float y);
    // A or Enter: opens the chosen box, or takes its cartridge out.
    void activate();
    // B or Esc: closes the open box. False if none was open.
    bool back();
    // A box open or on its way: the chosen game must not change.
    bool busy() const { return state_ != State::Browse; }
    // The chosen box is open (or opening), waiting for a click on its cartridge.
    bool boxOpen() const { return state_ == State::Opening || state_ == State::Open; }
    // True once, when the cartridge has gone into the console: play the chosen game.
    bool takeLaunch();
    // Back to the row of closed boxes, with no animation.
    void reset();
    // For screenshots (--boxes): holds still with the chosen box open (1), its cartridge
    // on the way to the console (2) or in it (3).
    void pose(int step);

private:
    enum class State { Browse, Opening, Open, Closing, Ejecting, Inserted };
    using Mat4 = std::array<float, 16>;
    struct Vec3 {
        float x = 0, y = 0, z = 0;
    };
    // One shape of the scene: a unit cube or cylinder placed by `model`.
    struct Part {
        Mat4 model;
        Vec3 colour;
        unsigned int texture = 0;
        int texFaces = 0;     // bit per face that shows the texture (see kCube)
        std::array<float, 4> texRect{0, 0, 1, 1};  // the part of it shown (u0, v0, u1, v1)
        bool cylinder = false;
        int mesh = -1;        // a console or a cartridge's shell instead (meshFirst_)
        int custom = -1;      // or a console's own model (custom_), or the cartridge's (kCartCustom)
        unsigned int label = 0;  // on a cartridge's own model: the game's label, over its own
        std::array<float, 4> labelRect{0, 0, 1, 1};  // the part of `label` shown
        bool labelWhole = false;  // a scan of the whole label (else art for its picture)
        float glow = 0;       // lights itself (the console's power light)
        float shine = 0.4f;   // how glossy
        bool reflect = true;  // seen in the floor
        int box = -1;         // the item whose box this is part of, for clicks
        bool cartridge = false;
        bool hidden = false;  // only there for clicks
    };

    void build(const std::vector<Item>& items, int selected);
    void addBox(const std::vector<Item>& items, int index, const Mat4& place, float lid);
    Mat4 cartridgeInBox(const Mat4& box, ConsoleSystem s) const;
    void addCartridge(const Item& item, const Mat4& place);
    // The console of the item's game, `rise` above its place (the Mega-CD's tray `tray`
    // open, 0 to 1); returns where the cartridge stands once it is in.
    Mat4 addConsole(const Item& item, float light, float rise, float tray);
    void camera(Vec3* eye, Vec3* target) const;
    // The part under (x, y), or nullptr.
    const Part* pick(float x, float y) const;
    void ensureTarget(int width, int height);
    void vertexArray(const std::vector<float>& vertices, gl::GLuint* vao, gl::GLuint* vbo);

    gl::GLuint program_ = 0;
    gl::GLuint vao_ = 0, vbo_ = 0;
    int cubeFirst_ = 0, cubeCount_ = 0, cylinderFirst_ = 0, cylinderCount_ = 0;
    // The consoles (ConsoleModel), then a cartridge's shell for each ConsoleSystem.
    static constexpr int kCartMeshes = 6, kMeshes = kCartMeshes + kSystemCount;
    int meshFirst_[kMeshes] = {}, meshCount_[kMeshes] = {};
    // Models given with setConsoleModel, by ConsoleModel.
    struct Custom {
        gl::GLuint vao = 0, vbo = 0;
        std::vector<GltfModel::Batch> batches;
        std::vector<gl::GLuint> textures;
        Mat4 fit{};      // from the file's space to the console's place on the floor
        float slot[3] = {0, 0, 0};
        bool hasLed = false;
        float led[3] = {0, 0, 0}, ledTilt = 0;
        // A cartridge's: where its label is in its texture, all of it and its picture.
        float labelArea[4] = {0, 0, 0, 0}, pictureArea[4] = {0, 0, 0, 0};
    };
    // The consoles' (by ConsoleModel), then the Mega Drive cartridge's.
    static constexpr int kCartCustom = static_cast<int>(ConsoleModel::Count);
    Custom custom_[kCartCustom + 1];
    void upload(Custom& c, const GltfModel& model);
    gl::GLuint fbo_ = 0, colour_ = 0, depth_ = 0;
    int width_ = 0, height_ = 0;

    State state_ = State::Browse;
    float scroll_ = 0;     // the item in the middle, gliding towards the chosen one
    bool scrolled_ = false;  // scroll_ has been set once
    float open_ = 0;       // 0 closed .. 1 the lid all the way open
    float eject_ = 0;      // 0 .. 1 the cartridge's way into the console
    float light_ = 0;      // seconds since it went in
    bool launch_ = false, launched_ = false;
    bool frozen_ = false;  // pose()
    int selected_ = 0, count_ = 0;
    ConsoleSystem system_ = ConsoleSystem::MegaDrive;  // of the chosen game

    std::vector<Part> parts_;
    Mat4 viewProj_{};
    Vec3 eye_, forward_, right_, up_;
    float tanHalf_ = 0, aspect_ = 1;
};

}  // namespace plumsg
