#include "music_track.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#define MINIMP3_NO_STDIO
#include "minimp3/minimp3_ex.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#include "stb/stb_vorbis.c"

namespace plumsg {

struct MusicTrack::Decoder {
    mp3dec_ex_t* mp3 = nullptr;
    stb_vorbis* ogg = nullptr;
    ~Decoder() {
        if (mp3) {
            mp3dec_ex_close(mp3);
            delete mp3;
        }
        if (ogg) stb_vorbis_close(ogg);
    }
};

namespace {

uint32_t le(const std::vector<uint8_t>& d, size_t at, int bytes) {
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = v << 8 | d[at + i];
    return v;
}

}  // namespace

std::unique_ptr<MusicTrack> MusicTrack::open(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::unique_ptr<MusicTrack> t(new MusicTrack);
    t->file_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const std::vector<uint8_t>& d = t->file_;
    size_t frames = 0;

    if (d.size() >= 12 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "WAVE", 4) == 0) {
        // The chunks: "fmt " says what the samples are, "data" holds them.
        int format = 0, blockAlign = 0;
        size_t dataSize = 0;
        for (size_t at = 12; at + 8 <= d.size();) {
            const size_t size = le(d, at + 4, 4);
            const size_t body = at + 8;
            if (std::memcmp(&d[at], "fmt ", 4) == 0 && body + 16 <= d.size()) {
                format = static_cast<int>(le(d, body, 2));
                t->channels_ = static_cast<int>(le(d, body + 2, 2));
                t->rate_ = static_cast<int>(le(d, body + 4, 4));
                blockAlign = static_cast<int>(le(d, body + 12, 2));
                t->bits_ = static_cast<int>(le(d, body + 14, 2));
                if (format == 0xFFFE && size >= 26 && body + 26 <= d.size()) format = static_cast<int>(le(d, body + 24, 2));
            } else if (std::memcmp(&d[at], "data", 4) == 0) {
                t->dataAt_ = body;
                dataSize = std::min(size, d.size() - body);
                break;
            }
            at = body + size + (size & 1);
        }
        t->isFloat_ = format == 3;
        const bool ok = (format == 1 && (t->bits_ == 8 || t->bits_ == 16 || t->bits_ == 24 || t->bits_ == 32)) ||
                        (format == 3 && t->bits_ == 32);
        if (!ok || t->dataAt_ == 0 || t->channels_ < 1 || blockAlign < t->channels_ * t->bits_ / 8) return nullptr;
        frames = dataSize / static_cast<size_t>(blockAlign);
        t->bits_ = blockAlign / t->channels_ * 8;  // the size each sample takes
    } else if (d.size() >= 4 && std::memcmp(d.data(), "OggS", 4) == 0) {
        int error = 0;
        stb_vorbis* ogg = stb_vorbis_open_memory(d.data(), static_cast<int>(d.size()), &error, nullptr);
        if (!ogg) return nullptr;
        t->decoder_ = std::make_shared<Decoder>();
        t->decoder_->ogg = ogg;
        const stb_vorbis_info info = stb_vorbis_get_info(ogg);
        t->rate_ = static_cast<int>(info.sample_rate);
        t->channels_ = info.channels;
        frames = stb_vorbis_stream_length_in_samples(ogg);
    } else {
        auto* mp3 = new mp3dec_ex_t{};
        if (mp3dec_ex_open_buf(mp3, d.data(), d.size(), MP3D_SEEK_TO_SAMPLE) != 0 || mp3->info.channels < 1) {
            mp3dec_ex_close(mp3);
            delete mp3;
            return nullptr;
        }
        t->decoder_ = std::make_shared<Decoder>();
        t->decoder_->mp3 = mp3;
        t->rate_ = mp3->info.hz;
        t->channels_ = mp3->info.channels;
        frames = static_cast<size_t>(mp3->samples) / static_cast<size_t>(t->channels_);
    }
    if (t->rate_ <= 0 || frames == 0) return nullptr;
    t->pcm_.assign(frames * 2, 0);
    t->worker_ = std::thread([p = t.get()] { p->decode(); });
    return t;
}

MusicTrack::~MusicTrack() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

void MusicTrack::decode() {
    const size_t frames = pcm_.size() / 2;
    constexpr size_t kChunk = 4096;
    std::vector<int16_t> buf(kChunk * static_cast<size_t>(std::max(channels_, 2)));
    size_t done = 0;
    while (done < frames && !stop_) {
        const size_t want = std::min(kChunk, frames - done);
        size_t got = 0;
        if (decoder_ && decoder_->ogg) {
            got = static_cast<size_t>(stb_vorbis_get_samples_short_interleaved(
                decoder_->ogg, 2, buf.data(), static_cast<int>(want * 2)));
            std::copy_n(buf.begin(), got * 2, pcm_.begin() + static_cast<std::ptrdiff_t>(done * 2));
        } else {
            if (decoder_ && decoder_->mp3) {
                got = mp3dec_ex_read(decoder_->mp3, buf.data(), want * static_cast<size_t>(channels_)) /
                      static_cast<size_t>(channels_);
            } else {
                // .wav: whatever the format, to 16 bits.
                const size_t bytes = static_cast<size_t>(bits_ / 8);
                got = want;
                for (size_t i = 0; i < want * static_cast<size_t>(channels_); ++i) {
                    const size_t at = dataAt_ + (done * static_cast<size_t>(channels_) + i) * bytes;
                    int s;
                    if (isFloat_) {
                        float f;
                        const uint32_t u = le(file_, at, 4);
                        std::memcpy(&f, &u, 4);
                        s = static_cast<int>(std::clamp(f, -1.0f, 1.0f) * 32767.0f);
                    } else if (bytes == 1) {
                        s = (file_[at] - 128) << 8;
                    } else {
                        s = static_cast<int16_t>(le(file_, at + bytes - 2, 2));
                    }
                    buf[i] = static_cast<int16_t>(s);
                }
            }
            for (size_t i = 0; i < got; ++i) {
                const int16_t* f = &buf[i * static_cast<size_t>(channels_)];
                pcm_[(done + i) * 2] = f[0];
                pcm_[(done + i) * 2 + 1] = channels_ > 1 ? f[1] : f[0];
            }
        }
        if (got == 0) break;
        done += got;
        ready_.store(done, std::memory_order_release);
    }
    // The decoders and the file are no longer needed.
    decoder_.reset();
    std::vector<uint8_t>().swap(file_);
}

}  // namespace plumsg
