#include "song_extract.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include "megadrive.h"
#include "roland.h"
#include "vgm.h"

namespace plumsg {

namespace {

constexpr int kFps = 60;
constexpr int kBootFrames = 45 * kFps;     // watching the game start
constexpr int kCheckpointEvery = 30;       // frames between the boot's savestates
constexpr int kCheckFrames = 2 * kFps;     // to tell whether a request makes sound
constexpr int kSilentFrames = 4 * kFps;    // this long without a note: the song ended
constexpr int kShortest = 2 * kFps;        // sounding less is a sound effect
constexpr int kLongest = 4 * 60 * kFps;    // songs that do not loop are cut here
constexpr int kShortestLoop = 6 * kFps;
constexpr int kMaxCandidates = 48;

// The notes a recording plays, in order: which channel and at what pitch each starts.
// A song's loop shows as the same notes coming back, even where the driver's tempo
// puts them a frame earlier or later the second time.
class NoteTracker {
public:
    struct Note {
        int frame;
        uint32_t key;
    };
    void add(const std::vector<uint8_t>& d, size_t from, size_t to, int frame) {
        size_t i = from;
        while (i < to) {
            const uint8_t c = d[i];
            if (c == 0x50 && i + 1 < to) {
                psg(d[i + 1], frame);
                i += 2;
            } else if ((c == 0x52 || c == 0x53) && i + 2 < to) {
                const int port = c & 1;
                const uint8_t reg = d[i + 1], v = d[i + 2];
                fm_[port][reg] = v;
                if (port == 0 && reg == 0x28 && (v & 0xF0) && (v & 3) != 3) {
                    const int p = (v & 4) ? 1 : 0, ch = v & 3;
                    const uint32_t pitch = static_cast<uint32_t>(fm_[p][0xA4 + ch] << 8 | fm_[p][0xA0 + ch]);
                    notes.push_back({frame, static_cast<uint32_t>((p * 3 + ch) << 24 | (v & 0xF0) << 12) ^ pitch});
                }
                i += 3;
            } else if (c == 0x61) {
                i += 3;
            } else {
                i += 1;
            }
        }
    }
    std::vector<Note> notes;

private:
    void psg(uint8_t v, int frame) {
        if (v & 0x80) {
            latch_ = (v >> 5) & 3;
            volume_ = (v & 0x10) != 0;
            if (volume_) {
                const bool was = attenuation_[latch_] != 15;
                attenuation_[latch_] = v & 0x0F;
                if (!was && attenuation_[latch_] != 15)
                    notes.push_back({frame, static_cast<uint32_t>((8 + latch_) << 24) ^ period_[latch_]});
            } else {
                period_[latch_] = static_cast<uint16_t>((period_[latch_] & 0x3F0) | (v & 0x0F));
            }
        } else if (!volume_) {
            period_[latch_] = static_cast<uint16_t>((period_[latch_] & 0x0F) | (v & 0x3F) << 4);
        }
    }
    uint8_t fm_[2][256] = {};
    int latch_ = 0;
    bool volume_ = false;
    uint16_t period_[4] = {};
    int attenuation_[4] = {15, 15, 15, 15};
};

// The loop of `notes`, in frames: where it starts and where it comes back, or -1.
// The length comes from the busiest channel: the shortest stretch of its notes that
// comes back, a loop's time later (give or take the driver's tempo). Then every channel
// must play the same notes again that long after, for two and a half loops (so that a
// part a song plays twice before going on is not taken for it); the loop starts where
// all of them have started going round.
std::pair<int, int> findLoop(const std::vector<NoteTracker::Note>& notes, int lastFrame, int shortestFrames) {
    constexpr int kSlack = 3;  // frames
    std::map<uint32_t, std::vector<const NoteTracker::Note*>> channels;
    for (const auto& note : notes) channels[note.key >> 24].push_back(&note);
    // (An FM channel if there is one: their notes are never missed. A PSG note that
    // starts before the last one faded out is.)
    const std::vector<const NoteTracker::Note*>* busiest = nullptr;
    for (const auto& [channel, list] : channels) {
        const bool fm = channel < 8;
        const bool busiestFm = busiest && ((*busiest)[0]->key >> 24) < 8;
        if (!busiest || (fm && !busiestFm && list.size() >= 16) || (fm == busiestFm && list.size() > busiest->size()))
            busiest = &list;
    }
    if (!busiest || busiest->size() < 16) return {-1, -1};
    const auto& list = *busiest;
    const int n = static_cast<int>(list.size());
    // Does a note come back `period` frames later?
    auto comesBack = [&](const std::vector<const NoteTracker::Note*>& in, size_t i, int period) {
        const int at = in[i]->frame + period;
        auto it = std::lower_bound(in.begin(), in.end(), at - kSlack,
                                   [](const NoteTracker::Note* x, int f) { return x->frame < f; });
        for (; it != in.end() && (*it)->frame <= at + kSlack; ++it)
            if ((*it)->key == in[i]->key) return true;
        return false;
    };
    for (int len = 1; 2 * len < n; ++len) {
        const int period = list[static_cast<size_t>(n - 1)]->frame - list[static_cast<size_t>(n - 1 - len)]->frame;
        if (period < shortestFrames) continue;
        if (lastFrame < period * 5 / 2) return {-1, -1};
        if (list[static_cast<size_t>(n - 1 - len)]->key != list[static_cast<size_t>(n - 1)]->key) continue;
        // Every channel: from where on do its notes come back a loop later?
        int start = 0;
        bool ok = true;
        for (const auto& [channel, in] : channels) {
            // The earliest note from which they come back (but the odd one: a PSG note
            // that starts before the last one faded can go unseen).
            int first = -1, checked = 0, missed = 0;
            for (int i = static_cast<int>(in.size()) - 1; i >= 0; --i) {
                if (in[static_cast<size_t>(i)]->frame + period + kSlack > lastFrame) continue;  // not due yet
                ++checked;
                if (!comesBack(in, static_cast<size_t>(i), period)) {
                    if (++missed > 1 + checked / 32) break;
                    continue;
                }
                first = i;
            }
            const int since = first < 0 ? lastFrame - period : in[static_cast<size_t>(first)]->frame;
            if (first < 0 && in.back()->frame + period + kSlack <= lastFrame) {
                ok = false;  // its last note never came back
                break;
            }
            start = std::max(start, since);
        }
        if (!ok) continue;
        if (lastFrame - start < period * 5 / 2) continue;
        return {start, start + period};
    }
    return {-1, -1};
}

std::string hex(uint32_t v, int digits) {
    char s[16];
    std::snprintf(s, sizeof s, "%0*X", digits, v);
    return s;
}

// A byte the game wrote a request to, which was then cleared.
struct Candidate {
    uint32_t address = 0;
    uint64_t firstFrame = 0;  // of its first request
    std::vector<std::pair<uint64_t, uint8_t>> requests;  // frame, number
    int driverTaken = 0;  // requests the game's program made and the sound driver took
    int sound = 0;  // notes the loudest request makes when asked again
    uint64_t soundFrame = 0;  // and where it was asked: nothing else plays there
};

}  // namespace

bool SongExtractor::run(const std::string& romPath, const std::string& game, const ProgressFn& progress,
                        const SongFn& onSong, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    auto say = [&](const std::string& s) {
        if (log_) log_(s);
    };
    effects_.clear();
    auto md = std::make_unique<MegaDrive>();
    if (!md->loadRom(romPath, error)) return false;
    if (md->isMegaCd()) return fail("Las canciones de los discos de Mega CD van en pistas de audio del disco, no en el chip.");
    md->setDrawing(false);

    Progress p;
    auto report = [&](const std::string& doing) {
        p.doing = doing;
        return !progress || progress(p);
    };
    auto frame = [&]() {
        md->runFrame();
        md->audio().clearSamples();
    };

    // 1. Boot, watching for requests: the 68000 writes a number to a byte and the
    // driver clears it soon after. Which code does each tells a request from a variable
    // of the game: the game's program asks, and the driver (the Z80, or the 68000 code
    // that writes the YM2612) takes it.
    std::map<uint32_t, Candidate> found;
    // What waits to be cleared, by byte (work RAM, then Z80 RAM), and who wrote it.
    std::vector<uint8_t> pendingValue(0x12000, 0);
    std::vector<uint64_t> pendingFrame(0x12000, 0);
    std::vector<uint32_t> pendingPc(0x12000, 0);
    std::map<uint32_t, int> ymCode;  // 68000 code writing the YM2612, by 256-byte page
    struct Taken {
        uint32_t address, askedFrom, takenFrom;  // takenFrom 0: by the Z80
    };
    std::vector<Taken> taken;
    md->setMemoryWatch([&](uint32_t addr, uint8_t v, bool m68k) {
        if (addr >= 0xA04000 && addr < 0xA04004) {
            ++ymCode[md->cpu().pc >> 8];
            return;
        }
        const size_t i = addr >= 0xFF0000 ? (addr & 0xFFFF) : 0x10000 + (addr & 0x1FFF);
        const uint64_t now = md->frameCount();
        if (v != 0 && v != 0x80) {
            if (m68k) {
                pendingValue[i] = v;
                pendingFrame[i] = now;
                pendingPc[i] = md->cpu().pc;
            }
            return;
        }
        if (!pendingValue[i]) return;
        if (now - pendingFrame[i] <= 3) {
            Candidate& c = found[addr];
            if (c.requests.empty()) {
                c.address = addr;
                c.firstFrame = pendingFrame[i];
            }
            if (c.requests.size() < 64) c.requests.push_back({pendingFrame[i], pendingValue[i]});
            taken.push_back({addr, pendingPc[i], m68k ? md->cpu().pc : 0});
        }
        pendingValue[i] = 0;
    });
    std::map<uint64_t, std::vector<uint8_t>> checkpoints;
    if (!report("Arrancando el juego")) return fail("");
    for (int f = 0; f < kBootFrames; ++f) {
        if (f % kCheckpointEvery == 0) md->saveState(checkpoints[md->frameCount()], true);
        frame();
    }
    md->setMemoryWatch({});
    auto inDriver = [&](uint32_t pc) {
        if (!pc) return true;  // the Z80
        for (const auto& [page, n] : ymCode)
            if (n > 50 && pc + 0x1000 >= (page << 8) && pc < (page << 8) + 0x1100) return true;
        return false;
    };
    for (const Taken& t : taken)
        if (!inDriver(t.askedFrom) && inDriver(t.takenFrom)) ++found[t.address].driverTaken;

    // The state at the start of frame `n` of the boot.
    auto goTo = [&](uint64_t n) {
        auto it = checkpoints.upper_bound(n);
        if (it == checkpoints.begin()) return false;
        --it;
        if (!md->loadState(it->second, nullptr)) return false;
        md->lockSoundRequests(0, 0);
        md->cpu().holdMain(false);
        while (md->frameCount() < n) frame();
        return true;
    };
    // Plays `frames` from the state now, recording; returns the notes heard (channel and
    // pitch, how many times each).
    auto listen = [&](int frames) {
        VgmWriter w;
        md->setVgmWriter(&w);
        for (int f = 0; f < frames; ++f) frame();
        md->setVgmWriter(nullptr);
        NoteTracker t;
        t.add(w.commands(), 0, w.commands().size(), 0);
        std::map<uint32_t, int> heard;
        for (const auto& note : t.notes) ++heard[note.key];
        return heard;
    };
    // The notes in `a` that are not in `b`.
    auto extra = [](const std::map<uint32_t, int>& a, const std::map<uint32_t, int>& b) {
        int n = 0;
        for (const auto& [key, times] : a) {
            auto it = b.find(key);
            n += std::max(0, times - (it == b.end() ? 0 : it->second));
        }
        return n;
    };
    auto count = [](const std::map<uint32_t, int>& a) {
        int n = 0;
        for (const auto& [key, times] : a) n += times;
        return n;
    };

    // 2. Which of them make sound: from just before the request, once with the game's
    // request kept out and once asking again ourselves.
    // (Not the bytes of a block copied in one go, as a Z80 program loaded and then
    // cleared: many of them start in the same frame, and that is their only "request".)
    std::map<uint64_t, int> starting;
    for (const auto& [addr, c] : found) ++starting[c.firstFrame];
    std::vector<Candidate> list;
    for (auto& [addr, c] : found)
        if (c.requests.size() > 1 || starting[c.firstFrame] < 16) list.push_back(c);
    // Those the driver took first, then in the order the game first used them.
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        if ((a.driverTaken > 0) != (b.driverTaken > 0)) return a.driverTaken > 0;
        return a.firstFrame != b.firstFrame ? a.firstFrame < b.firstFrame : a.address < b.address;
    });
    if (log_)
        for (const Candidate& c : list) {
            std::string r;
            for (size_t i = 0; i < c.requests.size() && i < 12; ++i)
                r += " " + hex(c.requests[i].second, 2) + "@" + std::to_string(c.requests[i].first);
            say("  " + hex(c.address, 6) + (c.driverTaken ? " (driver " + std::to_string(c.driverTaken) + ")" : "") + ":" + r);
        }
    if (list.size() > kMaxCandidates) list.resize(kMaxCandidates);
    std::vector<Candidate> queues;
    if (!report("Buscando dónde pide el juego la música")) return fail("");
    for (Candidate& c : list) {
        // Its first few different requests: the first may be a short sound.
        std::set<uint8_t> tried;
        for (const auto& [when, number] : c.requests) {
            if (tried.size() >= 4 || !tried.insert(number).second) continue;
            if (!goTo(when)) {
                say("No se puede volver al cuadro " + std::to_string(when));
                continue;
            }
            md->lockSoundRequests(c.address, 1);
            const int quiet = count(listen(kCheckFrames));
            goTo(when);
            md->lockSoundRequests(c.address, 1);
            md->poke(c.address, number);
            const int asked = count(listen(kCheckFrames));
            say("Candidato " + hex(c.address, 6) + ", petición " + hex(number, 2) + " en el cuadro " +
                std::to_string(when) + ": " + std::to_string(quiet) + " notas sin ella, " + std::to_string(asked) +
                " pidiéndola");
            if (asked - quiet > c.sound) {
                c.sound = asked - quiet;
                c.soundFrame = when;
            }
        }
        if (c.sound > 4) queues.push_back(c);
    }
    md->lockSoundRequests(0, 0);
    if (queues.empty())
        return fail("No he encontrado cómo pide la música este juego. Por ahora solo sé sacarla de los juegos con un driver "
                    "como el de Sonic (SMPS) y parecidos.");

    // Where the driver takes the requests first, then where asking made most music. A
    // queue where no number plays a song (a variable of the game that only looked like
    // one) gives way to the next.
    std::stable_sort(queues.begin(), queues.end(), [](const Candidate& a, const Candidate& b) {
        if ((a.driverTaken > 0) != (b.driverTaken > 0)) return a.driverTaken > 0;
        return a.sound > b.sound;
    });
    // The songs it found asking at `chosen`, or -1 if stopped.
    auto tryQueue = [&](const Candidate& chosen) -> int {
        // Ask where the request with most music went, from where the game made it (the
        // driver is ready there, and nothing else was playing), with every queue found kept
        // from the game.
        const Candidate* ask = &chosen;
        const uint64_t start = ask->soundFrame;
        uint32_t lockFrom = ask->address, lockTo = ask->address + 1;
        for (const Candidate& c : queues)
            if (c.address + 8 > ask->address && c.address < ask->address + 8) {  // the same queue
                lockFrom = std::min(lockFrom, c.address);
                lockTo = std::max(lockTo, c.address + 1);
            }
        bool high = true;
        for (const auto& r : ask->requests) high = high && r.second >= 0x81;
        const int first = high ? 0x81 : 0x01, last = 0xFF;
        say("Pido las canciones en " + hex(ask->address, 6) + ", del " + hex(first, 2) + " al " + hex(last, 2) +
            ", desde el cuadro " + std::to_string(start) + "; las peticiones del juego en " + hex(lockFrom, 6) + "-" +
            hex(lockTo - 1, 6) + " no pasan");
        // Better with the game held where it is, only its VBlank running: it cannot go on to
        // load a level (the music would stop meanwhile) or ask for other sounds. Some games
        // need their program running to feed the driver.
        // (Held, asking must add notes to what was playing: some games hand the request on to
        // the driver from their program.)
        goTo(start);
        md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
        md->cpu().holdMain(true);
        const auto heldQuiet = listen(kCheckFrames);
        md->cpu().holdMain(false);
        goTo(start);
        md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
        md->poke(ask->address, ask->requests.front().second);
        for (const auto& r : ask->requests)
            if (r.first == start) md->poke(ask->address, r.second);
        md->cpu().holdMain(true);
        const bool hold = extra(listen(kCheckFrames), heldQuiet) > 4;
        md->cpu().holdMain(false);
        say(hold ? "El juego se queda quieto mientras suena cada canción" : "El juego sigue andando mientras suena cada canción");
        goTo(start);
        std::vector<uint8_t> origin;
        md->saveState(origin, true);
        // What sounds there without asking for anything (what was playing goes on): a number
        // that only plays that is not a song.
        // The Roland sound listens (without a SoundFont, so silent) for the FM instruments
        // of each number: those of the short sounds that no song plays are the sound effects.
        RolandSound ear;
        md->setRoland(&ear, 100);
        struct Unplug {  // the console must not keep it once this try is over
            MegaDrive& md;
            ~Unplug() { md.setRoland(nullptr, 100); }
        } unplug{*md};
        auto instruments = [&](std::set<uint32_t>& into) {
            for (const auto& [fp, seen] : ear.seen())
                if (seen.kind == RolandSound::Kind::Fm) into.insert(fp);
        };
        md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
        md->cpu().holdMain(hold);
        ear.clearSeen();
        const auto before = listen(kCheckFrames);
        std::set<uint32_t> playing;  // and its instruments
        instruments(playing);

        // 3. Every number in turn.
        p.total = last - first + 1;
        std::set<uint64_t> heard;  // how each song found starts, to skip the same song twice
        std::set<uint32_t> musicSounds, shortSounds;
        std::vector<int> silent;  // numbers that played nothing
        struct Effects {  // worked out however run() ends from here
            SongExtractor& x;
            std::set<uint32_t>& music;
            std::set<uint32_t>& sounds;
            ~Effects() {
                for (uint32_t fp : sounds)
                    if (!music.count(fp)) x.effects_.insert(fp);
            }
        } effects{*this, musicSounds, shortSounds};
        for (int number = first; number <= last; ++number) {
            p.tried = number - first;
            if (!report("Probando el " + std::to_string(p.tried + 1))) return -1;
            md->loadState(origin, nullptr);
            md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
            md->poke(ask->address, static_cast<uint8_t>(number));
            md->cpu().holdMain(hold);
            ear.allNotesOff();
            ear.clearSeen();
            VgmWriter w;
            md->setVgmWriter(&w);
            std::vector<VgmWriter::Mark> marks;
            NoteTracker tracker;
            int lastNote = -1, firstNote = -1;
            int loopStart = -1, loopEnd = -1;
            bool same = false;  // it only adds a few notes to what was playing
            for (int f = 0; f < kLongest; ++f) {
                marks.push_back(w.mark(md->masterClocks()));
                frame();
                const size_t from = marks.back().bytes, to = w.mark(md->masterClocks()).bytes;
                const size_t had = tracker.notes.size();
                tracker.add(w.commands(), from, to, f);
                if (tracker.notes.size() > had) {
                    lastNote = f;
                    if (firstNote < 0) firstNote = f;
                }
                if (firstNote < 0 && f >= kCheckFrames) break;  // nothing
                if (f == kCheckFrames - 1 && firstNote >= 0) {
                    // Without waiting for it to end (what was playing may go on for minutes).
                    std::map<uint32_t, int> opened;
                    for (const auto& note : tracker.notes) ++opened[note.key];
                    if (extra(opened, before) < 8) {
                        same = true;
                        break;
                    }
                }
                if (lastNote >= 0 && f - lastNote >= kSilentFrames) break;  // it ended
                if (f % kFps == 0) {
                    const auto [start, end] = findLoop(tracker.notes, f, kShortestLoop);
                    if (start >= 0 && end <= f) {
                        loopStart = start;
                        loopEnd = end;
                        break;
                    }
                }
            }
            md->setVgmWriter(nullptr);
            if (same) {
                say(hex(number, 2) + ": suena lo mismo que sin pedir nada");
                // A sound effect over the music: its instruments are the new ones.
                std::set<uint32_t> now;
                instruments(now);
                for (uint32_t fp : now)
                    if (!playing.count(fp)) shortSounds.insert(fp);
                continue;
            }
            if (firstNote < 0 || lastNote - firstNote < kShortest) {
                if (firstNote >= 0) say(hex(number, 2) + ": un sonido corto");
                if (firstNote >= 0) instruments(shortSounds);
                else silent.push_back(number);
                continue;
            }
            std::map<uint32_t, int> opened;
            for (const auto& note : tracker.notes)
                if (note.frame < kCheckFrames) ++opened[note.key];
            if (extra(opened, before) < 8) {
                say(hex(number, 2) + ": suena lo mismo que sin pedir nada");
                continue;
            }
            // The same song as one found already (several numbers for one song)?
            uint64_t opening = 1469598103934665603ull;
            for (size_t i = 0; i < std::min<size_t>(tracker.notes.size(), 64); ++i)
                opening = (opening ^ tracker.notes[i].key) * 1099511628211ull;
            instruments(musicSounds);
            if (!heard.insert(opening).second) {
                say(hex(number, 2) + ": repetida");
                continue;
            }
            Song song;
            song.number = number;
            VgmWriter::Tag tag;
            tag.title = "Canción " + std::to_string(p.found + 1);
            tag.game = game;
            tag.notes = "Sacada de la ROM (petición " + hex(number, 2) + ")";
            if (loopStart >= 0) {
                const VgmWriter::Mark loop = marks[static_cast<size_t>(loopStart)];
                const VgmWriter::Mark end = marks[static_cast<size_t>(loopEnd)];
                song.vgz = w.finish(md->masterClocks(), tag, true, &end, &loop);
                song.seconds = static_cast<double>(end.samples) / 44100.0;
                song.loopSeconds = static_cast<double>(end.samples - loop.samples) / 44100.0;
            } else {
                const size_t endFrame = std::min(marks.size() - 1, static_cast<size_t>(lastNote + kFps));
                const VgmWriter::Mark end = marks[endFrame];
                song.vgz = w.finish(md->masterClocks(), tag, true, &end, nullptr);
                song.seconds = static_cast<double>(end.samples) / 44100.0;
            }
            ++p.found;
            say(hex(number, 2) + ": " + tag.title + ", " + std::to_string(static_cast<int>(song.seconds)) + " s" +
                (song.loopSeconds > 0 ? ", vuelta de " + std::to_string(static_cast<int>(song.loopSeconds)) + " s"
                                      : ", sin vuelta"));
            if (onSong) onSong(song);
        }

        // 4. Some drivers play sound effects only over music (Sonic's, after the fade out
        // that comes before a level's song): the numbers that played nothing, once more with
        // the game's own song started. What they add to it are their instruments.
        p.tried = p.total;
        if (!silent.empty() && report("Buscando los efectos de sonido")) {
            uint8_t song = ask->requests.front().second;
            for (const auto& r : ask->requests)
                if (r.first == start) song = r.second;
            md->loadState(origin, nullptr);
            md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
            md->poke(ask->address, song);
            md->cpu().holdMain(hold);
            for (int f = 0; f < kFps; ++f) frame();
            std::vector<uint8_t> playing;
            md->saveState(playing, true);
            auto over = [&](int number) {
                md->loadState(playing, nullptr);
                md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
                if (number) md->poke(ask->address, static_cast<uint8_t>(number));
                md->cpu().holdMain(hold);
                ear.allNotesOff();
                ear.clearSeen();
                for (int f = 0; f < kCheckFrames; ++f) frame();
                std::set<uint32_t> heard;
                instruments(heard);
                return heard;
            };
            const std::set<uint32_t> alone = over(0);
            for (int number : silent) {
                if (!report("Buscando los efectos de sonido")) break;
                for (uint32_t fp : over(number))
                    if (!alone.count(fp)) shortSounds.insert(fp);
            }
        }
        return p.found;
    };
    for (size_t q = 0; q < queues.size() && q < 3; ++q) {
        effects_.clear();
        const int songs = tryQueue(queues[q]);
        if (songs < 0) return fail("");
        if (songs > 0) break;
    }
    report("Hecho");
    if (p.found == 0) return fail("El juego responde a las peticiones, pero ninguna ha sonado como una canción.");
    return true;
}

}  // namespace plumsg
