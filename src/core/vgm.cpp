#include "vgm.h"

#include "i18n.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "miniz.h"
#include "roland.h"

namespace plumsg {

namespace {

constexpr double kTickRate = 44100.0;

uint32_t get32(const std::vector<uint8_t>& d, size_t at) {
    if (at + 4 > d.size()) return 0;
    return static_cast<uint32_t>(d[at]) | static_cast<uint32_t>(d[at + 1]) << 8 | static_cast<uint32_t>(d[at + 2]) << 16 |
           static_cast<uint32_t>(d[at + 3]) << 24;
}

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

void append32(std::vector<uint8_t>& d, uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// GD3 tags are UTF-16; plumsg's strings are UTF-8.
void appendUtf16(std::vector<uint8_t>& d, const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
        ++i;
        for (; more > 0 && i < s.size(); --more, ++i) c = c << 6 | (static_cast<uint8_t>(s[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            const uint32_t hi = 0xD800 + (c >> 10), lo = 0xDC00 + (c & 0x3FF);
            d.push_back(static_cast<uint8_t>(hi));
            d.push_back(static_cast<uint8_t>(hi >> 8));
            d.push_back(static_cast<uint8_t>(lo));
            d.push_back(static_cast<uint8_t>(lo >> 8));
        } else {
            d.push_back(static_cast<uint8_t>(c));
            d.push_back(static_cast<uint8_t>(c >> 8));
        }
    }
    d.push_back(0);
    d.push_back(0);
}

// Reads one UTF-16 string at `at`, moving past its terminator.
std::string readUtf16(const std::vector<uint8_t>& d, size_t& at, size_t end) {
    std::string out;
    while (at + 1 < end) {
        uint32_t c = d[at] | d[at + 1] << 8;
        at += 2;
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && at + 1 < end) {
            const uint32_t lo = d[at] | d[at + 1] << 8;
            at += 2;
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | c >> 12);
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | c >> 18);
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::vector<uint8_t> gzip(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
    size_t size = 0;
    void* deflated = tdefl_compress_mem_to_heap(in.data(), in.size(), &size, TDEFL_DEFAULT_MAX_PROBES);
    if (!deflated) return in;
    const uint8_t* p = static_cast<const uint8_t*>(deflated);
    out.insert(out.end(), p, p + size);
    mz_free(deflated);
    append32(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, in.data(), in.size())));
    append32(out, static_cast<uint32_t>(in.size()));
    return out;
}

bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
    const uint8_t flags = in[3];
    size_t at = 10;
    if (flags & 4) at += 2 + static_cast<size_t>(in[at] | in[at + 1] << 8);  // extra field
    for (int f : {8, 16})                                                     // name, comment
        if (flags & f) {
            while (at < in.size() && in[at]) ++at;
            ++at;
        }
    if (flags & 2) at += 2;  // header CRC
    if (at >= in.size()) return false;
    size_t size = 0;
    void* p = tinfl_decompress_mem_to_heap(in.data() + at, in.size() - at, &size, 0);
    if (!p) return false;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out->assign(b, b + size);
    mz_free(p);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------
// Recording

void VgmWriter::begin(const Ym2612& ym, const Psg& psg, uint64_t clock, double masterClock) {
    recording_ = true;
    data_.clear();
    start_ = clock;
    samples_ = 0;
    master_ = masterClock;
    psgClock_ = psg.clock();
    address_ = {};
    // The chips as they are: the PSG's tones and volumes, then every YM2612 register
    // that shapes a sound (the frequencies' high bytes first: they are latched).
    for (int ch = 0; ch < 3; ++ch) {
        const uint16_t period = psg.period(ch);
        data_.insert(data_.end(), {0x50, static_cast<uint8_t>(0x80 | ch << 5 | (period & 0x0F)), 0x50,
                                   static_cast<uint8_t>((period >> 4) & 0x3F)});
    }
    data_.insert(data_.end(), {0x50, static_cast<uint8_t>(0xE0 | (psg.period(3) & 7))});
    for (int ch = 0; ch < 4; ++ch)
        data_.insert(data_.end(), {0x50, static_cast<uint8_t>(0x90 | ch << 5 | (psg.attenuation(ch) & 0x0F))});
    ymWrite(0, 0x22, ym.reg(0x22));
    ymWrite(0, 0x27, ym.reg(0x27) & 0xC0);  // channel 3 mode, not the timers
    ymWrite(0, 0x2B, ym.reg(0x2B));
    for (int port = 0; port < 2; ++port) {
        for (int reg = 0x30; reg < 0xB8; ++reg) {
            if ((reg & 3) == 3 || (reg >= 0xA0 && reg < 0xB0)) continue;
            ymWrite(port, static_cast<uint8_t>(reg), ym.reg(port * 0x100 + reg));
        }
        for (int ch = 0; ch < 3; ++ch) {
            ymWrite(port, static_cast<uint8_t>(0xA4 + ch), ym.reg(port * 0x100 + 0xA4 + ch));
            ymWrite(port, static_cast<uint8_t>(0xA0 + ch), ym.reg(port * 0x100 + 0xA0 + ch));
        }
    }
    for (int ch = 0; ch < 3; ++ch) {  // channel 3's own operator frequencies
        ymWrite(0, static_cast<uint8_t>(0xAC + ch), ym.reg(0xAC + ch));
        ymWrite(0, static_cast<uint8_t>(0xA8 + ch), ym.reg(0xA8 + ch));
    }
    // The notes held now.
    for (int ch = 0; ch < 6; ++ch)
        if (ym.keys(ch)) ymWrite(0, 0x28, static_cast<uint8_t>(ym.keys(ch) | (ch % 3) | (ch >= 3 ? 4 : 0)));
}

double VgmWriter::secondsAt(uint64_t clock) const {
    return recording_ && clock > start_ ? static_cast<double>(clock - start_) / master_ : seconds();
}

void VgmWriter::ymWrite(int port, uint8_t reg, uint8_t value) {
    data_.insert(data_.end(), {static_cast<uint8_t>(port ? 0x53 : 0x52), reg, value});
}

void VgmWriter::wait(uint64_t clock) {
    if (clock < start_) return;  // rewound: carry on from where the file is
    const auto target = static_cast<uint64_t>(static_cast<double>(clock - start_) * kTickRate / master_);
    while (target > samples_) {
        const uint64_t n = std::min<uint64_t>(target - samples_, 65535);
        if (n <= 16) {
            data_.push_back(static_cast<uint8_t>(0x70 + n - 1));
        } else if (n == 735 || n == 882) {
            data_.push_back(n == 735 ? 0x62 : 0x63);
        } else {
            data_.insert(data_.end(), {0x61, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)});
        }
        samples_ += n;
    }
}

void VgmWriter::fm(int offset, uint8_t value, uint64_t clock) {
    if (!recording_) return;
    const int port = (offset >> 1) & 1;
    if (!(offset & 1)) {
        address_[static_cast<size_t>(port)] = value;
        return;
    }
    wait(clock);
    ymWrite(port, address_[static_cast<size_t>(port)], value);
}

void VgmWriter::psg(uint8_t value, uint64_t clock) {
    if (!recording_) return;
    wait(clock);
    data_.insert(data_.end(), {0x50, value});
}

void VgmWriter::fmReset(uint64_t clock) {
    if (!recording_) return;
    wait(clock);
    for (uint8_t ch : {0, 1, 2, 4, 5, 6}) ymWrite(0, 0x28, ch);
}

std::vector<uint8_t> VgmWriter::finish(uint64_t clock, const std::string& game, bool compress) {
    Tag tag;
    tag.game = game;
    return finish(clock, tag, compress, nullptr, nullptr);
}

VgmWriter::Mark VgmWriter::mark(uint64_t clock) {
    wait(clock);
    return {data_.size(), samples_};
}

std::vector<uint8_t> VgmWriter::finish(uint64_t clock, const Tag& tag, bool compress, const Mark* end,
                                       const Mark* loop) {
    if (end && end->bytes <= data_.size()) {
        data_.resize(end->bytes);
        samples_ = end->samples;
    } else {
        wait(clock);
    }
    if (loop && (loop->bytes > data_.size() || loop->samples >= samples_)) loop = nullptr;
    recording_ = false;
    std::vector<uint8_t> file(0x40, 0);
    std::memcpy(file.data(), "Vgm ", 4);
    put32(file, 0x08, 0x150);
    put32(file, 0x0C, static_cast<uint32_t>(std::lround(psgClock_)));
    put32(file, 0x18, static_cast<uint32_t>(samples_));
    put32(file, 0x24, master_ < 53500000 ? 50 : 60);
    file[0x28] = 0x09;  // the Sega PSG's noise feedback and shift register width
    file[0x2A] = 16;
    put32(file, 0x2C, static_cast<uint32_t>(std::lround(master_ / 7)));
    put32(file, 0x34, 0x40 - 0x34);
    if (loop) {
        put32(file, 0x1C, static_cast<uint32_t>(0x40 + loop->bytes - 0x1C));
        put32(file, 0x20, static_cast<uint32_t>(samples_ - loop->samples));
    }
    file.insert(file.end(), data_.begin(), data_.end());
    file.push_back(0x66);
    data_.clear();
    // The tag: title, game and system (each in English and Japanese), author, date,
    // who made the file, notes.
    put32(file, 0x14, static_cast<uint32_t>(file.size() - 0x14));
    std::vector<uint8_t> gd3;
    for (const std::string& s : {tag.title, std::string(), tag.game, std::string(), std::string("Sega Mega Drive / Genesis"),
                                 std::string(), std::string(), std::string(), std::string(), std::string("plumsg"),
                                 tag.notes})
        appendUtf16(gd3, s);
    file.insert(file.end(), {'G', 'd', '3', ' '});
    append32(file, 0x100);
    append32(file, static_cast<uint32_t>(gd3.size()));
    file.insert(file.end(), gd3.begin(), gd3.end());
    put32(file, 0x04, static_cast<uint32_t>(file.size() - 4));
    return compress ? gzip(file) : file;
}

// ---------------------------------------------------------------------------------
// Reading

bool VgmFile::parse(std::vector<uint8_t> bytes, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::vector<uint8_t> d;
    if (!gunzip(bytes, &d)) d = std::move(bytes);
    if (d.size() < 0x40 || std::memcmp(d.data(), "Vgm ", 4) != 0) return fail(tr("No es un archivo VGM."));
    const uint32_t version = get32(d, 0x08);
    psgClock = get32(d, 0x0C) & 0x3FFFFFFF;
    ymClock = version >= 0x110 ? get32(d, 0x2C) & 0x3FFFFFFF : get32(d, 0x10);
    if (ymClock == 0 && psgClock == 0) return fail(tr("El VGM no es de Mega Drive: no tiene YM2612 ni PSG."));
    start = version >= 0x150 && get32(d, 0x34) ? 0x34 + get32(d, 0x34) : 0x40;
    if (start >= d.size()) return fail(tr("El VGM está cortado."));
    samples = get32(d, 0x18);
    loop = get32(d, 0x1C) ? 0x1C + get32(d, 0x1C) : 0;
    loopSamples = loop ? get32(d, 0x20) : 0;
    if (loop >= d.size() || loopSamples > samples) loop = 0, loopSamples = 0;
    title.clear();
    game.clear();
    if (const uint32_t g = get32(d, 0x14); g && 0x14 + g + 12 <= d.size() && std::memcmp(&d[0x14 + g], "Gd3 ", 4) == 0) {
        size_t at = 0x14 + g + 12;
        const size_t end = std::min(d.size(), at + get32(d, 0x14 + g + 8));
        title = readUtf16(d, at, end);
        readUtf16(d, at, end);
        game = readUtf16(d, at, end);
    }
    data = std::move(d);
    return true;
}

// ---------------------------------------------------------------------------------
// Playing

VgmPlayer::VgmPlayer() { reset(); }

void VgmPlayer::load(VgmFile file) {
    file_ = std::move(file);
    reset();
}

void VgmPlayer::reset() {
    master_ = file_.ymClock > 0 ? file_.ymClock * 7 : file_.psgClock > 0 ? file_.psgClock * 15 : 53693175.0;
    ym_.reset();
    ym_.samples().clear();
    ym_.setMasterClock(master_);
    ym_.setClockPosition(0);
    psg_.reset();
    psg_.setClock(file_.psgClock > 0 ? file_.psgClock : Psg::kClockNtsc);
    psg_.setStereo(0xFF);
    mixer_.reset();
    mixer_.setFmRate(ym_.sampleRate());
    pc_ = file_.start;
    position_ = ticks_ = waitEnd_ = clock_ = 0;
    psgDone_ = 0;
    ended_ = file_.data.empty();
    pcm_.clear();
    blocks_.clear();
    pcmPos_ = 0;
    streams_ = {};
    if (roland_) roland_->allNotesOff();
    applyMasks();
}

void VgmPlayer::setRolandOutput(bool on) {
    if (on == rolandOutput_) return;
    rolandOutput_ = on;
    if (roland_ && !on) roland_->skipFrame(clock_);
    applyMasks();
}

void VgmPlayer::setListening(uint32_t mutedVoices, std::set<uint32_t> silenced) {
    mutedVoices_ = mutedVoices;
    silenced_ = silenced;
    if (roland_) roland_->setListening(mutedVoices & 0x3FF, std::move(silenced));
    applyMasks();
}

// Which chip channels are heard: in the Roland sound only the instruments left
// original; in the chips' own, all but the ones muted or silenced.
void VgmPlayer::applyMasks() {
    uint32_t quiet = mutedVoices_ & 0x3FF;
    if (roland_) {
        if (rolandOutput_) {
            quiet |= 0x3FF & ~roland_->chipChannels();
        } else {
            for (int v = 0; v < RolandSound::kVoices; ++v)
                if (silenced_.count(roland_->voiceInstrument(v))) quiet |= 1u << v;
        }
    }
    ym_.setMuted((quiet & 0x3F) | ((mutedVoices_ & kDacVoice) ? 0x40 : 0), 0);
    psg_.setMuted(quiet >> 6, 0);
}

void VgmPlayer::restart() { reset(); }

void VgmPlayer::seek(double seconds) {
    reset();
    runTo(clockAt(std::max(0.0, seconds) * kTickRate), true);
    if (roland_) roland_->allNotesOff();
}

void VgmPlayer::syncChips(uint64_t clock, bool silent) {
    if (clock <= clock_) return;
    if (silent) {
        ym_.setClockPosition(clock);
    } else {
        ym_.runUntil(clock);
        const double cycles = static_cast<double>(clock) * psg_.clock() / master_ - psgDone_;
        const int whole = static_cast<int>(cycles);
        psg_.run(whole);
        psgDone_ += whole;
    }
    if (silent) psgDone_ = static_cast<double>(clock) * psg_.clock() / master_;
    clock_ = clock;
}

void VgmPlayer::fmWrite(int port, uint8_t reg, uint8_t value, uint64_t clock, bool silent) {
    syncChips(clock, silent);
    ym_.write(port * 2, reg);
    ym_.write(port * 2 + 1, value);
    if (roland_) {
        roland_->fmWrite(port * 2, reg, ym_, clock);
        roland_->fmWrite(port * 2 + 1, value, ym_, clock);
        if (!silent) applyMasks();
    }
}

void VgmPlayer::psgWrite(uint8_t value, uint64_t clock, bool silent) {
    syncChips(clock, silent);
    psg_.write(value);
    if (roland_) {
        roland_->psgWrite(psg_, clock);
        if (!silent) applyMasks();
    }
}

void VgmPlayer::startStream(Stream& s, uint32_t offset, int mode, uint32_t length) {
    if (offset != 0xFFFFFFFF) s.begin = offset;
    s.pos = s.begin;
    s.loop = (mode & 0x80) != 0;
    switch (mode & 0x0F) {
        case 1: s.end = s.begin + length * static_cast<uint32_t>(s.step); break;                          // commands
        case 2: s.end = s.begin + static_cast<uint32_t>(length / 1000.0 * s.frequency) * static_cast<uint32_t>(s.step); break;  // ms
        default: s.end = static_cast<uint32_t>(pcm_.size()); break;  // to the end of the data
    }
    s.end = std::min<uint32_t>(s.end, static_cast<uint32_t>(pcm_.size()));
    s.active = s.frequency > 0 && s.pos < s.end;
    s.next = static_cast<double>(ticks_);
}

bool VgmPlayer::step(uint64_t clock, bool silent) {
    const std::vector<uint8_t>& d = file_.data;
    auto at = [&](uint32_t i) -> uint8_t { return i < d.size() ? d[i] : 0x66; };
    const uint8_t cmd = at(pc_);
    auto wait = [&](uint32_t n, uint32_t length) {
        waitEnd_ = ticks_ + n;
        pc_ += length;
    };
    switch (cmd) {
        case 0x4F: psg_.setStereo(at(pc_ + 1)); pc_ += 2; return true;
        case 0x50: psgWrite(at(pc_ + 1), clock, silent); pc_ += 2; return true;
        case 0x52:
        case 0x53: fmWrite(cmd & 1, at(pc_ + 1), at(pc_ + 2), clock, silent); pc_ += 3; return true;
        case 0x61: wait(at(pc_ + 1) | at(pc_ + 2) << 8, 3); return true;
        case 0x62: wait(735, 1); return true;
        case 0x63: wait(882, 1); return true;
        case 0x66:
            if (loop_ && file_.loop) {
                pc_ = file_.loop;
                position_ = file_.samples - file_.loopSamples;
                return true;
            }
            ended_ = true;
            return false;
        case 0x67: {  // a data block: 0x67 0x66 type size data
            const uint8_t type = at(pc_ + 2);
            const uint32_t size = get32(d, pc_ + 3) & 0x7FFFFFFF;
            if (type == 0x00 && pc_ + 7 + size <= d.size()) {
                blocks_.push_back(static_cast<uint32_t>(pcm_.size()));
                pcm_.insert(pcm_.end(), d.begin() + pc_ + 7, d.begin() + pc_ + 7 + size);
            }
            pc_ += 7 + size;
            return true;
        }
        case 0xE0: pcmPos_ = get32(d, pc_ + 1); pc_ += 5; return true;
        case 0x90: {  // stream setup: the chip and register it writes to
            Stream& s = streams_[at(pc_ + 1) & 7];
            s.port = at(pc_ + 3) & 1;
            s.reg = at(pc_ + 4);
            pc_ += 5;
            return true;
        }
        case 0x91: {  // stream data: the bank (only the YM2612's, 0) and step size
            Stream& s = streams_[at(pc_ + 1) & 7];
            s.step = std::max<int>(1, at(pc_ + 3));
            pc_ += 5;
            return true;
        }
        case 0x92: streams_[at(pc_ + 1) & 7].frequency = get32(d, pc_ + 2); pc_ += 6; return true;
        case 0x93: {
            Stream& s = streams_[at(pc_ + 1) & 7];
            startStream(s, get32(d, pc_ + 2), at(pc_ + 6), get32(d, pc_ + 7));
            pc_ += 11;
            return true;
        }
        case 0x94: {
            const uint8_t id = at(pc_ + 1);
            for (size_t i = 0; i < streams_.size(); ++i)
                if (id == 0xFF || (id & 7) == i) streams_[i].active = false;
            pc_ += 2;
            return true;
        }
        case 0x95: {  // start stream from block number
            Stream& s = streams_[at(pc_ + 1) & 7];
            const uint32_t block = at(pc_ + 2) | at(pc_ + 3) << 8;
            if (block < blocks_.size()) {
                const uint32_t end = block + 1 < blocks_.size() ? blocks_[block + 1] : static_cast<uint32_t>(pcm_.size());
                s.step = 1;
                startStream(s, blocks_[block], 3 | ((at(pc_ + 4) & 1) ? 0x80 : 0), 0);
                s.end = end;
            }
            pc_ += 5;
            return true;
        }
        default: break;
    }
    if (cmd >= 0x70 && cmd <= 0x7F) {
        wait(cmd - 0x6F, 1);
        return true;
    }
    if (cmd >= 0x80 && cmd <= 0x8F) {  // a DAC sample from the data, then a wait
        if (pcmPos_ < pcm_.size()) fmWrite(0, 0x2A, pcm_[pcmPos_++], clock, silent);
        wait(cmd & 0x0F, 1);
        return true;
    }
    // Commands for other chips: skipped by their length.
    uint32_t length = 0;
    if (cmd >= 0x30 && cmd <= 0x3F) length = 2;
    else if (cmd >= 0x40 && cmd <= 0x4E) length = 3;
    else if (cmd >= 0x51 && cmd <= 0x5F) length = 3;
    else if (cmd == 0x64) length = 4;
    else if (cmd == 0x68) length = 12;
    else if (cmd >= 0xA0 && cmd <= 0xBF) length = 3;
    else if (cmd >= 0xC0 && cmd <= 0xDF) length = 4;
    else if (cmd >= 0xE1) length = 5;
    if (!length) {  // unknown: the rest cannot be read
        ended_ = true;
        return false;
    }
    pc_ += length;
    return true;
}

void VgmPlayer::runTo(uint64_t target, bool silent) {
    while (!ended_) {
        // The earliest DAC stream write due.
        Stream* due = nullptr;
        for (Stream& s : streams_)
            if (s.active && (!due || s.next < due->next)) due = &s;
        if (waitEnd_ > ticks_) {
            if (due && due->next < static_cast<double>(waitEnd_)) {
                const uint64_t when = clockAt(due->next);
                if (when > target) break;
                fmWrite(due->port, static_cast<uint8_t>(due->reg), pcm_[due->pos], when, silent);
                due->pos += static_cast<uint32_t>(due->step);
                due->next += kTickRate / due->frequency;
                if (due->pos >= due->end) {
                    if (due->loop) due->pos = due->begin;
                    else due->active = false;
                }
                continue;
            }
            if (clockAt(static_cast<double>(waitEnd_)) > target) break;
            position_ += waitEnd_ - ticks_;
            ticks_ = waitEnd_;
            continue;
        }
        const uint64_t now = clockAt(static_cast<double>(ticks_));
        if (now > target) break;
        if (!step(now, silent)) break;
    }
    syncChips(target, silent);
}

void VgmPlayer::play(double seconds, double rate, std::vector<int16_t>& out) {
    if (!loaded()) return;
    psg_.setSampleRate(rate);
    mixer_.setOutputRate(rate);
    const uint64_t target = clock_ + static_cast<uint64_t>(seconds * master_);
    if (roland_) roland_->beginFrame(clock_);
    runTo(target, false);
    psgScratch_.insert(psgScratch_.end(), psg_.samples().begin(), psg_.samples().end());
    psg_.clearSamples();
    mixer_.mix(ym_.samples(), psgScratch_);
    const size_t from = out.size() / 2;
    out.insert(out.end(), mixer_.samples().begin(), mixer_.samples().end());
    mixer_.clearSamples();
    if (!roland_) return;
    if (rolandOutput_) roland_->endFrame(target, out, from, rate);
    else roland_->skipFrame(target);
}

void VgmPlayer::analyse() {
    const bool loop = loop_;
    loop_ = false;
    reset();
    runTo(UINT64_MAX / 2, true);
    loop_ = loop;
    reset();
}

}  // namespace plumsg
