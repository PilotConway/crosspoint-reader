#pragma once

// Template bodies for DashboardGrid's rendering. Included from DashboardGrid.h;
// separate only so the header stays readable.

#include <FreeInkUIGfxRenderer.h>
#include <components/controls/button.h>
#include <components/media/metric-card.h>

#include "fontIds.h"

namespace dashboard_grid_detail {

// Dot strip geometry, shared by the dots renderer.
constexpr int16_t kDotDiameter = 8;
constexpr int16_t kDotGap = 10;

inline freeink::ui::StyleSet cardStyles(bool on) {
  // Toggle/button "on" reads as a filled pill, "off" as an outline. e-ink has no
  // colour to lean on, so state has to be carried by fill vs stroke.
  freeink::ui::StyleSet styles = freeink::ui::defaultButtonStyles();
  if (on) {
    styles.normal.background = freeink::ui::Paint::solid(freeink::ui::Color::Black);
    styles.normal.foreground = freeink::ui::Paint::solid(freeink::ui::Color::White);
  }
  return styles;
}

}  // namespace dashboard_grid_detail

template <size_t MaxInteractions>
void DashboardGrid::DrawCarouselDots(freeink::ui::Frame<MaxInteractions>& frame, const CarouselState& carousel,
                                     const int slot) const {
  namespace fui = freeink::ui;
  using namespace dashboard_grid_detail;

  if (carousel.pageCount <= 1 || carousel.dotsRect.empty()) {
    return;
  }

  const int16_t stride = static_cast<int16_t>(kDotDiameter + kDotGap);
  const int16_t totalW = static_cast<int16_t>((carousel.pageCount * stride) - kDotGap);
  int16_t x = static_cast<int16_t>(carousel.dotsRect.x + ((carousel.dotsRect.width - totalW) / 2));
  const int16_t y =
      static_cast<int16_t>(carousel.dotsRect.y + ((carousel.dotsRect.height - kDotDiameter) / 2));

  for (int page = 0; page < carousel.pageCount; ++page) {
    const fui::Rect dot{x, y, kDotDiameter, kDotDiameter};
    // Register the tap target before painting so the dot picks up its own
    // pressed state; ensureMinTouchRect grows the 8px dot to a finger-sized hit
    // area without changing what's drawn.
    frame.hit(fui::ensureMinTouchRect(dot, frame.device().minTouchSize, frame.screen()), kActionDot,
              EncodeDot(slot, page), fui::InputDefault, fui::StateNormal);
    if (page == carousel.activePage) {
      frame.target().fill(dot, fui::Paint::solid(fui::Color::Black), kDotDiameter / 2);
    } else {
      frame.target().stroke(dot, fui::Paint::solid(fui::Color::Black), 1, kDotDiameter / 2);
    }
    x = static_cast<int16_t>(x + stride);
  }
}

template <size_t MaxInteractions>
void DashboardGrid::DrawCard(freeink::ui::Frame<MaxInteractions>& frame, const GridCard& card, const int cardIndex,
                             const dashboard::WeatherState& weather, const bool pressed) const {
  namespace fui = freeink::ui;
  using namespace dashboard_grid_detail;

  if (card.pixelRect.empty()) {
    return;
  }
  const fui::State state = pressed ? fui::StateActive : fui::StateNormal;

  switch (card.data.type) {
    case dashboard::DashboardCardType::kButton:
    case dashboard::DashboardCardType::kToggle: {
      // Both are tappable; only a toggle carries on/off styling. metricCard
      // isn't used here because a button's whole job is its label + state.
      fui::ButtonProps props;
      props.label = card.data.label.c_str();
      props.action = kActionCard;
      props.value = static_cast<int16_t>(cardIndex);
      props.state = state;
      props.text.font = fui::GfxRendererTarget::FONT_BODY;
      props.radius = card.data.style == dashboard::DashboardCardStyle::kRect ? 0 : 8;
      if (card.data.type == dashboard::DashboardCardType::kToggle) {
        props.styles = cardStyles(card.data.is_on);
      }
      fui::button(frame, card.pixelRect, props);
      break;
    }
    case dashboard::DashboardCardType::kWeather: {
      // The one card whose value comes from a separate MQTT topic rather than
      // the layout, so it reads WeatherState instead of card.data.
      char value[16];
      snprintf(value, sizeof(value), "%d", weather.temperature_f);
      char caption[48];
      snprintf(caption, sizeof(caption), "%d%% rain  %d mph", weather.rain_chance_pct, weather.wind_speed_mph);

      fui::MetricCardProps props;
      props.label = card.data.label.length() > 0 ? card.data.label.c_str() : weather.condition.c_str();
      props.value = value;
      props.unit = "\xC2\xB0" "F";
      props.caption = caption;
      props.labelText.font = fui::GfxRendererTarget::FONT_SMALL;
      props.valueText.font = fui::GfxRendererTarget::FONT_BODY;
      props.captionText.font = fui::GfxRendererTarget::FONT_SMALL;
      props.state = state;
      fui::metricCard(frame, card.pixelRect, props);
      break;
    }
    case dashboard::DashboardCardType::kText: {
      fui::MetricCardProps props;
      props.label = card.data.label.length() > 0 ? card.data.label.c_str() : nullptr;
      props.value = card.data.text.c_str();
      props.labelText.font = fui::GfxRendererTarget::FONT_SMALL;
      props.valueText.font = fui::GfxRendererTarget::FONT_BODY;
      props.state = state;
      fui::metricCard(frame, card.pixelRect, props);
      break;
    }
    case dashboard::DashboardCardType::kSpacer:
    case dashboard::DashboardCardType::kCalendar:
    case dashboard::DashboardCardType::kTasks:
    case dashboard::DashboardCardType::kCarousel:
    default:
      // Spacers reserve grid space only. Calendar/tasks/carousel arrive parsed
      // so sibling positions stay correct, but the carousel itself is drawn via
      // its member cards + dots, and tasks live on their own tab.
      break;
  }
}

template <size_t MaxInteractions>
void DashboardGrid::Draw(freeink::ui::Frame<MaxInteractions>& frame, const dashboard::WeatherState& weather,
                         const int pressedIndex) const {
  for (int i = 0; i < count_; ++i) {
    if (IsHiddenByCarousel(cards_[i])) {
      continue;
    }
    DrawCard(frame, cards_[i], i, weather, i == pressedIndex);
  }
  for (int slot = 0; slot < kMaxCarousels; ++slot) {
    if (carousels_[slot].used) {
      DrawCarouselDots(frame, carousels_[slot], slot);
    }
  }
}
