#include "screenscraper.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "../core/cartridge.h"
#include "../core/i18n.h"
#include "../render/gl.h"
#include "game_info_data.h"
#include "http.h"
#include "library.h"
#include "library_names.h"

// The developer account plumsg is built with: ScreenScraper gives one to each program
// that asks for it, and the installer's build passes it in (a secret of the repository).
#ifndef PLUMSG_SS_DEVID
#define PLUMSG_SS_DEVID ""
#endif
#ifndef PLUMSG_SS_DEVPASSWORD
#define PLUMSG_SS_DEVPASSWORD ""
#endif

namespace plumsg {

namespace fs = std::filesystem;

namespace {

constexpr long long kRequestGapMs = 1200;  // between two API requests
constexpr int kBusyRetries = 3;

const char* const kSystemFolders[kSystemCount] = {"MegaDrive", "32X", "MegaCD", "MasterSystem", "GameGear"};

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool readAll(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool writeAll(const fs::path& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

bool isImage(const std::vector<uint8_t>& d) {
    return (d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') ||
           (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8);
}

// What ScreenScraper sends instead of a file it does not have: a few words of text.
bool isFile(const std::vector<uint8_t>& d, bool image) { return image ? isImage(d) : d.size() > 4096; }

bool isDisc(const std::string& path) {
    std::string ext = fs::u8path(path).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".cue" || ext == ".chd" || ext == ".iso";
}

std::string answerError(const SsAnswer& a) {
    switch (a.result) {
        case SsAnswer::Ok: return {};
        case SsAnswer::NotFound: return tr("ScreenScraper no conoce este juego. Búscalo por su nombre.");
        case SsAnswer::Offline: return tr("No se pudo conectar con ScreenScraper. ¿Hay conexión a internet?");
        case SsAnswer::Login:
            return tr("ScreenScraper no acepta la cuenta: revisa el usuario y la contraseña (o las credenciales de desarrollador).");
        case SsAnswer::Busy:
            return tr("ScreenScraper está muy ocupado ahora mismo. Con una cuenta (es gratis) se puede usar igual; si no, "
                      "prueba dentro de un rato.");
        case SsAnswer::Quota: return tr("Se han acabado las peticiones de hoy de tu cuenta de ScreenScraper.");
        case SsAnswer::Closed: return tr("La API de ScreenScraper está cerrada por ahora. Prueba más tarde.");
        case SsAnswer::Error: break;
    }
    return a.message.empty() ? std::string(tr("ScreenScraper respondió con un error."))
                             : trf("ScreenScraper respondió con un error: %s", a.message.c_str());
}

// The answers that stop a round of downloads: asking again would fail the same way.
bool stopsRound(SsAnswer::Result r) {
    return r == SsAnswer::Offline || r == SsAnswer::Login || r == SsAnswer::Quota || r == SsAnswer::Closed;
}

}  // namespace

ScreenScraper::ScreenScraper() : credentials_(builtIn()), worker_([this] { work(); }) {}

ScreenScraper::~ScreenScraper() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        tasks_.clear();
    }
    wake_.notify_all();
    worker_.join();
}

SsCredentials ScreenScraper::builtIn() {
    SsCredentials c;
    c.devId = PLUMSG_SS_DEVID;
    c.devPassword = PLUMSG_SS_DEVPASSWORD;
    c.softName = "plumsg";
    return c;
}

void ScreenScraper::setCredentials(const SsCredentials& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_ = c;
}

SsCredentials ScreenScraper::credentials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return credentials_;
}

void ScreenScraper::setKinds(std::vector<std::string> kinds) {
    std::lock_guard<std::mutex> lock(mutex_);
    kinds_ = std::move(kinds);
}

std::vector<std::string> ScreenScraper::chosenKinds(const std::vector<std::string>& asked) const {
    if (!asked.empty()) return asked;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!kinds_.empty()) return kinds_;
    std::vector<std::string> out;
    for (const SsKind& k : ssKinds())
        if (k.byDefault) out.push_back(k.id);
    return out;
}

