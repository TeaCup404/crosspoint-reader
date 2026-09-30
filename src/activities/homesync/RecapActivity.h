#pragma once
#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

/**
 * "Story so far" (x4pro-homesync fork): asks reader-hub on homebot for a
 * spoiler-free recap of the open book up to the reading position, written by
 * the local LLM. reader-hub answers {"status": "preparing", done, total} while
 * it summarises the book, and this screen re-asks every few seconds until the
 * recap is ready. Back (or Confirm once shown) returns to the page.
 */
class RecapActivity final : public Activity, private UiAppHost {
 public:
  RecapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath, std::string title,
                std::string author, float percentage);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::WIFI || state == State::LOADING || state == State::WAITING; }

 private:
  enum class State { WIFI, LOADING, WAITING, SHOWN, FAILED };

  std::string epubPath;
  std::string title;
  std::string author;
  float percentage;
  State state = State::WIFI;
  std::string heading;  // "Red Rising - 12%"
  std::string body;     // recap text, progress or error
  unsigned long nextPollAt = 0;
  bool wifiUsed = false;

  static void rootScreen(UiScreen& screen, void* user);
  void draw(UiScreen& screen);
  void fetch();
  void backToBook();
};
