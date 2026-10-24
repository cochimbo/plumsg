#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace plumsg {

// A piece of music in a file (.wav, .mp3 or .ogg), decoded to 16-bit stereo on a thread
// of its own so that starting one does not hold up the frame: until a sample has been
// decoded it reads as silence.
class MusicTrack {
public:
    // nullptr when the file can't be read or decoded.
    static std::unique_ptr<MusicTrack> open(const std::filesystem::path& path);
    ~MusicTrack();
    MusicTrack(const MusicTrack&) = delete;
    MusicTrack& operator=(const MusicTrack&) = delete;

    int rate() const { return rate_; }
    size_t frames() const { return pcm_.size() / 2; }
    // Frame `i` (left, right); silence if it is not decoded yet.
    void frame(size_t i, int& l, int& r) const {
        if (i < ready_.load(std::memory_order_acquire)) {
            l = pcm_[i * 2];
            r = pcm_[i * 2 + 1];
        } else {
            l = r = 0;
        }
    }

private:
    MusicTrack() = default;
    void decode();

    struct Decoder;  // the .mp3 or .ogg decoder, opened to learn the length
    std::vector<uint8_t> file_;
    std::shared_ptr<Decoder> decoder_;
    int rate_ = 0, channels_ = 0;
    // .wav: where the samples start, their format.
    size_t dataAt_ = 0;
    int bits_ = 16;
    bool isFloat_ = false;
    std::vector<int16_t> pcm_;  // all of it, sized up front: the thread only fills it
    std::atomic<size_t> ready_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace plumsg
