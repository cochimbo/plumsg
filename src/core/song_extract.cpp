#include "song_extract.h"

#include "i18n.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

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
constexpr int kLongest = 6 * 60 * kFps;    // songs that do not loop are cut here
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
// all of them have started going round. (Twice is enough for a loop of over 45 s.)
std::pair<int, int> findLoopOf(const std::vector<NoteTracker::Note>& notes, int lastFrame, int shortestFrames,
                               int shortestHeard = 0) {
    constexpr int kSlack = 3;  // frames
    auto heard = [&](int period) { return std::max(shortestHeard, period < 45 * kFps ? period * 5 / 2 : period * 2); };
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
        if (lastFrame < heard(period)) return {-1, -1};
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
                    ++missed;
                    if (!shortestHeard && missed > 1 + checked / 32) break;
                    continue;
                }
                // (Taken together, the odd few in a row can go missing where the driver
                // gave a note's channel to another.)
                if (missed <= 1 + checked / 32) first = i;
            }
            const int since = first < 0 ? lastFrame - period : in[static_cast<size_t>(first)]->frame;
            if (first < 0 && in.back()->frame + period + kSlack <= lastFrame) {
                ok = false;  // its last note never came back
                break;
            }
            start = std::max(start, since);
        }
        if (!ok) continue;
        if (lastFrame - start < heard(period)) continue;
        return {start, start + period};
    }
    return {-1, -1};
}

// Some drivers (GEMS) play each note on whichever channel is free, so a note comes back a
// loop later on another channel: then the notes of all FM channels are taken together
// (without the PSG's, whose notes such a driver's envelopes blur), for a minute and a
// half at least (taken together, a part of the song that repeats a bar looks like a
// short loop for a while).
std::pair<int, int> findLoop(const std::vector<NoteTracker::Note>& notes, int lastFrame, int shortestFrames) {
    const auto loop = findLoopOf(notes, lastFrame, shortestFrames);
    if (loop.first >= 0) return loop;
    std::vector<NoteTracker::Note> pooled;
    for (const auto& note : notes)
        if ((note.key >> 24) < 8) pooled.push_back({note.frame, note.key & 0xFFFFFF});
    return findLoopOf(pooled, lastFrame, shortestFrames, 90 * kFps);
}

std::string hex(uint32_t v, int digits) {
    char s[16];
    std::snprintf(s, sizeof s, "%0*X", digits, v);
    return s;
}

constexpr uint32_t kKeep = 0xFFFFFFFF;  // a register a call leaves as it is

// Where the game asks for music: a byte it wrote a request to, which was then cleared,
// a routine of the driver it calls with the number in a register, or a ring of commands
// in the Z80's memory that the game writes messages to (GEMS's).
struct Candidate {
    uint32_t address = 0;  // of the byte, of the routine, or of the ring's write position
    bool call = false;
    bool z80 = false;  // a call that only leads to the Z80's memory, not to a driver found
    bool ring = false;
    uint32_t ringStart = 0, ringMask = 0;  // where the ring is, and its size less one
    std::vector<uint8_t> message;          // what goes before the number
    std::array<uint32_t, 15> regs{};  // a call's d0-d7 and a0-a6: what the game loads, or kKeep
    int numberReg = 0;                // and which of them holds the number
    int argBytes = 0;                 // or 2 or 4: the number goes on the stack (EA's driver)
    uint32_t pops = 0;                // bytes of it the routine takes off the stack itself
    std::vector<uint32_t> numbers;    // the numbers the game calls it with
    uint64_t firstFrame = 0;  // of its first request
    std::vector<std::pair<uint64_t, uint32_t>> requests;  // frame, number
    int driverTaken = 0;  // requests the game's program made and the sound driver took
    int sound = 0;  // notes the loudest request makes when asked again
    uint64_t soundFrame = 0;  // and where it was asked: nothing else plays there
    bool probed = false;      // only a number the game was not seen asking for made it
};

// Where a call takes its number, for the log.
std::string where(const Candidate& c) {
    return c.argBytes ? "la pila" : "d" + std::to_string(c.numberReg);
}

}  // namespace

bool SongExtractor::run(const std::string& romPath, const std::string& game, const ProgressFn& progress,
                        const SongFn& onSong, std::string* error) {
    bool notFound = false;
    if (tryRun(romPath, game, progress, onSong, error, false, &notFound)) return true;
    if (!notFound) return false;
    if (log_) log_("No suena nada que se pueda pedir; arranco otra vez pulsando Start de vez en cuando");
    return tryRun(romPath, game, progress, onSong, error, true, &notFound);
}

