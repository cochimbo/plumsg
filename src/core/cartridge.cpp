#include "cartridge.h"

#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#include "miniz.h"

namespace plumsg {

namespace {

bool isZip(const std::vector<uint8_t>& data) {
    return data.size() >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
}

bool hasRomExtension(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto endsWith = [&](const char* ext) {
        const std::string e(ext);
        return name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0;
    };
    return endsWith(".md") || endsWith(".gen") || endsWith(".bin") || endsWith(".smd") || endsWith(".32x");
}

// Replaces a zip archive with its first .md/.gen/.bin/.smd/.32x entry.
bool extractRomFromZip(std::vector<uint8_t>& data, const std::string& path, std::string* error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
        if (error) *error = trf("el zip está dañado: %s", path.c_str());
        return false;
    }
    bool found = false;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && !found; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory || !hasRomExtension(st.m_filename)) continue;
        size_t size = 0;
        void* rom = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!rom) {
            if (error) *error = trf("no se puede descomprimir %s de %s", st.m_filename, path.c_str());
            mz_zip_reader_end(&zip);
            return false;
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(rom);
        std::vector<uint8_t> extracted(bytes, bytes + size);
        mz_free(rom);
        data = std::move(extracted);
        found = true;
    }
    mz_zip_reader_end(&zip);
    if (!found && error) *error = trf("el zip no contiene ninguna ROM (.md, .gen, .bin, .smd o .32x): %s", path.c_str());
    return found;
}

// Super Magic Drive dumps: a 512-byte header, then 16 KB blocks with the odd bytes
// in the first half and the even bytes in the second.
bool isSmd(const std::vector<uint8_t>& d) {
    if (d.size() < 512 + 0x4000 || d.size() % 0x4000 != 512) return false;
    if (d[8] == 0xAA && d[9] == 0xBB) return true;
    // No signature: interleaved if the plain header is not where it should be.
    auto has = [&](size_t at, const char* s) { return std::equal(s, s + 4, d.begin() + static_cast<long>(at)); };
    return !has(512 + 0x100, "SEGA") && !has(512 + 0x101, "SEGA");
}

std::vector<uint8_t> deinterleaveSmd(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> out(d.size() - 512);
    for (size_t block = 0; block < out.size() / 0x4000; ++block) {
        const uint8_t* src = &d[512 + block * 0x4000];
        uint8_t* dst = &out[block * 0x4000];
        for (int i = 0; i < 0x2000; ++i) {
            dst[i * 2 + 1] = src[i];
            dst[i * 2] = src[0x2000 + i];
        }
    }
    return out;
}

std::string field(const std::vector<uint8_t>& rom, size_t at, size_t len) {
    std::string s;
    for (size_t i = at; i < at + len && i < rom.size(); ++i) s += static_cast<char>(rom[i] >= 32 && rom[i] < 127 ? rom[i] : ' ');
    // Collapse runs of spaces and trim.
    std::string out;
    for (char c : s)
        if (!(c == ' ' && (out.empty() || out.back() == ' '))) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

uint32_t be32(const std::vector<uint8_t>& r, size_t at) {
    return static_cast<uint32_t>(r[at] << 24 | r[at + 1] << 16 | r[at + 2] << 8 | r[at + 3]);
}

}  // namespace

bool Cartridge::loadFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = trf("no se puede abrir %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (isZip(data) && !extractRomFromZip(data, path, error)) return false;
    if (isSmd(data)) data = deinterleaveSmd(data);
    else if (data.size() % 0x4000 == 512) data.erase(data.begin(), data.begin() + 512);  // copier header
    if (data.size() < 0x200) {
        if (error) *error = trf("el archivo es demasiado pequeño para ser una ROM de Mega Drive: %s", path.c_str());
        return false;
    }
    loadData(std::move(data));
    path_ = path;
    return true;
}

