#pragma once

#include <Arduino.h>

#include "mqtt_dashboard_bridge/DashboardState.h"

namespace dashboard {

// Ported from the standalone dashboard's paper::mqtt::ParseWeatherPayload
// (lib/mqtt_client/src/HaMessages.cpp). Only weather and tasks are ported so
// far; calendar/dashboard-cards parsing follows the same pattern.
bool ParseWeatherPayload(const char* payload, WeatherState& out_weather);

// Ported from paper::mqtt::ParseTasksPayload, trimmed to content + due_label
// only (no labels/description/children/postpone options - this activity is
// read-only, it doesn't complete or postpone tasks).
bool ParseTasksPayload(const char* payload, TasksState& out_tasks);

// Command model for a generic HA entity action publish, ported from
// paper::mqtt::EntityActionCommand.
struct EntityActionCommand {
  String action;
  String entity_id;
  String data_json;
};

// Ported from paper::mqtt::BuildEntityActionPayload.
bool BuildEntityActionPayload(const EntityActionCommand& cmd, String& out_payload);

// Ported from paper::app::DashboardPage::ActionForCard (include/app/DashboardPage.h) -
// infers a default HA action from the card's id/entity domain when the
// layout payload didn't specify an explicit "action".
String DefaultActionForCard(const DashboardCard& card);

// Ported from paper::mqtt::ParseDashboardButtonsPayload, trimmed to only the
// v3 schema path (`dashboards.<name>.cards`, see
// schema/dashboard-v3.schema.json in the standalone dashboard repo) - the
// older flat-array/vstack-hstack-layout fallback formats aren't supported.
// Card positions are computed by a flowing left-to-right, top-to-bottom
// cursor (matching the original algorithm) since the schema has no
// row/col fields, only cols/rows spans. "carousel" cards reserve their
// declared span (so sibling cards stay positioned correctly) but aren't
// parsed further - DashboardGrid doesn't render carousels yet.
bool ParseDashboardCardsPayload(const char* payload, const char* dashboard_name, DashboardPageCards& out_cards);

// Ported from paper::mqtt::ParseDashboardStatePayload.
bool ParseDashboardStatePayload(const char* payload,
                                const char* dashboard_name,
                                DashboardCardStatePatch* out_items,
                                int max_items,
                                int& out_count);

}  // namespace dashboard
