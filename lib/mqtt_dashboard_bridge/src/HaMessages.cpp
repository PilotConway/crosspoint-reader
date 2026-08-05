#include "mqtt_dashboard_bridge/HaMessages.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace dashboard {
namespace {

// Today as a YYYYMMDD integer, or 0 if the system clock isn't set yet.
//
// The explicit 0ms timeout is load-bearing. getLocalTime()'s default is 5000ms
// and it does NOT return early on failure - it polls the clock in a
// delay(10) loop for the whole budget before giving up (see the Arduino core's
// esp32-hal-time.c). Calling it once per task with that default blocked the
// MQTT worker for 5s x task count before NTP had synced, which stopped
// anything from draining the socket and made later retained messages -
// papers3/dashboard/layout in particular - look like they were never sent.
int GetTodayKey() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    return 0;
  }
  return (timeinfo.tm_year + 1900) * 10000 + (timeinfo.tm_mon + 1) * 100 + timeinfo.tm_mday;
}

// Ported from paper::mqtt::SetTaskDueLabel (lib/mqtt_client/src/HaMessages.cpp).
// today_key comes from GetTodayKey(), resolved once per payload by the caller;
// pass 0 when the clock isn't set and the label is left blank rather than
// guessed.
//
// Note: the clock reflects whatever TZ HalClock synced with - currently
// HalClock::syncFromNTP() sets the system clock to UTC0 (see
// lib/hal/HalClock.cpp), not the device's local timezone, so this label can be
// off by a day near local midnight until Crosspoint gets real TZ support.
void SetTaskDueLabel(TaskItem& item, const char* due_str, int today_key) {
  item.due_label = "";
  if (today_key == 0 || due_str == nullptr || strlen(due_str) < 10) {
    return;
  }

  int due_year = 0;
  int due_month = 0;
  int due_day = 0;
  if (sscanf(due_str, "%4d-%2d-%2d", &due_year, &due_month, &due_day) != 3) {
    return;
  }

  const int due_key = due_year * 10000 + due_month * 100 + due_day;

  if (due_key < today_key) {
    item.due_label = "OVERDUE";
  } else if (due_key == today_key) {
    item.due_label = "TODAY";
  }
}

// Ported from the standalone dashboard's anonymous-namespace ParseCardStateIsOn.
bool ParseCardStateIsOn(JsonObjectConst card) {
  if (card["state"].is<bool>()) {
    return card["state"].as<bool>();
  }
  const char* state = card["state"];
  if (state == nullptr) {
    return false;
  }
  return strcmp(state, "on") == 0 || strcmp(state, "true") == 0 || strcmp(state, "1") == 0;
}

// Ported from the standalone dashboard's anonymous-namespace SerializeVariantToJson.
String SerializeVariantToJson(JsonVariantConst value) {
  String out;
  if (value.isNull()) {
    return out;
  }
  serializeJson(value, out);
  return out;
}

DashboardCardType ParseCardType(const char* type) {
  if (strcmp(type, "toggle") == 0) return DashboardCardType::kToggle;
  if (strcmp(type, "weather") == 0) return DashboardCardType::kWeather;
  if (strcmp(type, "text") == 0) return DashboardCardType::kText;
  if (strcmp(type, "calendar") == 0) return DashboardCardType::kCalendar;
  if (strcmp(type, "tasks") == 0) return DashboardCardType::kTasks;
  if (strcmp(type, "spacer") == 0) return DashboardCardType::kSpacer;
  if (strcmp(type, "carousel") == 0) return DashboardCardType::kCarousel;
  return DashboardCardType::kButton;
}

DashboardCardStyle ParseCardStyle(const char* style) {
  if (style == nullptr) return DashboardCardStyle::kRoundedRect;
  if (strcmp(style, "rect") == 0) return DashboardCardStyle::kRect;
  if (strcmp(style, "round") == 0) return DashboardCardStyle::kRound;
  return DashboardCardStyle::kRoundedRect;
}

// Describes which carousel (if any) the region being parsed belongs to.
// carousel_index < 0 means "not inside a carousel" - i.e. the page's own
// top-level region.
struct CarouselContext {
  int index = -1;
  int page_index = 0;
  int page_count = 1;
  int row = 1;
  int col = 1;
  int row_span = 1;
  int col_span = 1;
};

