#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/system.h"

namespace plumsg {

// The game library: the games of one or more folders (and their subfolders), each with
// the console it is for (Mega Drive, 32X or Mega CD) and its box art.
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
        ConsoleSystem system = ConsoleSystem::MegaDrive;
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
    // Lists the games of `folders` and starts looking for their covers. The .bin
    // tracks a .cue points to are part of that disc, not games of their own.
    void scan(const std::vector<std::string>& folders);
    const std::vector<std::string>& folders() const { return folders_; }
    bool scanned() const { return scanned_; }
    std::vector<Game>& games() { return games_; }

    // Queues a download for every game with no cover yet.
    void downloadMissing();
    // How many covers are still being looked for or downloaded.
    int pending() const { return pending_.load(); }
    // Status of the last download round, for the window ("12 carátulas descargadas...").
    std::string downloadStatus() const;

    // Looking for one game's cover by hand ("Buscar carátula..." in its menu): the
    // names of the console's box art set that look like what the user typed, a preview
    // of the one picked, and using it. The set's list of names is downloaded once.
    struct CoverSearch {
        bool busy = false;               // listing, ranking or downloading a preview
        std::string status;              // an error, or empty
        std::vector<std::string> names;  // best first
        std::string previewName;         // the cover in `preview`
        unsigned int preview = 0;        // texture, 0 while there is none
        int width = 0, height = 0;
    };
    void searchCovers(ConsoleSystem s, const std::string& query);
    void previewCover(ConsoleSystem s, const std::string& name);
    // Saves the previewed cover as `game`'s own (in the covers folder, where it is
    // looked for first) and shows it in the grid.
    bool useCover(size_t game);
    // Forgets the search and frees the preview.
    void endCoverSearch();
    const CoverSearch& coverSearch() const { return search_; }

    // Main thread, once a frame: uploads a few decoded covers as textures.
    void update();
    void clearTextures();

private:
    struct Job {
        size_t index;
        unsigned generation;
        std::string path, stem;
        ConsoleSystem system;
        bool download;
    };
    struct Result {
        size_t index;
        unsigned generation;
        bool found;
        std::vector<uint32_t> pixels;
        int width, height;
    };

    // One request to the search thread; a newer one replaces it if it has not started.
    struct SearchTask {
        enum Kind { Names, Preview } kind;
        unsigned serial;
        ConsoleSystem system;
        std::string text;  // the query, or the name to preview
    };
    struct SearchReply {
        SearchTask::Kind kind;
        unsigned serial;
        std::string status;
        std::vector<std::string> names;
        std::string name;
        std::vector<uint8_t> image;  // the preview's file
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
    };

    void work();
    void searchWork();
    bool loadCoverIndex(ConsoleSystem s, std::string* error);
    void askSearch(SearchTask task);
    bool findLocal(const Job& job, std::string* coverPath) const;
    bool fetch(const Job& job, std::string* coverPath);
    void queue(Job job);

    std::string systemCovers(ConsoleSystem s) const;

    std::vector<std::string> folders_;
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

    CoverSearch search_;            // main thread only
    std::vector<uint8_t> previewImage_;
    unsigned searchSerial_ = 0;     // the latest request; older replies are dropped
    bool hasTask_ = false, hasReply_ = false;  // guarded by mutex_
    SearchTask task_;
    SearchReply reply_;
    std::condition_variable searchWake_;
    std::vector<std::string> coverIndex_[kSystemCount];  // search thread only
    std::thread searcher_;
};

}  // namespace plumsg