std::string ScreenScraper::gameFolder(const Rom& rom) const {
    if (folder_.empty()) return {};
    return (fs::u8path(folder_) / kSystemFolders[static_cast<int>(rom.system)] / fs::u8path(rom.stem)).u8string() +
           static_cast<char>(fs::path::preferred_separator);
}

void ScreenScraper::queue(Task task, bool first) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (first) tasks_.push_front(std::move(task));
        else tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void ScreenScraper::post(Reply reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    replies_.push_back(std::move(reply));
}

void ScreenScraper::setProgress(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = text;
}

void ScreenScraper::show(const Rom& rom) {
    if (rom.path == sheet_.path) return;
    clearTextures();
    sheet_ = Sheet();
    sheet_.path = rom.path;
    sheet_.busy = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shown_ = rom.path;
        shownSerial_ = serial_ + 1;
    }
    Task t;
    t.kind = Task::Show;
    t.serial = ++serial_;
    t.rom = rom;
    queue(std::move(t), true);
}

void ScreenScraper::download(const Rom& rom, int gameId, std::vector<std::string> kinds) {
    if (rom.path != sheet_.path) show(rom);
    sheet_.busy = true;
    sheet_.status.clear();
    Task t;
    t.kind = Task::Download;
    t.serial = serial_;
    t.rom = rom;
    t.gameId = gameId;
    t.kinds = std::move(kinds);
    // After the Show already queued, ahead of a round of downloads.
    std::lock_guard<std::mutex> lock(mutex_);
    auto at = tasks_.begin();
    while (at != tasks_.end() && at->kind == Task::Show) ++at;
    tasks_.insert(at, std::move(t));
    wake_.notify_one();
}

void ScreenScraper::search(const std::string& text, ConsoleSystem s) {
    search_.busy = true;
    search_.status.clear();
    search_.results.clear();
    Task t;
    t.kind = Task::Search;
    t.serial = ++searchSerial_;
    t.text = text;
    t.system = s;
    queue(std::move(t), true);
}

void ScreenScraper::preview(const SsGame& game, const std::vector<std::string>& regions) {
    const SsMedia* m = nullptr;
    for (const char* id : {"portada", "portada-completa", "caja-3d", "titulo", "captura"})
        if ((m = ssPickMedia(game, *ssKind(id), regions))) break;
    search_.previewId = game.id;
    if (search_.preview) {
        gl::DeleteTextures(1, &search_.preview);
        search_.preview = 0;
    }
    if (!m) return;
    search_.previewBusy = true;
    Task t;
    t.kind = Task::Preview;
    t.serial = searchSerial_;
    t.gameId = game.id;
    t.url = m->url;
    queue(std::move(t), true);
}

void ScreenScraper::close() {
    ++serial_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shown_.clear();
    }
    ++searchSerial_;
    clearTextures();
    sheet_ = Sheet();
    search_ = Search();
}

void ScreenScraper::downloadAll(const std::vector<Rom>& roms) {
    std::lock_guard<std::mutex> lock(mutex_);
    batchError_.clear();
    batchDone_ = batchMissed_ = 0;
    int added = 0;
    std::error_code ec;
    for (const Rom& r : roms) {
        if (fs::is_regular_file(fs::u8path(gameFolder(r) + "ficha.json"), ec)) continue;
        Task t;
        t.kind = Task::Batch;
        t.rom = r;
        tasks_.push_back(std::move(t));
        ++added;
    }
    batchLeft_ += added;
    batchTotal_ = batchLeft_;
    wake_.notify_one();
}

void ScreenScraper::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto end = std::remove_if(tasks_.begin(), tasks_.end(), [](const Task& t) { return t.kind == Task::Batch; });
    batchLeft_ -= static_cast<int>(std::distance(end, tasks_.end()));
    tasks_.erase(end, tasks_.end());
}

int ScreenScraper::batchLeft() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return batchLeft_;
}

