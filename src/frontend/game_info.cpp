#include "game_info.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "../core/i18n.h"
#include "../render/gl.h"
#include "http.h"
#include "library.h"
#include "library_names.h"

namespace plumsg {

namespace fs = std::filesystem;

namespace {

// Each console's folder for what is downloaded, and its name in libretro-database
// (the Mega CD has no metadat files there).
struct InfoSet {
    const char* folder;
    const char* database;
};
constexpr InfoSet kInfoSets[] = {
    {"MegaDrive", "Sega - Mega Drive - Genesis"},
    {"32X", "Sega - 32X"},
    {"MegaCD", nullptr},
    {"MasterSystem", "Sega - Master System - Mark III"},
    {"GameGear", "Sega - Game Gear"},
};
static_assert(std::size(kInfoSets) == kSystemCount, "one set per console");

const char* const kFields[] = {"developer", "publisher", "genre", "releaseyear", "maxusers"};
const char* const kMetadat = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/";
const char* const kUserAgent = "plumsg (https://github.com/cochimbo/plumsg)";
constexpr auto kSettle = std::chrono::milliseconds(300);  // before looking for a game picked
constexpr auto kRetry = std::chrono::minutes(2);          // before downloading a console's data again

bool readText(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

// The word that makes Wikipedia's search find the game rather than what it is named after.
const char* gameWord(const std::string& lang) {
    if (lang == "es") return "videojuego";
    if (lang == "fr") return "jeu vidéo";
    if (lang == "pt") return "jogo eletrônico";
    return "video game";
}

// A page listing the meanings of a name, not one about the game.
bool disambiguation(const std::string& extract) {
    for (const char* s : {"may refer to", "puede referirse a", "puede hacer referencia a", "peut désigner",
                          "peut faire référence", "pode referir-se", "pode se referir"})
        if (extract.find(s) != std::string::npos) return true;
    return false;
}

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Texts for the user, translated on the main thread as they are shown.
const char* const kOffline = N_("No se pudo conectar con Wikipedia.");
const char* const kNotFound = N_("No se ha encontrado este juego en Wikipedia.");

}  // namespace

GameInfo::GameInfo() : worker_(&GameInfo::work, this) {}

GameInfo::~GameInfo() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    worker_.join();
}

std::string GameInfo::consoleFolder(ConsoleSystem s) const {
    return (fs::u8path(folder_) / kInfoSets[static_cast<int>(s)].folder).u8string() +
           static_cast<char>(fs::path::preferred_separator);
}

void GameInfo::ask(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task.kind == Task::LoadSheet)
            for (auto it = tasks_.begin(); it != tasks_.end();)
                it = it->kind == Task::LoadSheet ? tasks_.erase(it) : it + 1;
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

const GameDetails* GameInfo::details(ConsoleSystem s, const std::string& fileStem) {
    const int i = static_cast<int>(s);
    if (!kInfoSets[i].database || folder_.empty()) return nullptr;
    if (!tables_[i]) {
        const auto now = std::chrono::steady_clock::now();
        if (!asked_[i] && now >= retryAt_[i]) {
            asked_[i] = true;
            ++detailsPending_;
            Task t;
            t.kind = Task::LoadDetails;
            t.system = s;
            ask(std::move(t));
        }
        return nullptr;
    }
    return tables_[i]->find(fileStem);
}

void GameInfo::show(const std::string& path, const std::string& stem, const std::string& title, ConsoleSystem s) {
    if (path == sheet_.path) return;
    clearTextures();
    sheet_ = Sheet();
    sheet_.path = path;
    ++serial_;  // a reply on its way for another game is dropped
    if (path.empty() || folder_.empty()) {
        hasNext_ = false;
        return;
    }
    sheet_.busy = true;
    next_ = Task();
    next_.kind = Task::LoadSheet;
    next_.system = s;
    next_.path = path;
    next_.stem = stem;
    next_.title = title;
    hasNext_ = true;
    nextAt_ = std::chrono::steady_clock::now() + kSettle;
}

void GameInfo::refresh() {
    if (sheet_.path.empty() || sheet_.busy) return;
    Task t = next_;
    if (t.path != sheet_.path) return;
    const std::string path = sheet_.path;
    clearTextures();
    sheet_ = Sheet();
    sheet_.path = path;
    sheet_.busy = true;
    t.refresh = true;
    t.serial = ++serial_;
    ask(std::move(t));
}

bool GameInfo::detailsBusy() const { return detailsPending_ > 0; }

void GameInfo::update() {
    if (hasNext_ && std::chrono::steady_clock::now() >= nextAt_) {
        hasNext_ = false;
        Task t = next_;
        t.serial = serial_;
        ask(std::move(t));
    }
    for (;;) {
        Reply r;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (replies_.empty()) return;
            r = std::move(replies_.front());
            replies_.pop_front();
        }
        const int i = static_cast<int>(r.system);
        if (r.kind == Task::LoadDetails) {
            --detailsPending_;
            if (r.table) {
                tables_[i] = std::move(r.table);
                ++detailsVersion_;
            } else {
                asked_[i] = false;  // try again in a while
                retryAt_[i] = std::chrono::steady_clock::now() + kRetry;
            }
            continue;
        }
        if (r.serial != serial_) continue;
        sheet_.busy = false;
        sheet_.description = std::move(r.description);
        sheet_.source = std::move(r.source);
        sheet_.sourceTitle = std::move(r.sourceTitle);
        sheet_.status = r.status.empty() ? std::string() : std::string(tr(r.status.c_str()));
        if (!r.pixels.empty()) {
            sheet_.snap = Library::uploadTexture(sheet_.snap, r.pixels, r.width, r.height);
            sheet_.width = r.width;
            sheet_.height = r.height;
        }
    }
}