// Ported from the standalone dashboard's ParseRemoteFlowCardsArray
// (lib/mqtt_client/src/HaMessages.cpp), including the carousel recursion
// branch: a "carousel" card's nested "pages" are each parsed as their own
// region offset to where the carousel sits, and every card produced is tagged
// with its carousel/page so DashboardGrid can show one page at a time. The
// flow cursor, row-height tracking, and spacer handling match the original
// exactly.
//
// region_row/region_col are the 1-based absolute grid position this region
// starts at; region_rows/region_cols are its extent. The flow cursor runs in
// region-local coordinates and is offset into absolute coordinates when a
// card is emitted, so nested carousels compose correctly.
void ParseCardsRegion(JsonArrayConst cards, DashboardPageCards& out_cards, int region_row, int region_col,
                      int region_rows, int region_cols, const CarouselContext& carousel) {
  int cursor_row = 1;  // 1-based, region-local
  int cursor_col = 1;  // 1-based, region-local
  int row_height = 1;

  for (JsonVariantConst v : cards) {
    if (out_cards.count >= DashboardPageCards::kMaxCards || !v.is<JsonObjectConst>()) {
      break;
    }
    JsonObjectConst card = v.as<JsonObjectConst>();
    const char* type = card["type"];
    if (type == nullptr || strlen(type) == 0) {
      continue;
    }

    while (cursor_col > region_cols) {
      cursor_col -= region_cols;
      cursor_row += row_height;
      row_height = 1;
    }
    if (cursor_row > region_rows) {
      break;
    }

    int cols = card["cols"] | 2;
    int rows = card["rows"] | 1;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (rows > row_height) {
      row_height = rows;
    }

    if (strcmp(type, "spacer") == 0) {
      cursor_col += cols;
      continue;
    }

    if (strcmp(type, "carousel") == 0) {
      if (!card["pages"].is<JsonArrayConst>()) {
        cursor_col += cols;
        continue;
      }
      JsonArrayConst pages = card["pages"].as<JsonArrayConst>();
      const int page_count = static_cast<int>(pages.size());
      if (page_count <= 0) {
        cursor_col += cols;
        continue;
      }

      const int nested_row = region_row + cursor_row - 1;
      const int nested_col = region_col + cursor_col - 1;
      // The layout JSON gives carousels no id, so synthesise a stable one from
      // where this carousel sits. Distinct carousels on a page can't collide
      // because two of them cannot start at the same grid cell. A nested
      // carousel inherits its parent's index so paging stays attached to the
      // outermost one the user can actually see.
      CarouselContext nested = carousel;
      if (nested.index < 0) {
        nested.index = cursor_row * 1000 + cursor_col * 10 + region_row + region_col;
      }
      nested.page_count = page_count;
      nested.row = nested_row;
      nested.col = nested_col;
      nested.row_span = rows;
      nested.col_span = cols;

      for (int p = 0; p < page_count; ++p) {
        JsonVariantConst page_var = pages[p];
        if (!page_var.is<JsonObjectConst>()) {
          continue;
        }
        JsonObjectConst page = page_var.as<JsonObjectConst>();
        if (!page["cards"].is<JsonArrayConst>()) {
          continue;
        }
        nested.page_index = p;
        ParseCardsRegion(page["cards"].as<JsonArrayConst>(), out_cards, nested_row, nested_col, rows, cols, nested);
      }
      cursor_col += cols;
      continue;
    }

    const char* entity_id = card["entity_id"];
    const char* id = card["id"];
    const char* label = card["label"];
    const char* text = card["text"];
    const char* icon = card["icon"];
    const char* action = card["action"];

    DashboardCard out{};
    out.type = ParseCardType(type);
    if ((strcmp(type, "button") == 0 || strcmp(type, "toggle") == 0) && entity_id != nullptr &&
        strlen(entity_id) > 0) {
      out.id = String(entity_id);
    } else if (id != nullptr && strlen(id) > 0) {
      out.id = String(id);
    }

    if (label != nullptr && strlen(label) > 0) {
      out.label = String(label);
    } else if (text != nullptr && strlen(text) > 0) {
      out.label = String(text);
    } else if (strcmp(type, "calendar") == 0) {
      out.label = "Calendar";
    } else if (strcmp(type, "tasks") == 0) {
      out.label = "Tasks";
    } else if (strcmp(type, "weather") == 0) {
      out.label = "Weather";
    } else {
      out.label = out.id;
    }
    if (out.label.length() == 0) {
      cursor_col += cols;
      continue;
    }

    out.style = ParseCardStyle(card["style"]);
    out.action = action != nullptr ? String(action) : String("");
    out.icon = icon != nullptr ? String(icon) : String("");
    out.text = text != nullptr ? String(text) : String("");
    out.action_data_json = SerializeVariantToJson(card["data"]);
    if (out.type == DashboardCardType::kToggle || out.type == DashboardCardType::kButton) {
      out.is_on = ParseCardStateIsOn(card);
    }

    out.row = region_row + cursor_row - 1;
    out.col = region_col + cursor_col - 1;
    out.row_span = rows;
    out.col_span = cols;

    if (carousel.index >= 0) {
      out.in_carousel = true;
      out.carousel_index = carousel.index;
      out.carousel_page_index = carousel.page_index;
      out.carousel_page_count = carousel.page_count > 0 ? carousel.page_count : 1;
      out.carousel_row = carousel.row;
      out.carousel_col = carousel.col;
      out.carousel_row_span = carousel.row_span;
      out.carousel_col_span = carousel.col_span;
    }

    out_cards.items[out_cards.count++] = out;
    cursor_col += cols;
  }
}

}  // namespace

