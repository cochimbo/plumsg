#include "library.h"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "../core/i18n.h"
#include "../render/gl.h"
#include "library_names.h"
#include "screenscraper_data.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace plumsg {

namespace fs = std::filesystem;

namespace {

constexpr int kCoverHeight = 280;  // covers are shrunk to this many pixels tall
constexpr int kSleeveHeight = 720;  // but a whole sleeve (back, spine and front) to this, for the 3D boxes
constexpr int kMaxDepth = 3;       // subfolders scanned below the library folder

// Each console's subfolder of the covers folder ("" for the Mega Drive's, as before).
constexpr const char* kCoverSubfolders[] = {"", "32X", "MegaCD", "MasterSystem", "GameGear"};
static_assert(std::size(kCoverSubfolders) == kSystemCount, "one subfolder per console");

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool readAll(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool isImage(const std::vector<uint8_t>& d) {
    static const uint8_t png[] = {0x89, 'P', 'N', 'G'};
    return (d.size() > 8 && std::equal(png, png + 4, d.begin())) || (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8);
}

// Decodes a PNG or JPEG and shrinks it to kCoverHeight lines (kSleeveHeight if it is much
// wider than tall, a whole sleeve) with a box filter, as 0xAABBGGRR in memory order R, G, B, A.
bool decode(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, int* width, int* height) {
    int w = 0, h = 0, n = 0;
    stbi_uc* rgba = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return false;
    }
    const int oh = std::min(h, w >= h * 6 / 5 ? kSleeveHeight : kCoverHeight);
    const int ow = std::max(1, static_cast<int>(static_cast<long long>(w) * oh / h));
    pixels.assign(static_cast<size_t>(ow) * oh, 0);
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += rgba[(static_cast<size_t>(sy) * w + sx) * 4 + c];
            const uint32_t count = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            uint32_t p = 0;
            for (int c = 0; c < 4; ++c) p |= (sum[c] / count) << (8 * c);
            pixels[static_cast<size_t>(y) * ow + x] = p;
        }
    }
    stbi_image_free(rgba);
    *width = ow;
    *height = oh;
    return true;
}

}  // namespace

Library::Library() : worker_([this] { work(); }) {}

Library::~Library() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    worker_.join();
}

void Library::scan(const std::vector<std::string>& folders) {
    clearTextures();
    games_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_ -= static_cast<int>(jobs_.size());
        jobs_.clear();
        results_.clear();
    }
    folders_ = folders;
    scanned_ = true;
    std::vector<fs::path> files;
    std::vector<std::string> tracks;  // files that .cue sheets point to, in canonical form
    std::error_code ec;
    for (const std::string& folder : folders) {
        const fs::path root = fs::u8path(folder);
        if (folder.empty() || !fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
            if (!it->is_regular_file(ec) || !isGameFile(it->path().u8string())) continue;
            // BIOS files ("bios_CD_U.bin", "[BIOS] Mega-CD (Japan).bin") are not games.
            if (lower(it->path().filename().u8string()).find("bios") != std::string::npos) continue;
            files.push_back(it->path());
            if (lower(it->path().extension().u8string()) == ".cue")
                for (const std::string& t : cueFiles(it->path().u8string()))
                    tracks.push_back(lower(fs::weakly_canonical(fs::u8path(t), ec).u8string()));
            // An MSU-MD cartridge's .cue is its music, not a game of its own.
            else if (const std::string cue = msuMdCue(it->path().u8string()); !cue.empty())
                tracks.push_back(lower(fs::weakly_canonical(fs::u8path(cue), ec).u8string()));
        }
        ec.clear();
    }
    std::sort(tracks.begin(), tracks.end());
    std::vector<std::string> seen;  // the same folder listed twice, or one inside another
    for (const fs::path& p : files) {
        const std::string canonical = lower(fs::weakly_canonical(p, ec).u8string());
        if (std::binary_search(tracks.begin(), tracks.end(), canonical)) continue;
        if (std::find(seen.begin(), seen.end(), canonical) != seen.end()) continue;
        seen.push_back(canonical);
        Game g;
        g.path = p.u8string();
        g.stem = p.stem().u8string();
        g.title = gameTitle(g.stem);
        g.tags = gameTags(g.stem);
        g.key = searchKey(g.title + " " + g.tags + " " + g.stem);
        g.system = detectSystem(g.path);
        games_.push_back(std::move(g));
    }
    // In title order, with "The Lion King" among the L's.
    auto order = [](const Game& g) {
        std::string k = searchKey(g.title);
        return k.compare(0, 4, "the ") == 0 ? k.substr(4) : k;
    };
    std::sort(games_.begin(), games_.end(), [&](const Game& a, const Game& b) {
        const std::string ka = order(a), kb = order(b);
        if (ka != kb) return ka < kb;
        if (a.system != b.system) return a.system < b.system;
        return a.stem < b.stem;
    });
    for (size_t i = 0; i < games_.size(); ++i) {
        games_[i].cover = Cover::Pending;
        queue({i, generation_, games_[i].path, games_[i].stem, games_[i].system});
    }
}

