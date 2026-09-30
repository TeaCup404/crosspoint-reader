#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

/**
 * "Server" screen (x4pro-homesync fork): a status page for the home server,
 * painted from reader-hub's /dash JSON. reader-hub decides every string and
 * severity; this screen only lays them out. LAN first, Tailscale when away.
 * Confirm refreshes, Back goes home.
 */
class ServerDashActivity final : public Activity, private UiAppHost {
 public:
  ServerDashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::WIFI || state == State::LOADING; }

 private:
  enum class State { WIFI, LOADING, SHOWN, FAILED };

  struct Card {
    std::string label, value, caption;
  };
  struct Row {
    std::string label, detail, value;
    int sev = 0;  // 0 ok, 1 warn, 2 crit
  };

  State state = State::WIFI;
  std::string status;
  std::string error;
  std::string host = "Server";
  std::string footer;
  std::string fetchedAt;
  bool viaTailnet = false;
  int okCount = 0;
  std::vector<Card> cards;
  std::vector<Row> detail;
  std::vector<Row> checks;

  static void rootScreen(UiScreen& screen, void* user);
  void draw(UiScreen& screen);
  void load();
  bool parse(const std::string& json);
};
