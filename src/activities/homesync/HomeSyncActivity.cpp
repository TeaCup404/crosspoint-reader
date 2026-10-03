#include "HomeSyncActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "homesync/QuietWifi.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "homesync/WifiLinger.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_CANCEL = 1;
constexpr int PROGRESS_STEP_PERCENT = 10;
constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 5000;
}  // namespace

HomeSyncActivity::HomeSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server,
                                   const bool automatic)
    : Activity("HomeSync", renderer, mappedInput),
      UiAppHost(renderer),
      server(std::move(server)),
      automatic(automatic) {}

void HomeSyncActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.on(ACTION_CANCEL, &HomeSyncActivity::onCancelEvent, this);
  app.setScreen(&HomeSyncActivity::rootScreen, this);
  cancel = false;
  status = "Connecting to Wi-Fi...";
  if (automatic) {
    // No Wi-Fi picker on wake: loop() joins a saved network quietly or goes
    // home (e.g. away from every known network).
    homesync::noteAutoAttempt();
    state = State::AUTO_WIFI;
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::READY;  // loop() starts the sync after this frame is on screen
    requestUpdate();
    return;
  }
  state = State::WIFI;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             // Automatic sync away from known Wi-Fi: just go home.
                             if (automatic) {
                               onGoHome();
                               return;
                             }
                             state = State::DONE;
                             summary = "Not synced";
                             detail = "No Wi-Fi connection";
                             requestUpdate();
                             return;
                           }
                           state = State::READY;
                           requestUpdate();
                         });
}

void HomeSyncActivity::onExit() {
  Activity::onExit();
  // Same teardown as the OPDS browser: a silent reboot returns the heap the
  // Wi-Fi stack fragmented.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (keepWifiAfterUse()) return;
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void HomeSyncActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<HomeSyncActivity*>(user);
  if (self->state == State::SYNCING) self->cancel = true;
}

void HomeSyncActivity::loop() {
  switch (state) {
    case State::WIFI:
      return;
    case State::AUTO_WIFI:
      if (homesync::quietConnect()) {
        state = State::READY;
      } else {
        LOG_INF("SYNC", "Auto-sync: no known Wi-Fi in range");
        onGoHome();
      }
      return;
    case State::READY:
      startSync();
      return;
    case State::SYNCING:
      return;
    case State::DONE: {
      int tx = 0;
      int ty = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
        onGoHome();
      }
      return;
    }
  }
}

void HomeSyncActivity::startSync() {
  state = State::SYNCING;
  progress = homesync::Progress{};
  requestUpdate(true);

  // Same pre-flight as the OPDS download: drop rebuildable font caches so the
  // transfers have heap.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();

  int lastPercent = -1;
  int lastIndex = 0;
  unsigned long lastUpdateMs = 0;
  const auto pump = [this] {
    // The loop is blocked for the whole sync; pump input here so Back / the
    // Cancel button can stop it between chunks.
    mappedInput.update(true);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) cancel = true;
    routeTouch(mappedInput);
  };

  const auto result = homesync::run(
      server,
      [this, &pump](const char* msg) {
        status = msg;
        pump();
        requestUpdate(true);
      },
      [&](const homesync::Progress& p) {
        progress = p;
        pump();
        const int percent =
            p.totalBytes > 0 ? static_cast<int>(static_cast<uint64_t>(p.bytes) * 100 / p.totalBytes) : 0;
        const unsigned long now = millis();
        if (p.index != lastIndex || percent >= lastPercent + PROGRESS_STEP_PERCENT ||
            now - lastUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
          lastIndex = p.index;
          lastPercent = percent;
          lastUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancel);
  finish(result);
}

void HomeSyncActivity::finish(const homesync::Result& result) {
  state = State::DONE;
  if (cancel) {
    summary = "Sync stopped";
  } else if (!result.error.empty()) {
    summary = "Sync failed";
  } else if (result.downloaded == 0) {
    summary = "Up to date";
  } else {
    summary = std::to_string(result.downloaded) + (result.downloaded == 1 ? " new book" : " new books");
  }
  detail = result.error.empty() ? result.source : result.error;
  if (result.failed > 0) detail += " - " + std::to_string(result.failed) + " failed";
  LOG_INF("SYNC", "%s (%s)", summary.c_str(), detail.c_str());
  requestUpdate(true);
}

void HomeSyncActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<HomeSyncActivity*>(user);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();

  fui::HeaderProps header;
  header.title = "Sync library";
  header.borderEdges = fui::EdgeBottom;
  GUI.applyHeaderStatus(self->renderer, header);
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  const auto frameRect = screen.frame().screen();
  fui::header(screen.frame(),
              fui::Rect{frameRect.x, static_cast<int16_t>(metrics.topPadding), frameRect.width,
                        static_cast<int16_t>(metrics.headerHeight)},
              header);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;

  if (self->state == State::DONE) {
    const int16_t blockH = static_cast<int16_t>(lh * 2 + gap);
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), self->summary.c_str(), centered);
    screen.target().text(screen.takeTop(lh), self->detail.c_str(), centered);
    return;
  }

  if (self->state != State::SYNCING || self->progress.total == 0) {
    screen.centeredText(self->status.c_str(), centered);
    return;
  }

  // Downloading: "Book 2 of 5", title, progress bar, cancel button.
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  const std::string count =
      "Book " + std::to_string(self->progress.index) + " of " + std::to_string(self->progress.total);
  screen.target().text(screen.takeTop(lh, gap), count.c_str(), centered);
  screen.target().text(screen.takeTop(lh, gap), self->progress.title.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (self->progress.totalBytes > 0) {
    fui::ProgressBarProps bp;
    bp.value = static_cast<int32_t>(self->progress.bytes);
    bp.max = static_cast<int32_t>(self->progress.totalBytes);
    bp.border = fui::Paint::solid(fui::Color::Black);
    bp.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, bp);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancelBtn;
  cancelBtn.label = tr(STR_CANCEL);
  cancelBtn.action = ACTION_CANCEL;
  screen.button(cancelBtn,
                fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void HomeSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();
  MappedInputManager::Labels labels;
  switch (state) {
    case State::SYNCING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::DONE:
      labels = mappedInput.mapLabels(tr(STR_BACK), "OK", "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderUi();
  renderer.displayBuffer();
}