std::string ScreenScraper::batchStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!batchError_.empty()) return batchError_;
    if (batchLeft_ > 0) return trf("ScreenScraper: %d de %d juegos...", batchTotal_ - batchLeft_ + 1, batchTotal_);
    if (batchDone_ == 0 && batchMissed_ == 0) return {};
    std::string s = trf(batchDone_ == 1 ? N_("ScreenScraper: %d juego descargado") : N_("ScreenScraper: %d juegos descargados"),
                        batchDone_);
    if (batchMissed_) s += trf(", %d sin encontrar", batchMissed_);
    return s;
}

bool ScreenScraper::takeForLibrary(ForLibrary& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (forLibrary_.empty()) return false;
    out = std::move(forLibrary_.front());
    forLibrary_.pop_front();
    return true;
}

void ScreenScraper::update() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sheet_.busy) sheet_.progress = progress_;
    }
    for (;;) {
        Reply r;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (replies_.empty()) return;
            r = std::move(replies_.front());
            replies_.pop_front();
        }
        if (r.kind == Task::Search) {
            if (r.serial != searchSerial_) continue;
            search_.busy = false;
            search_.status = r.status;
            search_.results = std::move(r.results);
            continue;
        }
        if (r.kind == Task::Preview) {
            if (r.serial != searchSerial_ || r.previewId != search_.previewId) continue;
            search_.previewBusy = false;
            if (!r.images.empty()) {
                search_.preview = Library::uploadTexture(search_.preview, r.images[0].pixels, r.images[0].width, r.images[0].height);
                search_.width = r.images[0].width;
                search_.height = r.images[0].height;
            }
            continue;
        }
        if (r.serial != serial_ || r.path != sheet_.path) continue;
        // A download's reply comes with the folder read again, like a Show's.
        clearTextures();
        sheet_.busy = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Task& t : tasks_)
                if (t.serial == serial_ && (t.kind == Task::Show || t.kind == Task::Download)) sheet_.busy = true;
        }
        if (r.kind == Task::Download || !r.status.empty()) sheet_.status = r.status;
        sheet_.known = r.known;
        sheet_.game = std::move(r.game);
        sheet_.media.clear();
        for (Image& im : r.images) {
            Medium m;
            m.kind = im.kind;
            m.file = im.file;
            if (!im.pixels.empty()) {
                m.texture = Library::uploadTexture(0, im.pixels, im.width, im.height);
                m.width = im.width;
                m.height = im.height;
            }
            sheet_.media.push_back(std::move(m));
        }
    }
}

void ScreenScraper::clearTextures() {
    for (Medium& m : sheet_.media)
        if (m.texture) {
            gl::DeleteTextures(1, &m.texture);
            m.texture = 0;
        }
    if (search_.preview) {
        gl::DeleteTextures(1, &search_.preview);
        search_.preview = 0;
    }
}

void ScreenScraper::readFolder(const Rom& rom, Reply& reply) const {
    const std::string dir = gameFolder(rom);
    if (dir.empty()) return;
    std::vector<uint8_t> data;
    if (readAll(fs::u8path(dir + "ficha.json"), data)) {
        SsAnswer a = parseSsAnswer(200, std::string(data.begin(), data.end()));
        if (!a.games.empty()) {
            reply.known = true;
            reply.game = std::move(a.games[0]);
        }
    }
    std::error_code ec;
    for (const SsKind& k : ssKinds()) {
        for (const char* ext : {".png", ".jpg", ".mp4", ".pdf"}) {
            const fs::path p = fs::u8path(dir + k.id + ext);
            if (!fs::is_regular_file(p, ec)) continue;
            Image im;
            im.kind = k.id;
            im.file = p.u8string();
            if (k.image && readAll(p, data) && !Library::decodeImage(data, im.pixels, &im.width, &im.height)) im.pixels.clear();
            reply.images.push_back(std::move(im));
            break;
        }
    }
}

