#include "ServerDashActivity.h"

#if HOMESYNC

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <ctime>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "homesync/Tailnet.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
// reader-hub on homebot: LAN at home, tailnet IP (plain HTTP inside WireGuard)
// when away; the tailnet grant allows tag:reader -> homebot tcp:8790.
constexpr const char* DASH_LAN_URL = "http://192.168.1.124:8790/dash";
constexpr const char* DASH_TAILNET_URL = "http://100.86.140.113:8790/dash";
constexpr const char* TAILNET_IP = "100.86.140.113";
constexpr size_t MAX_BODY = 8192;
constexpr size_t MAX_ALARMS = 5;
}  // namespace

ServerDashActivity::ServerDashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ServerDash", renderer, mappedInput), UiAppHost(renderer) {}

void ServerDashActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&ServerDashActivity::rootScreen, this);
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::LOADING;
    status = "Loading...";
    requestUpdate();
    return;
  }
  state = State::WIFI;
  status = "Connecting to Wi-Fi...";
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             state = State::FAILED;
                             error = "No Wi-Fi connection";
                           } else {
                             state = State::LOADING;
                             status = "Loading...";
                           }
                           requestUpdate();
                         });
}

void ServerDashActivity::onExit() {
  Activity::onExit();
  homesync::tailnet::down();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ServerDashActivity::loop() {
  if (state == State::WIFI) return;
  if (state == State::LOADING) {
    load();
    return;
  }
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
    state = State::LOADING;
    status = "Refreshing...";
    requestUpdate(true);
  }
}

void ServerDashActivity::load() {
  std::string body;
  const auto collect = [&body](const uint8_t* data, size_t len) {
    if (body.size() + len > MAX_BODY) return false;
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  };

  viaTailnet = false;
  bool ok = HttpDownloader::fetchUrl(std::string(DASH_LAN_URL), collect);
  if (!ok && homesync::tailnet::configured()) {
    status = "Home unreachable, using Tailscale...";
    requestUpdate(true);
    body.clear();
    ok = homesync::tailnet::up(
             TAILNET_IP,
             [this](const char* msg) {
               status = msg;
               requestUpdate(true);
             },
             nullptr) &&
         homesync::tailnet::fetch(DASH_TAILNET_URL, collect, "", "");
    viaTailnet = ok;
  }

  if (!ok || !parse(body)) {
    state = State::FAILED;
    error = ok ? "Unexpected reply from the server" : "Server not reachable";
    requestUpdate(true);
    return;
  }
  char buf[8] = "";
  if (halClock.formatTime(buf, sizeof(buf), false)) {
    fetchedAt = buf;
  } else {
    fetchedAt.clear();
  }
  state = State::SHOWN;
  requestUpdate(true);
}

bool ServerDashActivity::parse(const std::string& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc["cards"].is<JsonArrayConst>()) return false;
  host = doc["host"] | "Server";
  footer = doc["footer"] | "";
  okCount = doc["okCount"] | 0;
  cards.clear();
  detail.clear();
  checks.clear();
  for (JsonObjectConst c : doc["cards"].as<JsonArrayConst>()) {
    const char* value = c["value"] | "";
    const char* unit = c["unit"] | "";
    cards.push_back(Card{c["label"] | "", std::string(value) + unit, c["caption"] | ""});
  }
  for (JsonObjectConst r : doc["detail"].as<JsonArrayConst>()) {
    detail.push_back(Row{r["label"] | "", r["detail"] | "", r["value"] | "", 0});
  }
  for (JsonObjectConst r : doc["checks"].as<JsonArrayConst>()) {
    checks.push_back(Row{r["label"] | "", r["detail"] | "", r["value"] | "", r["sev"] | 1});
  }
  return true;
}

void ServerDashActivity::rootScreen(UiScreen& screen, void* user) { static_cast<ServerDashActivity*>(user)->draw(screen); }

