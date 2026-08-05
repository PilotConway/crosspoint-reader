#pragma once

#include <FreeInkUI.h>

#include <atomic>

#include "../Activity.h"
#include "DashboardGrid.h"
#include "DashboardMqttWorker.h"
#include "mqtt_dashboard_bridge/DashboardState.h"

// Integrates the standalone paper-s3-personal-dashboard's Home Assistant / MQTT
// dashboard as a CrossPoint Activity.
//
// Home and Remote tabs are driven by papers3/dashboard/layout (retained card
// grid) + papers3/dashboard/state (frequent on/off patches) via DashboardGrid.
// The Tasks tab is a flat read-only list of what HA sends.
//
// UI is built with FreeInkUI: render() emits components that register their own
// hit rects into `interactions`, and loop() routes touch against that buffer, so
// this activity does no coordinate hit-testing of its own. Back is the global
// left-edge swipe (folded into Button::Back by MappedInputManager), so there is
// no footer chrome to special-case.
//
// MQTT itself runs on a background task (DashboardMqttWorker) rather than inline
// in loop() - PubSubClient's message read is slow enough for large payloads that
// running it on the same task as touch input made the whole screen appear
// frozen. This Activity just polls the worker each loop() tick.
class DashboardActivity final : public Activity {
  enum class State {
    ConnectingWifi,
    WifiFailed,
    ConnectingMqtt,
    WaitingForData,
    ShowingDashboard,
  };

  enum class Tab : int16_t {
    Home = 0,
    Tasks = 1,
    Remote = 2,
  };

  static constexpr int kTabCount = 3;
  // Cards (up to 64) + page dots + tabs. Sized generously; the buffer is a
  // fixed-size member, so headroom costs bytes rather than heap.
  static constexpr size_t kMaxInteractions = 96;

  // Action ids for this activity's own chrome. DashboardGrid owns 10/11.
  static constexpr freeink::ui::ActionId kActionTab = 1;

  State state = State::ConnectingWifi;
  Tab activeTab = Tab::Home;
  DashboardMqttWorker* mqttWorker = nullptr;

  dashboard::WeatherState weather;
  bool hasWeather = false;
  dashboard::TasksState tasks;
  bool hasTasks = false;

  DashboardGrid homeGrid;
  DashboardGrid remoteGrid;
  // Reused scratch buffers for draining DashboardMqttWorker's TryTake*() results
  // each loop() tick - one of each, applied to home then remote sequentially,
  // rather than one per grid, to halve the footprint. Members (not locals)
  // because DashboardPageCards is ~12-13KB - see DashboardMqttWorker.h for why
  // that can never be a stack local here.
  dashboard::DashboardPageCards scratchCards;
  dashboard::DashboardCardStatePatch scratchStatePatches[dashboard::DashboardPageCards::kMaxCards];

  // Card index within the active tab's grid currently mid tap-feedback flash,
  // or -1. Only meaningful for the single render() between the tap and the
  // flash clearing - see handleAction().
  int pressedCardIndex = -1;

  // Hit rects registered by the last render(). render() runs on the render task
  // while loop() routes touch on the main task, so the ready flag gates routing
  // until a frame has actually populated the buffer (mirrors
  // KeyboardEntryActivity).
  freeink::ui::InteractionBuffer<kMaxInteractions> interactions;
  std::atomic<bool> interactionsReady{false};

  // Pixel bands, recomputed whenever the content area changes.
  int contentTopY = 0;
  int contentBottomY = 0;

  void layoutBands();
  void onWifiSelectionComplete(bool connected);
  void pollMqttWorker();
  void handleTouch();
  void handleAction(freeink::ui::ActionId action, int16_t value);
  void publishCardAction(const DashboardGrid& grid, int cardIndex);
  DashboardGrid* activeGrid();
  const DashboardGrid* activeGrid() const;
  static const char* tabLabel(Tab tab);

 public:
  explicit DashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Dashboard", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
