#include "LibraryCoverThumbs.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

namespace library_covers {
namespace {
// Headroom for one cover decode plus whatever the caller keeps live. The
// largest-block check is what matters on the C3-sized internal heap; on PSRAM
// boards the default heap includes the external pool.
constexpr size_t MIN_INTERNAL_FREE = 32 * 1024;
constexpr size_t MIN_LARGEST_BLOCK = 48 * 1024;
}  // namespace

std::string thumbPathFor(const std::string& bookPath) {
  // Constructors only derive cache paths; keep the large parser objects off
  // the task stack all the same.
  if (FsHelpers::hasReflowableBookExtension(bookPath)) {
    auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
    return epub ? epub->getThumbBmpPath(THUMB_HEIGHT) : std::string();
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
    return xtc ? xtc->getThumbBmpPath(THUMB_HEIGHT) : std::string();
  }
  return {};
}

// Same generation paths as the Home cover grid (HomeActivity::loadGridCover):
// EPUB/TXT locate the cover without building spine or TOC caches; XTC needs its
// header loaded first.
bool generateThumb(const std::string& bookPath) {
  if (FsHelpers::hasReflowableBookExtension(bookPath)) {
    auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
    if (!epub) {
      LOG_ERR("COVR", "OOM: cover EPUB");
      return false;
    }
    return epub->generateThumbBmpFromSource(THUMB_HEIGHT);
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("COVR", "OOM: cover XTC");
      return false;
    }
    if (Storage.exists(xtc->getThumbBmpPath(THUMB_HEIGHT).c_str())) return true;
    return xtc->load() && xtc->generateThumbBmp(THUMB_HEIGHT);
  }
  return false;
}

bool heapAllowsThumb() {
  const auto internal = HalMemory::getInternalHeap();
  const auto heap = HalMemory::getDefaultHeap();
  return internal.freeBytes >= MIN_INTERNAL_FREE && heap.largestBlockBytes >= MIN_LARGEST_BLOCK;
}

}  // namespace library_covers
