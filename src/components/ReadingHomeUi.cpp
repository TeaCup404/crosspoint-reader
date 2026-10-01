#include "ReadingHomeUi.h"

#if HOMESYNC

#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "UITheme.h"
#include "homesync/HomeSync.h"
#include "icons/blocks.h"
#include "icons/book.h"
#include "icons/customListIcons.h"
#include "icons/folder.h"
#include "icons/hotspot.h"
#include "icons/library.h"
#include "icons/listIcons.h"
#include "icons/recent.h"
#include "icons/settings2.h"
#include "icons/transfer.h"
#include "util/BookProgress.h"
#include "util/LibraryCoverThumbs.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId SELECT = 1;
// Hero: the Library's cover size, so its thumb is usually there already.
constexpr int16_t COVER_H = library_covers::COVER_HEIGHT;
constexpr int16_t COVER_W = library_covers::COVER_WIDTH;
// Recent strip: smaller, so four fit with air between them. Thumbs are made
// at exactly this slot (+8 bleed, as for the Library): never rescaled.
constexpr int16_t STRIP_H = 136;
constexpr int16_t STRIP_W = STRIP_H * 2 / 3;
constexpr int STRIP_THUMB_H = STRIP_H + 8;
// Focus ring around a strip cover: gap outside the cover, then the ring.
constexpr int16_t RING_GAP = 4;
constexpr int16_t RING_W = 3;
constexpr int16_t RING = RING_GAP + RING_W;
constexpr int16_t HERO_PAD = 6;
constexpr int16_t ICON = 32;
constexpr int16_t TILE_MAX_H = 104;
constexpr uint8_t RADIUS = 8;

struct TileInfo {
  const char* label;
  const uint8_t* icon;  // legacy 32px icon; nullptr = Light (SDK sun icon)
};
const TileInfo TILES[ReadingHomeUi::TILE_COUNT] = {
    {"Library", LibraryIcon}, {"Files", FolderIcon},       {"Store", BlocksIcon},      {"Sync", RecentIcon},
    {"Server", HotspotIcon},  {"Transfer", TransferIcon}, {"Settings", Settings2Icon}, {"Light", nullptr},
};

// Focus everywhere is a heavy outline: a dithered fill muddies the text, and
// a solid black one would swallow the black-only legacy icons.
fui::StyleSet focusStyles() {
  fui::StyleSet s;
  s.explicitlySet = true;
  s.selected.border = fui::Paint::solid(fui::Color::Black);
  s.selected.borderWidth = 3;
  s.selected.radius = RADIUS;
  s.active = s.selected;
  return s;
}

std::string ago(time_t seconds) {
  char buf[16];
  if (seconds < 60) return "just now";
  if (seconds < 3600) {
    snprintf(buf, sizeof(buf), "%dm ago", static_cast<int>(seconds / 60));
  } else if (seconds < 86400) {
    snprintf(buf, sizeof(buf), "%dh ago", static_cast<int>(seconds / 3600));
  } else {
    snprintf(buf, sizeof(buf), "%dd ago", static_cast<int>(seconds / 86400));
  }
  return buf;
}
}  // namespace

ReadingHomeUi::ReadingHomeUi(GfxRenderer& renderer) : UiAppHost(renderer), coverCache(renderer), renderer(renderer) {}

void ReadingHomeUi::begin(const std::vector<RecentBook>& recent, const bool opds) {
  books = &recent;
  hasOpds = opds;
  hasLight = Frontlight.present();
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &ReadingHomeUi::onAction, this);
  app.setScreen(&ReadingHomeUi::screenFn, this);
  for (int i = 0; i < bookCount(); ++i) {
    thumbPaths[i] = library_covers::thumbPathFor((*books)[i].path, thumbHeight(i));
    thumbReady[i] = !thumbPaths[i].empty() && Storage.exists(thumbPaths[i].c_str());
    thumbTried[i] = false;
  }
  progress = bookCount() > 0 ? loadBookProgress(books->front().path) : -1;
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
  buildStatusLine();
}

int ReadingHomeUi::thumbHeight(const int book) { return book == 0 ? library_covers::THUMB_HEIGHT : STRIP_THUMB_H; }

int ReadingHomeUi::bookCount() const {
  return books ? std::min(static_cast<int>(books->size()), static_cast<int>(MAX_BOOKS)) : 0;
}

ReadingHomeUi::Tile ReadingHomeUi::tileAt(const int index) const {
  const int t = index - bookCount();
  return t >= 0 && t < TILE_COUNT ? static_cast<Tile>(t) : TILE_COUNT;
}

