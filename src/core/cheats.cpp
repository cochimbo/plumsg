#include "cheats.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace plumsg {

namespace {

// The Genesis Game Genie's alphabet: each letter is 5 bits (no I, O, Q or U).
constexpr const char kGenie[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";

int genieValue(char c) {
    const char* p = std::strchr(kGenie, c);
    return c && p ? static_cast<int>(p - kGenie) : -1;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool allHex(const std::string& s) {
    for (char c : s)
        if (hexValue(c) < 0) return false;
    return !s.empty();
}

uint32_t parseHex(const std::string& s) {
    uint32_t v = 0;
    for (char c : s) v = v << 4 | static_cast<uint32_t>(hexValue(c));
    return v;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// The 40 bits of the 8 letters, as Charles MacDonald's Game Genie notes lay them out:
//   ijklm nopIJ KLMNO PABCD EFGHd efgha bcQRS TUVWX
// address = ABCDEFGH IJKLMNOP QRSTUVWX, value = abcdefgh ijklmnop. The same shifts as
// Genesis Plus GX's decode_cheat().
bool decodeGenie(const std::string& letters, CheatPatch* out) {
    uint32_t address = 0, value = 0;
    for (int i = 0; i < 8; ++i) {
        const int v = genieValue(letters[static_cast<size_t>(i)]);
        if (v < 0) return false;
        const uint32_t n = static_cast<uint32_t>(v);
        switch (i) {
            case 0: value |= n << 3; break;
            case 1: value |= n >> 2; address |= (n & 3) << 14; break;
            case 2: address |= n << 9; break;
            case 3: address |= (n & 0xF) << 20 | (n >> 4) << 8; break;
            case 4: value |= (n & 1) << 12; address |= (n >> 1) << 16; break;
            case 5: value |= (n & 1) << 15 | (n >> 1) << 8; break;
            case 6: value |= (n >> 3) << 13; address |= (n & 7) << 5; break;
            case 7: address |= n; break;
        }
    }
    out->address = address;
    out->value = static_cast<uint16_t>(value);
    out->byte = false;
    return true;
}

}  // namespace

std::string normalizeCheat(const std::string& code) {
    std::string s;
    for (char c : code)
        if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string encodeGameGenie(uint32_t address, uint16_t value) {
    const uint32_t a = address, d = value;
    const uint32_t n[8] = {
        (d >> 3) & 31,
        ((d & 7) << 2) | ((a >> 14) & 3),
        (a >> 9) & 31,
        (((a >> 8) & 1) << 4) | ((a >> 20) & 15),
        (((a >> 16) & 15) << 1) | ((d >> 12) & 1),
        (((d >> 8) & 15) << 1) | ((d >> 15) & 1),
        (((d >> 13) & 3) << 3) | ((a >> 5) & 7),
        a & 31,
    };
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (i == 4) s += '-';
        s += kGenie[n[i]];
    }
    return s;
}

bool decodeCheat(const std::string& code, CheatPatch* out, std::string* error) {
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    const std::string s = normalizeCheat(code);
    if (s.empty()) return fail("Escribe un código.");

    // Action Replay: 6 hex digits of address, then 4 (a word) or 2 (a byte) of value,
    // with or without a ':' between them. Eight hex digits with no separator are only
    // taken as Action Replay when they point at RAM (FFxxxx), since a Game Genie code
    // can be all hex digits too.
    std::string address, value;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) {
        address = s.substr(0, colon);
        value = s.substr(colon + 1);
    } else if (s.size() == 10 && allHex(s)) {
        address = s.substr(0, 6);
        value = s.substr(6);
    } else if (s.size() == 8 && allHex(s) && s.compare(0, 2, "FF") == 0) {
        address = s.substr(0, 6);
        value = s.substr(6);
    }
    if (!address.empty() || colon != std::string::npos) {
        if (address.size() != 6 || !allHex(address) || (value.size() != 4 && value.size() != 2) || !allHex(value))
            return fail("Un código Action Replay es como FF0A12:0009 (6 cifras hexadecimales y luego 4, o 2 para un byte).");
        CheatPatch p;
        p.address = parseHex(address);
        p.value = static_cast<uint16_t>(parseHex(value));
        p.byte = value.size() == 2;
        if (p.address >= 0x400000 && p.address < 0xE00000)
            return fail("El código apunta fuera de la ROM y de la RAM.");
        if (!p.byte && (p.address & 1)) return fail("Un código de 4 cifras necesita una dirección par.");
        *out = p;
        return true;
    }

    std::string letters = s;
    if (letters.size() == 9 && letters[4] == '-') letters.erase(4, 1);
    if (letters.size() != 8)
        return fail("Un código Game Genie tiene 8 letras (ABCD-EFGH) y uno Action Replay es como FF0A12:0009.");
    for (char c : letters)
        if (genieValue(c) < 0) return fail("Los códigos Game Genie no usan las letras I, O, Q ni U.");
    CheatPatch p;
    decodeGenie(letters, &p);
    if (p.address >= 0x400000) return fail("El código Game Genie apunta fuera de la ROM.");
    if (p.address & 1) return fail("El código Game Genie apunta a una dirección impar.");
    *out = p;
    return true;
}

std::string formatCheats(const std::vector<Cheat>& cheats) {
    std::ostringstream out;
    out << "# Trucos de plumsg para este juego. Una línea por código: si o no (activado),\n"
           "# el código (Game Genie ABCD-EFGH o Action Replay FF0A12:0009) y un nombre.\n";
    for (const Cheat& c : cheats) out << (c.enabled ? "si " : "no ") << c.code << (c.name.empty() ? "" : " ") << c.name << "\n";
    return out.str();
}

std::vector<Cheat> parseCheats(const std::string& text) {
    std::vector<Cheat> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        std::istringstream words(line);
        std::string first, code;
        words >> first;
        Cheat c;
        if (first == "si" || first == "sí" || first == "no") {
            c.enabled = first != "no";
            words >> code;
        } else {
            code = first;
        }
        if (code.empty()) continue;
        c.code = normalizeCheat(code);
        std::string rest;
        std::getline(words, rest);
        c.name = trim(rest);
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace plumsg
