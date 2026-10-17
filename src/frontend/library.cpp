#include "library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "../render/gl.h"
#include "http.h"
#include "library_names.h"

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
constexpr int kMaxDepth = 3;       // subfolders scanned below the library folder

// The libretro-thumbnails box art of the Mega Drive, on libretro's server and in its
// GitHub copy.
constexpr const char* kLibretro = "https://thumbnails.libretro.com/Sega%20-%20Mega%20Drive%20-%20Genesis/Named_Boxarts/";
constexpr const char* kGithub =
    "https://raw.githubusercontent.com/libretro-thumbnails/Sega_-_Mega_Drive_-_Genesis/master/Named_Boxarts/";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isRom(const fs::path& p) {
    const std::string ext = lower(p.extension().string());
    return ext == ".md" || ext == ".gen" || ext == ".smd" || ext == ".bin" || ext == ".zip";
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

// Decodes a PNG or JPEG and shrinks it to kCoverHeight lines (box filter), as 0xAABBGGRR
// in memory order R, G, B, A.
bool decode(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, int* width, int* height) {
    int w = 0, h = 0, n = 0;
    stbi_uc* rgba = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return false;
    }
    const int oh = std::min(h, kCoverHeight);
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

void Library::scan(const std::string& folder) {
    clearTextures();
    games_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_ -= static_cast<int>(jobs_.size());
        jobs_.clear();
        results_.clear();
    }
    folder_ = folder;
    scanned_ = true;
    std::error_code ec;
    const fs::path root = fs::u8path(folder);
    if (folder.empty() || !fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
        if (!it->is_regular_file(ec) || !isRom(it->path())) continue;
        Game g;
        g.path = it->path().u8string();
        g.stem = it->path().stem().u8string();
        g.title = gameTitle(g.stem);
        g.tags = gameTags(g.stem);
        g.key = searchKey(g.title + " " + g.tags + " " + g.stem);
        games_.push_back(std::move(g));
    }
    // In title order, with "The Lion King" among the L's.
    auto order = [](const Game& g) {
        std::string k = searchKey(g.title);
        return k.compare(0, 4, "the ") == 0 ? k.substr(4) : k;
    };
    std::sort(games_.begin(), games_.end(), [&](const Game& a, const Game& b) {
        const std::string ka = order(a), kb = order(b);
        return ka != kb ? ka < kb : a.stem < b.stem;
    });
    for (size_t i = 0; i < games_.size(); ++i) {
        games_[i].cover = Cover::Pending;
        queue({i, generation_, games_[i].path, games_[i].stem, false});
    }
}

void Library::downloadMissing() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        downloaded_ = notFound_ = 0;
        offline_ = libretroDown_ = false;
    }
    for (size_t i = 0; i < games_.size(); ++i) {
        if (games_[i].cover != Cover::Missing) continue;
        games_[i].cover = Cover::Pending;
        queue({i, generation_, games_[i].path, games_[i].stem, true});
    }
}

std::string Library::downloadStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offline_) return "No se pudo conectar con el servidor de carátulas.";
    if (downloaded_ == 0 && notFound_ == 0) return {};
    std::string s = std::to_string(downloaded_) + (downloaded_ == 1 ? " carátula descargada" : " carátulas descargadas");
    if (notFound_) s += ", " + std::to_string(notFound_) + " sin encontrar";
    return s;
}

void Library::queue(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++pending_;
    }
    wake_.notify_one();
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
    for (Game& g : games_)
        if (g.texture) {
            gl::DeleteTextures(1, &g.texture);
            g.texture = 0;
            if (g.cover == Cover::Ready) g.cover = Cover::Unknown;
        }
}

bool Library::findLocal(const Job& job, std::string* coverPath) const {
    std::error_code ec;
    const fs::path rom = fs::u8path(job.path);
    const fs::path dir = rom.parent_path();
    const fs::path stem = fs::u8path(job.stem);
    std::vector<fs::path> folders = {fs::u8path(coversFolder_), dir};
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
    return false;
}

// Tries each candidate name on libretro's server, or on GitHub once that one has
// failed to answer, and saves the first image found as covers/<rom name>.png.
bool Library::fetch(const Job& job, std::string* coverPath) {
    if (coversFolder_.empty()) return false;
    std::vector<uint8_t> body;
    for (const std::string& name : coverNames(job.stem)) {
        const std::string file = urlEncode(name) + ".png";
        bool libretroDown;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_ || offline_) return false;
            libretroDown = libretroDown_;
        }
        int status = libretroDown ? 0 : httpGet(kLibretro + file, body);
        if (status == 0 && !libretroDown) {
            std::lock_guard<std::mutex> lock(mutex_);
            libretroDown_ = libretroDown = true;
        }
        if (libretroDown) status = httpGet(kGithub + file, body);
        if (status == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            offline_ = true;
            return false;
        }
        if (status != 200 || !isImage(body)) continue;
        std::error_code ec;
        fs::create_directories(fs::u8path(coversFolder_), ec);
        const fs::path out = fs::u8path(coversFolder_ + job.stem + ".png");
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        if (!f) return false;
        *coverPath = out.u8string();
        return true;
    }
    return false;
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
        Result r{job.index, job.generation, false, {}, 0, 0};
        std::string cover;
        bool found = findLocal(job, &cover);
        bool fetched = false;
        if (!found && job.download) found = fetched = fetch(job, &cover);
        std::vector<uint8_t> data;
        if (found) r.found = readAll(fs::u8path(cover), data) && decode(data, r.pixels, &r.width, &r.height);
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.download) {
            if (fetched) ++downloaded_;
            else if (!offline_) ++notFound_;
        }
        --pending_;
        if (job.generation == generation_) results_.push_back(std::move(r));
    }
}

}  // namespace plumsg