bool Library::decodeImage(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, int* width, int* height) {
    return isImage(data) && decode(data, pixels, width, height);
}

unsigned int Library::uploadTexture(unsigned int texture, const std::vector<uint32_t>& pixels, int width, int height) {
    if (!texture) gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());
    return texture;
}

std::string Library::systemCovers(ConsoleSystem s) const {
    const char* sub = kCoverSubfolders[static_cast<int>(s)];
    if (coversFolder_.empty() || !*sub) return coversFolder_;
    return (fs::u8path(coversFolder_) / fs::u8path(sub)).u8string() + static_cast<char>(fs::path::preferred_separator);
}

void Library::queue(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++pending_;
    }
    wake_.notify_one();
}

bool Library::labelFromFile(size_t game, const std::string& file, std::string* error) {
    std::vector<uint8_t> image;
    if (!readAll(fs::u8path(file), image)) {
        *error = trf("No se pudo leer %s", file.c_str());
        return false;
    }
    if (!isImage(image)) {
        *error = trf("%s no es una imagen PNG o JPEG", fs::u8path(file).filename().u8string().c_str());
        return false;
    }
    return saveCover(game, image, error, true);
}

bool Library::coverFromFile(size_t game, const std::string& file, std::string* error) {
    std::vector<uint8_t> image;
    if (!readAll(fs::u8path(file), image)) {
        *error = trf("No se pudo leer %s", file.c_str());
        return false;
    }
    int w = 0, h = 0, n = 0;
    if (!isImage(image) || !stbi_info_from_memory(image.data(), static_cast<int>(image.size()), &w, &h, &n)) {
        *error = trf("%s no es una imagen PNG o JPEG", fs::u8path(file).filename().u8string().c_str());
        return false;
    }
    return saveCover(game, image, error);
}

void Library::useDownloaded(const std::string& path, const std::vector<uint8_t>& cover, const std::vector<uint8_t>& label,
                            bool replace) {
    for (size_t i = 0; i < games_.size(); ++i) {
        const Game& g = games_[i];
        if (g.path != path) continue;
        std::string error;
        int w = 0, h = 0, n = 0;
        const bool sleeve = !cover.empty() && stbi_info_from_memory(cover.data(), static_cast<int>(cover.size()), &w, &h, &n) &&
                            w >= h * 6 / 5;
        const bool hasSleeve = g.cover == Cover::Ready && g.width >= g.height * 6 / 5;
        const bool hasLabel = g.label != 0;
        if (!cover.empty() && isImage(cover) &&
            (replace || g.cover == Cover::Missing || g.cover == Cover::Unknown || (sleeve && !hasSleeve)))
            saveCover(i, cover, &error);
        if (!label.empty() && isImage(label) && (replace || !hasLabel)) saveCover(i, label, &error, true);
        return;
    }
}

bool Library::saveCover(size_t game, const std::vector<uint8_t>& image, std::string* error, bool label) {
    if (game >= games_.size()) return false;
    Game& g = games_[game];
    std::string target = systemCovers(g.system);
    if (!target.empty() && label) target = (fs::u8path(target) / "etiquetas").u8string() + static_cast<char>(fs::path::preferred_separator);
    if (target.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::u8path(target), ec);
    // The .png is looked for before the .jpg: the old one, of either kind, must go.
    for (const char* ext : {".png", ".jpg", ".jpeg"}) fs::remove(fs::u8path(target + g.stem + ext), ec);
    const bool png = image.size() > 4 && image[0] == 0x89 && image[1] == 'P';
    {
        std::ofstream f(fs::u8path(target + g.stem + (png ? ".png" : ".jpg")), std::ios::binary);
        f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!f) {
            *error = trf("No se pudo guardar la carátula en %s", target.c_str());
            return false;
        }
    }
    g.cover = Cover::Pending;
    queue({game, generation_, g.path, g.stem, g.system});
    return true;
}

