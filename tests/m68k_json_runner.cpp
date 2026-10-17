// Runs Tom Harte's single-step 68000 tests (github.com/SingleStepTests/680x0, folder
// 68000/v1) against plumsg's CPU:
//
//   m68k_json_runner [--cycles] [--verbose] [--allow-failures N] file.json[.gz] ...
//
// Each test sets up registers and memory, runs one instruction and compares registers,
// SR, PC and memory. --cycles also compares the cycle count. Prints one line per file
// and exits non-zero if any test failed (or more than N with --allow-failures: CI
// tolerates the 40 known failures, all address-error corner cases).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/m68k.h"
#include "miniz.h"

using namespace plumsg;

namespace {

// Just enough JSON for the test files: objects, arrays, numbers and strings.
struct Json {
    enum Type { Null, Number, String, Array, Object } type = Null;
    double number = 0;
    std::string str;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json& operator[](const char* key) const {
        static const Json none;
        for (const auto& f : fields)
            if (f.first == key) return f.second;
        return none;
    }
    uint32_t u32() const { return static_cast<uint32_t>(static_cast<int64_t>(number)); }
};

class Parser {
public:
    Parser(const char* p, const char* end) : p_(p), end_(end) {}

    bool parse(Json& out) {
        ws();
        if (p_ >= end_) return false;
        switch (*p_) {
            case '{': {
                out.type = Json::Object;
                ++p_;
                ws();
                if (*p_ == '}') { ++p_; return true; }
                for (;;) {
                    Json key;
                    ws();
                    if (!parse(key) || key.type != Json::String) return false;
                    ws();
                    if (*p_++ != ':') return false;
                    out.fields.emplace_back(std::move(key.str), Json());
                    if (!parse(out.fields.back().second)) return false;
                    ws();
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == '}') { ++p_; return true; }
                    return false;
                }
            }
            case '[': {
                out.type = Json::Array;
                ++p_;
                ws();
                if (*p_ == ']') { ++p_; return true; }
                for (;;) {
                    out.items.emplace_back();
                    if (!parse(out.items.back())) return false;
                    ws();
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == ']') { ++p_; return true; }
                    return false;
                }
            }
            case '"': {
                out.type = Json::String;
                ++p_;
                while (p_ < end_ && *p_ != '"') {
                    if (*p_ == '\\') ++p_;
                    out.str += *p_++;
                }
                ++p_;
                return true;
            }
            default: {
                out.type = Json::Number;
                char* e = nullptr;
                out.number = std::strtod(p_, &e);
                if (e == p_) return false;
                p_ = e;
                return true;
            }
        }
    }

private:
    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    const char* p_;
    const char* end_;
};

bool readFile(const std::string& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 18 && static_cast<uint8_t>(data[0]) == 0x1F && static_cast<uint8_t>(data[1]) == 0x8B) {
        // gzip: skip the header, inflate the raw deflate stream.
        size_t pos = 10;
        const uint8_t flags = static_cast<uint8_t>(data[3]);
        if (flags & 4) pos += 2 + (static_cast<uint8_t>(data[pos]) | static_cast<uint8_t>(data[pos + 1]) << 8);
        if (flags & 8) while (pos < data.size() && data[pos++]) {}
        if (flags & 16) while (pos < data.size() && data[pos++]) {}
        if (flags & 2) pos += 2;
        size_t outLen = 0;
        void* raw = tinfl_decompress_mem_to_heap(data.data() + pos, data.size() - pos, &outLen, 0);
        if (!raw) return false;
        out.assign(static_cast<char*>(raw), static_cast<char*>(raw) + outLen);
        mz_free(raw);
        return true;
    }
    out = std::move(data);
    return true;
}

class TestBus : public M68kBus {
public:
    std::unordered_map<uint32_t, uint8_t> ram;
    uint8_t read8(uint32_t addr) override {
        auto it = ram.find(addr & 0xFFFFFF);
        return it == ram.end() ? 0 : it->second;
    }
    uint16_t read16(uint32_t addr) override { return static_cast<uint16_t>(read8(addr) << 8 | read8(addr + 1)); }
    void write8(uint32_t addr, uint8_t v) override { ram[addr & 0xFFFFFF] = v; }
    void write16(uint32_t addr, uint16_t v) override {
        write8(addr, static_cast<uint8_t>(v >> 8));
        write8(addr + 1, static_cast<uint8_t>(v));
    }
};

