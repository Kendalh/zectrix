#include "png_decode.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace png {

namespace {

// ---- inflate (canonical Huffman decoding in the style of zlib's puff.c: small, a bit slow, no big tables) ----------

constexpr int kMaxBits = 15;
constexpr int kMaxLCodes = 286;
constexpr int kMaxDCodes = 30;
constexpr int kFixLCodes = 288;

struct Huffman {
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[kFixLCodes];
};

struct Stream {
    const uint8_t* in;
    size_t size;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    std::vector<uint8_t>* out;
    const char* error = nullptr;
};

int Bits(Stream& s, int need) {
    uint32_t val = s.bitbuf;
    while (s.bitcnt < need) {
        if (s.pos >= s.size) {
            s.error = "truncated deflate stream";
            return -1;
        }
        val |= static_cast<uint32_t>(s.in[s.pos++]) << s.bitcnt;
        s.bitcnt += 8;
    }
    s.bitbuf = val >> need;
    s.bitcnt -= need;
    return static_cast<int>(val & ((1u << need) - 1));
}

int Decode(Stream& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        const int bit = Bits(s, 1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s.error = "bad huffman code";
    return -1;
}

// @return 0 complete code, >0 incomplete, <0 over-subscribed
int Construct(Huffman* h, const int16_t* length, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int sym = 0; sym < n; ++sym) ++h->count[length[sym]];
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; ++sym)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = static_cast<uint16_t>(sym);
    return left;
}

bool Codes(Stream& s, const Huffman& lencode, const Huffman& distcode) {
    static const int16_t kLens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int16_t kLext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int16_t kDists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int16_t kDext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    std::vector<uint8_t>& out = *s.out;
    for (;;) {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) {
                s.error = "bad length symbol";
                return false;
            }
            int extra = Bits(s, kLext[sym]);
            if (extra < 0) return false;
            const int len = kLens[sym] + extra;
            sym = Decode(s, distcode);
            if (sym < 0) return false;
            if (sym >= kMaxDCodes) {
                s.error = "bad distance symbol";
                return false;
            }
            extra = Bits(s, kDext[sym]);
            if (extra < 0) return false;
            const size_t dist = static_cast<size_t>(kDists[sym] + extra);
            if (dist > out.size()) {
                s.error = "distance too far back";
                return false;
            }
            size_t from = out.size() - dist;
            for (int i = 0; i < len; ++i) out.push_back(out[from + i]);  // may overlap: byte by byte
        }
    }
}

bool Stored(Stream& s) {
    s.bitbuf = 0;
    s.bitcnt = 0;
    if (s.pos + 4 > s.size) {
        s.error = "truncated stored block";
        return false;
    }
    const unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8);
    const unsigned nlen = s.in[s.pos + 2] | (s.in[s.pos + 3] << 8);
    s.pos += 4;
    if (len != (~nlen & 0xFFFF) || s.pos + len > s.size) {
        s.error = "bad stored block";
        return false;
    }
    s.out->insert(s.out->end(), s.in + s.pos, s.in + s.pos + len);
    s.pos += len;
    return true;
}

bool Fixed(Stream& s) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        int16_t lengths[kFixLCodes];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < kFixLCodes; ++sym) lengths[sym] = 8;
        Construct(&lencode, lengths, kFixLCodes);
        for (sym = 0; sym < kMaxDCodes; ++sym) lengths[sym] = 5;
        Construct(&distcode, lengths, kMaxDCodes);
        built = true;
    }
    return Codes(s, lencode, distcode);
}

bool Dynamic(Stream& s) {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = Bits(s, 5) + 257;
    const int ndist = Bits(s, 5) + 1;
    const int ncode = Bits(s, 4) + 4;
    if (s.error) return false;
    if (nlen > kMaxLCodes || ndist > kMaxDCodes) {
        s.error = "bad dynamic block counts";
        return false;
    }
    int16_t lengths[kMaxLCodes + kMaxDCodes];
    int index = 0;
    for (; index < ncode; ++index) {
        const int v = Bits(s, 3);
        if (v < 0) return false;
        lengths[kOrder[index]] = static_cast<int16_t>(v);
    }
    for (; index < 19; ++index) lengths[kOrder[index]] = 0;
    Huffman lencode, distcode;
    if (Construct(&lencode, lengths, 19) != 0) {
        s.error = "bad code-length code";
        return false;
    }
    index = 0;
    while (index < nlen + ndist) {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[index++] = static_cast<int16_t>(sym);
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) {
                    s.error = "repeat with no previous length";
                    return false;
                }
                len = lengths[index - 1];
                rep = 3 + Bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + Bits(s, 3);
            } else {
                rep = 11 + Bits(s, 7);
            }
            if (s.error) return false;
            if (index + rep > nlen + ndist) {
                s.error = "too many lengths";
                return false;
            }
            while (rep--) lengths[index++] = static_cast<int16_t>(len);
        }
    }
    if (lengths[256] == 0) {
        s.error = "no end-of-block code";
        return false;
    }
    int err = Construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
        s.error = "bad literal/length code";
        return false;
    }
    err = Construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
        s.error = "bad distance code";
        return false;
    }
    return Codes(s, lencode, distcode);
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

int Paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

}  // namespace

bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t expected, std::string* error) {
    Stream s;
    s.in = data;
    s.size = size;
    s.out = out;
    out->clear();
    out->reserve(expected);
    int last;
    do {
        last = Bits(s, 1);
        const int type = last < 0 ? -1 : Bits(s, 2);
        bool ok = false;
        if (type == 0) ok = Stored(s);
        else if (type == 1) ok = Fixed(s);
        else if (type == 2) ok = Dynamic(s);
        else if (!s.error) s.error = "bad block type";
        if (!ok) {
            if (error) *error = s.error ? s.error : "inflate failed";
            return false;
        }
    } while (!last);
    return true;
}

bool Decode(const uint8_t* data, size_t size, Image* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 || memcmp(data, kSig, 8) != 0) return fail("not a PNG");

    uint32_t width = 0, height = 0;
    int color_type = -1, bit_depth = 0, interlace = 0;
    std::vector<uint8_t> idat;
    uint8_t palette[256 * 3] = {};
    size_t pos = 8;
    bool seen_end = false;
    while (pos + 12 <= size && !seen_end) {
        const uint32_t len = Be32(data + pos);
        const char* type = reinterpret_cast<const char*>(data + pos + 4);
        const uint8_t* body = data + pos + 8;
        if (len > size - pos - 12) return fail("truncated chunk");
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            width = Be32(body);
            height = Be32(body + 4);
            bit_depth = body[8];
            color_type = body[9];
            interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            memcpy(palette, body, std::min<size_t>(len, sizeof(palette)));
        } else if (!memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!memcmp(type, "IEND", 4)) {
            seen_end = true;
        }
        pos += 12 + len;
    }
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return fail("bad PNG size");
    if (bit_depth != 8) return fail("only 8-bit PNG is supported");
    if (interlace != 0) return fail("interlaced PNG is not supported");
    int channels;
    switch (color_type) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return fail("unsupported PNG colour type");
    }
    if (idat.size() < 6) return fail("no image data");

    const size_t stride = static_cast<size_t>(width) * channels;
    const size_t raw_size = (stride + 1) * height;
    std::vector<uint8_t> raw;
    std::string inflate_error;
    // Skip the 2-byte zlib header (and ignore the trailing Adler-32).
    if (!Inflate(idat.data() + 2, idat.size() - 2, &raw, raw_size, &inflate_error)) {
        if (error) *error = "inflate: " + inflate_error;
        return false;
    }
    idat = std::vector<uint8_t>();  // free before allocating the output
    if (raw.size() < raw_size) return fail("PNG data too short");

    out->width = width;
    out->height = height;
    out->rgb.assign(static_cast<size_t>(width) * height * 3, 255);
    for (uint32_t y = 0; y < height; ++y) {  // unfilter in place, then convert the row
        uint8_t* row = raw.data() + y * (stride + 1);
        const int filter = row[0];
        uint8_t* cur = row + 1;
        const uint8_t* prev = y ? raw.data() + (y - 1) * (stride + 1) + 1 : nullptr;
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= static_cast<size_t>(channels) ? cur[i - channels] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= static_cast<size_t>(channels)) ? prev[i - channels] : 0;
            int add = 0;
            switch (filter) {
                case 0: break;
                case 1: add = a; break;
                case 2: add = b; break;
                case 3: add = (a + b) / 2; break;
                case 4: add = Paeth(a, b, c); break;
                default: return fail("bad PNG filter");
            }
            cur[i] = static_cast<uint8_t>(cur[i] + add);
        }
        uint8_t* dst = out->rgb.data() + static_cast<size_t>(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            int r, g, b, alpha = 255;
            const uint8_t* px = cur + static_cast<size_t>(x) * channels;
            switch (color_type) {
                case 0: r = g = b = px[0]; break;
                case 2: r = px[0]; g = px[1]; b = px[2]; break;
                case 3: r = palette[px[0] * 3]; g = palette[px[0] * 3 + 1]; b = palette[px[0] * 3 + 2]; break;
                case 4: r = g = b = px[0]; alpha = px[1]; break;
                default: r = px[0]; g = px[1]; b = px[2]; alpha = px[3]; break;
            }
            if (alpha != 255) {  // composite on white: e-ink paper
                r = (r * alpha + 255 * (255 - alpha)) / 255;
                g = (g * alpha + 255 * (255 - alpha)) / 255;
                b = (b * alpha + 255 * (255 - alpha)) / 255;
            }
            dst[x * 3] = static_cast<uint8_t>(r);
            dst[x * 3 + 1] = static_cast<uint8_t>(g);
            dst[x * 3 + 2] = static_cast<uint8_t>(b);
        }
    }
    return true;
}

}  // namespace png
