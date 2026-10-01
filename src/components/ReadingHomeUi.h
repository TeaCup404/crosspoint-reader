#pragma once

#if HOMESYNC

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "activities/Activity.h"
#include "components/media/book-card.h"

// "Reading first" Home (x4pro-homesync fork), used for every theme but the
// cover grid: the current book as a hero card, the next four recent books as
// small covers, a 4x2 grid of tiles, and one status line (last sync, last
// server check).
//
// Items are addressed by one flat index, matching HomeActivity's selector:
// 0..bookCount()-1 are books (0 = hero, then the strip), then the TILE_COUNT
// tiles in row-major order. The hero cover uses the Library's fixed thumb
// size (library_covers), so a book synced or seen in the Library Covers view
// already has it; the strip has its own smaller size. Missing thumbs are
// generated one per render pass.
class ReadingHomeUi final : public UiAppHost {
 public:
  static constexpr int RECENT_COUNT = 4;
  static constexpr int MAX_BOOKS = 1 + RECENT_COUNT;
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);

  enum Tile : uint8_t {
    TILE_LIBRARY,
    TILE_FILES,
    TILE_STORE,
    TILE_SYNC,
    TILE_SERVER,
    TILE_TRANSFER,
    TILE_SETTINGS,
    TILE_LIGHT,
    TILE_COUNT
  };

  explicit ReadingHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasOpds);

  int bookCount() const;
  int itemCount() const { return bookCount() + TILE_COUNT; }
  // Tile for a flat index, or TILE_COUNT when the index is a book.
  Tile tileAt(int index) const;
  bool isEnabled(int index) const;
  // Flat index to focus when Home is entered from `item` (0 when unknown).
  int indexFor(HomeMenuItem item) const;
  // Next enabled index in direction dir (+1/-1), wrapping.
  int step(int index, int dir) const;

  void setSelection(int selection) { selected = selection; }
  // Flat index of a tapped item on release, else -1.
  int selectedAction(const MappedInputManager& input);
  // Generates at most one missing cover thumb; true when one was made (the
  // caller repaints).
  bool generateNextThumb();

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawHero(UiScreen& screen, freeink::ui::Rect rect);
  void drawEmptyHero(UiScreen& screen, freeink::ui::Rect rect);
  void drawStrip(UiScreen& screen, freeink::ui::Rect rect);
  void drawTiles(UiScreen& screen, freeink::ui::Rect rect);
  void drawTile(UiScreen& screen, freeink::ui::Rect rect, Tile tile, int index);
  void drawCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, int book);
  void buildStatusLine();
  static int thumbHeight(int book);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> thumbPaths;
  std::array<bool, MAX_BOOKS> thumbReady{};
  std::array<bool, MAX_BOOKS> thumbTried{};
  bool hasOpds = false;
  bool hasLight = false;
  // Set by draw(); the strip is dropped when the screen is too short for it.
  bool stripShown = true;
  int selected = 0;
  int pending = -1;
  int progress = -1;
  char progressText[8]{};
  std::string statusLine;
  freeink::ui::TextStyle coverTitleText{};
  freeink::ui::BookCardProps card;
};

#endif  // HOMESYNC
