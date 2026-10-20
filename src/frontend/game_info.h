#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/system.h"
#include "game_info_data.h"

namespace plumsg {

// What the library's detail panel shows of a game besides its cover: developer,
// publisher, genre, year and players from libretro-database's metadat files (one set
// per console, downloaded once and kept), and for the game picked a description from
// Wikipedia and a screenshot from libretro-thumbnails, kept in a folder of their own.
// A worker thread does the downloading; update() hands its results to the main thread.
class GameInfo {
public:
    // The game in the panel.
    struct Sheet {
        std::string path;         // the ROM, empty while there is none
        bool busy = false;        // looking for its description and screenshot
        std::string description;  // empty if there is none
        std::string source;       // the Wikipedia page it comes from
        std::string sourceTitle;
        std::string status;       // why there is no description, or empty
        unsigned int snap = 0;    // the screenshot's texture, 0 if there is none
        int width = 0, height = 0;
    };

    GameInfo();
    ~GameInfo();
    GameInfo(const GameInfo&) = delete;
    GameInfo& operator=(const GameInfo&) = delete;

    // Where the downloads are kept (ending in a separator).
    void setFolder(const std::string& folder) { folder_ = folder; }

    // libretro-database's data of the console's games: loaded (or downloaded) the first
    // time it is asked for, so null until then and for the Mega CD, which has none.
    const GameDetails* details(ConsoleSystem s, const std::string& fileStem);
    // Changes each time a console's data arrives, to sort the list again.
    unsigned detailsVersion() const { return detailsVersion_; }
    bool detailsBusy() const;

    // Shows the game in the panel, looking for its description and screenshot a moment
    // later (so that running through the grid does not download every game's). The
    // title is the one to look for on Wikipedia.
    void show(const std::string& path, const std::string& stem, const std::string& title, ConsoleSystem s);
    // Forgets what was found for the game in the panel and looks for it again.
    void refresh();
    const Sheet& sheet() const { return sheet_; }

    // Main thread, once a frame while the library is open.
    void update();
    void clearTextures();

private:
    struct Task {
        enum Kind { LoadDetails, LoadSheet } kind = LoadSheet;
        unsigned serial = 0;
        ConsoleSystem system = ConsoleSystem::MegaDrive;
        std::string path, stem, title;
        bool refresh = false;
    };
    struct Reply {
        Task::Kind kind = Task::LoadSheet;
        unsigned serial = 0;
        ConsoleSystem system = ConsoleSystem::MegaDrive;
        std::shared_ptr<DetailsTable> table;
        std::string description, source, sourceTitle, status;
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
    };

    void work();
    void ask(Task task);
    std::shared_ptr<DetailsTable> loadDetails(ConsoleSystem s);
    void loadSheet(const Task& task, Reply& reply);
    std::string consoleFolder(ConsoleSystem s) const;

    std::string folder_;
    std::shared_ptr<DetailsTable> tables_[kSystemCount];  // main thread
    bool asked_[kSystemCount] = {};
    std::chrono::steady_clock::time_point retryAt_[kSystemCount] = {};  // after a failed download
    unsigned detailsVersion_ = 0;
    int detailsPending_ = 0;

    Sheet sheet_;
    Task next_;  // the sheet to ask for once the selection settles
    bool hasNext_ = false;
    std::chrono::steady_clock::time_point nextAt_;
    unsigned serial_ = 0;  // of the latest sheet asked for; older replies are dropped

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;  // details first, then at most one sheet
    std::deque<Reply> replies_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace plumsg
