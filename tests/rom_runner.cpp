// Runs a ROM without a window: a number of frames, then writes the picture as PNG
// (optionally each VDP layer too) and prints a hash of the picture and the level of
// the sound. Used for regression checks and to get screenshots out of CI.
//
//   rom_runner <rom|--test-rom> [--frames N] [--png out.png] [--layers prefix]
//              [--pal] [--write-test-rom out.bin] [--wav out.wav] [--save-state out]
//              [--bios cd_bios.bin]   (Mega CD discs: the BIOS to boot them with)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "megadrive.h"
#include "miniz.h"
#include "test_rom.h"

using namespace plumsg;

namespace {

bool writePng(const std::string& path, const uint32_t* argb, int width, int height, int stride) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint32_t p = argb[y * stride + x];
            uint8_t* o = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            o[0] = static_cast<uint8_t>(p >> 16);
            o[1] = static_cast<uint8_t>(p >> 8);
            o[2] = static_cast<uint8_t>(p);
            o[3] = static_cast<uint8_t>(p >> 24);
        }
    size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgba.data(), width, height, 4, &size);
    if (!png) return false;
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(png), static_cast<std::streamsize>(size));
    mz_free(png);
    return static_cast<bool>(out);
}

bool writeWav(const std::string& path, const std::vector<int16_t>& samples, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
    return static_cast<bool>(out);
}

uint32_t frameHash(const VideoFrameMd& f) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            h ^= f.pixel(x, y);
            h *= 16777619u;
        }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    std::string rom, png, layers, wav, writeRom, stateOut, bios;
    int frames = 120;
    bool pal = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--png") png = next();
        else if (a == "--layers") layers = next();
        else if (a == "--wav") wav = next();
        else if (a == "--pal") pal = true;
        else if (a == "--save-state") stateOut = next();
        else if (a == "--write-test-rom") writeRom = next();
        else if (a == "--bios") bios = next();
        else rom = a;
    }
    if (!writeRom.empty()) {
        const auto data = testrom::build();
        std::ofstream(writeRom, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
        std::printf("wrote %s (%zu bytes)\n", writeRom.c_str(), data.size());
        if (rom.empty()) return 0;
    }
    if (rom.empty()) {
        std::fprintf(stderr, "usage: rom_runner <rom|--test-rom> [--frames N] [--png out.png] [--layers prefix]\n");
        return 2;
    }

    MegaDrive md;
    if (pal) md.setRegion(Region::EuropePal);
    if (!bios.empty()) {
        // The one BIOS for whatever region the disc is.
        std::ifstream in(bios, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (Region r : {Region::JapanNtsc, Region::UsaNtsc, Region::EuropePal}) md.setCdBios(r, data);
    }
    if (rom == "--test-rom") {
        md.loadRomData(testrom::build());
    } else {
        std::string error;
        if (!md.loadRom(rom, &error)) {
            std::fprintf(stderr, "cannot load %s: %s\n", rom.c_str(), error.c_str());
            return 1;
        }
    }
    md.setSampleRate(44100);
    std::vector<int16_t> sound;
    for (int i = 0; i < frames; ++i) {
        md.runFrame();
        sound.insert(sound.end(), md.audio().samples().begin(), md.audio().samples().end());
        md.audio().clearSamples();
    }

    const VideoFrameMd& f = md.frame();
    double sumSq = 0;
    for (int16_t s : sound) sumSq += static_cast<double>(s) * s;
    const double rms = sound.empty() ? 0 : std::sqrt(sumSq / static_cast<double>(sound.size()));
    std::printf("title: %s\nframes: %d  size: %dx%d  hash: %08x\naudio: %zu samples  rms: %.1f\npc: %06x\n",
                md.cartridge().header().title.c_str(), frames, f.width, f.height, frameHash(f), sound.size() / 2, rms,
                md.cpu().pc);
    if (md.isMegaCd()) std::printf("sub pc: %06x\n", md.megaCd()->sub().pc);
    if (!png.empty() && !writePng(png, f.rgba.data(), f.width, f.height, VideoFrameMd::kMaxWidth)) return 1;
    if (!layers.empty()) {
        writePng(layers + "_planeB.png", f.planeB.data(), f.width, f.height, VideoFrameMd::kMaxWidth);
        writePng(layers + "_planeA.png", f.planeA.data(), f.width, f.height, VideoFrameMd::kMaxWidth);
        writePng(layers + "_sprites.png", f.sprites.data(), f.width, f.height, VideoFrameMd::kMaxWidth);
    }
    if (!wav.empty()) writeWav(wav, sound, 44100);
    if (!stateOut.empty()) {
        std::vector<uint8_t> state;
        if (!md.saveState(state)) return 1;
        std::ofstream(stateOut, std::ios::binary).write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
    }
    return 0;
}
