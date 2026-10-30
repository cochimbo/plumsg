#pragma once

#include <array>
#include <string>
#include <vector>

#include "../core/system.h"
#include "console_models.h"
#include "gl.h"
#include "gltf_model.h"

namespace plumsg {

// The library's 3D view: the games' boxes on a shelf, their spines to the viewer, over
// a floor that reflects them. The wheel zooms out to the whole shelf and dragging moves
// along it. A click takes a box out to the front, where dragging turns it; a click on it
// opens its lid on the hinge at the left, like a Mega Drive case, and brings its cartridge
// (the disc for the Mega CD) out, which dragging turns as well; a click on the cartridge
// puts it into the slot of the game's console, whose light comes on, and then the game is
// played (takeLaunch()). A click anywhere else goes a step back.
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
    // What is in front: the shelf, a box taken out, its cartridge out, or the console.
    enum class Stage { Shelf, Box, Cartridge, Console };

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
    // Draws the system's cartridges with `model` (a glTF model of one), turned `turnX` then
    // `turnZ` degrees to stand with its label to the front (+z), stretched to their real
    // size; `labelArea` and `pictureArea` (u0, v0, u1, v1) are where its texture has the
    // label and the picture in it, which a game's label scan or cover replaces, the label
    // turned `labelTurn` quarter turns there; `pictureAspect` is the picture's width / height,
    // and a cover shows there its part `pictureMiddle` of the way down.
    void setCartridgeModel(ConsoleSystem system, const GltfModel& model, float turnX, float turnZ, const float labelArea[4],
                           const float pictureArea[4], int labelTurn, float pictureAspect, float pictureMiddle);
    // Loads the consoles' and cartridges' models that plumsg knows how to place
    // (megadrive.glb, mastersystem.glb and their cartridges), from the first of `folders`
    // that has each. Those it cannot read keep the built-in model.
    void loadConsoleModels(const std::vector<std::string>& folders);
    BoxShelf(const BoxShelf&) = delete;
    BoxShelf& operator=(const BoxShelf&) = delete;

    // Moves the animations on by `dt` seconds and draws the shelf with `selected` in the
    // middle into a width x height texture, which it returns (OpenGL's rows, bottom up).
    // Leaves framebuffer 0 bound.
    unsigned int render(const std::vector<Item>& items, int selected, int width, int height, float dt);

    // A click at (x, y), from 0 to 1 across the last picture drawn, from its top left. On
    // the shelf a box is taken out (its index is returned, to choose it); a box out opens
    // and gives its cartridge, and the cartridge goes into the console; a click elsewhere
    // goes back a step.
    int click(float x, float y);
    // What is under (x, y), for the pointer: something there can be clicked.
    Target hover(float x, float y);
    // The mouse dragged by (dx, dy), as parts of the picture: turns the box or cartridge in
    // front, or moves along the shelf.
    void drag(float dx, float dy);
    // The wheel: up comes closer to the shelf, down steps back until it is all in view.
    void zoom(float steps);
    // A or Enter: the next step (takes the box out, its cartridge, into the console).
    void activate();
    // B or Esc: a step back. False on the shelf.
    bool back();
    Stage stage() const;
    // Something is out of the shelf: the chosen game must not change.
    bool busy() const { return state_ != State::Browse; }
    // The boxes on each row of the shelf, for up and down.
    int columns() const { return columns_; }
    // True once, when the cartridge has gone into the console: play the chosen game.
    bool takeLaunch();
    // Back to the row of closed boxes, with no animation.
    void reset();
    // For screenshots (--boxes): holds still with the chosen box out (1), its cartridge
    // out (2), on the way to the console (3) or in it (4).
    void pose(int step);

private:
    enum class State { Browse, Taking, Held, Returning, Opening, Open, Closing, Ejecting, Inserted };
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
        int custom = -1;      // or a console's own model (custom_), or a cartridge's (kCartCustom + system)
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
    // A closed box standing on the shelf, its spine out.
    void addShelfBox(const Item& item, int index, const Mat4& place);
    void addShelf();
    // Where box `index` stands on the shelf (its middle), and how big the shelf is.
    Vec3 shelfSpot(int index) const;
    void shelfSize(float* width, float* height, float* bottom) const;
    // The box and cartridge in front, as they are now.
    Mat4 heldBox() const;
    Mat4 heldCartridge() const;
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
        int labelTurn = 0;
        float pictureAspect = 1, pictureMiddle = 0.5f;
    };
    // The consoles' (by ConsoleModel), then the cartridges' (by ConsoleSystem).
    static constexpr int kCartCustom = static_cast<int>(ConsoleModel::Count);
    Custom custom_[kCartCustom + kSystemCount];
    void upload(Custom& c, const GltfModel& model);
    gl::GLuint fbo_ = 0, colour_ = 0, depth_ = 0;
    int width_ = 0, height_ = 0;

    State state_ = State::Browse;
    float take_ = 0;       // 0 on the shelf .. 1 the box out in front
    float open_ = 0;       // 0 closed .. 1 the lid open and the cartridge out
    float eject_ = 0;      // 0 .. 1 the cartridge's way into the console
    float light_ = 0;      // seconds since it went in
    float pull_ = 0;       // the chosen box on the shelf, slid a little out
    int pulled_ = -1;      // which one that is
    bool launch_ = false, launched_ = false;
    bool frozen_ = false;  // pose()
    bool placed_ = false;  // the camera has been put on the chosen box once
    int selected_ = 0, count_ = 0, columns_ = 1, rows_ = 1;
    ConsoleSystem system_ = ConsoleSystem::MegaDrive;  // of the chosen game
    // Turned by dragging (degrees): the box out and its cartridge.
    float boxYaw_ = 0, boxPitch_ = 0, cartYaw_ = 0, cartPitch_ = 0;
    // The shelf's camera: how far back it is, what it looks at and how far it was dragged.
    float distance_ = 3.0f;
    Vec3 look_, pan_;
    float stageX_ = 0;     // across from the box taken out, where it and the console are
    std::vector<Part> parts_;
    Mat4 viewProj_{};
    Vec3 eye_, forward_, right_, up_;
    float tanHalf_ = 0, aspect_ = 1;
};

}  // namespace plumsg