bool ParseWeatherPayload(const char* payload, WeatherState& out_weather) {
  if (payload == nullptr) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return false;
  }

  const char* condition = doc["condition"];
  if (condition != nullptr) {
    out_weather.condition = String(condition);
  }
  if (!doc["temp"].isNull() && doc["temp"].is<int>()) {
    out_weather.temperature_f = doc["temp"].as<int>();
  }
  if (!doc["rain_chance"].isNull() && doc["rain_chance"].is<int>()) {
    out_weather.rain_chance_pct = doc["rain_chance"].as<int>();
  }
  if (!doc["wind_speed"].isNull() && doc["wind_speed"].is<int>()) {
    out_weather.wind_speed_mph = doc["wind_speed"].as<int>();
  }

  return true;
}

bool ParseTasksPayload(const char* payload, TasksState& out_tasks) {
  if (payload == nullptr) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return false;
  }

  out_tasks.count = 0;

  if (doc["tasks"].isNull() || !doc["tasks"].is<JsonArray>()) {
    return true;
  }

  // Resolved once, not per task - see GetTodayKey().
  const int today_key = GetTodayKey();

  JsonArray tasks = doc["tasks"].as<JsonArray>();
  for (JsonVariant task : tasks) {
    if (out_tasks.count >= TasksState::kMaxTasks) {
      break;
    }

    const char* summary = task["summary"];
    if (summary == nullptr) {
      summary = task["title"];
    }
    if (summary == nullptr) {
      continue;
    }

    TaskItem& item = out_tasks.items[out_tasks.count];
    item.content = String(summary);
    const char* due_str = task["due"];
    SetTaskDueLabel(item, due_str, today_key);

    out_tasks.count++;
  }

  return true;
}

bool BuildEntityActionPayload(const EntityActionCommand& cmd, String& out_payload) {
  if (cmd.action.length() == 0 || cmd.entity_id.length() == 0) {
    return false;
  }

  JsonDocument doc;
  doc["action"] = cmd.action;
  doc["entity_id"] = cmd.entity_id;
  if (cmd.data_json.length() > 0) {
    JsonDocument data_doc;
    const DeserializationError data_err = deserializeJson(data_doc, cmd.data_json.c_str());
    if (data_err || !data_doc.is<JsonObject>()) {
      return false;
    }
    doc["data"] = data_doc.as<JsonObjectConst>();
  }

  out_payload = "";
  return serializeJson(doc, out_payload) > 0;
}

String DefaultActionForCard(const DashboardCard& card) {
  if (card.action.length() > 0) {
    return card.action;
  }

  const int dot = card.id.indexOf('.');
  if (dot <= 0) {
    return card.type == DashboardCardType::kToggle ? String("entity.toggle") : String("entity.press");
  }

  const String domain = card.id.substring(0, dot);
  if (card.type == DashboardCardType::kToggle) {
    return domain + ".toggle";
  }

  if (domain == "scene" || domain == "script") {
    return domain + ".turn_on";
  }
  if (domain == "button") {
    return String("button.press");
  }
  return domain + ".turn_on";
}

bool ParseDashboardCardsPayload(const char* payload, const char* dashboard_name, DashboardPageCards& out_cards) {
  out_cards.count = 0;
  if (payload == nullptr || dashboard_name == nullptr) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return false;
  }

  JsonVariantConst dashboard_cards = doc["dashboards"][dashboard_name]["cards"];
  if (dashboard_cards.isNull() || !dashboard_cards.is<JsonArrayConst>()) {
    return false;
  }

  ParseCardsRegion(dashboard_cards.as<JsonArrayConst>(), out_cards, 1, 1, DashboardPageCards::kGridRows,
                   DashboardPageCards::kGridCols, CarouselContext{});
  return out_cards.count > 0;
}

bool ParseDashboardStatePayload(const char* payload,
                                const char* dashboard_name,
                                DashboardCardStatePatch* out_items,
                                int max_items,
                                int& out_count) {
  out_count = 0;
  if (payload == nullptr || dashboard_name == nullptr || out_items == nullptr || max_items <= 0) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return false;
  }

  JsonVariantConst states = doc["dashboards"][dashboard_name]["states"];
  if (states.isNull() || !states.is<JsonArrayConst>()) {
    return false;
  }

  for (JsonVariantConst entry : states.as<JsonArrayConst>()) {
    if (out_count >= max_items) {
      break;
    }

    const char* id = entry["id"];
    if (id == nullptr || strlen(id) == 0) {
      continue;
    }

    out_items[out_count].id = String(id);
    out_items[out_count].is_on = ParseCardStateIsOn(entry.as<JsonObjectConst>());
    out_count++;
  }

  return out_count > 0;
}

}  // namespace dashboard