bool ReadingHomeUi::isEnabled(const int index) const {
  if (index < 0) return false;
  if (index < bookCount()) return index == 0 || stripShown;
  switch (tileAt(index)) {
    case TILE_STORE:
    case TILE_SYNC:
      return hasOpds;
    case TILE_LIGHT:
      return hasLight;
    case TILE_COUNT:
      return false;
    default:
      return true;
  }
}

int ReadingHomeUi::indexFor(const HomeMenuItem item) const {
  Tile tile;
  switch (item) {
    case HomeMenuItem::LIBRARY:
      tile = TILE_LIBRARY;
      break;
    case HomeMenuItem::FILE_BROWSER:
      tile = TILE_FILES;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      tile = TILE_STORE;
      break;
    case HomeMenuItem::SYNC_LIBRARY:
      tile = TILE_SYNC;
      break;
    case HomeMenuItem::SERVER_DASH:
      tile = TILE_SERVER;
      break;
    case HomeMenuItem::FILE_TRANSFER:
      tile = TILE_TRANSFER;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      tile = TILE_SETTINGS;
      break;
    default:
      return 0;
  }
  const int index = bookCount() + tile;
  return isEnabled(index) ? index : 0;
}

int ReadingHomeUi::step(const int index, const int dir) const {
  const int n = itemCount();
  int i = index;
  for (int k = 0; k < n; ++k) {
    i = (i + dir + n) % n;
    if (isEnabled(i)) return i;
  }
  return index;
}

void ReadingHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<ReadingHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int ReadingHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

bool ReadingHomeUi::generateNextThumb() {
  for (int i = 0; i < bookCount(); ++i) {
    if (thumbReady[i] || thumbTried[i] || thumbPaths[i].empty()) continue;
    if (!library_covers::heapAllowsThumb()) return false;
    thumbTried[i] = true;
    // Coverless books fail fast; keep going so one of them never stalls the rest.
    if (library_covers::generateThumb((*books)[i].path, thumbHeight(i)) && Storage.exists(thumbPaths[i].c_str())) {
      thumbReady[i] = true;
      coverCache.invalidate(i);  // the cache holds the placeholder snapshot
      return true;
    }
  }
  return false;
}

void ReadingHomeUi::buildStatusLine() {
  statusLine.clear();
  const auto add = [this](const std::string& part) {
    if (!statusLine.empty()) statusLine += " \xC2\xB7 ";  // middle dot
    statusLine += part;
  };
  const time_t now = homesync::nowEpoch();
  const auto sync = homesync::lastSyncSummary();
  if (sync.at > 0 && now > 0) add("Synced " + ago(std::max<time_t>(0, now - sync.at)));
  if (sync.downloaded > 0) add(std::to_string(sync.downloaded) + " new");
  const auto dash = homesync::loadDashSummary();
  if (dash.known) {
    std::string part = dash.alarms > 0 ? "Server: " + std::to_string(dash.alarms) + (dash.alarms == 1 ? " alarm" : " alarms")
                                       : std::string("Server OK");
    struct tm t{};
    if (dash.at > 0 && localtime_r(&dash.at, &t)) {
      // snprintf, not strftime: strftime drags several KB of locale code in.
      char hhmm[12];
      if (SETTINGS.clockFormat == 1) {
        const int h = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
        snprintf(hhmm, sizeof(hhmm), " %d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
      } else {
        snprintf(hhmm, sizeof(hhmm), " %02d:%02d", t.tm_hour, t.tm_min);
      }
      part += hhmm;
    }
    add(part);
  }
}

void ReadingHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<ReadingHomeUi*>(user)->draw(screen); }

void ReadingHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto& target = screen.target();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  const int16_t side = std::max<int16_t>(0, static_cast<int16_t>(metrics.headerSidePadding - safe.x));
  screen.insetContent(fui::Insets{theme.spaceSm, side, theme.spaceSm, side});
  // Slot for the status band (drawn at its fixed position below).
  screen.takeTop(static_cast<int16_t>(metrics.batteryBarHeight), theme.spaceMd);

  auto small = theme.smallText;
  coverTitleText = small;
  const int16_t smallH = target.lineHeight(small.font);
  if (!statusLine.empty()) {
    auto status = small;
    status.align = fui::TextAlign::Center;
    const auto rect = screen.takeBottom(smallH, theme.spaceMd);
    target.line(fui::Point{rect.x, static_cast<int16_t>(rect.y - theme.spaceSm)},
                fui::Point{rect.right(), static_cast<int16_t>(rect.y - theme.spaceSm)}, 1,
                fui::Paint::dither(fui::Color::LightGray));
    target.text(rect, statusLine.c_str(), status);
  }

  const auto hero = screen.takeTop(static_cast<int16_t>(COVER_H + 2 * HERO_PAD), theme.spaceLg);
  if (bookCount() > 0) {
    drawHero(screen, hero);
  } else {
    drawEmptyHero(screen, hero);
  }

  // Short screens (landscape): the strip yields to the tiles.
  constexpr int16_t MIN_TILE_BAND = 2 * 56 + 4;
  const int16_t stripNeed = static_cast<int16_t>(smallH + theme.spaceSm + STRIP_H + 2 * RING + theme.spaceLg);
  stripShown = screen.body().height - stripNeed >= MIN_TILE_BAND;
  if (bookCount() > 1 && stripShown) {
    auto label = small;
    label.bold = true;
    target.text(screen.takeTop(smallH, theme.spaceSm), "Recent", label);
    drawStrip(screen, screen.takeTop(static_cast<int16_t>(STRIP_H + 2 * RING), theme.spaceLg));
  }

  drawTiles(screen, screen.body());

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);
}