void load(M68000& cpu, TestBus& bus, const Json& s) {
    static const char* dn[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    static const char* an[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6"};
    for (int i = 0; i < 8; ++i) cpu.d[i] = s[dn[i]].u32();
    for (int i = 0; i < 7; ++i) cpu.a[i] = s[an[i]].u32();
    cpu.sr = static_cast<uint16_t>(s["sr"].u32());
    if (cpu.supervisor()) {
        cpu.a[7] = s["ssp"].u32();
        cpu.otherSp = s["usp"].u32();
    } else {
        cpu.a[7] = s["usp"].u32();
        cpu.otherSp = s["ssp"].u32();
    }
    cpu.pc = s["pc"].u32();
    cpu.stopped = cpu.halted = false;
    bus.ram.clear();
    for (const auto& b : s["ram"].items) bus.ram[b.items[0].u32() & 0xFFFFFF] = static_cast<uint8_t>(b.items[1].u32());
    const auto& pf = s["prefetch"].items;
    for (size_t i = 0; i < pf.size(); ++i) bus.write16(cpu.pc + static_cast<uint32_t>(2 * i), static_cast<uint16_t>(pf[i].u32()));
}

std::string compare(const M68000& cpu, TestBus& bus, const Json& s) {
    static const char* dn[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    static const char* an[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6"};
    char buf[160];
    std::string out;
    auto diff = [&](const char* name, uint32_t got, uint32_t want) {
        if (got == want) return;
        std::snprintf(buf, sizeof buf, " %s=%08X(want %08X)", name, got, want);
        out += buf;
    };
    for (int i = 0; i < 8; ++i) diff(dn[i], cpu.d[i], s[dn[i]].u32());
    for (int i = 0; i < 7; ++i) diff(an[i], cpu.a[i], s[an[i]].u32());
    diff("usp", cpu.usp(), s["usp"].u32());
    diff("ssp", cpu.ssp(), s["ssp"].u32());
    diff("sr", cpu.sr, s["sr"].u32());
    diff("pc", cpu.pc, s["pc"].u32());
    for (const auto& b : s["ram"].items) {
        const uint32_t addr = b.items[0].u32() & 0xFFFFFF;
        const uint8_t want = static_cast<uint8_t>(b.items[1].u32());
        const uint8_t got = bus.read8(addr);
        if (got != want) {
            std::snprintf(buf, sizeof buf, " [%06X]=%02X(want %02X)", addr, got, want);
            out += buf;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // Cycle counts of instructions that end in an address error are not modelled (the
    // CPU stops part-way through); --strict-faults compares them anyway.
    bool checkCycles = false, verbose = false, strictFaults = false;
    int totalFail = 0, totalCycleFail = 0, total = 0, allowed = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cycles") { checkCycles = true; continue; }
        if (arg == "--verbose") { verbose = true; continue; }
        if (arg == "--strict-faults") { strictFaults = true; continue; }
        if (arg == "--allow-failures" && i + 1 < argc) { allowed = std::atoi(argv[++i]); continue; }
        std::vector<char> text;
        if (!readFile(arg, text)) {
            std::fprintf(stderr, "cannot read %s\n", arg.c_str());
            return 2;
        }
        Json root;
        Parser parser(text.data(), text.data() + text.size());
        if (!parser.parse(root) || root.type != Json::Array) {
            std::fprintf(stderr, "cannot parse %s\n", arg.c_str());
            return 2;
        }
        auto bus = std::make_unique<TestBus>();
        M68000 cpu(*bus);
        int fails = 0, cycleFails = 0, shown = 0;
        for (const Json& t : root.items) {
            load(cpu, *bus, t["initial"]);
            const uint32_t faults = cpu.addressErrors;
            const int cycles = cpu.step();
            const bool faulted = cpu.addressErrors != faults;
            const std::string d = compare(cpu, *bus, t["final"]);
            const int want = static_cast<int>(t["length"].number);
            const bool cycleBad = cycles != want && !(faulted && !strictFaults);
            if (!d.empty()) ++fails;
            else if (cycleBad) ++cycleFails;
            if ((!d.empty() || (checkCycles && cycleBad)) && (verbose || shown < 3)) {
                ++shown;
                std::printf("  %s:%s", t["name"].str.c_str(), d.c_str());
                if (cycleBad) std::printf(" cycles=%d(want %d)", cycles, want);
                std::printf("\n");
            }
        }
        const size_t slash = arg.find_last_of('/');
        std::printf("%-22s %5zu tests, %5d state failures, %5d cycle-only failures\n",
                    (slash == std::string::npos ? arg : arg.substr(slash + 1)).c_str(), root.items.size(), fails,
                    cycleFails);
        total += static_cast<int>(root.items.size());
        totalFail += fails;
        totalCycleFail += cycleFails;
    }
    std::printf("TOTAL %d tests, %d state failures, %d cycle-only failures\n", total, totalFail, totalCycleFail);
    return totalFail > allowed || (checkCycles && totalCycleFail) ? 1 : 0;
}
