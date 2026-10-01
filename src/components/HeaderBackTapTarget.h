#pragma once

// Tap rect of the back button BaseTheme::drawHeader paints on touch boards.
// The header's FreeInkUI frame is non-interactive, so the rect is recorded
// here at draw time and consumed by MappedInputManager's Back mapping; every
// screen that draws a titled header gets tap-to-go-back without its own
// routing. Cleared on activity exit (and by headerless draws) so a stale rect
// never eats taps on a new screen. Written by the render task and read by the
// loop task; a torn int read at worst misroutes one tap on a band that is
// being redrawn, so no lock is taken.
namespace HeaderBackTapTarget {
inline int x = 0;
inline int y = 0;
inline int w = 0;
inline int h = 0;

inline void set(const int newX, const int newY, const int newW, const int newH) {
  x = newX;
  y = newY;
  w = newW;
  h = newH;
}

inline void clear() { w = 0; }

// Finger slop around the drawn button: a near miss would otherwise land on the
// status band beside it, which opens the light panel. Barely any downward, so
// the first content row below the header keeps its taps.
inline constexpr int SLOP = 28;
inline constexpr int SLOP_BELOW = 4;

inline bool contains(const int tx, const int ty) {
  return w > 0 && tx >= x - SLOP && tx < x + w + SLOP && ty >= y - SLOP && ty < y + h + SLOP_BELOW;
}
}  // namespace HeaderBackTapTarget
