#pragma once

#include <string>

// Cover thumbnails for the Library's Covers view, shared with anything that
// wants those thumbs ready ahead of time (home sync pre-generates them for the
// books it downloads).
//
// One fixed cover size on every board and orientation: the grid only varies
// how many columns and rows fit. Thumbs must be generated at exactly the drawn
// height (rescaling a dithered 1-bit image aliases badly), and a fixed height
// is what lets a thumb made outside the Library land at the size it will use.
namespace library_covers {

// Drawn cover slot (2:3).
constexpr int COVER_HEIGHT = 160;
constexpr int COVER_WIDTH = COVER_HEIGHT * 2 / 3;
// Generation height: the slot plus the same +8 bleed the Home cover grid uses,
// so the centered art never exposes a slot edge after its rightward nudge.
constexpr int THUMB_HEIGHT = COVER_HEIGHT + 8;

// Thumb path for a book at THUMB_HEIGHT; empty for formats without covers.
// Only derives the cache path, no parsing.
std::string thumbPathFor(const std::string& bookPath);

// Generates the book's thumb when it is missing. Blocking (one book, typically
// well under a second); true when the thumb exists afterwards. Not every book
// has a cover, so false is an ordinary outcome.
bool generateThumb(const std::string& bookPath);

// Thumb generation decodes JPEG/PNG covers; skip it when memory is tight
// rather than risk the caller's own work.
bool heapAllowsThumb();

}  // namespace library_covers
