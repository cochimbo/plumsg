#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../core/roland.h"
#include "../core/vgm.h"

namespace plumsg {

// What the editor needs to know about the emulator each frame.
struct EditorView {
    std::string loadedGame;  // path of the game running, empty for none
    bool recording = false;
    double recordedSeconds = 0;
    bool soundfontReady = false;  // the Roland sound can play
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
// What is chosen is kept per game in sonido/<game>.ini and, unless told otherwise,
// also in sonido/cache.ini, which every game shares: the same FM sound in another
// game comes out assigned already. The game's own file wins over the cache.
class InstrumentEditor {
public:
    explicit InstrumentEditor(std::string soundFolder);

    // What a game's instruments play: the shared cache, with the game's own on top.
    static RolandSound::Assignments assignmentsFor(const std::string& soundFolder, const std::string& game);

    // Opens the editor for a game (its path; empty for none: then what is chosen goes
    // to the shared cache only).
    void open(const std::string& game);
    bool isOpen() const { return open_; }
    const std::string& game() const { return game_; }
    // Playing a track: the game waits meanwhile and the sound comes from play().
    bool playing() const { return open_ && playing_; }

    // The SoundFont to play the Roland sound with, shared with the emulator's.
    void useSoundfont(const RolandSound& from, int reverb, int chorus);
    void play(double seconds, double rate, std::vector<int16_t>& out);

    // Opens a .vgm or .vgz; false (and the reason shown in the window) if it cannot.
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
    void listTracks();
    void loadAssignments();
    void assign(uint32_t fingerprint, const RolandSound::Assignment* a, EditorRequests& out);  // null: back to the rules
    void applyListening();
    void stop();
    void drawTrack(const EditorView& view, EditorRequests& out);
    void drawInstruments(const std::vector<Instrument>& list);
    void drawChannels();
    void drawAssignment(const std::vector<Instrument>& list, EditorRequests& out);

    std::string folder_;  // sonido/, ending in a separator
    bool open_ = false;
    std::string game_;
    std::string gameName_;  // file name without extension
    RolandSound synth_;
    VgmPlayer player_;
    std::string track_;               // the open track's path
    std::vector<std::string> tracks_;  // the game's recordings, and tracks opened since
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
    bool share_ = true;  // what is chosen also goes to the shared cache
    std::string message_;
};

}  // namespace plumsg
