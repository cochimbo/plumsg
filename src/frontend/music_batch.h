#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace plumsg {

// Sacar la música de todos los juegos: Sacar canciones (SongExtractor) for every game of
// the library, one after another on a thread of its own, into each game's track folder
// (sound/tracks/<game>/, as the instrument editor does). Mega CD discs are left out:
// their music is the disc's audio tracks. Games that already have songs there, or where
// an earlier round found none (sound/tracks/sin-musica.txt), are skipped, so a round
// stopped (or the program closed) goes on where it was the next time.
// The work waits while a game is being played (setPaused) and goes on after.
class MusicBatch {
public:
    struct Game {
        std::string path, stem, title;
    };
    struct Status {
        bool running = false;
        bool paused = false;
        int done = 0, total = 0;  // games gone through of the round, of
        int withMusic = 0;        // of them, those it took songs out of
        std::string game;         // the one it is on
        float gameFraction = 0;   // and how far it has got with it, 0..1
    };

    MusicBatch() = default;
    ~MusicBatch();
    MusicBatch(const MusicBatch&) = delete;
    MusicBatch& operator=(const MusicBatch&) = delete;

    // sound/, ending in a separator.
    void setFolder(const std::string& folder) { folder_ = folder; }
    // A round over `games` (those that are not discs); one running is stopped first.
    void start(const std::vector<Game>& games);
    void stop();  // and waits for the game it was on to let go
    void setPaused(bool paused);
    Status status() const;

    // The games with songs in their track folder, by file name without extension, and
    // how many: the library marks them. Read again when a game gets songs.
    const std::map<std::string, int>& gamesWithMusic();
    // Something else (the instrument editor) took songs out: read the folders again.
    void changed() { ++version_; }

    // How many songs a game's track folder holds (.vgm, .vgz and .zip packs).
    static int songsIn(const std::string& folder);

private:
    void work(std::vector<Game> games);
    std::string tracksFolder() const { return folder_ + "tracks/"; }

    std::string folder_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool paused_ = false;  // under mutex_
    std::atomic<bool> stop_{false};
    Status status_;  // under mutex_
    std::atomic<int> version_{1};
    int seenVersion_ = 0;
    std::map<std::string, int> withMusic_;
};

}  // namespace plumsg
