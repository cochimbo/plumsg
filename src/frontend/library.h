#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plumsg {

// The game library: the ROMs of a folder (and its subfolders), each with its box art.
// Covers are looked for next to the ROMs (same name .png/.jpg, or in a caratulas,
// covers or boxart folder there) and in the user's own covers folder; the ones that
// are missing can be downloaded from the libretro-thumbnails sets into that folder.
// A worker thread finds, downloads and decodes the covers; update() turns the decoded
// ones into OpenGL textures on the main thread.
class Library {
public:
    enum class Cover { Unknown, Pending, Missing, Ready };
    struct Game {
        std::string path;   // the ROM
        std::string stem;   // its file name without extension
        std::string title;  // the name to show
        std::string tags;   // region, revision...
        std::string key;    // searchKey() of title and tags
        Cover cover = Cover::Unknown;
        unsigned int texture = 0;
        int width = 0, height = 0;
    };

    Library();
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // Where downloaded covers go (ending in a separator).
    void setCoversFolder(const std::string& folder) { coversFolder_ = folder; }
    // Lists the ROMs of `folder` and starts looking for their covers.
    void scan(const std::string& folder);
    const std::string& folder() const { return folder_; }
    bool scanned() const { return scanned_; }
    std::vector<Game>& games() { return games_; }

    // Queues a download for every game with no cover yet.
    void downloadMissing();
    // How many covers are still being looked for or downloaded.
    int pending() const { return pending_.load(); }
    // Status of the last download round, for the window ("12 carátulas descargadas...").
    std::string downloadStatus() const;

    // Main thread, once a frame: uploads a few decoded covers as textures.
    void update();
    void clearTextures();

private:
    struct Job {
        size_t index;
        unsigned generation;
        std::string path, stem;
        bool download;
    };
    struct Result {
        size_t index;
        unsigned generation;
        bool found;
        std::vector<uint32_t> pixels;
        int width, height;
    };

    void work();
    bool findLocal(const Job& job, std::string* coverPath) const;
    bool fetch(const Job& job, std::string* coverPath);
    void queue(Job job);

    std::string folder_;
    std::string coversFolder_;
    bool scanned_ = false;
    std::vector<Game> games_;
    unsigned generation_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    std::atomic<int> pending_{0};
    int downloaded_ = 0, notFound_ = 0;
    bool offline_ = false;      // no cover server could be reached in this round
    bool libretroDown_ = false;  // thumbnails.libretro.com failed: use GitHub's copy
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace plumsg
