#pragma once
#include <string>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "homesync/HomeSync.h"

/**
 * "Sync library" screen (x4pro-homesync fork): connects Wi-Fi, pulls new
 * books from the home library (see homesync::run) and shows a summary.
 * Started from the OPDS browser's "Sync library" row, or automatically on a
 * wake that lands on Home when homesync::autoSyncDue().
 */
class HomeSyncActivity final : public Activity, private UiAppHost {
 public:
  HomeSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server, bool automatic);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state != State::DONE; }

 private:
  enum class State { WIFI, AUTO_WIFI, READY, SYNCING, DONE };

  OpdsServer server;
  bool automatic;
  State state = State::WIFI;
  bool cancel = false;
  std::string status;
  std::string summary;
  std::string detail;
  homesync::Progress progress;

  static void rootScreen(UiScreen& screen, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void startSync();
  void finish(const homesync::Result& result);
};