bool SongExtractor::tryRun(const std::string& romPath, const std::string& game, const ProgressFn& progress,
                           const SongFn& onSong, std::string* error, bool pressStart, bool* notFound) {
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
    if (md->isMegaCd()) return fail(tr("Las canciones de los discos de Mega CD van en pistas de audio del disco, no en el chip."));
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
    struct ZWrite {
        uint64_t frame;
        uint16_t address;
        uint8_t value;
    };
    std::vector<ZWrite> z80Writes;  // what the 68000 writes to the Z80's memory, in order
    struct ZWriter {
        uint64_t lastFrame = ~0ull;
        int frames = 0, writes = 0;
    };
    std::map<uint32_t, ZWriter> z80Code;  // 68000 code writing the Z80's memory, by 256-byte page
    md->setMemoryWatch([&](uint32_t addr, uint8_t v, bool m68k) {
        if (addr >= 0xA04000 && addr < 0xA04004) {
            ++ymCode[md->cpu().pc >> 8];
            return;
        }
        if (m68k && addr >= 0xA00000 && addr < 0xA02000) {
            if (z80Writes.size() < 500000) z80Writes.push_back({md->frameCount(), static_cast<uint16_t>(addr & 0x1FFF), v});
            ZWriter& w = z80Code[md->cpu().pc >> 8];
            ++w.writes;
            if (w.lastFrame != md->frameCount()) {
                w.lastFrame = md->frameCount();
                ++w.frames;
            }
        }
        // (Not the stack: what the program pushes, and a routine it calls then overwrites.)
        if (m68k && addr >= 0xFF0000 && addr - (md->cpu().a[7] & 0xFFFFFF) < 0x100) return;
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
    if (!report(tr("Arrancando el juego"))) return fail("");
    for (int f = 0; f < kBootFrames; ++f) {
        if (f % kCheckpointEvery == 0) md->saveState(checkpoints[md->frameCount()], true);
        if (pressStart) {  // for a tenth of a second every three, once past the logos
            PadState pad;
            pad.start = f >= 5 * kFps && f % (3 * kFps) < 6;
            md->setPads(pad, PadState{});
        }
        frame();
    }
    md->setPads(PadState{}, PadState{});
    md->setMemoryWatch({});
    // Some drivers run on the 68000 and hand the YM2612's writes to the Z80 (Streets of
    // Rage 3's): code that writes the Z80's memory a lot, frame after frame, is one too.
    for (const auto& [page, w] : z80Code)
        if (w.frames > 50 && w.writes >= 6 * w.frames) ymCode[page] += 51;
    auto inDriver = [&](uint32_t pc) {
        if (!pc) return true;  // the Z80
        for (const auto& [page, n] : ymCode)
            if (n > 50 && pc + 0x1000 >= (page << 8) && pc < (page << 8) + 0x1100) return true;
        return false;
    };
    for (const Taken& t : taken)
        if (!inDriver(t.askedFrom) && inDriver(t.takenFrom)) ++found[t.address].driverTaken;

    // Other drivers are asked by calling one of their routines with the number in a
    // register (Streets of Rage 2's: the music and each kind of sound with a number of
    // its own) or on the stack (EA's, through a routine of the game that calls the
    // driver's). The routines the game's program calls from many places, loading a
    // register or pushing a different number before each call.
    std::vector<Candidate> calls;
    bool heldBack = false;  // calls left for the second boot
    {
        constexpr size_t kStack = 15;  // where a site's number pushed on the stack goes
        struct Site {
            std::array<uint32_t, 16> regs;  // d0-d7, a0-a6, then the stack
            int argBytes;
            bool callerPops;  // the caller takes the number off the stack after the call
        };
        std::map<uint32_t, std::vector<Site>> sites;  // by routine
        const uint32_t size = static_cast<uint32_t>(std::min<size_t>(md->cartridge().romSize(), 0x400000));
        auto word = [&](uint32_t at) { return md->peek16(at); };
        // Where a call (JSR, JMP, BSR) at `at` goes, and how long it is; 0 if it is not one.
        auto callAt = [&](uint32_t at, uint32_t& length) -> uint32_t {
            const uint16_t op = word(at);
            length = 4;
            if (op == 0x4EB9 || op == 0x4EF9) {  // JSR/JMP (xxx).L
                length = 6;
                return (static_cast<uint32_t>(word(at + 2)) << 16 | word(at + 4)) & 0xFFFFFF;
            }
            if (op == 0x4EB8 || op == 0x4EF8)  // JSR/JMP (xxx).W
                return static_cast<uint32_t>(static_cast<int16_t>(word(at + 2))) & 0xFFFFFF;
            if (op == 0x6100 || op == 0x4EBA)  // BSR.W, JSR (d16,PC)
                return (at + 2 + static_cast<uint32_t>(static_cast<int16_t>(word(at + 2)))) & 0xFFFFFF;
            if ((op & 0xFF00) == 0x6100 && (op & 0xFF) != 0xFF) {  // BSR.B
                length = 2;
                return (at + 2 + static_cast<uint32_t>(static_cast<int8_t>(op & 0xFF))) & 0xFFFFFF;
            }
            return 0;
        };
        // A routine that writes the Z80's memory (where a Z80 driver takes its orders,
        // Ecco's), from its first bytes: an address there in its code.
        auto writesZ80 = [&](uint32_t routine) {
            for (uint32_t at = routine; at < routine + 0x100 && at + 4 <= size; at += 2)
                if (word(at) == 0x00A0 && word(at + 2) < 0x2000) return true;
            return false;
        };
        // A routine of the driver, or one that calls one, or a routine that does, from its
        // first bytes (the game's own "play this", in front of the driver's).
        // (z80: or to one that writes the Z80's memory.)
        std::map<std::tuple<uint32_t, int, bool>, bool> leads;
        std::function<bool(uint32_t, int, bool)> leadsToDriver = [&](uint32_t routine, int depth, bool z80) {
            if (inDriver(routine) || (z80 && writesZ80(routine))) return true;
            if (depth == 0) return false;
            const auto key = std::make_tuple(routine, depth, z80);
            if (auto it = leads.find(key); it != leads.end()) return it->second;
            leads[key] = false;
            bool yes = false;
            for (uint32_t at = routine; !yes && at < routine + 0x100 && at + 6 <= size; at += 2) {
                uint32_t length;
                const uint32_t to = callAt(at, length);
                if (to && to < size && !(to & 1) && to != routine) yes = leadsToDriver(to, depth - 1, z80);
            }
            return leads[key] = yes;
        };
        for (uint32_t at = 0x200; at + 6 <= size; at += 2) {
            const uint16_t op = word(at);
            if (op != 0x4EB9 && op != 0x4EF9 && op != 0x4EB8 && op != 0x4EF8) continue;
            uint32_t length;
            const uint32_t target = callAt(at, length);
            if (target >= size || (target & 1) || inDriver(at) || !leadsToDriver(target, 2, true)) continue;
            // The registers given a number right before (MOVE.L/MOVE.W/MOVE.B #n,Dn, MOVEQ),
            // and the number pushed last (MOVE.W/MOVE.L #n,-(A7), PEA (n).W) or put where
            // the stack has room for it already (MOVE.L/MOVE.W Dn,(A7), as C compilers do:
            // the caller's room, which it takes off itself).
            Site site;
            site.regs.fill(kKeep);
            site.argBytes = 0;
            site.callerPops = false;
            uint32_t pos = at;
            bool any = false;
            int stackFrom = -1;  // the register put on the stack
            for (int k = 0; k < 3; ++k) {
                size_t reg;
                uint32_t value;
                int pushed = 0;
                if (pos >= 6 && (word(pos - 6) & 0xF1FF) == 0x203C) {
                    reg = (word(pos - 6) >> 9) & 7;
                    value = static_cast<uint32_t>(word(pos - 4)) << 16 | word(pos - 2);
                    pos -= 6;
                } else if (pos >= 4 && ((word(pos - 4) & 0xF1FF) == 0x303C || (word(pos - 4) & 0xF1FF) == 0x103C)) {
                    reg = (word(pos - 4) >> 9) & 7;
                    value = word(pos - 2) & ((word(pos - 4) & 0x2000) ? 0xFFFF : 0xFF);
                    pos -= 4;
                } else if (pos >= 2 && (word(pos - 2) & 0xF100) == 0x7000) {
                    reg = (word(pos - 2) >> 9) & 7;
                    value = static_cast<uint32_t>(static_cast<int8_t>(word(pos - 2) & 0xFF));
                    pos -= 2;
                } else if (pos >= 4 && word(pos - 4) == 0x3F3C) {
                    reg = kStack;
                    value = word(pos - 2);
                    pushed = 2;
                    pos -= 4;
                } else if (pos >= 4 && word(pos - 4) == 0x4878) {
                    reg = kStack;
                    value = static_cast<uint32_t>(static_cast<int16_t>(word(pos - 2)));
                    pushed = 4;
                    pos -= 4;
                } else if (pos >= 6 && word(pos - 6) == 0x2F3C) {
                    reg = kStack;
                    value = static_cast<uint32_t>(word(pos - 4)) << 16 | word(pos - 2);
                    pushed = 4;
                    pos -= 6;
                } else if (pos >= 2 && k == 0 && ((word(pos - 2) & 0xFFF8) == 0x2E80 || (word(pos - 2) & 0xFFF8) == 0x3E80)) {
                    stackFrom = word(pos - 2) & 7;
                    site.argBytes = (word(pos - 2) & 0x1000) ? 2 : 4;
                    site.callerPops = true;
                    pos -= 2;
                    continue;
                } else {
                    break;
                }
                if (pushed) {
                    if (site.argBytes) break;  // a second argument: not one of these
                    site.argBytes = pushed;
                }
                if (site.regs[reg] == kKeep) site.regs[reg] = value;  // the last one loaded
                any = true;
            }
            if (stackFrom >= 0) {
                site.regs[kStack] = site.regs[static_cast<size_t>(stackFrom)];
                site.regs[static_cast<size_t>(stackFrom)] = kKeep;
            }
            if (!any || (stackFrom >= 0 && site.regs[kStack] == kKeep)) continue;
            // ADDQ #n,A7 or LEA (d,A7),A7 right after.
            const uint16_t next = word(at + length);
            site.callerPops = site.callerPops || (next & 0xF13F) == 0x500F || next == 0x4FEF;
            sites[target].push_back(site);
        }
        for (const auto& [routine, list] : sites) {
            if (list.size() < 8) continue;
            // The number goes where the game gives most different values; the registers
            // keep what the game gives them most often.
            Candidate c;
            c.address = routine;
            c.call = true;
            c.z80 = !leadsToDriver(routine, 2, false);
            c.regs.fill(kKeep);
            size_t most = 0, numberAt = 0;
            for (size_t r = 0; r < 16; ++r) {
                if (r >= 8 && r < kStack) continue;
                std::map<uint32_t, int> values;
                size_t loaded = 0;
                for (const Site& site : list)
                    if (site.regs[r] != kKeep) {
                        ++values[site.regs[r]];
                        ++loaded;
                    }
                if (values.size() > most) {
                    most = values.size();
                    numberAt = r;
                }
                if (r < 8 && loaded * 2 >= list.size()) {
                    auto common = std::max_element(values.begin(), values.end(),
                                                   [](const auto& a, const auto& b) { return a.second < b.second; });
                    c.regs[r] = common->first;
                }
            }
            if (most < 3) continue;
            std::set<uint32_t> numbers;
            int callerPops = 0, pushes = 0;
            for (const Site& site : list)
                if (site.regs[numberAt] <= 0xFFFF) {
                    numbers.insert(site.regs[numberAt]);
                    if (numberAt == kStack) {
                        c.argBytes = std::max(c.argBytes, site.argBytes);
                        callerPops += site.callerPops;
                        ++pushes;
                    }
                }
            if (numberAt == kStack) {
                c.numberReg = -1;
                if (callerPops * 2 < pushes) c.pops = static_cast<uint32_t>(c.argBytes);
            } else {
                c.numberReg = static_cast<int>(numberAt);
            }
            c.numbers.assign(numbers.begin(), numbers.end());
            if (c.numbers.size() < 3) continue;
            // Asked with a few of them (one of each kind first: the number's high byte), and
            // the next few after the last (the songs may come after the sounds), where they
            // are found (below).
            std::vector<uint32_t> sample;
            std::set<uint32_t> kinds;
            for (uint32_t n : c.numbers)
                if (n > 0xFF && kinds.insert(n >> 8).second) sample.push_back(n);
            for (uint32_t n : c.numbers)
                if (n && std::find(sample.begin(), sample.end(), n) == sample.end()) sample.push_back(n);
            if (sample.size() > 4) sample.resize(4);
            for (uint32_t step = 1; step <= 3; ++step)
                if ((c.numbers.back() & 0xFF) + step <= 0xFF) sample.push_back(c.numbers.back() + step);
            for (uint32_t n : sample) c.requests.push_back({0, n});
            c.driverTaken = static_cast<int>(list.size());
            calls.push_back(c);
        }
        // The driver's own routines first, then the game's in front of them, then those
        // writing the Z80's memory; those called from most places first.
        std::sort(calls.begin(), calls.end(), [&](const Candidate& a, const Candidate& b) {
            if (inDriver(a.address) != inDriver(b.address)) return inDriver(a.address);
            if (a.z80 != b.z80) return b.z80;
            return a.driverTaken > b.driverTaken;
        });
        // Those writing the Z80's memory only on the second boot, pressing Start: a driver of
        // the 68000's may show itself only then (NHL 96's), and the game's own routines in
        // front of it are better.
        std::vector<Candidate> kept;
        int own = 0, front = 0, z80 = 0;
        for (const Candidate& c : calls)
            if (c.z80 && !pressStart)
                heldBack = true;
            else if (inDriver(c.address) ? own++ < 4 : c.z80 ? z80++ < 6 : front++ < 4)
                kept.push_back(c);
        calls = std::move(kept);
    }
    // Other drivers (GEMS) take commands from a ring in the Z80's memory: the game writes a
    // message (as FF 10 n, start song n) a byte at a time after the last, and then where
    // the next will go, to a byte of its own. That byte: the one written right after other
    // bytes of the Z80's memory, each time with where the next goes in a ring they are all
    // in (the smallest that fits them).
    std::vector<Candidate> rings;
    {
        std::map<uint64_t, int> perFrame;
        for (const ZWrite& w : z80Writes) ++perFrame[w.frame];
        auto loaded = [&](const ZWrite& w) { return perFrame[w.frame] > 64; };  // a program, or data
        std::map<uint16_t, std::vector<std::pair<uint16_t, uint8_t>>> after;  // by position byte: byte written, its value
        for (size_t i = 1; i < z80Writes.size(); ++i) {
            const ZWrite &a = z80Writes[i - 1], &b = z80Writes[i];
            if (a.address != b.address && a.frame == b.frame && !loaded(b)) after[b.address].push_back({a.address, b.value});
        }
        for (const auto& [position, pairs] : after) {
            if (pairs.size() < 4) continue;
            Candidate c;
            for (uint32_t mask : {0x0Fu, 0x1Fu, 0x3Fu, 0x7Fu, 0xFFu}) {
                std::set<uint32_t> starts;
                for (const auto& [written, next] : pairs) starts.insert((written - ((next - 1u) & mask)) & 0x1FFF);
                if (starts.size() != 1) continue;
                c.ringStart = 0xA00000 | *starts.begin();
                c.ringMask = mask;
                break;
            }
            if (!c.ringMask) continue;
            c.address = 0xA00000 | position;
            c.ring = true;
            // The messages written to the ring, those of each frame split where the byte most of
            // them start with comes again.
            std::vector<std::vector<uint8_t>> messages;
            std::vector<uint64_t> when;
            std::map<uint8_t, int> firsts;
            uint64_t last = ~0ull;
            for (const ZWrite& w : z80Writes) {
                if (w.address < (c.ringStart & 0x1FFF) || w.address > (c.ringStart & 0x1FFF) + c.ringMask || loaded(w))
                    continue;
                if (w.frame != last) {
                    messages.emplace_back();
                    when.push_back(w.frame);
                    ++firsts[w.value];
                    last = w.frame;
                }
                messages.back().push_back(w.value);
            }
            if (messages.empty()) continue;
            const uint8_t lead = std::max_element(firsts.begin(), firsts.end(), [](const auto& a, const auto& b) {
                                     return a.second < b.second;
                                 })->first;
            // Each kind of message (all but its last byte) and the numbers it ends with.
            std::map<std::vector<uint8_t>, std::vector<std::pair<uint64_t, uint32_t>>> kinds;
            for (size_t m = 0; m < messages.size(); ++m) {
                std::vector<uint8_t> one;
                for (size_t i = 0; i <= messages[m].size(); ++i) {
                    if (i == messages[m].size() || (i > 0 && messages[m][i] == lead)) {
                        if (one.size() >= 2)
                            kinds[std::vector<uint8_t>(one.begin(), one.end() - 1)].push_back({when[m], one.back()});
                        one.clear();
                    }
                    if (i < messages[m].size()) one.push_back(messages[m][i]);
                }
            }
            // The kind ending in most different numbers (GEMS's start song; not, say, its
            // stop song).
            const std::vector<uint8_t>* most = nullptr;
            size_t mostNumbers = 1;
            for (const auto& [message, requests] : kinds) {
                std::set<uint32_t> numbers;
                for (const auto& r : requests) numbers.insert(r.second);
                if (numbers.size() > mostNumbers) {
                    most = &message;
                    mostNumbers = numbers.size();
                }
            }
            for (const auto& [message, requests] : kinds) {
                if (&message != most) continue;
                std::set<uint32_t> numbers;
                for (const auto& r : requests) numbers.insert(r.second);
                Candidate k = c;
                k.message = message;
                k.numbers.assign(numbers.begin(), numbers.end());
                k.requests = requests;
                if (k.requests.size() > 64) k.requests.resize(64);
                k.firstFrame = requests.front().first;
                k.driverTaken = static_cast<int>(requests.size());
                rings.push_back(k);
            }
        }
        std::sort(rings.begin(), rings.end(), [](const Candidate& a, const Candidate& b) { return a.numbers.size() > b.numbers.size(); });
        if (rings.size() > 4) rings.resize(4);
    }
    z80Writes.clear();
    z80Writes.shrink_to_fit();

    // Keeping the game's own requests out, and asking.
    auto lock = [&](const Candidate& c, uint32_t from, uint32_t to) {
        if (c.call) {
            md->cpu().skipRoutine = c.address;
            md->cpu().skipPops = c.pops;
        } else if (c.ring)
            md->lockSoundRequests(c.address, 1, true);  // where the next goes may be 0
        else
            md->lockSoundRequests(from, static_cast<int>(to - from));
    };
    auto ask = [&](const Candidate& c, uint32_t number) {  // after holdMain(), if any
        if (c.call) {
            uint32_t regs[15];
            std::copy(c.regs.begin(), c.regs.end(), regs);
            if (c.argBytes)
                md->cpu().callRoutine(c.address, regs, kKeep, number, c.argBytes);
            else {
                regs[c.numberReg] = number;
                md->cpu().callRoutine(c.address, regs, kKeep);
            }
        } else if (c.ring) {
            // After the last message, and then where the next goes.
            const uint32_t at = md->peek(c.address);
            std::vector<uint8_t> bytes(c.message);
            bytes.push_back(static_cast<uint8_t>(number));
            for (size_t i = 0; i < bytes.size(); ++i)
                md->poke(c.ringStart + ((at + i) & c.ringMask), bytes[i]);
            md->poke(c.address, static_cast<uint8_t>((at + bytes.size()) & c.ringMask));
        } else {
            md->poke(c.address, static_cast<uint8_t>(number));
        }
    };

    // The state at the start of frame `n` of the boot.
    auto goTo = [&](uint64_t n) {
        auto it = checkpoints.upper_bound(n);
        if (it == checkpoints.begin()) return false;
        --it;
        if (!md->loadState(it->second, nullptr)) return false;
        md->lockSoundRequests(0, 0);
        md->cpu().skipRoutine = 0;
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
    // A routine is asked where nothing plays with the game's calls kept out (a song asked
    // over another, a sound effect could pass for one): at some of those moments of the
    // boot, spread over them, the earliest left out (the driver may not be ready yet). Or
    // over the second half of the boot, if it is never quiet.
    for (Candidate& c : calls) {
        std::vector<uint64_t> quiet;
        for (const auto& [at, state] : checkpoints) {
            if (at % (4 * kCheckpointEvery) != 0 || !goTo(at)) continue;
            lock(c, 0, 0);
            if (count(listen(kCheckFrames)) == 0) quiet.push_back(at);
        }
        if (quiet.size() > 1) quiet.erase(quiet.begin());
        const size_t n = c.requests.size();
        for (size_t i = 0; i < n; ++i) {
            const uint64_t spread = static_cast<uint64_t>(kBootFrames / 2) + i * static_cast<uint64_t>(kBootFrames / 2 - kFps) / n;
            c.requests[i].first = quiet.empty() ? spread / kCheckpointEvery * kCheckpointEvery : quiet[(i + 1) * quiet.size() / (n + 1)];
        }
        c.firstFrame = c.requests.front().first;
    }

    // 2. Which of them make sound: from just before the request, once with the game's
    // request kept out and once asking again ourselves.
    // (Not the bytes of a block copied in one go, as a Z80 program loaded and then
    // cleared: many of them start in the same frame, and that is their only "request".)
    std::map<uint64_t, int> starting;
    for (const auto& [addr, c] : found) ++starting[c.firstFrame];
    std::vector<Candidate> list(calls);
    list.insert(list.end(), rings.begin(), rings.end());
    for (auto& [addr, c] : found)
        if (c.requests.size() > 1 || starting[c.firstFrame] < 16) list.push_back(c);
    // The driver's routines first, then those the driver took, then in the order the game
    // first used them.
    std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) {
        if (a.call != b.call) return a.call;
        if (a.ring != b.ring) return a.ring;
        if ((a.driverTaken > 0) != (b.driverTaken > 0)) return a.driverTaken > 0;
        return a.firstFrame != b.firstFrame ? a.firstFrame < b.firstFrame : a.address < b.address;
    });
    if (log_)
        for (const Candidate& c : list) {
            std::string r;
            for (size_t i = 0; i < c.requests.size() && i < 12; ++i)
                r += " " + hex(c.requests[i].second, 2) + "@" + std::to_string(c.requests[i].first);
            say("  " + hex(c.address, 6) + (c.call ? " (rutina, " + where(c) + ")" : "") +
                (c.ring ? " (anillo en " + hex(c.ringStart, 6) + ")" : "") +
                (c.driverTaken ? " (driver " + std::to_string(c.driverTaken) + ")" : "") + ":" + r);
        }
    if (list.size() > kMaxCandidates) list.resize(kMaxCandidates);
    std::vector<Candidate> queues;
    if (!report(tr("Buscando dónde pide el juego la música"))) return fail("");
    for (Candidate& c : list) {
        // Its first few different requests: the first may be a short sound. A byte the
        // driver takes that the game asks only one or two things of (a jingle, a fade) is
        // also asked for the numbers after its last one, where the game made it.
        std::set<uint32_t> tried;
        std::vector<std::pair<uint64_t, uint32_t>> tries;
        for (const auto& [when, number] : c.requests)
            if ((c.call || tried.size() < 4) && tried.insert(number).second) tries.push_back({when, number});
        const size_t own = tries.size();
        if (!c.call && !c.ring && c.driverTaken > 0 && tried.size() < 3)
            for (uint32_t step = 1; step <= 3; ++step) {
                const uint32_t number = (c.requests.back().second + step) & 0xFF;
                if (number && number != 0x80 && tried.insert(number).second)
                    tries.push_back({c.requests.back().first, number});
            }
        int ownSound = 0;  // what the game's own requests made
        for (size_t t = 0; t < tries.size(); ++t) {
            const auto [when, number] = tries[t];
            if (!goTo(when)) {
                say("No se puede volver al cuadro " + std::to_string(when));
                continue;
            }
            lock(c, c.address, c.address + 1);
            const int quiet = count(listen(kCheckFrames));
            goTo(when);
            lock(c, c.address, c.address + 1);
            ask(c, number);
            const int asked = count(listen(kCheckFrames));
            say("Candidato " + hex(c.address, 6) + ", petición " + hex(number, 2) + " en el cuadro " +
                std::to_string(when) + ": " + std::to_string(quiet) + " notas sin ella, " + std::to_string(asked) +
                " pidiéndola");
            if (asked - quiet > c.sound) {
                c.sound = asked - quiet;
                c.soundFrame = when;
            }
            if (t < own) ownSound = std::max(ownSound, asked - quiet);
        }
        c.probed = ownSound <= 4;
        if (c.sound > 4) queues.push_back(c);
    }
    md->lockSoundRequests(0, 0);
    md->cpu().skipRoutine = 0;
    if (queues.empty()) {
        *notFound = true;
        return fail(tr("No he encontrado cómo pide la música este juego. Por ahora sé sacarla de los juegos cuyo driver toma "
                       "las peticiones de un byte de la memoria (como el de Sonic, SMPS), de una llamada a una rutina suya "
                       "(como el de Streets of Rage 2 o el de EA) o de mensajes en la memoria del Z80 (GEMS)."));
    }

    // Where the driver takes the requests first (a ring of messages before a byte), then
    // where the game's own requests made music (not only the numbers after them), then
    // where asking made most music. A queue where no number plays a song (a variable of
    // the game that only looked like one) gives way to the next.
    std::stable_sort(queues.begin(), queues.end(), [](const Candidate& a, const Candidate& b) {
        if ((a.driverTaken > 0) != (b.driverTaken > 0)) return a.driverTaken > 0;
        if (a.ring != b.ring) return a.ring;
        if (a.probed != b.probed) return !a.probed;
        return a.sound > b.sound;
    });
    // The songs it finds asking at `chosen` (into `songs`), or -1 if stopped.
    // (adding: to the songs found already, behind another routine.)
    std::vector<Song> songs;
    std::set<uint64_t> heard;  // how each song found starts, to skip the same song twice
    auto tryQueue = [&](const Candidate& chosen, bool adding) -> int {
        if (!adding) {
            songs.clear();
            heard.clear();
            p.found = 0;
        }
        const size_t heardBefore = heard.size();
        // Ask where the request with most music went, from where the game made it (the
        // driver is ready there, and nothing else was playing), with every queue found kept
        // from the game.
        const Candidate& c = chosen;
        const uint64_t start = c.soundFrame;
        uint32_t lockFrom = c.address, lockTo = c.address + 1;
        if (!c.call && !c.ring)
            for (const Candidate& o : queues)
                // The same queue (where the game asks: not a byte the driver passes requests on
                // to, which it must still write).
                if (!o.call && !o.ring && o.driverTaken > 0 && o.address + 8 > c.address && o.address < c.address + 8) {
                    lockFrom = std::min(lockFrom, o.address);
                    lockTo = std::max(lockTo, o.address + 1);
                }
        // The numbers to ask for: for a queue, every byte (from 0x81 if the game only asks
        // for those); for a call, those of each kind the game uses (the high byte), from 0
        // to a little past the last.
        std::vector<uint32_t> numbers;
        if (c.call) {
            std::map<uint32_t, uint32_t> kinds;  // high byte, highest low byte
            for (uint32_t n : c.numbers) kinds[n >> 8] = std::max(kinds[n >> 8], n & 0xFF);
            for (const auto& [kind, top] : kinds)
                for (uint32_t low = kind ? 0 : 1; low <= std::min<uint32_t>(0xFF, top + 0x10); ++low)
                    numbers.push_back(kind << 8 | low);
        } else if (c.ring) {
            // Not the byte messages start with.
            const uint32_t top = std::min<uint32_t>(0xFE, c.numbers.back() + 0x20);
            for (uint32_t n = 0; n <= top; ++n)
                if (c.message.empty() || n != c.message.front()) numbers.push_back(n);
        } else {
            bool high = true;
            for (const auto& r : c.requests) high = high && r.second >= 0x81;
            for (uint32_t n = high ? 0x81 : 0x01; n <= 0xFF; ++n) numbers.push_back(n);
        }
        const int digits = numbers.back() > 0xFF ? 4 : 2;
        if (c.call)
            say("Pido las canciones llamando a " + hex(c.address, 6) + " con el número en " + where(c) + ", del " +
                hex(numbers.front(), digits) + " al " + hex(numbers.back(), digits) + ", desde el cuadro " +
                std::to_string(start) + "; las llamadas del juego no pasan");
        else if (c.ring) {
            std::string m;
            for (uint8_t b : c.message) m += hex(b, 2) + " ";
            say("Pido las canciones con el mensaje " + m + "n en el anillo de " + hex(c.ringStart, 6) + ", del " +
                hex(numbers.front(), 2) + " al " + hex(numbers.back(), 2) + ", desde el cuadro " + std::to_string(start) +
                "; los mensajes del juego no pasan");
        } else
            say("Pido las canciones en " + hex(c.address, 6) + ", del " + hex(numbers.front(), 2) + " al " +
                hex(numbers.back(), 2) + ", desde el cuadro " + std::to_string(start) + "; las peticiones del juego en " +
                hex(lockFrom, 6) + "-" + hex(lockTo - 1, 6) + " no pasan");
        // The game's own request there, to play over.
        uint32_t own = c.requests.front().second;
        for (const auto& r : c.requests)
            if (r.first == start) own = r.second;
        // Better with the game held where it is, only its VBlank running: it cannot go on to
        // load a level (the music would stop meanwhile) or ask for other sounds. Some games
        // need their program running to feed the driver.
        // (Held, asking must add notes to what was playing: some games hand the request on to
        // the driver from their program.)
        goTo(start);
        lock(c, lockFrom, lockTo);
        md->cpu().holdMain(true);
        const auto heldQuiet = listen(kCheckFrames);
        md->cpu().holdMain(false);
        goTo(start);
        lock(c, lockFrom, lockTo);
        md->cpu().holdMain(true);
        ask(c, own);
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
            ~Unplug() {
                md.setRoland(nullptr, 100);
                md.cpu().skipRoutine = 0;
            }
        } unplug{*md};
        auto instruments = [&](std::set<uint32_t>& into) {
            for (const auto& [fp, seen] : ear.seen())
                if (seen.kind == RolandSound::Kind::Fm) into.insert(fp);
        };
        lock(c, lockFrom, lockTo);
        md->cpu().holdMain(hold);
        ear.clearSeen();
        const auto before = listen(kCheckFrames);
        std::set<uint32_t> playing;  // and its instruments
        instruments(playing);

        // 3. Every number in turn.
        p.total = static_cast<int>(numbers.size());
        auto opening = [](const NoteTracker& t) {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < std::min<size_t>(t.notes.size(), 64); ++i) h = (h ^ t.notes[i].key) * 1099511628211ull;
            return h;
        };
        std::set<uint32_t> musicSounds, shortSounds;
        std::vector<uint32_t> silent;  // numbers that played nothing
        struct Effects {  // worked out however run() ends from here
            SongExtractor& x;
            std::set<uint32_t>& music;
            std::set<uint32_t>& sounds;
            ~Effects() {
                for (uint32_t fp : sounds)
                    if (!music.count(fp)) x.effects_.insert(fp);
            }
        } effects{*this, musicSounds, shortSounds};
        uint32_t firstSong = 0;  // the number of the first song found
        int repeated = 0;        // numbers that played it again
        for (size_t at = 0; at < numbers.size(); ++at) {
            const uint32_t number = numbers[at];
            p.tried = static_cast<int>(at);
            if (!report(trf("Probando el %d", p.tried + 1))) return -1;
            md->loadState(origin, nullptr);
            lock(c, lockFrom, lockTo);
            md->cpu().holdMain(hold);
            ask(c, number);
            ear.allNotesOff();
            ear.clearSeen();
            VgmWriter w;
            md->setVgmWriter(&w);
            std::vector<VgmWriter::Mark> marks;
            NoteTracker tracker;
            int lastNote = -1, firstNote = -1;
            int loopStart = -1, loopEnd = -1;
            bool same = false;  // it only adds a few notes to what was playing
            bool again = false;  // it starts as a song found already
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
                // A song found already starts the same: no need to hear it all again.
                if (had < 64 && tracker.notes.size() >= 64 && heard.count(opening(tracker))) {
                    again = true;
                    break;
                }
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
                say(hex(number, digits) + ": suena lo mismo que sin pedir nada");
                // A sound effect over the music: its instruments are the new ones.
                std::set<uint32_t> now;
                instruments(now);
                for (uint32_t fp : now)
                    if (!playing.count(fp)) shortSounds.insert(fp);
                continue;
            }
            // On one or two channels it is a sound effect, however long (an engine, an alarm
            // that goes round): songs play on more.
            std::set<uint32_t> channels;
            for (const auto& note : tracker.notes) channels.insert(note.key >> 24);
            if (again) {
                // (Cut short: the checks below need it all.)
            } else if (firstNote >= 0 && lastNote - firstNote >= kShortest && channels.size() < 3) {
                say(hex(number, digits) + ": un sonido largo");
                instruments(shortSounds);
                continue;
            } else if (firstNote < 0 || lastNote - firstNote < kShortest) {
                if (firstNote >= 0) say(hex(number, digits) + ": un sonido corto");
                if (firstNote >= 0) instruments(shortSounds);
                else silent.push_back(number);
                continue;
            }
            std::map<uint32_t, int> opened;
            for (const auto& note : tracker.notes)
                if (note.frame < kCheckFrames) ++opened[note.key];
            if (!again && extra(opened, before) < 8) {
                say(hex(number, digits) + ": suena lo mismo que sin pedir nada");
                continue;
            }
            // The same song as one found already (several numbers for one song)?
            instruments(musicSounds);
            if (again || !heard.insert(opening(tracker)).second) {
                say(hex(number, digits) + ": repetida");
                // Every number the same song: the driver does not take the number from here.
                if (heard.size() <= heardBefore + 1 && ++repeated >= 8) {
                    say("Todas suenan igual; aquí no se elige la canción");
                    break;
                }
                continue;
            }
            if (!firstSong) firstSong = number;
            // A routine's songs may go on past the numbers the game was seen to call it with:
            // a little past each one found.
            if (c.call && numbers.back() >> 8 == number >> 8)
                for (uint32_t n = numbers.back() + 1; n <= number + 0x10 && n >> 8 == number >> 8; ++n) numbers.push_back(n);
            p.total = static_cast<int>(numbers.size());
            Song song;
            song.number = static_cast<int>(number);
            VgmWriter::Tag tag;
            tag.title = trf("Canción %d", p.found + 1);
            tag.game = game;
            tag.notes = trf("Sacada de la ROM (petición %s)", hex(number, digits).c_str());
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
            say(hex(number, digits) + ": " + tag.title + ", " + std::to_string(static_cast<int>(song.seconds)) + " s" +
                (song.loopSeconds > 0 ? ", vuelta de " + std::to_string(static_cast<int>(song.loopSeconds)) + " s"
                                      : ", sin vuelta"));
            songs.push_back(std::move(song));
        }

        // 4. Some drivers play sound effects only over music (Sonic's, after the fade out
        // that comes before a level's song): the numbers that played nothing, once more with
        // the game's own song started. What they add to it are their instruments.
        p.tried = p.total;
        if (!silent.empty() && report(tr("Buscando los efectos de sonido"))) {
            md->loadState(origin, nullptr);
            lock(c, lockFrom, lockTo);
            md->cpu().holdMain(hold);
            ask(c, !c.call && !c.ring ? own : firstSong ? firstSong : own);
            for (int f = 0; f < kFps; ++f) frame();
            std::vector<uint8_t> playing;
            md->saveState(playing, true);
            auto over = [&](uint32_t number) {
                md->loadState(playing, nullptr);
                lock(c, lockFrom, lockTo);
                md->cpu().holdMain(hold);
                if (number) ask(c, number);
                ear.allNotesOff();
                ear.clearSeen();
                for (int f = 0; f < kCheckFrames; ++f) frame();
                std::set<uint32_t> heard;
                instruments(heard);
                return heard;
            };
            const std::set<uint32_t> alone = over(0);
            for (uint32_t number : silent) {
                if (!report(tr("Buscando los efectos de sonido"))) break;
                for (uint32_t fp : over(number))
                    if (!alone.count(fp)) shortSounds.insert(fp);
            }
        }
        return p.found;
    };
    // The songs behind each routine add up (NHL 96 has two, each with songs of its own).
    std::vector<Song> best;
    std::set<uint32_t> bestEffects;
    bool stopped = false;  // what it found so far is kept
    songs.clear();
    heard.clear();
    p.found = 0;
    effects_.clear();
    int routines = 0;
    for (const Candidate& c : queues)
        if (c.call && !c.z80 && !stopped && routines++ < 4) stopped = tryQueue(c, true) < 0;
    best = songs;
    bestEffects = effects_;
    // Then the other ways of asking, until one plays a few songs (a byte that only looked
    // like the queue may play the odd one, a number that lands on a song by chance); else
    // the one with most.
    int others = 0;
    for (size_t q = 0; !stopped && q < queues.size() && others < 3 && best.size() < 4; ++q) {
        if (queues[q].call) continue;
        ++others;
        effects_.clear();
        stopped = tryQueue(queues[q], false) < 0;
        if (songs.size() > best.size()) {
            best = std::move(songs);
            bestEffects = effects_;
        }
    }
    // Last, the routines that write the Z80's memory (Ecco's), adding up as the others.
    if (!stopped && best.size() < 4) {
        songs.clear();
        heard.clear();
        p.found = 0;
        effects_.clear();
        routines = 0;
        for (const Candidate& c : queues)
            if (c.z80 && !stopped && routines++ < 4) stopped = tryQueue(c, true) < 0;
        if (songs.size() > best.size()) {
            best = std::move(songs);
            bestEffects = effects_;
        }
    }
    effects_ = bestEffects;
    p.found = static_cast<int>(best.size());
    for (const Song& song : best)
        if (onSong) onSong(song);
    if (!report(tr("Hecho"))) return fail("");
    if (best.empty() && heldBack) *notFound = true;
    if (best.empty()) return fail(tr("El juego responde a las peticiones, pero ninguna ha sonado como una canción."));
    return true;
}

}  // namespace plumsg
