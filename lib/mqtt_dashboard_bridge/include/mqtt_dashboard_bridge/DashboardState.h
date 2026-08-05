#pragma once

#include <Arduino.h>

namespace dashboard {

// Trimmed from the standalone dashboard's WeatherState (include/app/AppState.h).
// Only the weather model is ported for this prototype; calendar/lights follow
// the same shape and can be added the same way.
struct WeatherState {
  String condition;
  int temperature_f = 0;
  int rain_chance_pct = 0;
  int wind_speed_mph = 0;
};

// Trimmed from the standalone dashboard's TaskItem (include/app/AppState.h).
// Read-only display only: no child/subtask, postpone, or completion fields -
// this activity doesn't publish task commands, it only renders what HA sends.
struct TaskItem {
  String content;
  String due_label;  // "TODAY", "OVERDUE", or "" - see ParseTasksPayload.
};

// Trimmed from the standalone dashboard's TasksState. Uses a smaller fixed
// cap than the original (AppConfig::kMaxTasks = 50) since this is a
// glanceable list on a single screen, not a scrollable/interactive page.
struct TasksState {
  static constexpr int kMaxTasks = 20;
  TaskItem items[kMaxTasks];
  int count = 0;
};

// Card types from schema/dashboard-v3.schema.json in the standalone dashboard
// repo. kCalendar/kTasks/kCarousel are parsed (so sibling card positions stay
// correct) but not rendered yet - see DashboardGrid, which silently skips
// drawing/hit-testing them until those subsystems are ported.
enum class DashboardCardType : uint8_t {
  kSpacer,
  kWeather,
  kButton,
  kToggle,
  kText,
  kCalendar,
  kTasks,
  kCarousel,
};

enum class DashboardCardStyle : uint8_t {
  kRoundedRect,
  kRect,
  kRound,
};

// Trimmed from the standalone dashboard's DashboardPage::DashboardCard
// (include/app/DashboardPage.h) - drops pixel_rect since that's runtime
// layout state owned by DashboardGrid, not parsed MQTT data.
//
// Carousels are flattened rather than nested: every card on every page of a
// carousel lands in the same flat list, tagged with which carousel and which
// page it belongs to. DashboardGrid then draws only the cards whose
// carousel_page_index matches that carousel's active page. This mirrors the
// standalone dashboard's RemoteButtonState layout exactly
// (lib/mqtt_client/include/mqtt_client/HaMessages.h), and keeps the card
// array a flat fixed-size POD block - a nested page structure would mean
// either dynamic allocation or a second worst-case-sized array.
struct DashboardCard {
  DashboardCardType type = DashboardCardType::kButton;
  String id;
  String label;
  String text;
  String icon;
  String action;
  String action_data_json;
  DashboardCardStyle style = DashboardCardStyle::kRoundedRect;
  bool is_on = false;
  // Absolute grid position, already resolved through any enclosing carousel's
  // region offset - so DashboardGrid's pixel layout needs no carousel
  // awareness beyond the visibility filter.
  int row = 1;
  int col = 1;
  int row_span = 1;
  int col_span = 1;
  // Carousel membership. carousel_index identifies which carousel on the page
  // (a synthetic id derived from its grid position, since the layout JSON
  // gives carousels no id of their own); the *_row/col/span fields describe
  // that carousel's own region, which is what the page dots are drawn in.
  bool in_carousel = false;
  int carousel_index = -1;
  int carousel_page_index = 0;
  int carousel_page_count = 1;
  int carousel_row = 1;
  int carousel_col = 1;
  int carousel_row_span = 1;
  int carousel_col_span = 1;
};

// One named dashboard's full card list (e.g. "home" or "remote"), replaced
// wholesale on each papers3/dashboard/layout message - no in-place diffing,
// since DashboardGrid always does a full redraw rather than root's granular
// per-card partial refresh.
struct DashboardPageCards {
  static constexpr int kMaxCards = 64;
  // Matches the standalone dashboard's DashboardPage::kDefaultGridRows/Cols -
  // shared by both the flow-cursor parser (ParseDashboardCardsPayload) and
  // DashboardGrid's pixel layout so card positions agree between the two.
  static constexpr int kGridRows = 10;
  static constexpr int kGridCols = 6;
  DashboardCard items[kMaxCards];
  int count = 0;
};

// Lightweight id-keyed on/off patch from papers3/dashboard/state.
struct DashboardCardStatePatch {
  String id;
  bool is_on = false;
};

}  // namespace dashboard
