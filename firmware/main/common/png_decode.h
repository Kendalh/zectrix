/**
 * @file png_decode.h
 * @brief Minimal PNG decoder (8-bit, non-interlaced) with its own inflate: hardware-free, shared with the simulator.
 *
 * The AI painter downloads a PNG from the image model and only needs it as RGB rows to scale and dither down to the
 * 400x300 four-colour panel. ESP-IDF ships no zlib and LVGL's lodepng allocates through lv_malloc (a small pool), so the
 * few hundred lines needed live here. Memory: the compressed file plus the inflated scanlines (height x (1 + width x bytes
 * per pixel)); nothing else is proportional to the image size.
 */

#ifndef PNG_DECODE_H
#define PNG_DECODE_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace png {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3, alpha composited on white
};

/** Decode an 8-bit grey / RGB / palette / grey+alpha / RGBA PNG. @return false with *error (ASCII) otherwise. */
bool Decode(const uint8_t* data, size_t size, Image* out, std::string* error);

/** RFC 1951 inflate of a raw deflate stream; `expected` is only a reserve hint. Exposed for tests. */
bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>* out, size_t expected, std::string* error);

}  // namespace png

#endif  // PNG_DECODE_H