SsAnswer ScreenScraper::ask(const std::string& endpoint, const std::string& params) {
    SsAnswer a;
    for (int attempt = 0; attempt <= kBusyRetries; ++attempt) {
        SsCredentials c;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            c = credentials_;
            // One request at a time, and not too often.
            const long long wait = lastRequest_ + kRequestGapMs * (attempt ? 4 : 1) - nowMs();
            if (wait > 0 && wake_.wait_for(lock, std::chrono::milliseconds(wait), [this] { return stop_; })) {
                a.result = SsAnswer::Error;
                return a;
            }
        }
        if (!c.usable()) {
            a.result = SsAnswer::Error;
            a.message.clear();
            return a;
        }
        std::vector<uint8_t> body;
        std::string url = ssUrl(c, endpoint, params);
        // For the tests: another server that answers like ScreenScraper's.
        if (const char* api = std::getenv("PLUMSG_SS_API"); api && *api) url = api + url.substr(url.find("/api2/") + 6);
        const int status = httpRequest(url, {}, "plumsg", body);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastRequest_ = nowMs();
        }
        a = parseSsAnswer(status, std::string(body.begin(), body.end()));
        if (a.result != SsAnswer::Busy) break;
    }
    return a;
}

bool ScreenScraper::fetchGame(const Task& task, std::string* error, bool batch) {
    const Rom& rom = task.rom;
    if (!credentials().usable()) {
        *error = tr("Falta una cuenta de desarrollador de ScreenScraper: ponla en la pestaña Cuenta.");
        return false;
    }
    const int system = static_cast<int>(rom.system);
    SsAnswer a;
    if (task.gameId) {
        setProgress(tr("Pidiendo los datos del juego..."));
        a = ask("jeuInfos.php", "systemeid=" + std::to_string(screenScraperSystem(system)) + "&gameid=" + std::to_string(task.gameId));
    } else {
        // By the ROM: its CRC32 and size (a disc only by its name: its tracks are too big to read).
        setProgress(tr("Identificando el juego..."));
        const bool disc = isDisc(rom.path);
        unsigned crc = 0;
        unsigned long long size = 0;
        std::error_code ec;
        if (!disc) {
            Cartridge cart;
            if (cart.loadFile(rom.path, nullptr)) {
                crc = cart.crc32();
                size = cart.romSize();
            }
        } else {
            size = fs::file_size(fs::u8path(rom.path), ec);
            if (ec) size = 0;
        }
        a = ask("jeuInfos.php", ssRomParams(system, fs::u8path(rom.path).filename().u8string(), size, crc, disc));
        // Not known by its ROM: by its name, if a game is called just that.
        if (a.result == SsAnswer::NotFound) {
            SsAnswer s = ask("jeuRecherche.php", "systemeid=" + std::to_string(screenScraperSystem(system)) +
                                                     "&recherche=" + urlEncode(rom.title));
            const std::vector<std::string> regions = ssRegions(rom.tags);
            for (SsGame& g : s.games)
                if (wikiTitleDistance(rom.title, g.name(regions)) == 0) {
                    a = SsAnswer();
                    a.result = SsAnswer::Ok;
                    a.games.push_back(std::move(g));
                    break;
                }
            if (s.result != SsAnswer::Ok && s.result != SsAnswer::NotFound) a = s;
        }
    }
    if (a.result != SsAnswer::Ok || a.games.empty()) {
        *error = answerError(a);
        if (batch && stopsRound(a.result)) {
            std::lock_guard<std::mutex> lock(mutex_);
            batchError_ = *error;
        }
        return false;
    }
    SsGame game = std::move(a.games[0]);
    const std::string dir = gameFolder(rom);
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    // Another game than before: its old media go.
    {
        std::vector<uint8_t> old;
        if (readAll(fs::u8path(dir + "ficha.json"), old)) {
            const SsAnswer before = parseSsAnswer(200, std::string(old.begin(), old.end()));
            if (!before.games.empty() && before.games[0].id != game.id)
                for (const SsKind& k : ssKinds())
                    for (const char* ext : {".png", ".jpg", ".mp4", ".pdf"}) fs::remove(fs::u8path(dir + k.id + ext), ec);
        }
    }
    const std::string json = ssGameJson(game);
    if (!writeAll(fs::u8path(dir + "ficha.json"), json.data(), json.size())) {
        *error = trf("No se pudo guardar en %s", dir.c_str());
        return false;
    }
    const std::vector<std::string> regions = ssRegions(rom.tags);
    const SsCredentials c = credentials();
    ForLibrary lib;
    lib.path = rom.path;
    lib.replace = !batch;
    std::vector<uint8_t> front;
    int got = 0;
    for (const std::string& id : chosenKinds(task.kinds)) {
        const SsKind* k = ssKind(id);
        if (!k) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return false;
        }
        const SsMedia* m = ssPickMedia(game, *k, regions);
        if (!m) continue;
        setProgress(trf("Descargando: %s...", tr(k->label)));
        std::vector<uint8_t> body;
        const int status = httpRequest(ssWithCredentials(m->url, c), {}, "plumsg", body);
        if (status == 0) {
            *error = answerError(parseSsAnswer(0, {}));
            return false;
        }
        if (status != 200 || !isFile(body, k->image)) continue;
        std::string ext = k->image ? (body[0] == 0x89 ? ".png" : ".jpg") : "." + (m->format.empty() ? std::string("mp4") : m->format);
        for (const char* e : {".png", ".jpg", ".mp4", ".pdf"}) fs::remove(fs::u8path(dir + k->id + e), ec);
        if (!writeAll(fs::u8path(dir + k->id + ext), body.data(), body.size())) continue;
        ++got;
        if (id == "portada-completa") lib.cover = body;
        else if (id == "portada") front = body;
        else if (id == "etiqueta") lib.label = std::move(body);
    }
    // The whole sleeve is the cover the 3D boxes want; the front alone otherwise.
    if (lib.cover.empty()) lib.cover = std::move(front);
    if (!lib.cover.empty() || !lib.label.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        forLibrary_.push_back(std::move(lib));
    }
    const std::string name = game.name(regions);
    *error = got ? trf(got == 1 ? N_("«%s»: %d archivo descargado.") : N_("«%s»: %d archivos descargados."), name.c_str(), got)
                 : trf("«%s»: ScreenScraper no tiene imágenes de este juego.", name.c_str());
    return true;
}

