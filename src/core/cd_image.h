#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace plumsg {

// A Mega CD disc image: a .cue sheet with its .bin (or .wav) files, a lone .iso or
// .bin, or a compressed .chd. It is read one raw 2352-byte sector at a time, whatever
// the image stores: a data sector stored as 2048 bytes gets its sync pattern and
// header made up, and audio comes out as little-endian 16-bit stereo.
//
// Sectors are numbered by LBA, as the drive does: 0 is where track 1's data starts
// (00:02:00 on the disc), and the 150 sectors before it are track 1's pregap.
class CdImage {
public:
    static constexpr int kSectorSize = 2352;
    static constexpr int kPregap = 150;  // LBA 0 is 00:02:00 in absolute time

    struct Track {
        int number = 0;
        bool audio = false;
        int32_t begin = 0;  // first LBA of the track, its pregap included (index 00)
        int32_t start = 0;  // LBA of index 01, where the track starts for the drive
        int32_t end = 0;    // last LBA, inclusive

        // Where its sectors are stored.
        int file = -1;             // in files_; -1 for a .chd
        uint64_t offset = 0;       // byte offset in the file of sector `dataBegin`
        int32_t dataBegin = 0;     // first LBA stored (sectors before it are silence)
        int32_t dataEnd = -1;      // last LBA stored
        int sectorBytes = 2352;    // 2352, or 2048 for data stored without its header
        bool bigEndian = false;    // audio stored big-endian (MOTOROLA, .chd)
        int64_t chdFrame = 0;      // .chd: frame number of sector `dataBegin`
    };

    CdImage();
    ~CdImage();
    CdImage(const CdImage&) = delete;
    CdImage& operator=(const CdImage&) = delete;

    // Opens a .cue, .iso, .bin or .chd. On failure says why (in Spanish).
    bool open(const std::string& path, std::string* error);
    // A disc made in memory, for tests: `data` holds raw 2352-byte sectors from LBA 0.
    void openTracks(std::vector<Track> tracks, std::vector<uint8_t> data);
    void close();
    bool loaded() const { return !tracks_.empty(); }

    const std::vector<Track>& tracks() const { return tracks_; }
    int firstTrack() const { return tracks_.empty() ? 0 : tracks_.front().number; }
    int lastTrack() const { return tracks_.empty() ? 0 : tracks_.back().number; }
    int32_t leadOut() const { return tracks_.empty() ? 0 : tracks_.back().end + 1; }
    // The track that holds `lba` (index into tracks()), or -1 outside them all.
    int trackAt(int32_t lba) const;
    const Track* track(int number) const;

    // Reads one raw sector. Sectors with nothing stored (pregaps, past the end) read as
    // zeroes, with a made-up header when they belong to a data track.
    void readSector(int32_t lba, uint8_t* out);
    // The 2048 bytes of user data of a data sector.
    void readData(int32_t lba, uint8_t* out);

    // CRC32 of the disc's first data sectors (its header and boot program): what
    // savestates are tied to.
    uint32_t identity();

    // Track layout of a .cue sheet's text. `fileSizes` are the sizes of the files it
    // names, in order. Exposed for the tests.
    static bool parseCue(const std::string& text, const std::vector<uint64_t>& fileSizes,
                         std::vector<Track>& tracks, std::string* error);

private:
    bool openCue(const std::string& path, std::string* error);
    bool openPlain(const std::string& path, std::string* error);
    bool openChd(const std::string& path, std::string* error);
    bool readStored(const Track& t, int32_t lba, uint8_t* out);

    std::vector<Track> tracks_;
    std::vector<std::FILE*> files_;
    std::vector<uint8_t> memory_;  // openTracks()

    struct Chd;
    std::unique_ptr<Chd> chd_;
};

// MSF helpers: absolute time (minutes, seconds, frames) of an LBA, and back.
inline void lbaToMsf(int32_t lba, int& m, int& s, int& f) {
    int32_t frames = lba + CdImage::kPregap;
    if (frames < 0) frames += 100 * 60 * 75;
    m = frames / (60 * 75) % 100;
    s = frames / 75 % 60;
    f = frames % 75;
}
inline int32_t msfToLba(int m, int s, int f) { return (m * 60 + s) * 75 + f - CdImage::kPregap; }

std::FILE* openFileUtf8(const std::string& path, const char* mode);

}  // namespace plumsg
