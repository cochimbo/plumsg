/* Computations the 32X test program runs on the SH-2 and the test runs natively: the
   results must match bit for bit. Plain C that is also valid C++. They lean on what a
   compiler makes the SH-2 do: libgcc's division (DIV0S/DIV1), 64-bit arithmetic,
   soft-float, jump tables, recursion, byte and word accesses. */

static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static uint32_t lcg(uint32_t* seed) {
    *seed = *seed * 1103515245u + 12345u;
    return *seed >> 8;
}

static uint32_t crc32Of(const uint8_t* data, int size) {
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int pick(int v) {
    switch (v) {
        case 0: return 11;
        case 1: return -7;
        case 2: return 300;
        case 3: return 5;
        case 4: return -1000;
        case 5: return 42;
        case 6: return 9;
        case 7: return 1 << 20;
        default: return v * 3;
    }
}

static void runChecks(uint32_t* out) {
    uint32_t seed = 1234;
    out[0] = fib(20);

    int32_t sdiv = 0;
    uint32_t udiv = 0;
    for (int i = -500; i < 500; i += 7) {
        int32_t a = i * 7919, b = (i % 13) - 6;
        if (b == 0) b = 3;
        sdiv += a / b + a % b;
        udiv += (uint32_t)(a) / (uint32_t)(b * b + 1) + (uint32_t)a % 97u;
    }
    out[1] = (uint32_t)sdiv;
    out[2] = udiv;

    uint64_t big = 0x0123456789ABCDEFull;
    for (int i = 0; i < 20; ++i) big = big * 6364136223846793005ull + (uint64_t)lcg(&seed);
    out[3] = (uint32_t)big;
    out[4] = (uint32_t)(big >> 32);
    out[5] = (uint32_t)(big / 1000003u);
    out[6] = (uint32_t)((int64_t)(int32_t)lcg(&seed) * -987654321 >> 17);

    uint8_t buffer[512];
    for (int i = 0; i < 512; ++i) buffer[i] = (uint8_t)lcg(&seed);
    out[7] = crc32Of(buffer, 512);

    int16_t sorted[64];
    for (int i = 0; i < 64; ++i) sorted[i] = (int16_t)lcg(&seed);
    for (int i = 1; i < 64; ++i) {
        int16_t v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = v;
    }
    uint32_t sortSum = 0;
    for (int i = 0; i < 64; ++i) sortSum = sortSum * 31u + (uint16_t)sorted[i];
    out[8] = sortSum;

    int picked = 0;
    for (int i = 0; i < 12; ++i) picked += pick(i) * (i + 1);
    out[9] = (uint32_t)picked;

    float f = 0.0f;
    for (int i = 1; i < 50; ++i) f += (float)i * 0.5f / (float)(i + 3) - 0.125f;
    uint32_t fbits;
    memcpy(&fbits, &f, 4);
    out[10] = fbits;
    double d = 1.0;
    for (int i = 1; i < 20; ++i) d = d * 1.0625 + 1.0 / (double)i;
    uint64_t dbits;
    memcpy(&dbits, &d, 8);
    out[11] = (uint32_t)dbits;
    out[12] = (uint32_t)(dbits >> 32);
    out[13] = (uint32_t)(int32_t)(d * -1000.0);

    uint32_t bits = 0, rot = 0x80000001u;
    for (int i = 0; i < 33; ++i) {
        uint32_t v = lcg(&seed) ^ (rot = (rot << 3) | (rot >> 29));
        while (v) {
            bits += v & 1;
            v >>= 1;
        }
    }
    out[14] = bits ^ rot;

    char text[32];
    const char* word = "plumsg 32X";
    int len = 0;
    while (word[len]) {
        text[len] = (char)(word[len] ^ 0x20);
        ++len;
    }
    signed char acc = 0;
    for (int i = 0; i < len; ++i) acc = (signed char)(acc * 3 + text[i]);
    out[15] = (uint32_t)(int32_t)acc + (uint32_t)len * 1000u;
}
