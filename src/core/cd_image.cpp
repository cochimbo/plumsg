#include "cd_image.h"

#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <sstream>

// libchdr's headers trip MSVC's /W4; they are third-party code, so their warnings are muted.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <libchdr/cdrom.h>
#include <libchdr/chd.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "miniz.h"

namespace plumsg {

namespace fs = std::filesystem;

std::FILE* openFileUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(fs::u8path(path).c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

namespace {

// libchdr's CDROM_TRACK_METADATA*_FORMAT, with every %s bounded to the 64-byte buffers it fills, so a
// damaged or crafted .chd cannot overflow them.
constexpr const char* kTrackMetadataFormat = "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d";
constexpr const char* kTrackMetadata2Format =
    "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d PREGAP:%d PGTYPE:%63s PGSUB:%63s POSTGAP:%d";

bool seekFile(std::FILE* f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

uint64_t fileSize(std::FILE* f) {
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    return static_cast<uint64_t>(_ftelli64(f));
#else
    fseeko(f, 0, SEEK_END);
    return static_cast<uint64_t>(ftello(f));
#endif
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

uint8_t bcd(int v) { return static_cast<uint8_t>(v / 10 << 4 | v % 10); }

// Sync pattern and header of a mode 1 data sector at `lba`.
void makeHeader(int32_t lba, uint8_t* out) {
    out[0] = 0;
    std::memset(out + 1, 0xFF, 10);
    out[11] = 0;
    int m, s, f;
    lbaToMsf(lba, m, s, f);
    out[12] = bcd(m);
    out[13] = bcd(s);
    out[14] = bcd(f);
    out[15] = 1;
}

const uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};

// "mm:ss:ff" to a sector count; -1 when malformed.
int parseMsf(const std::string& text) {
    int m = 0, s = 0, f = 0;
    char c1 = 0, c2 = 0;
    std::istringstream in(text);
    if (!(in >> m >> c1 >> s >> c2 >> f) || c1 != ':' || c2 != ':' || s > 59 || f > 74 || m < 0) return -1;
    return (m * 60 + s) * 75 + f;
}

// Splits a .cue line into words; a quoted string is one word.
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            const size_t end = line.find('"', i + 1);
            out.push_back(line.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
            i = end == std::string::npos ? line.size() : end + 1;
        } else {
            size_t end = i;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
            out.push_back(line.substr(i, end - i));
            i = end;
        }
    }
    return out;
}

// Where the samples of a .wav start, and how many bytes they take.
bool waveData(std::FILE* f, uint64_t& offset, uint64_t& size) {
    uint8_t head[12];
    if (!seekFile(f, 0) || std::fread(head, 1, 12, f) != 12) return false;
    if (std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) return false;
    uint64_t pos = 12;
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t chunk[8];
        if (!seekFile(f, pos) || std::fread(chunk, 1, 8, f) != 8) return false;
        const uint32_t len = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | static_cast<uint32_t>(chunk[7]) << 24;
        if (std::memcmp(chunk, "data", 4) == 0) {
            offset = pos + 8;
            size = len;
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

}  // namespace

struct CdImage::Chd {
    chd_file* file = nullptr;
    std::FILE* stdio = nullptr;
    uint32_t hunkBytes = 0;
    uint32_t framesPerHunk = 0;
    uint32_t hunks = 0;
    int64_t cached = -1;
    std::vector<uint8_t> hunk;

    ~Chd() {
        if (file) chd_close(file);
        if (stdio) std::fclose(stdio);
    }
};

CdImage::CdImage() = default;
CdImage::~CdImage() { close(); }

void CdImage::close() {
    for (std::FILE* f : files_) std::fclose(f);
    files_.clear();
    chd_.reset();
    tracks_.clear();
    memory_.clear();
}

bool CdImage::open(const std::string& path, std::string* error) {
    close();
    const std::string ext = lower(fs::u8path(path).extension().u8string());
    bool ok = false;
    if (ext == ".cue") ok = openCue(path, error);
    else if (ext == ".chd") ok = openChd(path, error);
    else ok = openPlain(path, error);
    if (!ok) close();
    return ok;
}

void CdImage::openTracks(std::vector<Track> tracks, std::vector<uint8_t> data) {
    close();
    tracks_ = std::move(tracks);
    memory_ = std::move(data);
}

int CdImage::trackAt(int32_t lba) const {
    for (size_t i = 0; i < tracks_.size(); ++i)
        if (lba >= tracks_[i].begin && lba <= tracks_[i].end) return static_cast<int>(i);
    return -1;
}

const CdImage::Track* CdImage::track(int number) const {
    for (const Track& t : tracks_)
        if (t.number == number) return &t;
    return nullptr;
}

bool CdImage::parseCue(const std::string& text, const std::vector<uint64_t>& fileSizes, std::vector<Track>& tracks,
                       std::string* error) {
    struct CueTrack {
        int number = 0;
        int file = 0;
        bool audio = false;
        int sectorBytes = 2352;
        bool bigEndian = false;
        int index0 = -1, index1 = -1;
        int pregap = 0;
    };
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::vector<CueTrack> cue;
    int file = -1;
    bool bigEndian = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> w = words(line);
        if (w.empty()) continue;
        const std::string key = upper(w[0]);
        if (key == "FILE") {
            ++file;
            bigEndian = w.size() > 2 && upper(w[2]) == "MOTOROLA";
        } else if (key == "TRACK" && w.size() >= 3) {
            if (file < 0) return fail(tr("la hoja .cue tiene una pista antes de su archivo"));
            CueTrack t;
            t.number = std::atoi(w[1].c_str());
            t.file = file;
            const std::string mode = upper(w[2]);
            t.audio = mode == "AUDIO";
            t.bigEndian = t.audio && bigEndian;
            if (mode == "MODE1/2048") t.sectorBytes = 2048;
            else if (mode == "AUDIO" || mode == "MODE1/2352" || mode == "MODE2/2352") t.sectorBytes = 2352;
            else return fail(trf("la hoja .cue usa un tipo de pista que plumsg no lee (%s)", w[2].c_str()));
            cue.push_back(t);
        } else if (key == "INDEX" && w.size() >= 3 && !cue.empty()) {
            const int n = std::atoi(w[1].c_str());
            const int at = parseMsf(w[2]);
            if (at < 0) return fail(trf("la hoja .cue tiene un tiempo mal escrito (%s)", w[2].c_str()));
            if (n == 0) cue.back().index0 = at;
            if (n == 1) cue.back().index1 = at;
        } else if (key == "PREGAP" && w.size() >= 2 && !cue.empty()) {
            const int at = parseMsf(w[1]);
            if (at < 0) return fail(trf("la hoja .cue tiene un tiempo mal escrito (%s)", w[1].c_str()));
            cue.back().pregap = at;
        }
    }
    if (cue.empty()) return fail(tr("la hoja .cue no tiene pistas"));
    if (static_cast<size_t>(file + 1) != fileSizes.size())
        return fail(tr("la hoja .cue no encaja con sus archivos"));

    tracks.clear();
    int32_t fileBase = 0;
    for (size_t first = 0; first < cue.size();) {
        size_t last = first;
        while (last + 1 < cue.size() && cue[last + 1].file == cue[first].file) ++last;
        if (first == 0) fileBase = -(cue[0].index1 < 0 ? 0 : cue[0].index1);
        int32_t shift = 0;
        uint64_t byteOffset = 0;
        int prevSector = 0, prevBytes = cue[first].sectorBytes;
        for (size_t i = first; i <= last; ++i) {
            const CueTrack& c = cue[i];
            if (c.index1 < 0) return fail(trf("a la pista %d le falta su INDEX 01", c.number));
            const int firstSector = c.index0 >= 0 ? c.index0 : c.index1;
            if (firstSector < prevSector) return fail(tr("los tiempos de la hoja .cue van hacia atrás"));
            byteOffset += static_cast<uint64_t>(firstSector - prevSector) * static_cast<uint64_t>(prevBytes);
            prevSector = firstSector;
            prevBytes = c.sectorBytes;
            if (i > 0) shift += c.pregap;
            Track t;
            t.number = c.number;
            t.audio = c.audio;
            t.file = c.file;
            t.sectorBytes = c.sectorBytes;
            t.bigEndian = c.bigEndian;
            t.offset = byteOffset;
            t.start = fileBase + shift + c.index1;
            t.dataBegin = fileBase + shift + firstSector;
            t.begin = i == 0 ? -kPregap : c.index0 >= 0 ? t.dataBegin : t.start - c.pregap;
            if (i < last) {
                const CueTrack& next = cue[i + 1];
                const int nextFirst = next.index0 >= 0 ? next.index0 : next.index1;
                t.dataEnd = t.dataBegin + (nextFirst - firstSector) - 1;
            } else {
                const uint64_t size = fileSizes[static_cast<size_t>(c.file)];
                const uint64_t count = size > byteOffset ? (size - byteOffset) / static_cast<uint64_t>(c.sectorBytes) : 0;
                t.dataEnd = t.dataBegin + static_cast<int32_t>(count) - 1;
            }
            tracks.push_back(t);
        }
        // Each track ends where the next begins; the file's last one with its data.
        for (size_t i = first; i <= last; ++i) {
            Track& t = tracks[i];
            t.end = i < last ? tracks[i + 1].begin - 1 : t.dataEnd;
            if (t.end < t.start) return fail(trf("la pista %d está vacía", t.number));
        }
        fileBase = tracks[last].end + 1;
        first = last + 1;
    }
    return true;
}

bool CdImage::openCue(const std::string& path, std::string* error) {
    std::FILE* f = openFileUtf8(path, "rb");
    if (!f) {
        if (error) *error = trf("no se puede abrir %s", path.c_str());
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0 && text.size() < (1u << 20)) text.append(buffer, n);
    std::fclose(f);

    // Open the files it names, next to it.
    const fs::path folder = fs::u8path(path).parent_path();
    std::vector<uint64_t> sizes, bases;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> w = words(line);
        if (w.size() < 2 || upper(w[0]) != "FILE") continue;
        const std::string name = (folder / fs::u8path(w[1])).u8string();
        std::FILE* data = openFileUtf8(name, "rb");
        if (!data) {
            if (error) *error = trf("no se encuentra %s, que la hoja .cue necesita", w[1].c_str());
            return false;
        }
        files_.push_back(data);
        uint64_t base = 0, size = fileSize(data);
        if (w.size() > 2 && upper(w[2]) == "WAVE" && !waveData(data, base, size)) {
            if (error) *error = trf("%s no es un .wav que plumsg sepa leer", w[1].c_str());
            return false;
        }
        bases.push_back(base);
        sizes.push_back(size);
    }
    if (!parseCue(text, sizes, tracks_, error)) return false;
    for (Track& t : tracks_) t.offset += bases[static_cast<size_t>(t.file)];
    return true;
}

bool CdImage::openPlain(const std::string& path, std::string* error) {
    std::FILE* f = openFileUtf8(path, "rb");
    if (!f) {
        if (error) *error = trf("no se puede abrir %s", path.c_str());
        return false;
    }
    files_.push_back(f);
    const uint64_t size = fileSize(f);
    uint8_t head[12] = {};
    seekFile(f, 0);
    const bool raw = std::fread(head, 1, 12, f) == 12 && std::memcmp(head, kSync, 12) == 0;
    Track t;
    t.number = 1;
    t.file = 0;
    t.sectorBytes = raw ? 2352 : 2048;
    t.begin = -kPregap;
    t.start = t.dataBegin = 0;
    t.dataEnd = static_cast<int32_t>(size / static_cast<uint64_t>(t.sectorBytes)) - 1;
    t.end = t.dataEnd;
    if (t.end < 16) {
        if (error) *error = tr("el archivo es demasiado pequeño para ser un disco");
        return false;
    }
    tracks_.push_back(t);
    return true;
}

bool CdImage::openChd(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    chd_ = std::make_unique<Chd>();
    chd_->stdio = openFileUtf8(path, "rb");
    if (!chd_->stdio) return fail(trf("no se puede abrir %s", path.c_str()));
    if (chd_open_file(chd_->stdio, CHD_OPEN_READ, nullptr, &chd_->file) != CHDERR_NONE) {
        chd_->file = nullptr;
        return fail(tr("el .chd está dañado o es de un tipo que plumsg no lee"));
    }
    const chd_header* h = chd_get_header(chd_->file);
    chd_->hunkBytes = h->hunkbytes;
    chd_->hunks = h->totalhunks;
    chd_->framesPerHunk = h->hunkbytes / CD_FRAME_SIZE;
    if (chd_->framesPerHunk == 0 || h->hunkbytes % CD_FRAME_SIZE != 0) return fail(tr("el .chd no es de un CD"));
    chd_->hunk.resize(h->hunkbytes);

    int64_t frame = 0;
    int32_t lba = 0;
    for (uint32_t i = 0; i < 99; ++i) {
        char meta[256] = {};
        char type[64] = {}, subtype[64] = {}, pgtype[64] = {}, pgsub[64] = {};
        int number = 0, frames = 0, pregap = 0, postgap = 0;
        uint32_t len = 0;
        if (chd_get_metadata(chd_->file, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof meta - 1, &len, nullptr, nullptr) ==
            CHDERR_NONE) {
            if (std::sscanf(meta, kTrackMetadata2Format, &number, type, subtype, &frames, &pregap, pgtype, pgsub,
                            &postgap) != 8)
                return fail(tr("el .chd tiene una pista que plumsg no entiende"));
        } else if (chd_get_metadata(chd_->file, CDROM_TRACK_METADATA_TAG, i, meta, sizeof meta - 1, &len, nullptr,
                                    nullptr) == CHDERR_NONE) {
            if (std::sscanf(meta, kTrackMetadataFormat, &number, type, subtype, &frames) != 4)
                return fail(tr("el .chd tiene una pista que plumsg no entiende"));
        } else {
            break;
        }
        const std::string kind = type;
        Track t;
        t.number = number;
        t.audio = kind == "AUDIO";
        t.bigEndian = t.audio;
        if (kind == "MODE1" || kind == "MODE2_FORM1") t.sectorBytes = 2048;
        else if (kind == "MODE1_RAW" || kind == "MODE2_RAW" || kind == "AUDIO") t.sectorBytes = 2352;
        else return fail(trf("el .chd usa un tipo de pista que plumsg no lee (%s)", kind.c_str()));
        const bool pregapStored = pgtype[0] == 'V';
        const int storedPregap = pregapStored ? pregap : 0;
        if (tracks_.empty()) {
            // Track 1's index 01 is LBA 0 and its pregap the 150 sectors before it.
            lba = -pregap;
            t.begin = -kPregap;
        } else {
            t.begin = lba;
        }
        t.start = lba + pregap;
        t.chdFrame = frame;
        t.dataBegin = pregapStored ? lba : t.start;
        t.dataEnd = t.dataBegin + frames - 1;
        t.end = t.start + (frames - storedPregap) - 1 + postgap;
        if (t.end < t.start) return fail(tr("el .chd tiene una pista vacía"));
        tracks_.push_back(t);
        lba = t.end + 1;
        frame += (frames + 3) & ~3;  // tracks are padded to 4 frames
    }
    if (tracks_.empty()) return fail(tr("el .chd no es de un CD"));
    return true;
}

bool CdImage::readStored(const Track& t, int32_t lba, uint8_t* out) {
    if (lba < t.dataBegin || lba > t.dataEnd) return false;
    const int bytes = t.sectorBytes;
    uint8_t* dest = bytes == 2048 ? out + 16 : out;
    const int64_t index = lba - t.dataBegin;
    if (!memory_.empty()) {
        const uint64_t at = t.offset + static_cast<uint64_t>(index) * static_cast<uint64_t>(bytes);
        if (at + static_cast<uint64_t>(bytes) > memory_.size()) return false;
        std::memcpy(dest, memory_.data() + at, static_cast<size_t>(bytes));
    } else if (chd_) {
        const int64_t frame = t.chdFrame + index;
        const int64_t hunk = frame / chd_->framesPerHunk;
        if (hunk >= chd_->hunks) return false;
        if (hunk != chd_->cached) {
            if (chd_read(chd_->file, static_cast<uint32_t>(hunk), chd_->hunk.data()) != CHDERR_NONE) {
                chd_->cached = -1;
                return false;
            }
            chd_->cached = hunk;
        }
        std::memcpy(dest, chd_->hunk.data() + (frame % chd_->framesPerHunk) * CD_FRAME_SIZE, static_cast<size_t>(bytes));
    } else {
        if (t.file < 0 || static_cast<size_t>(t.file) >= files_.size()) return false;
        std::FILE* f = files_[static_cast<size_t>(t.file)];
        if (!seekFile(f, t.offset + static_cast<uint64_t>(index) * static_cast<uint64_t>(bytes)) ||
            std::fread(dest, 1, static_cast<size_t>(bytes), f) != static_cast<size_t>(bytes))
            return false;
    }
    if (t.audio && t.bigEndian)
        for (int i = 0; i < kSectorSize; i += 2) std::swap(out[i], out[i + 1]);
    return true;
}

void CdImage::readSector(int32_t lba, uint8_t* out) {
    std::memset(out, 0, kSectorSize);
    const int i = trackAt(lba);
    if (i < 0) return;
    const Track& t = tracks_[static_cast<size_t>(i)];
    const bool stored = readStored(t, lba, out);
    if (!t.audio && (!stored || t.sectorBytes == 2048)) makeHeader(lba, out);
}

void CdImage::readData(int32_t lba, uint8_t* out) {
    uint8_t sector[kSectorSize];
    readSector(lba, sector);
    // Mode 2 form 1 has an 8-byte subheader before its data.
    std::memcpy(out, sector + (sector[15] == 2 ? 24 : 16), 2048);
}

uint32_t CdImage::identity() {
    mz_ulong crc = MZ_CRC32_INIT;
    uint8_t data[2048];
    for (int32_t lba = 0; lba < 16; ++lba) {
        readData(lba, data);
        crc = mz_crc32(crc, data, sizeof data);
    }
    return static_cast<uint32_t>(crc);
}

}  // namespace plumsg
