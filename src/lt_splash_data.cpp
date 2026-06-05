// Default: no embedded splash bitmap → ltShowSplash() draws a GPU wordmark.
// To embed a real splash, run:  python3 tools/png_to_fbox8.py <image.png>
// which regenerates this file with w/h set and 8bpp (RGB332) pixel data.
#include "lt_assets.hpp"

const uint16_t lt_splash_w = 0;
const uint16_t lt_splash_h = 0;
const uint8_t  lt_splash_data[] = { 0 };