void ScreenScraper::work() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        Reply r;
        r.kind = task.kind;
        r.serial = task.serial;
        r.path = task.rom.path;
        switch (task.kind) {
            case Task::Show: readFolder(task.rom, r); break;
            case Task::Download: {
                std::string message;
                fetchGame(task, &message, false);
                r.status = message;
                readFolder(task.rom, r);
                break;
            }
            case Task::Batch: {
                bool stopped;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stopped = !batchError_.empty();
                }
                std::string message;
                const bool ok = !stopped && fetchGame(task, &message, true);
                std::lock_guard<std::mutex> lock(mutex_);
                --batchLeft_;
                if (ok) ++batchDone_;
                else if (!stopped && batchError_.empty()) ++batchMissed_;
                // Its game may be the one in the window.
                r.kind = Task::Show;
                r.serial = shownSerial_;
                if (task.rom.path != shown_) r.path.clear();
                break;
            }
            case Task::Search: {
                const SsAnswer a = ask("jeuRecherche.php", "systemeid=" + std::to_string(screenScraperSystem(static_cast<int>(task.system))) +
                                                               "&recherche=" + urlEncode(task.text));
                if (!credentials().usable())
                    r.status = tr("Falta una cuenta de desarrollador de ScreenScraper: ponla en la pestaña Cuenta.");
                else if (a.result == SsAnswer::NotFound) r.status = tr("Ningún juego se llama así en ScreenScraper.");
                else r.status = answerError(a);
                r.results = a.games;
                break;
            }
            case Task::Preview: {
                r.previewId = task.gameId;
                std::vector<uint8_t> body;
                Image im;
                if (httpRequest(ssWithCredentials(task.url, credentials()), {}, "plumsg", body) == 200 && isImage(body) &&
                    Library::decodeImage(body, im.pixels, &im.width, &im.height))
                    r.images.push_back(std::move(im));
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_.clear();
        }
        // A finished batch game is only worth a reply if it is in the window.
        if (task.kind == Task::Batch) {
            if (r.path.empty()) continue;
            readFolder(task.rom, r);
        }
        post(std::move(r));
    }
}

}  // namespace plumsg