void ReadingHomeUi::drawHero(UiScreen& screen, fui::Rect rect) {
  const auto& theme = screen.theme();
  const auto& book = books->front();
  card.title = book.title.c_str();
  card.author = book.author.empty() ? nullptr : book.author.c_str();
  card.meta = "Continue \xE2\x80\xBA";  // single right angle quote
  card.progressLabel = progress >= 0 ? progressText : nullptr;
  card.progress = std::max(0, progress);
  card.progressMax = progress >= 0 ? 100 : 0;
  card.action = SELECT;
  card.value = 0;
  card.state = selected == 0 ? fui::StateSelected : fui::StateNormal;
  card.styles = focusStyles();
  card.titleText = theme.bodyText;
  card.titleText.bold = true;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.metaText = theme.smallText;
  card.metaText.bold = true;
  card.progressText = theme.smallText;
  card.progressHeight = 6;
  card.padding = fui::Insets{HERO_PAD, HERO_PAD, HERO_PAD, HERO_PAD};
  card.gap = theme.spaceLg;
  card.centerTextOnCover = true;
  card.coverSize = fui::Size{COVER_W, COVER_H};
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, fui::Rect cover, const fui::BookCardProps&, void* user) {
    static_cast<ReadingHomeUi*>(user)->drawCover(target, cover, 0);
    return true;
  };
  fui::bookCard(screen.frame(), rect, card);
}

void ReadingHomeUi::drawEmptyHero(UiScreen& screen, const fui::Rect rect) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  target.stroke(rect, fui::Paint::dither(fui::Color::LightGray), 2, RADIUS);
  auto title = theme.bodyText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.smallText;
  message.align = fui::TextAlign::Center;
  const int16_t titleH = target.lineHeight(title.font);
  const int16_t messageH = target.lineHeight(message.font);
  int16_t y = static_cast<int16_t>(rect.y + (rect.height - ICON - theme.spaceMd - titleH - messageH) / 2);
  renderer.drawIcon(BookIcon, rect.x + (rect.width - ICON) / 2, y, ICON);
  y = static_cast<int16_t>(y + ICON + theme.spaceMd);
  target.text(fui::Rect{rect.x, y, rect.width, titleH}, tr(STR_NO_OPEN_BOOK), title);
  y = static_cast<int16_t>(y + titleH);
  target.text(fui::Rect{rect.x, y, rect.width, messageH}, "Pick one in Library", message);
}

void ReadingHomeUi::drawStrip(UiScreen& screen, const fui::Rect rect) {
  auto& target = screen.target();
  // Four fixed slots spread edge to edge; empty slots stay blank.
  const int16_t span = static_cast<int16_t>(rect.width - 2 * RING - STRIP_W);
  for (int slot = 0; slot < RECENT_COUNT; ++slot) {
    const int book = slot + 1;
    if (book >= bookCount()) break;
    const fui::Rect cover{static_cast<int16_t>(rect.x + RING + span * slot / (RECENT_COUNT - 1)),
                          static_cast<int16_t>(rect.y + RING), STRIP_W, STRIP_H};
    const fui::Rect cell{static_cast<int16_t>(cover.x - RING), rect.y, static_cast<int16_t>(STRIP_W + 2 * RING),
                         rect.height};
    screen.frame().hit(cell, SELECT, static_cast<int16_t>(book));
    drawCover(target, cover, book);
    if (selected == book) {
      target.stroke(fui::Rect{static_cast<int16_t>(cover.x - RING), static_cast<int16_t>(cover.y - RING),
                              static_cast<int16_t>(cover.width + 2 * RING),
                              static_cast<int16_t>(cover.height + 2 * RING)},
                    fui::Paint::solid(fui::Color::Black), RING_W, 4);
    }
  }
}

