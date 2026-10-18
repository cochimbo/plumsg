#include "song_extract.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include "megadrive.h"
#include "vgm.h"

namespace plumsg {

namespace {

constexpr int kFps = 60;
constexpr int kBootFrames = 45 * kFps;     // watching the game start
constexpr int kCheckpointEvery = 30;       // frames between the boot's savestates
constexpr int kCheckFrames = 2 * kFps;     // to tell whether a request makes sound
constexpr int kSilentFrames = 4 * kFps;    // this long without a note: the song ended
constexpr int kShortest = 3 * kFps;        // sounding less is a sound effect
constexpr int kLongest = 4 * 60 * kFps;    // songs that do not loop are cut here
constexpr int kShortestLoop = 6 * kFps;
constexpr int kMaxCandidates = 8;

// What one frame of a recording did: its writes (but the DAC's samples and the waits)
// in one number, so that a loop shows as the same numbers coming back, and how many
// notes started.
struct FrameSound {
    uint64_t signature = 0;
    int notes = 0;
};

FrameSound scan(const std::vector<uint8_t>& d, size_t from, size_t to) {
    FrameSound f;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
    size_t i = from;
    while (i < to) {
        const uint8_t c = d[i];
        if (c == 0x50 && i + 1 < to) {
            const uint8_t v = d[i + 1];
            mix(0x50);
            mix(v);
            if ((v & 0x90) == 0x90 && (v & 0x0F) != 0x0F) ++f.notes;  // a volume set: a PSG note
            i += 2;
        } else if ((c == 0x52 || c == 0x53) && i + 2 < to) {
            const uint8_t reg = d[i + 1], v = d[i + 2];
            if (reg == 0x2A) {  // a DAC sample: drums and voices
                i += 3;
                continue;
            }
            mix(c);
            mix(reg);
            mix(v);
            if (c == 0x52 && reg == 0x28 && (v & 0xF0)) ++f.notes;
            i += 3;
        } else if (c == 0x61) {
            i += 3;
        } else {
            i += 1;  // 0x62, 0x63, 0x7n
        }
    }
    f.signature = h;
    return f;
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
    int sound = 0;  // notes the loudest request makes when asked again
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
    // driver clears it soon after.
    std::map<uint32_t, Candidate> found;
    // What waits to be cleared, by byte: work RAM, then Z80 RAM.
    std::vector<uint8_t> pendingValue(0x12000, 0);
    std::vector<uint64_t> pendingFrame(0x12000, 0);
    md->setMemoryWatch([&](uint32_t addr, uint8_t v, bool m68k) {
        const size_t i = addr >= 0xFF0000 ? (addr & 0xFFFF) : 0x10000 + (addr & 0x1FFF);
        const uint64_t now = md->frameCount();
        if (v != 0 && v != 0x80) {
            if (m68k) {
                pendingValue[i] = v;
                pendingFrame[i] = now;
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

    // The state at the start of frame `n` of the boot.
    auto goTo = [&](uint64_t n) {
        auto it = checkpoints.upper_bound(n);
        if (it == checkpoints.begin()) return false;
        --it;
        if (!md->loadState(it->second, nullptr)) return false;
        md->lockSoundRequests(0, 0);
        while (md->frameCount() < n) frame();
        return true;
    };
    // Plays `frames` from the state now, recording; returns the notes heard.
    auto listen = [&](int frames) {
        VgmWriter w;
        md->setVgmWriter(&w);
        for (int f = 0; f < frames; ++f) frame();
        md->setVgmWriter(nullptr);
        return scan(w.commands(), 0, w.commands().size()).notes;
    };

    // 2. Which of them make sound: from just before the request, once with the game's
    // request kept out and once asking again ourselves.
    std::vector<Candidate> list;
    for (auto& [addr, c] : found) list.push_back(c);
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        return a.firstFrame != b.firstFrame ? a.firstFrame < b.firstFrame : a.address < b.address;
    });
    if (list.size() > kMaxCandidates) list.resize(kMaxCandidates);
    std::vector<Candidate> queues;
    if (!report("Buscando dónde pide el juego la música")) return fail("");
    for (Candidate& c : list) {
        // Its first few different requests: the first may be a short sound.
        std::set<uint8_t> tried;
        for (const auto& [when, number] : c.requests) {
            if (tried.size() >= 4 || !tried.insert(number).second) continue;
            if (!goTo(when)) continue;
            md->lockSoundRequests(c.address, 1);
            const int quiet = listen(kCheckFrames);
            goTo(when);
            md->lockSoundRequests(c.address, 1);
            md->poke(c.address, number);
            const int asked = listen(kCheckFrames);
            say("Candidato " + hex(c.address, 6) + ", petición " + hex(number, 2) + " en el cuadro " +
                std::to_string(when) + ": " + std::to_string(quiet) + " notas sin ella, " + std::to_string(asked) +
                " pidiéndola");
            c.sound = std::max(c.sound, asked - quiet);
        }
        if (c.sound > 4) queues.push_back(c);
    }
    md->lockSoundRequests(0, 0);
    if (queues.empty())
        return fail("No he encontrado cómo pide la música este juego. Por ahora solo sé sacarla de los juegos con un driver "
                    "como el de Sonic (SMPS) y parecidos.");

    // Ask where the request with most music went, from before the first request of all,
    // with every queue found kept from the game.
    const Candidate* ask = &queues.front();
    uint64_t start = queues.front().firstFrame;
    for (const Candidate& c : queues) {
        if (c.sound > ask->sound) ask = &c;
        start = std::min(start, c.firstFrame);
    }
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
    goTo(start);
    std::vector<uint8_t> origin;
    md->saveState(origin, true);
    checkpoints.clear();

    // 3. Every number in turn.
    p.total = last - first + 1;
    std::set<uint64_t> heard;  // how each song found starts, to skip the same song twice
    for (int number = first; number <= last; ++number) {
        p.tried = number - first;
        if (!report("Probando el " + std::to_string(p.tried + 1))) return fail("");
        md->loadState(origin, nullptr);
        md->lockSoundRequests(lockFrom, static_cast<int>(lockTo - lockFrom));
        md->poke(ask->address, static_cast<uint8_t>(number));
        VgmWriter w;
        md->setVgmWriter(&w);
        std::vector<VgmWriter::Mark> marks;
        std::vector<FrameSound> frames;
        int lastNote = -1, firstNote = -1;
        int loopStart = -1, loopLength = 0;
        for (int f = 0; f < kLongest; ++f) {
            marks.push_back(w.mark(md->masterClocks()));
            frame();
            const size_t from = marks.back().bytes;
            frames.push_back(scan(w.commands(), from, w.mark(md->masterClocks()).bytes));
            if (frames.back().notes) {
                lastNote = f;
                if (firstNote < 0) firstNote = f;
            }
            if (firstNote < 0 && f >= kCheckFrames) break;  // nothing
            if (lastNote >= 0 && f - lastNote >= kSilentFrames) break;  // it ended
            // Looped: the last stretch is the same as the one before it.
            if (f % kFps == 0 && f > 2 * kShortestLoop) {
                const int n = f + 1;
                for (int len = kShortestLoop; 2 * len <= n - firstNote; ++len) {
                    bool same = true;
                    for (int i = 0; i < len && same; ++i)
                        same = frames[static_cast<size_t>(n - 1 - i)].signature ==
                               frames[static_cast<size_t>(n - 1 - len - i)].signature;
                    if (!same) continue;
                    int notes = 0;
                    for (int i = n - len; i < n; ++i) notes += frames[static_cast<size_t>(i)].notes;
                    if (notes < 8) continue;  // silence repeating
                    // Where it first starts going round: move back while the same.
                    int s = n - 2 * len;
                    while (s > firstNote && frames[static_cast<size_t>(s - 1)].signature ==
                                                frames[static_cast<size_t>(s - 1 + len)].signature)
                        --s;
                    loopStart = s;
                    loopLength = len;
                    break;
                }
                if (loopStart >= 0) break;
            }
        }
        md->setVgmWriter(nullptr);
        if (firstNote < 0 || lastNote - firstNote < kShortest) {
            if (firstNote >= 0) say(hex(number, 2) + ": un sonido corto");
            continue;
        }
        // The same song as one found already (several numbers for one song)?
        uint64_t opening = 1469598103934665603ull;
        for (int i = firstNote; i < std::min<int>(firstNote + 10 * kFps, static_cast<int>(frames.size())); ++i)
            opening = (opening ^ frames[static_cast<size_t>(i)].signature) * 1099511628211ull;
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
            const VgmWriter::Mark end = marks[static_cast<size_t>(loopStart + loopLength)];
            song.vgz = w.finish(md->masterClocks(), tag, true, &end, &loop);
            song.seconds = static_cast<double>(end.samples) / 44100.0;
            song.loopSeconds = static_cast<double>(end.samples - loop.samples) / 44100.0;
        } else {
            const size_t endFrame = std::min(frames.size() - 1, static_cast<size_t>(lastNote + kFps));
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
    p.tried = p.total;
    report("Hecho");
    if (p.found == 0) return fail("El juego responde a las peticiones, pero ninguna ha sonado como una canción.");
    return true;
}

}  // namespace plumsg