void Cartridge::loadData(std::vector<uint8_t> rom) {
    if (rom.size() & 1) rom.push_back(0xFF);
    if (rom.size() < 0x200) rom.resize(0x200, 0xFF);
    rom_ = std::move(rom);
    path_.clear();
    crc_ = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, rom_.data(), rom_.size()));

    header_ = RomHeader{};
    header_.system = field(rom_, 0x100, 16);
    header_.title = field(rom_, 0x150, 48);
    if (header_.title.empty()) header_.title = field(rom_, 0x120, 48);
    header_.serial = field(rom_, 0x180, 14);
    // Region: letters J/U/E, or one hex digit (bit 0 Japan, bit 2 USA, bit 3 Europe).
    const std::string regions = field(rom_, 0x1F0, 3);
    for (char c : regions) {
        if (c == 'J') header_.japan = true;
        else if (c == 'U') header_.usa = true;
        else if (c == 'E') header_.europe = true;
    }
    if (!header_.japan && !header_.usa && !header_.europe && !regions.empty()) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(regions[0])));
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        header_.japan = v & 1;
        header_.usa = v & 4;
        header_.europe = v & 8;
    }

    // Battery RAM: "RA" at 0x1B0 with its address range, else 64 KB at 0x200000 for ROMs
    // that leave that space free.
    sramPresent_ = false;
    if (rom_.size() >= 0x1BC && rom_[0x1B0] == 'R' && rom_[0x1B1] == 'A') {
        uint32_t start = be32(rom_, 0x1B4), end = be32(rom_, 0x1B8);
        if (start <= end && end - start < 0x100000 && start >= 0x200000 && start < 0x400000) {
            sramStart_ = start & ~1u;
            sramEnd_ = end | 1u;
            sramPresent_ = true;
        }
    }
    if (!sramPresent_ && rom_.size() <= 0x200000) {
        sramStart_ = 0x200000;
        sramEnd_ = 0x20FFFF;
        sramPresent_ = true;
    }
    header_.hasSram = sramPresent_;
    header_.sramStart = sramStart_;
    header_.sramEnd = sramEnd_;
    sram_.assign(sramPresent_ ? sramEnd_ - sramStart_ + 1 : 0, 0xFF);
    sramUsed_ = false;
    mapper_ = rom_.size() > 0x400000;
    reset();
}

Region Cartridge::preferredRegion() const {
    if (header_.usa) return Region::UsaNtsc;
    if (header_.japan) return Region::JapanNtsc;
    if (header_.europe) return Region::EuropePal;
    return Region::UsaNtsc;
}

void Cartridge::reset() {
    // Over 2 MB the SRAM shares its addresses with ROM and starts switched off.
    sramMapped_ = rom_.size() <= sramStart_ || !sramPresent_;
    sramWritable_ = true;
    for (int i = 0; i < 8; ++i) banks_[i] = static_cast<uint8_t>(i);
}

bool Cartridge::patchRom(uint32_t offset, uint8_t value, uint8_t* previous) {
    if (offset >= rom_.size()) return false;
    if (previous) *previous = rom_[offset];
    rom_[offset] = value;
    return true;
}

bool Cartridge::sramAt(uint32_t addr) const {
    return sramPresent_ && sramMapped_ && addr >= sramStart_ && addr <= sramEnd_;
}

uint8_t Cartridge::read8(uint32_t addr) const {
    addr &= 0x3FFFFF;
    if (sramAt(addr)) return sram_[addr - sramStart_];
    uint32_t offset = addr;
    if (mapper_) offset = (static_cast<uint32_t>(banks_[addr >> 19]) << 19) | (addr & 0x7FFFF);
    return offset < rom_.size() ? rom_[offset] : 0xFF;
}

uint16_t Cartridge::read16(uint32_t addr) const {
    addr &= 0x3FFFFE;
    if (sramAt(addr)) return static_cast<uint16_t>(sram_[addr - sramStart_] << 8 | sram_[addr + 1 - sramStart_]);
    uint32_t offset = addr;
    if (mapper_) offset = (static_cast<uint32_t>(banks_[addr >> 19]) << 19) | (addr & 0x7FFFF);
    return offset + 1 < rom_.size() ? static_cast<uint16_t>(rom_[offset] << 8 | rom_[offset + 1]) : 0xFFFF;
}

void Cartridge::write8(uint32_t addr, uint8_t value) {
    addr &= 0x3FFFFF;
    if (!sramAt(addr) || !sramWritable_) return;
    uint8_t& b = sram_[addr - sramStart_];
    if (b != value) sramUsed_ = true;
    b = value;
}

void Cartridge::write16(uint32_t addr, uint16_t value) {
    write8(addr & ~1u, static_cast<uint8_t>(value >> 8));
    write8(addr | 1, static_cast<uint8_t>(value));
}

void Cartridge::writeTimeRegister(uint32_t addr, uint8_t value) {
    addr &= 0xFF;
    if (addr == 0xF1) {
        sramMapped_ = (value & 1) != 0 || rom_.size() <= sramStart_;
        sramWritable_ = (value & 2) == 0;
    } else if (mapper_ && addr >= 0xF3 && (addr & 1)) {
        banks_[(addr - 0xF1) >> 1] = value & 0x3F;
    }
}

bool Cartridge::loadSave(const std::string& path) {
    if (sram_.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(sram_.data()), static_cast<std::streamsize>(sram_.size()));
    sramUsed_ = in.gcount() > 0;
    return sramUsed_;
}

bool Cartridge::writeSave(const std::string& path) const {
    if (sram_.empty()) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(sram_.data()), static_cast<std::streamsize>(sram_.size()));
    return static_cast<bool>(out);
}

}  // namespace plumsg