void ServerDashActivity::draw(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  auto& target = screen.target();

  // Header (same chrome as the other homesync screens).
  fui::HeaderProps header;
  header.title = host.c_str();
  header.borderEdges = fui::EdgeBottom;
  GUI.applyHeaderStatus(renderer, header);
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
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
                  theme.spaceLg, static_cast<int16_t>(metrics.buttonHintsHeight), theme.spaceLg});

  fui::TextStyle body = theme.bodyText;
  fui::TextStyle centered = body;
  centered.align = fui::TextAlign::Center;
  if (state != State::SHOWN) {
    screen.centeredText(state == State::FAILED ? error.c_str() : status.c_str(), centered);
    return;
  }

  fui::TextStyle small = body;
  small.font = theme.fontSmall;
  fui::TextStyle bold = body;
  bold.bold = true;
  fui::TextStyle big = theme.titleText;
  big.align = fui::TextAlign::Center;
  const int16_t lh = target.lineHeight(body.font);
  const int16_t sh = target.lineHeight(small.font);
  const int16_t gap = theme.spaceMd;

  // Summary line: the good news is one quiet line; alarms get the space.
  const std::string summary = checks.empty() ? "All " + std::to_string(okCount) + " checks OK"
                                             : std::to_string(checks.size()) +
                                                   (checks.size() == 1 ? " ALARM" : " ALARMS") + " - " +
                                                   std::to_string(okCount) + " OK";
  target.text(screen.takeTop(lh, gap), summary.c_str(), checks.empty() ? body : bold);

  // Alarms: CRIT inverted, WARN plain with a border.
  for (size_t i = 0; i < checks.size() && i < MAX_ALARMS; ++i) {
    const Row& r = checks[i];
    const fui::Rect row = screen.takeTop(static_cast<int16_t>(lh + sh + gap), theme.spaceSm);
    const bool crit = r.sev >= 2;
    if (crit) {
      target.fill(row, fui::Paint::solid(fui::Color::Black), 6);
    } else {
      target.stroke(row, fui::Paint::solid(fui::Color::Black), 2, 6);
    }
    const fui::Rect inner = row.inset(fui::Insets{theme.spaceSm, theme.spaceMd, theme.spaceSm, theme.spaceMd});
    fui::TextStyle label = bold;
    fui::TextStyle value = bold;
    value.align = fui::TextAlign::Right;
    fui::TextStyle sub = small;
    label.inverted = value.inverted = sub.inverted = crit;
    target.text(fui::Rect{inner.x, inner.y, inner.width, lh}, r.label.c_str(), label);
    target.text(fui::Rect{inner.x, inner.y, inner.width, lh}, r.value.c_str(), value);
    target.text(fui::Rect{inner.x, static_cast<int16_t>(inner.y + lh), inner.width, sh}, r.detail.c_str(), sub);
  }
  if (checks.size() > MAX_ALARMS) {
    const std::string more = "+" + std::to_string(checks.size() - MAX_ALARMS) + " more";
    target.text(screen.takeTop(sh, gap), more.c_str(), small);
  }

  // Three headline cards.
  if (!cards.empty()) {
    screen.spacer(gap);
    const int16_t bigH = target.lineHeight(big.font);
    const fui::Rect band = screen.takeTop(static_cast<int16_t>(sh * 2 + bigH + gap * 2), gap);
    const int n = static_cast<int>(cards.size());
    const int16_t w = static_cast<int16_t>((band.width - gap * (n - 1)) / n);
    fui::TextStyle label = small;
    label.align = fui::TextAlign::Center;
    for (int i = 0; i < n; ++i) {
      const fui::Rect card{static_cast<int16_t>(band.x + i * (w + gap)), band.y, w, band.height};
      target.stroke(card, fui::Paint::solid(fui::Color::Black), 2, 8);
      int16_t y = static_cast<int16_t>(card.y + gap);
      target.text(fui::Rect{card.x, y, card.width, sh}, cards[i].label.c_str(), label);
      y = static_cast<int16_t>(y + sh);
      target.text(fui::Rect{card.x, y, card.width, bigH}, cards[i].value.c_str(), big);
      y = static_cast<int16_t>(y + bigH);
      target.text(fui::Rect{card.x, y, card.width, sh}, cards[i].caption.c_str(), label);
    }
  }

  // Detail rows: label + detail on the left, value on the right.
  for (const Row& r : detail) {
    const fui::Rect row = screen.takeTop(static_cast<int16_t>(lh + sh), gap);
    fui::TextStyle value = bold;
    value.align = fui::TextAlign::Right;
    target.text(fui::Rect{row.x, row.y, row.width, lh}, r.label.c_str(), bold);
    target.text(fui::Rect{row.x, row.y, row.width, lh}, r.value.c_str(), value);
    target.text(fui::Rect{row.x, static_cast<int16_t>(row.y + lh), row.width, sh}, r.detail.c_str(), small);
    target.line(fui::Point{row.x, static_cast<int16_t>(row.y + lh + sh + gap / 2)},
                fui::Point{static_cast<int16_t>(row.x + row.width), static_cast<int16_t>(row.y + lh + sh + gap / 2)}, 1,
                fui::Paint::solid(fui::Color::Black));
  }

  // Footer.
  std::string foot = footer;
  if (!fetchedAt.empty()) foot += " - " + fetchedAt;
  if (viaTailnet) foot += " - via Tailscale";
  fui::TextStyle footStyle = small;
  footStyle.maxLines = 2;
  target.text(screen.takeTop(static_cast<int16_t>(sh * 2), 0), foot.c_str(), footStyle);
}

void ServerDashActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == State::SHOWN ? "Refresh" : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderUi();
  renderer.displayBuffer();
}

#endif  // HOMESYNC