void ReadingHomeUi::drawCover(fui::DrawTarget& target, const fui::Rect rect, const int book) {
  if (book < 0 || book >= bookCount()) return;
  if (thumbReady[book]) {
    coverCache.paint(rect, book, thumbPaths[book]);
    return;
  }
  // No thumb (yet): a plain card with the title, so the slot still says what it is.
  target.fill(rect, fui::Paint::solid(fui::Color::White));
  target.stroke(rect, fui::Paint::solid(fui::Color::Black), 1);
  target.fill(fui::Rect{rect.x, rect.y, 4, rect.height}, fui::Paint::dither(fui::Color::DarkGray));
  auto title = coverTitleText;
  title.align = fui::TextAlign::Center;
  title.maxLines = 5;
  const auto& b = (*books)[book];
  target.text(rect.inset(fui::Insets{8, 3, 8, 6}), b.title.c_str(), title);
}

void ReadingHomeUi::drawTiles(UiScreen& screen, const fui::Rect rect) {
  constexpr int COLS = 4;
  constexpr int ROWS = TILE_COUNT / COLS;
  const int16_t gap = screen.theme().spaceSm;
  const int16_t tileW = static_cast<int16_t>((rect.width - (COLS - 1) * gap) / COLS);
  const int16_t tileH =
      static_cast<int16_t>(std::max(0, std::min<int>(TILE_MAX_H, (rect.height - (ROWS - 1) * gap) / ROWS)));
  if (tileH <= 0) return;
  // Top-anchored: the tiles keep one place whatever the strip shows.
  const int16_t top = rect.y;
  for (int t = 0; t < TILE_COUNT; ++t) {
    const int col = t % COLS;
    const int row = t / COLS;
    // Last column takes the rounding remainder so the grid spans the full width.
    const int16_t x = static_cast<int16_t>(rect.x + col * (tileW + gap));
    const int16_t w = col == COLS - 1 ? static_cast<int16_t>(rect.right() - x) : tileW;
    drawTile(screen, fui::Rect{x, static_cast<int16_t>(top + row * (tileH + gap)), w, tileH}, static_cast<Tile>(t),
             bookCount() + t);
  }
}

void ReadingHomeUi::drawTile(UiScreen& screen, const fui::Rect rect, const Tile tile, const int index) {
  auto& target = screen.target();
  const bool enabled = isEnabled(index);
  const bool focused = enabled && selected == index;
  if (focused) {
    target.stroke(rect, fui::Paint::solid(fui::Color::Black), 3, RADIUS);
  } else {
    target.stroke(rect, enabled ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::LightGray), 1,
                  RADIUS);
  }
  if (enabled) screen.frame().hit(rect, SELECT, static_cast<int16_t>(index));

  auto label = screen.theme().smallText;
  label.align = fui::TextAlign::Center;
  label.bold = focused;
  if (!enabled) label.color = fui::Color::DarkGray;
  const int16_t labelH = target.lineHeight(label.font);
  const int16_t iconY = static_cast<int16_t>(rect.y + (rect.height - ICON - screen.theme().spaceSm - labelH) / 2);
  const int16_t iconX = static_cast<int16_t>(rect.x + (rect.width - ICON) / 2);
  const char* text = TILES[tile].label;
  if (tile == TILE_LIGHT) {
    const bool on = hasLight && Frontlight.isOn();
    text = !hasLight ? "Light" : on ? "Light on" : "Light off";
    target.bitmap(fui::Rect{iconX, iconY, ICON, ICON}, fui::bitmapFromIcon(on ? icon_sun_filled_32 : icon_sun_32),
                  fui::BitmapMode::Center,
                  enabled ? fui::Paint::solid(fui::Color::Black) : fui::Paint::dither(fui::Color::LightGray));
  } else if (enabled) {
    renderer.drawIcon(TILES[tile].icon, iconX, iconY, ICON);
  } else {
    // Dimmed: every other ink pixel of the legacy 1bpp icon (same mapping as
    // GfxRenderer::drawIcon).
    const uint8_t* bits = TILES[tile].icon;
    constexpr int ROW_BYTES = (ICON + 7) / 8;
    for (int r = 0; r < ICON; ++r) {
      for (int c = 0; c < ICON; ++c) {
        const bool ink = ((bits[r * ROW_BYTES + (c >> 3)] >> (7 - (c & 7))) & 1) == 0;
        const int px = iconX + (ICON - 1 - r);
        const int py = iconY + c;
        if (ink && ((px + py) & 1) == 0) renderer.drawPixel(px, py, true);
      }
    }
  }
  target.text(fui::Rect{rect.x, static_cast<int16_t>(iconY + ICON + screen.theme().spaceSm), rect.width, labelH}, text,
              label);
}

#endif  // HOMESYNC