void GameInfo::clearTextures() {
    if (sheet_.snap) gl::DeleteTextures(1, &sheet_.snap);
    sheet_.snap = 0;
    sheet_.width = sheet_.height = 0;
}

// The console's metadat files, from the folder or downloaded into it.
std::shared_ptr<DetailsTable> GameInfo::loadDetails(ConsoleSystem s) {
    const char* database = kInfoSets[static_cast<int>(s)].database;
    const fs::path dir = fs::u8path(consoleFolder(s));
    auto table = std::make_shared<DetailsTable>();
    for (const char* field : kFields) {
        const fs::path file = dir / (std::string(field) + ".dat");
        std::string text;
        if (!readText(file, text) || text.empty()) {
            std::vector<uint8_t> body;
            const std::string url = std::string(kMetadat) + field + "/" + urlEncode(database) + ".dat";
            if (httpGet(url, body) != 200 || body.empty()) return nullptr;
            text.assign(body.begin(), body.end());
            writeFile(file, text.data(), text.size());
        }
        table->add(field, text);
    }
    return table;
}

// The game's description and screenshot: the ones kept from an earlier look, or found
// on Wikipedia (in the user's language, else in English) and libretro-thumbnails.
void GameInfo::loadSheet(const Task& task, Reply& r) {
    const fs::path dir = fs::u8path(consoleFolder(task.system));
    const fs::path sheetFile = dir / fs::u8path(task.stem + ".txt");
    const fs::path snapFile = dir / fs::u8path(task.stem + ".png");
    std::string text;
    bool haveSnap = false;
    if (!task.refresh && readText(sheetFile, text)) {
        // "pagina = ...", "url = ...", "captura = si|no", then "---" and the description.
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line) && line != "---") {
            const size_t eq = line.find(" = ");
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 3);
            if (key == "pagina") r.sourceTitle = value;
            else if (key == "url") r.source = value;
            else if (key == "captura") haveSnap = value == "si";
        }
        r.description = trimmed(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        if (r.description.empty()) r.status = kNotFound;
        std::string image;
        if (haveSnap && readText(snapFile, image)) {
            const std::vector<uint8_t> data(image.begin(), image.end());
            Library::decodeImage(data, r.pixels, &r.width, &r.height);
        }
        return;
    }

    bool reached = false;
    std::vector<std::string> langs = {language()};
    if (langs[0] != "en") langs.push_back("en");
    for (const std::string& lang : langs) {
        const std::string host = "https://" + lang + ".wikipedia.org/";
        const std::string url = host +
                                "w/api.php?action=query&format=json&formatversion=2&redirects=1&generator=search"
                                "&gsrlimit=5&gsrsearch=" +
                                urlEncode(task.title + " " + gameWord(lang)) +
                                "&prop=extracts&exintro=1&explaintext=1&exlimit=5";
        std::vector<uint8_t> body;
        const int status = httpRequest(url, {}, kUserAgent, body);
        if (status == 0) continue;
        reached = true;
        if (status != 200) continue;
        int best = -1;
        for (const WikiPage& page : parseWikiSearch(std::string(body.begin(), body.end()))) {
            const int d = wikiTitleDistance(task.title, page.title);
            if (d < 0 || (best >= 0 && d >= best)) continue;
            const std::string extract = trimmed(page.extract);
            if (extract.empty() || disambiguation(extract)) continue;
            best = d;
            r.description = extract;
            r.sourceTitle = page.title;
            std::string name = page.title;
            for (char& c : name)
                if (c == ' ') c = '_';
            r.source = host + "wiki/" + urlEncode(name);
        }
        if (best >= 0) break;
    }

    std::vector<uint8_t> image;
    for (bool github : {false, true}) {
        bool answered = false;
        for (const std::string& name : coverNames(task.stem)) {
            const int status = httpGet(Library::thumbnailsUrl(task.system, "Named_Snaps", github) + urlEncode(name) + ".png", image);
            if (status == 0) break;
            answered = true;
            if (status == 200 && Library::decodeImage(image, r.pixels, &r.width, &r.height)) {
                haveSnap = writeFile(snapFile, image.data(), image.size());
                break;
            }
            r.pixels.clear();
        }
        if (answered) break;
    }
    if (!haveSnap) r.pixels.clear();

    if (!reached) {
        r.status = kOffline;  // nothing kept, so it is looked for again next time
        return;
    }
    if (r.description.empty()) r.status = kNotFound;
    std::string out = "pagina = " + r.sourceTitle + "\nurl = " + r.source + "\ncaptura = " + (haveSnap ? "si" : "no") +
                      "\n---\n" + r.description + "\n";
    writeFile(sheetFile, out.data(), out.size());
}

void GameInfo::work() {
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
        r.system = task.system;
        if (task.kind == Task::LoadDetails) r.table = loadDetails(task.system);
        else loadSheet(task, r);
        std::lock_guard<std::mutex> lock(mutex_);
        replies_.push_back(std::move(r));
    }
}

}  // namespace plumsg
