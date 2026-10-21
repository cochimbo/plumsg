#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/roland.h"
#include "../core/song_extract.h"
#include "../core/vgm.h"

namespace plumsg {

// What the editor needs to know about the emulator each frame.
struct EditorView {
    std::string loadedGame;  // path of the game running, empty for none
    bool recording = false;
    double recordedSeconds = 0;
    bool soundfontReady = false;  // the Roland sound can play
    std::function<void()> drawSoundfont;  // the SoundFont drop-down, next to Sintetizador
    std::function<void()> drawEqualizer;  // the synthesizer's equalizer, which the settings keep
};

// What the editor asks of the emulator this frame.
struct EditorRequests {
    bool chooseTrack = false;     // pick a .vgm to open
    std::string load;             // open this game (to record it)
    bool startRecording = false;  // record the running game's music
    bool stopRecording = false;   // and hand the file to saveRecording()
    bool assignmentsChanged = false;  // the running game's instruments may sound different now
};

// The instrument editor (Sonido > Editor de instrumentos, or a game's menu in the
// library): plays a piece of a game's music, recorded while playing or opened from a
// .vgm, lists the instruments in it and lets each one be given a General MIDI
// instrument, a drum, or its own sound, comparing with the original as it plays.
//
// What is chosen is kept per game in sonido/<game>.ini. With no game it goes to
// sonido/cache.ini, which every game reads (and where plumsg 0.0.22 and older also put
// each game's choices); the game's own file wins over it.
class InstrumentEditor {
public:
    explicit InstrumentEditor(std::string soundFolder);
    ~InstrumentEditor();
    InstrumentEditor(const InstrumentEditor&) = delete;
    InstrumentEditor& operator=(const InstrumentEditor&) = delete;

    // What a game's instruments play: the shared cache, with the game's own on top.
    static RolandSound::Assignments assignmentsFor(const std::string& soundFolder, const std::string& game);
    // A game's sound effects, as Sacar canciones found them (none if it was not run).
    static std::set<uint32_t> effectsFor(const std::string& soundFolder, const std::string& game);

    // Opens the editor for a game (its path; empty for none: then what is chosen goes
    // to the shared cache only).
    void open(const std::string& game);
    bool isOpen() const { return open_; }
    const std::string& game() const { return game_; }
    // Playing a track: the game waits meanwhile and the sound comes from play().
    bool playing() const { return open_ && playing_; }

    // The SoundFont to play the Roland sound with, shared with the emulator's.
    void useSoundfont(const RolandSound& from, int reverb, int chorus, const std::array<int, RolandSound::kEqBands>& equalizer);
    void useEqualizer(const std::array<int, RolandSound::kEqBands>& equalizer) { synth_.setEqualizer(equalizer); }
    void play(double seconds, double rate, std::vector<int16_t>& out);

    // Opens a .vgm or .vgz, or a .zip of them (a vgmrips pack: all its songs go in the
    // list); false (and the reason shown in the window) if it cannot. A file from
    // elsewhere is copied into the game's track folder, so it is there next time.
    bool openTrack(const std::string& path);
    // A recording of `game` (its path): saved in sonido/pistas/ and opened.
    void saveRecording(const std::vector<uint8_t>& file, const std::string& game);

    // Draws the window; call between ImGui::NewFrame and ImGui::Render.
    void draw(const EditorView& view, EditorRequests& out);

private:
    struct Instrument {
        uint32_t fingerprint = 0;
        RolandSound::Seen seen;
        bool heard = false;  // in this track (else only in the game's file)
        std::string label;   // "FM 3", "PSG 1", "Ruido blanco"...
    };
    std::vector<Instrument> instruments() const;
    std::string gamePath() const;  // sonido/<game>.ini
    std::string cachePath() const { return folder_ + "cache.ini"; }
    std::string tracksFolder() const { return folder_ + "pistas/"; }
    // A song in the list: a file, or a file inside a .zip.
    struct Track {
        std::string path;
        std::string member;  // in the .zip; empty for a file
        std::string title;   // what the list shows
        bool operator==(const Track& o) const { return path == o.path && member == o.member; }
    };
    void listTracks();
    void addTracks(const std::string& path);  // a .vgm/.vgz, or every song in a .zip
    bool loadTrack(const Track& track);
    std::string gameTracksFolder() const { return tracksFolder() + gameName_ + "/"; }
    void loadAssignments();
    void assign(uint32_t fingerprint, const RolandSound::Assignment* a, EditorRequests& out);  // null: back to the rules
    void resetAll(const std::vector<Instrument>& list, EditorRequests& out);  // everything back to the rules
    void save(EditorRequests& out);  // writes the files and hands the synth what they say
    void applyListening();
    void stop();
    void drawTrack(const EditorView& view, EditorRequests& out);
    void drawInstruments(const std::vector<Instrument>& list);
    void drawChannels(const std::vector<Instrument>& list);
    void drawAssignment(const std::vector<Instrument>& list, EditorRequests& out);
    // Sacar canciones: the game's songs taken out of its ROM, on a thread of its own,
    // into its track folder as they are found.
    struct Extraction {
        std::thread thread;
        std::mutex mutex;
        SongExtractor::Progress progress;  // these three under the mutex
        int saved = 0;                     // songs written so far
        std::string error;
        std::atomic<bool> stop{false}, done{false};
        std::atomic<bool> effects{false};  // it wrote the game's sound effects
    };
    void startExtraction();
    void endExtraction();  // stops it if it is running, and waits
    void drawExtraction(EditorRequests& out);
    std::unique_ptr<Extraction> extraction_;
    int extractionSeen_ = 0;  // songs of it already in the list

    std::string folder_;  // sonido/, ending in a separator
    bool open_ = false;
    std::string game_;
    std::string gameName_;  // file name without extension
    bool smsGame_ = false;  // Master System or Game Gear: Get songs is experimental there
    RolandSound synth_;
    VgmPlayer player_;
    Track track_;               // the open one
    std::vector<Track> tracks_;  // the game's songs and recordings, and tracks opened since
    std::map<uint32_t, RolandSound::Seen> trackSeen_;  // the open track's instruments
    bool playing_ = false;
    bool rolandOutput_ = true;
    bool soundfontReady_ = false;
    bool loop_ = true;
    float seekTo_ = -1;  // seconds, while the position slider is dragged
    RolandSound::Assignments cache_, own_;
    uint32_t selected_ = 0;
    uint32_t mutedVoices_ = 0, soloVoices_ = 0;  // RolandSound voice bits, and bit 10 the DAC
    uint32_t soloInstrument_ = 0;
    bool pendingSlide_ = false;  // the volume slider changed an instrument not yet saved
    std::string message_;
};

}  // namespace plumsg
