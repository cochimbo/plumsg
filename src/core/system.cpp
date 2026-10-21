#include "system.h"

#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "miniz.h"

namespace plumsg {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extensionOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot));
}

bool startsWith(const std::vector<uint8_t>& d, size_t at, const char* text) {
    const size_t n = std::strlen(text);
    return d.size() >= at + n && std::equal(text, text + n, d.begin() + static_cast<long>(at));
}

std::vector<uint8_t> readStart(const std::string& path, size_t bytes) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::vector<uint8_t> out(bytes);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(bytes));
    out.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
    return out;
}

}  // namespace

const char* systemName(ConsoleSystem s) {
    switch (s) {
        case ConsoleSystem::Mega32X: return "32X";
        case ConsoleSystem::MegaCD: return "Mega CD";
        case ConsoleSystem::MasterSystem: return "Master System";
        case ConsoleSystem::GameGear: return "Game Gear";
        default: return "Mega Drive";
    }
}

bool isGameFile(const std::string& path) {
    const std::string e = extensionOf(path);
    return e == ".md" || e == ".gen" || e == ".smd" || e == ".bin" || e == ".zip" || e == ".32x" || e == ".sms" || e == ".gg" || e == ".cue" ||
           e == ".chd" || e == ".iso";
}

ConsoleSystem detectSystemFromData(const std::vector<uint8_t>& d) {
    // A disc's first sector: the ISO data straight away, or a raw 2352-byte sector
    // with its 12-byte sync pattern and 4-byte header first.
    if (startsWith(d, 0, "SEGADISCSYSTEM") || startsWith(d, 0x10, "SEGADISCSYSTEM")) return ConsoleSystem::MegaCD;
    // A 32X cartridge says so in its header, and has the "MARS" security code.
    if (startsWith(d, 0x100, "SEGA 32X") || startsWith(d, 0x101, "SEGA 32X")) return ConsoleSystem::Mega32X;
    // A Mega Drive cartridge says SEGA at 0x100; a Master System or Game Gear one has
    // "TMR SEGA" at the end of its first 8, 16 or 32 KB, and its region code says which
    // (5, 6 and 7 are the Game Gear's).
    if (startsWith(d, 0x100, "SEGA") || startsWith(d, 0x101, "SEGA")) return ConsoleSystem::MegaDrive;
    for (size_t at : {0x7FF0, 0x3FF0, 0x1FF0})
        if (startsWith(d, at, "TMR SEGA")) {
            const int code = d[at + 0xF] >> 4;
            return code >= 5 && code <= 7 ? ConsoleSystem::GameGear : ConsoleSystem::MasterSystem;
        }
    return ConsoleSystem::MegaDrive;
}

Region smsRegion(const std::vector<uint8_t>& rom) {
    for (size_t at : {0x7FF0, 0x3FF0, 0x1FF0})
        if (startsWith(rom, at, "TMR SEGA")) {
            const int code = rom[at + 0xF] >> 4;
            return code == 3 || code == 5 ? Region::JapanNtsc : Region::UsaNtsc;
        }
    return Region::UsaNtsc;
}

ConsoleSystem detectSystem(const std::string& path) {
    const std::string e = extensionOf(path);
    if (e == ".32x") return ConsoleSystem::Mega32X;
    if (e == ".sms") return ConsoleSystem::MasterSystem;
    if (e == ".gg") return ConsoleSystem::GameGear;
    if (e == ".cue" || e == ".chd" || e == ".iso") return ConsoleSystem::MegaCD;
    if (e == ".bin") return detectSystemFromData(readStart(path, 0x8000));
    if (e == ".zip") {
        // Only the central directory is needed for the names, but miniz wants the file
        // in memory; zips of cartridges are small.
        std::error_code ec;
        if (fs::is_directory(fs::u8path(path), ec)) return ConsoleSystem::MegaDrive;  // reading it would throw
        std::ifstream in(fs::u8path(path), std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        mz_zip_archive zip{};
        ConsoleSystem s = ConsoleSystem::MegaDrive;
        if (mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
            const mz_uint count = mz_zip_reader_get_num_files(&zip);
            for (mz_uint i = 0; i < count; ++i) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
                const std::string inner = extensionOf(st.m_filename);
                if (inner == ".32x" || inner == ".sms" || inner == ".gg") {
                    s = inner == ".32x"   ? ConsoleSystem::Mega32X
                        : inner == ".sms" ? ConsoleSystem::MasterSystem
                                          : ConsoleSystem::GameGear;
                    break;
                }
            }
            mz_zip_reader_end(&zip);
        }
        return s;
    }
    return ConsoleSystem::MegaDrive;
}

