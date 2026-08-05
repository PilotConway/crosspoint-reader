#include "DashboardActivity.h"

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <components/bars/tab-bar.h>
#include <components/lists/list.h>

#include <cstdio>

#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "mqtt_dashboard_bridge/HaMessages.h"

// Broker connection details are intentionally NOT hardcoded here - they come
// from platformio.local.ini (gitignored), mirroring how the standalone
// dashboard bakes credentials in as build flags.
#ifndef DASHBOARD_MQTT_SERVER
#define DASHBOARD_MQTT_SERVER ""
#endif
#ifndef DASHBOARD_MQTT_PORT
#define DASHBOARD_MQTT_PORT 1883
#endif
#ifndef DASHBOARD_MQTT_USER
#define DASHBOARD_MQTT_USER ""
#endif
#ifndef DASHBOARD_MQTT_PASSWORD
#define DASHBOARD_MQTT_PASSWORD ""
#endif

namespace fui = freeink::ui;

namespace {
constexpr const char* kLogTag = "DASH";
// Matches the standalone dashboard's -DAPP_TAP_FEEDBACK_MS=120 build flag.
constexpr unsigned long kTapFeedbackMs = 120;
}  // namespace

void DashboardActivity::onEnter() {
  Activity::onEnter();

  state = State::ConnectingWifi;
  activeTab = Tab::Home;
  hasWeather = false;
  hasTasks = false;
  pressedCardIndex = -1;
  interactionsReady.store(false);

  layoutBands();
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  LOG_DBG(kLogTag, "Not connected, launching WifiSelectionActivity (autoConnect)");
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DashboardActivity::onExit() {
  Activity::onExit();

  // Offline-first: don't keep MQTT (and the WiFi radio it depends on) alive once
  // the user leaves this screen - matches CrossPoint's battery-first design
  // instead of the standalone dashboard's always-connected model.
  //
  // RequestStop() is non-blocking - see DashboardMqttWorker.h for why we never
  // wait on it here (a still-in-progress slow network read must not block
  // Activity teardown).
  if (mqttWorker != nullptr) {
    mqttWorker->RequestStop();
    mqttWorker = nullptr;
  }
}

void DashboardActivity::layoutBands() {
  // The tab bar and header are laid out by the Screen builder in render(); this
  // only needs the resulting content band, which the grids use for their pixel
  // layout. Keep the arithmetic in sync with render()'s takeTop() calls.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  contentTopY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  contentBottomY = pageHeight - metrics.verticalSpacing;

  homeGrid.SetContentBounds(contentTopY, contentBottomY, pageWidth);
  remoteGrid.SetContentBounds(contentTopY, contentBottomY, pageWidth);
}

void DashboardActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_ERR(kLogTag, "WiFi connection failed or cancelled");
    state = State::WifiFailed;
    requestUpdate();
    return;
  }

  LOG_DBG(kLogTag, "WiFi connected, starting MQTT worker task");
  state = State::ConnectingMqtt;
  requestUpdate();

  mqttWorker = DashboardMqttWorker::Start(DASHBOARD_MQTT_SERVER, DASHBOARD_MQTT_PORT, DASHBOARD_MQTT_USER,
                                          DASHBOARD_MQTT_PASSWORD);
}

void DashboardActivity::loop() {
  // Back is the global left-edge swipe, folded into Button::Back by
  // MappedInputManager, so it needs no chrome of its own here.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  handleTouch();

  if (mqttWorker == nullptr) {
    return;
  }

  pollMqttWorker();
}

