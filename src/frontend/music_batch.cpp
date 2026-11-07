#include "music_batch.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "../core/song_extract.h"

namespace plumsg {

namespace fs = std::filesystem;

namespace {

bool isSong(const fs::path& p) {
    std::string ext = p.extension().u8string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".vgm" || ext == ".vgz" || ext == ".zip";
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream out(fs::u8path(path), std::ios::binary);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

// sin-musica.txt: a game's file name without extension on each line.
std::set<std::string> readList(const std::string& path) {
    std::set<std::string> list;
    std::ifstream in(fs::u8path(path));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) list.insert(line);
    }
    return list;
}

}  // namespace

MusicBatch::~MusicBatch() { stop(); }

int MusicBatch::songsIn(const std::string& folder) {
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && isSong(it->path())) ++n;
    return n;
}

const std::map<std::string, int>& MusicBatch::gamesWithMusic() {
    const int v = version_.load();
    if (v == seenVersion_) return withMusic_;
    seenVersion_ = v;
    withMusic_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(tracksFolder()), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec))
            if (const int n = songsIn(it->path().u8string())) withMusic_[it->path().filename().u8string()] = n;
    return withMusic_;
}

void MusicBatch::start(const std::vector<Game>& games) {
    stop();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = Status{};
        status_.running = true;
        status_.paused = paused_;
        status_.total = static_cast<int>(games.size());
    }
    thread_ = std::thread([this, games]() { work(games); });
}

void MusicBatch::stop() {
    stop_ = true;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.running = false;
}

void MusicBatch::setPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_ == paused) return;
        paused_ = paused;
        status_.paused = paused;
    }
    wake_.notify_all();
}

MusicBatch::Status MusicBatch::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void MusicBatch::work(std::vector<Game> games) {
    const std::string none = tracksFolder() + "sin-musica.txt";
    std::set<std::string> failed = readList(none);
    auto wait = [this]() {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this]() { return !paused_ || stop_.load(); });
    };
    for (const Game& g : games) {
        if (stop_) break;
        wait();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.game = g.title;
            status_.gameFraction = 0;
        }
        const std::string folder = tracksFolder() + g.stem + "/";
        const bool skip = failed.count(g.stem) || songsIn(folder) > 0;
        int saved = 0;
        bool stopped = false;
        if (!skip) {
            SongExtractor extractor;
            extractor.setWait(wait);
            std::string error;
            extractor.run(
                g.path, g.stem,
                [this, &stopped](const SongExtractor::Progress& p) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (p.total > 0) status_.gameFraction = static_cast<float>(p.tried) / static_cast<float>(p.total);
                    stopped = stop_.load();
                    return !stopped;
                },
                [&](const SongExtractor::Song& song) {
                    char name[32];
                    std::snprintf(name, sizeof name, "Song %02d.vgz", saved + 1);
                    if (writeFile(folder + name, song.vgz.data(), song.vgz.size())) ++saved;
                },
                &error);
            // The sound effects it heard, as the instrument editor's Sacar canciones keeps them.
            if (!stopped && !extractor.effectInstruments().empty()) {
                std::string text = "; Los efectos de sonido de " + g.stem + " (huellas de instrumentos FM), de Sacar canciones.\n";
                char line[16];
                for (uint32_t fp : extractor.effectInstruments()) {
                    std::snprintf(line, sizeof line, "%08x\n", fp);
                    text += line;
                }
                writeFile(folder_ + g.stem + ".effects", text.data(), text.size());
            }
            if (stopped && saved == 0) break;  // not tried to the end: the next round does it again
            if (!stopped && saved == 0) {
                failed.insert(g.stem);
                std::string text;
                for (const std::string& s : failed) text += s + "\n";
                writeFile(none, text.data(), text.size());
            }
            if (saved > 0) ++version_;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.done;
        if (saved > 0 || (skip && !failed.count(g.stem))) ++status_.withMusic;
        if (stopped) break;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.running = false;
    status_.game.clear();
}

}  // namespace plumsg