void Library::update() {
    // A few per frame: each upload is quick, but dozens at once would stutter the game.
    for (int n = 0; n < 4; ++n) {
        Result r;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (results_.empty()) return;
            r = std::move(results_.front());
            results_.pop_front();
        }
        if (r.generation != generation_ || r.index >= games_.size()) continue;
        Game& g = games_[r.index];
        if (!r.found) {
            g.cover = Cover::Missing;
            continue;
        }
        if (!r.label.empty()) g.label = uploadTexture(g.label, r.label, r.labelWidth, r.labelHeight);
        g.labelWidth = r.labelWidth;
        g.labelHeight = r.labelHeight;
        if (g.texture) gl::DeleteTextures(1, &g.texture);
        gl::GenTextures(1, &g.texture);
        gl::BindTexture(gl::TEXTURE_2D, g.texture);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, r.width, r.height, 0, gl::RGBA, gl::UNSIGNED_BYTE, r.pixels.data());
        g.width = r.width;
        g.height = r.height;
        g.cover = Cover::Ready;
    }
}

void Library::clearTextures() {
    for (Game& g : games_) {
        if (g.label) {
            gl::DeleteTextures(1, &g.label);
            g.label = 0;
        }
        if (g.texture) {
            gl::DeleteTextures(1, &g.texture);
            g.texture = 0;
            if (g.cover == Cover::Ready) g.cover = Cover::Unknown;
        }
    }
}

bool Library::findLocal(const Job& job, std::string* coverPath) const {
    std::error_code ec;
    const fs::path rom = fs::u8path(job.path);
    const fs::path dir = rom.parent_path();
    const fs::path stem = fs::u8path(job.stem);
    std::vector<fs::path> folders = {fs::u8path(systemCovers(job.system)), dir};
    for (const char* sub : {"caratulas", "carátulas", "covers", "boxart", "Named_Boxarts"}) folders.push_back(dir / fs::u8path(sub));
    for (const auto& f : folders) {
        if (f.empty()) continue;
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            fs::path p = f / stem;
            p += ext;
            if (fs::is_regular_file(p, ec)) {
                *coverPath = p.u8string();
                return true;
            }
        }
    }
    // What ScreenScraper downloaded of it: the whole sleeve (for the 3D boxes), or the front.
    return findMedia(job, {"portada-completa", "portada"}, coverPath);
}

bool Library::findMedia(const Job& job, std::initializer_list<const char*> kinds, std::string* path) const {
    if (mediaFolder_.empty()) return false;
    std::error_code ec;
    const std::string dir = ssGameFolder(mediaFolder_, static_cast<int>(job.system), job.stem);
    for (const char* kind : kinds)
        for (const char* ext : {".png", ".jpg"}) {
            const fs::path p = fs::u8path(dir + kind + ext);
            if (fs::is_regular_file(p, ec)) {
                *path = p.u8string();
                return true;
            }
        }
    return false;
}

bool Library::findLabel(const Job& job, std::string* labelPath) const {
    std::error_code ec;
    const fs::path dir = fs::u8path(job.path).parent_path();
    const fs::path stem = fs::u8path(job.stem);
    std::vector<fs::path> folders;
    if (!systemCovers(job.system).empty()) folders.push_back(fs::u8path(systemCovers(job.system)) / "etiquetas");
    for (const char* sub : {"etiquetas", "labels", "Named_Labels"}) folders.push_back(dir / fs::u8path(sub));
    for (const auto& f : folders)
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            fs::path p = f / stem;
            p += ext;
            if (fs::is_regular_file(p, ec)) {
                *labelPath = p.u8string();
                return true;
            }
        }
    return findMedia(job, {"etiqueta"}, labelPath);
}

void Library::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Result r{job.index, job.generation, false, {}, 0, 0, {}, 0, 0};
        std::string cover;
        const bool found = findLocal(job, &cover);
        std::vector<uint8_t> data;
        if (found) r.found = readAll(fs::u8path(cover), data) && decode(data, r.pixels, &r.width, &r.height);
        std::string label;
        if (r.found && findLabel(job, &label) && readAll(fs::u8path(label), data) &&
            !decode(data, r.label, &r.labelWidth, &r.labelHeight))
            r.label.clear(), r.labelWidth = r.labelHeight = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_;
        if (job.generation == generation_) results_.push_back(std::move(r));
    }
}

}  // namespace plumsg