void DashboardActivity::handleTouch() {
  if (!interactionsReady.load()) {
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (!mappedInput.wasScreenTapped(tapX, tapY)) {
    return;
  }

  fui::InputSnapshot snapshot;
  snapshot.touchReleased = true;
  snapshot.touchX = static_cast<int16_t>(tapX);
  snapshot.touchY = static_cast<int16_t>(tapY);
  const fui::ActionEvent event = interactions.route(snapshot);
  if (!event) {
    return;
  }
  handleAction(event.action, event.value);
}

void DashboardActivity::handleAction(const fui::ActionId action, const int16_t value) {
  if (action == kActionTab) {
    const Tab tapped = static_cast<Tab>(value);
    if (tapped != activeTab) {
      activeTab = tapped;
      requestUpdate();
    }
    return;
  }

  if (action == DashboardGrid::kActionDot) {
    DashboardGrid* grid = activeGrid();
    if (grid != nullptr &&
        grid->SetCarouselPage(DashboardGrid::DecodeDotSlot(value), DashboardGrid::DecodeDotPage(value))) {
      requestUpdate();
    }
    return;
  }

  if (action != DashboardGrid::kActionCard) {
    return;
  }

  DashboardGrid* grid = activeGrid();
  if (grid == nullptr) {
    return;
  }
  const int cardIndex = value;
  const dashboard::DashboardCard* card = grid->GetCard(cardIndex);
  if (card == nullptr) {
    return;
  }

  // Optimistic local echo so a toggle reads as flipped before the broker
  // confirms; a contradicting state patch will correct it.
  grid->ToggleLocally(cardIndex);

  // Flash the card, then publish. requestUpdateAndWait() forces the pressed
  // frame onto the panel before the delay, otherwise the feedback is invisible.
  pressedCardIndex = cardIndex;
  requestUpdateAndWait();
  delay(kTapFeedbackMs);
  pressedCardIndex = -1;
  requestUpdate();

  publishCardAction(*grid, cardIndex);
}

void DashboardActivity::pollMqttWorker() {
  if (!mqttWorker->IsConnected()) {
    if (state != State::WifiFailed) {
      state = State::ConnectingMqtt;
    }
    return;
  }
  if (state == State::ConnectingMqtt) {
    state = State::WaitingForData;
    requestUpdate();
  }

  bool changed = false;

  if (mqttWorker->TryTakeWeather(weather)) {
    hasWeather = true;
    changed = true;
  }
  if (mqttWorker->TryTakeTasks(tasks)) {
    hasTasks = true;
    changed = true;
  }
  if (mqttWorker->TryTakeHomeCards(scratchCards)) {
    homeGrid.ApplyCards(scratchCards);
    changed = true;
  }
  if (mqttWorker->TryTakeRemoteCards(scratchCards)) {
    remoteGrid.ApplyCards(scratchCards);
    changed = true;
  }
  int patchCount = 0;
  if (mqttWorker->TryTakeHomeStatePatch(scratchStatePatches, dashboard::DashboardPageCards::kMaxCards, patchCount)) {
    changed = homeGrid.ApplyStatePatch(scratchStatePatches, patchCount) || changed;
  }
  if (mqttWorker->TryTakeRemoteStatePatch(scratchStatePatches, dashboard::DashboardPageCards::kMaxCards, patchCount)) {
    changed = remoteGrid.ApplyStatePatch(scratchStatePatches, patchCount) || changed;
  }

  if (changed) {
    state = State::ShowingDashboard;
    requestUpdate();
  }
}

void DashboardActivity::publishCardAction(const DashboardGrid& grid, const int cardIndex) {
  const dashboard::DashboardCard* card = grid.GetCard(cardIndex);
  if (card == nullptr || card->id.length() == 0 || mqttWorker == nullptr) {
    return;
  }
  const String action = dashboard::DefaultActionForCard(*card);
  mqttWorker->RequestPublish(action, card->id, card->action_data_json);
}

DashboardGrid* DashboardActivity::activeGrid() {
  switch (activeTab) {
    case Tab::Home:
      return &homeGrid;
    case Tab::Remote:
      return &remoteGrid;
    case Tab::Tasks:
      return nullptr;
  }
  return nullptr;
}

const DashboardGrid* DashboardActivity::activeGrid() const {
  return const_cast<DashboardActivity*>(this)->activeGrid();
}

const char* DashboardActivity::tabLabel(const Tab tab) {
  switch (tab) {
    case Tab::Home:
      return tr(STR_DASHBOARD_TAB_HOME);
    case Tab::Tasks:
      return tr(STR_DASHBOARD_TAB_TASKS);
    case Tab::Remote:
      return tr(STR_DASHBOARD_TAB_REMOTE);
  }
  return "";
}

void DashboardActivity::render(RenderLock&&) {
  renderer.clearScreen();

  fui::GfxRendererTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};

  interactions.clear();
  fui::Frame<kMaxInteractions> frame(target, device, noInput, interactions);
  fui::Screen<kMaxInteractions> screen(frame, fui::defaultThemeTokens());

  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header
  {
    const fui::Rect headerRect = screen.takeTop(static_cast<int16_t>(metrics.headerHeight));
    fui::TextStyle titleStyle;
    titleStyle.font = fui::GfxRendererTarget::FONT_BODY;
    titleStyle.align = fui::TextAlign::Center;
    fui::drawText(target, headerRect, tr(STR_DASHBOARD_TITLE), titleStyle);
  }

  // Tab bar
  {
    const fui::TabItem tabs[kTabCount] = {
        {tabLabel(Tab::Home), {}, {}, static_cast<int16_t>(Tab::Home), activeTab == Tab::Home, true},
        {tabLabel(Tab::Tasks), {}, {}, static_cast<int16_t>(Tab::Tasks), activeTab == Tab::Tasks, true},
        {tabLabel(Tab::Remote), {}, {}, static_cast<int16_t>(Tab::Remote), activeTab == Tab::Remote, true},
    };
    fui::TabBarProps props;
    props.tabs = tabs;
    props.count = kTabCount;
    props.action = kActionTab;
    props.text.font = fui::GfxRendererTarget::FONT_BODY;
    props.divider = true;
    fui::tabBar(frame, screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight),
                                      static_cast<int16_t>(metrics.verticalSpacing)),
                props);
  }

  const fui::Rect body = screen.body();
  fui::TextStyle bodyStyle;
  bodyStyle.font = fui::GfxRendererTarget::FONT_BODY;
  bodyStyle.align = fui::TextAlign::Center;

  // Connection states own the whole body; the dashboard only draws once data
  // has actually landed.
  const char* statusMessage = nullptr;
  switch (state) {
    case State::ConnectingWifi:
      statusMessage = tr(STR_DASHBOARD_CONNECTING_WIFI);
      break;
    case State::WifiFailed:
      statusMessage = tr(STR_DASHBOARD_WIFI_FAILED);
      break;
    case State::ConnectingMqtt:
      statusMessage = tr(STR_DASHBOARD_CONNECTING_MQTT);
      break;
    case State::WaitingForData:
      statusMessage = tr(STR_DASHBOARD_WAITING_DATA);
      break;
    case State::ShowingDashboard:
      break;
  }
  if (statusMessage != nullptr) {
    fui::drawText(target, body, statusMessage, bodyStyle);
    interactionsReady.store(true);
    renderer.displayBuffer();
    return;
  }

  if (activeTab == Tab::Tasks) {
    if (!hasTasks || tasks.count == 0) {
      fui::drawText(target, body, tr(STR_DASHBOARD_WAITING_DATA), bodyStyle);
    } else {
      // Read-only glanceable list: no action id, so it registers no hit rects.
      // Build the row array up front: fui::list takes ListItem values, not row
      // callbacks. Capped at kMaxTaskRows so this stays a fixed stack cost.
      constexpr int kMaxTaskRows = dashboard::TasksState::kMaxTasks;
      fui::ListItem rows[kMaxTaskRows];
      const int rowCount = tasks.count > kMaxTaskRows ? kMaxTaskRows : tasks.count;
      for (int i = 0; i < rowCount; ++i) {
        rows[i].label = tasks.items[i].content.c_str();
        rows[i].subtitle = tasks.items[i].due_label.length() > 0 ? tasks.items[i].due_label.c_str() : nullptr;
      }

      // Read-only glanceable list: no action id, so it registers no hit rects.
      fui::ListProps props;
      props.items = rows;
      props.count = static_cast<uint16_t>(rowCount);
      props.selectedIndex = -1;
      props.rowHeight = static_cast<int16_t>(metrics.listRowHeight);
      props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
      props.subtitleText.font = fui::GfxRendererTarget::FONT_SMALL;
      fui::list(frame, body, props);
    }
    interactionsReady.store(true);
    renderer.displayBuffer();
    return;
  }

  const DashboardGrid* grid = activeGrid();
  if (grid == nullptr || !grid->HasData()) {
    fui::drawText(target, body, tr(STR_DASHBOARD_LOADING_GRID), bodyStyle);
  } else {
    const_cast<DashboardGrid*>(grid)->Draw(frame, weather, pressedCardIndex);
  }

  interactionsReady.store(true);
  renderer.displayBuffer();
}
