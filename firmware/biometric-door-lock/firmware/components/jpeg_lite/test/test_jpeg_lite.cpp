// Host test for jpeg_lite. Structural checks here; tests/run_host_tests.sh also decodes the output with Pillow when
// python3 + Pillow are present (pixel colours are checked there).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "jpeg_lite.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    const int W = 240, H = 240;
    std::vector<uint8_t> rgb565(W * H * 2);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) { // red-ish left half, blue-ish right half, with a gradient
            unsigned r = x < W / 2 ? 28 : 2, g = (y * 63) / H, b = x < W / 2 ? 3 : 28;
            unsigned v = (r << 11) | (g << 5) | b;
            rgb565[(y * W + x) * 2] = (uint8_t)(v >> 8);      // big endian, like the camera pipeline
            rgb565[(y * W + x) * 2 + 1] = (uint8_t)v;
        }
    std::vector<uint8_t> out(40000);
    int n = jpeglite_encode(rgb565.data(), W, H, JPEGLITE_RGB565_BE, 40, out.data(), out.size());
    CHECK(n > 500 && n < 30000);
    CHECK(out[0] == 0xFF && out[1] == 0xD8);          // SOI
    CHECK(out[n - 2] == 0xFF && out[n - 1] == 0xD9);  // EOI
    printf("jpeg_lite: %dx%d -> %d bytes\n", W, H, n);
    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); fwrite(out.data(), 1, n, f); fclose(f); }

    // too small a buffer and bad arguments fail cleanly instead of overrunning
    CHECK(jpeglite_encode(rgb565.data(), W, H, JPEGLITE_RGB565_BE, 40, out.data(), 100) == -1);
    CHECK(jpeglite_encode(nullptr, W, H, JPEGLITE_RGB565_BE, 40, out.data(), out.size()) == -1);
    CHECK(jpeglite_encode(rgb565.data(), 0, H, JPEGLITE_RGB565_BE, 40, out.data(), out.size()) == -1);
    CHECK(jpeglite_encode(rgb565.data(), W, H, JPEGLITE_RGB565_BE, 0, out.data(), out.size()) == -1);
    // odd size (not a multiple of 8) still produces a valid stream
    std::vector<uint8_t> rgb(13 * 9 * 3, 200);
    n = jpeglite_encode(rgb.data(), 13, 9, JPEGLITE_RGB888, 80, out.data(), out.size());
    CHECK(n > 100 && out[n - 2] == 0xFF && out[n - 1] == 0xD9);
    if (argc > 2) { FILE *f = fopen(argv[2], "wb"); fwrite(out.data(), 1, n, f); fclose(f); }
    printf("jpeg_lite: all tests passed\n");
    return 0;
}