std::vector<std::string> cueFilesFromText(const std::string& text, const std::string& folder) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || lower(line.substr(i, 5)) != "file ") continue;
        i = line.find_first_not_of(" \t", i + 5);
        if (i == std::string::npos) continue;
        std::string name;
        if (line[i] == '"') {
            const size_t end = line.find('"', i + 1);
            if (end == std::string::npos) continue;
            name = line.substr(i + 1, end - i - 1);
        } else {
            const size_t end = line.find_first_of(" \t", i);
            name = line.substr(i, end == std::string::npos ? std::string::npos : end - i);
        }
        if (!name.empty()) out.push_back(folder + name);
    }
    return out;
}

std::vector<std::string> cueFiles(const std::string& cuePath) {
    std::ifstream in(fs::u8path(cuePath));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const fs::path dir = fs::u8path(cuePath).parent_path();
    std::vector<std::string> out;
    for (const std::string& name : cueFilesFromText(text, ""))
        out.push_back((dir / fs::u8path(name)).u8string());
    return out;
}

std::string msuMdCue(const std::string& romPath) {
    if (extensionOf(romPath) == ".cue") return {};
    const fs::path rom = fs::u8path(romPath);
    std::error_code ec;
    for (const char* ext : {".cue", ".CUE"}) {
        fs::path cue = rom;
        cue.replace_extension(ext);
        if (!fs::is_regular_file(cue, ec)) continue;
        // A .cue that points at the file itself makes it a disc's track, not a cartridge.
        for (const std::string& f : cueFiles(cue.u8string()))
            if (fs::equivalent(fs::u8path(f), rom, ec)) return {};
        return cue.u8string();
    }
    return {};
}

BiosInfo identifyCdBios(const std::vector<uint8_t>& data) {
    BiosInfo info;
    if (data.size() != 0x20000) {
        info.problem = tr("no mide 128 KB, como las BIOS del Mega CD");
        return info;
    }
    if (!startsWith(data, 0x100, "SEGA")) {
        info.problem = tr("no tiene la cabecera de SEGA");
        return info;
    }
    Cartridge c;
    c.loadData(data);
    const RomHeader& h = c.header();
    info.name = h.title;
    const int regions = int{h.japan} + int{h.usa} + int{h.europe};
    if (regions == 1) info.region = h.japan ? Region::JapanNtsc : h.usa ? Region::UsaNtsc : Region::EuropePal;
    info.valid = true;
    return info;
}

BiosInfo identifyCdBiosFile(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) {
        BiosInfo info;
        info.problem = tr("no se encuentra el archivo");
        return info;
    }
    if (size != 0x20000) {
        BiosInfo info;
        info.problem = tr("no mide 128 KB, como las BIOS del Mega CD");
        return info;
    }
    return identifyCdBios(readStart(path, 0x20000));
}

std::array<std::string, 3> findCdBios(const std::vector<std::string>& folders) {
    std::array<std::string, 3> found;
    std::error_code ec;
    for (const std::string& folder : folders) {
        const fs::path root = fs::u8path(folder);
        if (folder.empty() || !fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() >= 3) it.disable_recursion_pending();
            if (!it->is_regular_file(ec) || it->file_size(ec) != 0x20000) continue;
            const std::string path = it->path().u8string();
            const BiosInfo info = identifyCdBiosFile(path);
            if (!info.valid) continue;
            // A 128 KB cartridge has the same header: a BIOS calls itself a boot ROM, or
            // its file says it is a BIOS.
            const std::string name = lower(it->path().filename().u8string());
            const std::string title = lower(info.name);
            const bool biosName = name.find("bios") != std::string::npos;
            if (!biosName && title.find("boot") == std::string::npos) continue;
            Region r = info.region;
            if (r == Region::Auto) {
                const std::string stem = lower(it->path().stem().u8string());
                auto has = [&](const char* s) { return stem.find(s) != std::string::npos; };
                if (has("japan") || has("_j") || has("(j)") || (has("mega-cd") && !has("europe"))) r = Region::JapanNtsc;
                if (has("usa") || has("_u") || has("(u)") || has("sega cd") || has("segacd")) r = Region::UsaNtsc;
                if (has("europe") || has("_e") || has("(e)")) r = Region::EuropePal;
            }
            const int slot = r == Region::JapanNtsc ? 0 : r == Region::UsaNtsc ? 1 : r == Region::EuropePal ? 2 : -1;
            if (slot >= 0 && found[static_cast<size_t>(slot)].empty()) found[static_cast<size_t>(slot)] = path;
        }
        ec.clear();
    }
    return found;
}

}  // namespace plumsg
