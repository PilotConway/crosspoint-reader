#pragma once

namespace dashboard {

// MQTT topics this prototype speaks, ported from the standalone dashboard's
// paper::Topics (see ../paper-s3-personal-dashboard/include/app/Topics.h).
// Calendar topics (kCalendarRefresh/kCalendarState) aren't ported yet - the
// Calendar card type is parsed but not rendered, see DashboardGrid.
struct Topics {
  static constexpr const char* kWeatherState = "papers3/weather/state";
  static constexpr const char* kTasksState = "papers3/tasks/state";
  // Publishing "1" here is what makes the HA-side automation compute and
  // publish kTasksState - unlike weather, tasks aren't pushed unprompted.
  static constexpr const char* kTasksRefresh = "papers3/tasks/refresh";
  // Retained; changes rarely. Card structure/positions for the grid dashboard.
  static constexpr const char* kDashboardLayout = "papers3/dashboard/layout";
  // Not retained; changes often. Lightweight id-keyed on/off patches only.
  static constexpr const char* kDashboardState = "papers3/dashboard/state";
  // Generic outbound command topic (name is historical - carries all entity
  // actions, not just living-room light commands). Matches the standalone
  // dashboard's paper::Topics::kCmd exactly since it's the same HA broker.
  static constexpr const char* kCmd = "papers3/livingroom/cmd";
};

}  // namespace dashboard
