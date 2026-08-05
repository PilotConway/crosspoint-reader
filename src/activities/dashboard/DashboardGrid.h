#pragma once

#include <FreeInkUI.h>

#include "mqtt_dashboard_bridge/DashboardState.h"

// Renders one named dashboard's card grid (e.g. "home" or "remote").
//
// Built on FreeInkUI: every card is emitted as a component that registers its
// own hit rect into the frame's interaction buffer, so this class no longer
// hit-tests anything itself. Taps come back as routed ActionEvents, which
// DashboardActivity decodes via DecodeCardAction / DecodeDotAction below.
//
// Carousels are supported: cards arrive pre-flattened and tagged with their
// carousel/page (see DashboardCard in DashboardState.h), and this class shows
// one page per carousel at a time, with tappable page dots beneath it. Paging
// is by tapping the dots — horizontal swipes belong to the global back gesture
// on this stack, so the grid deliberately claims none.
class DashboardGrid {
 public:
  // Action ids emitted into the frame. DashboardActivity owns the numbering for
  // its own chrome (tabs) and must not reuse these.
  static constexpr freeink::ui::ActionId kActionCard = 10;
  static constexpr freeink::ui::ActionId kActionDot = 11;

  // Dot action values pack the carousel slot and target page into one int16_t.
  static constexpr int16_t EncodeDot(int slot, int page) {
    return static_cast<int16_t>((slot << 8) | (page & 0xFF));
  }
  static constexpr int DecodeDotSlot(int16_t value) { return (value >> 8) & 0xFF; }
  static constexpr int DecodeDotPage(int16_t value) { return value & 0xFF; }

  // top_y/bottom_y/width bound the drawable content area (excludes header, tab
  // bar, and any footer chrome).
  void SetContentBounds(int topY, int bottomY, int width);

  // Replaces the whole card set (matches DashboardActivity's full-replace
  // model). Recomputes pixel layout.
  void ApplyCards(const dashboard::DashboardPageCards& cards);

  // Applies id-keyed on/off patches to button/toggle cards already present.
  // Returns true if any card's is_on value actually changed.
  bool ApplyStatePatch(const dashboard::DashboardCardStatePatch* patches, int count);

  bool HasData() const { return hasData_; }

  // Flips a toggle card's local state (optimistic echo before the broker
  // confirms). No-op for non-toggle cards. Returns true if anything changed.
  bool ToggleLocally(int cardIndex);

  // Advances a carousel to a page. Returns true if the active page changed.
  bool SetCarouselPage(int slot, int page);

  const dashboard::DashboardCard* GetCard(int index) const;

  // Emits every visible card plus each carousel's page dots into the frame.
  // pressedIndex, when >= 0, renders that card in its pressed state for tap
  // feedback; the caller owns the flash timing.
  template <size_t MaxInteractions>
  void Draw(freeink::ui::Frame<MaxInteractions>& frame, const dashboard::WeatherState& weather,
            int pressedIndex = -1) const;

 private:
  struct GridCard {
    dashboard::DashboardCard data;
    freeink::ui::Rect pixelRect{0, 0, 0, 0};
  };

  // One carousel's paging state. Rebuilt from the card list on every
  // ApplyCards(), so a layout change can't leave a stale active page pointing
  // past the end of a shrunk carousel.
  struct CarouselState {
    bool used = false;
    int index = -1;
    int pageCount = 1;
    int activePage = 0;
    freeink::ui::Rect regionRect{0, 0, 0, 0};
    freeink::ui::Rect dotsRect{0, 0, 0, 0};
  };

  void RecomputePixelLayout();
  void RebuildCarousels();
  bool IsHiddenByCarousel(const GridCard& card) const;
  const CarouselState* FindCarousel(int index) const;
  CarouselState* FindCarousel(int index);

  template <size_t MaxInteractions>
  void DrawCard(freeink::ui::Frame<MaxInteractions>& frame, const GridCard& card, int cardIndex,
                const dashboard::WeatherState& weather, bool pressed) const;
  template <size_t MaxInteractions>
  void DrawCarouselDots(freeink::ui::Frame<MaxInteractions>& frame, const CarouselState& carousel, int slot) const;

  static constexpr int kMaxCards = dashboard::DashboardPageCards::kMaxCards;
  static constexpr int kMaxCarousels = 4;
  GridCard cards_[kMaxCards];
  int count_ = 0;
  bool hasData_ = false;
  CarouselState carousels_[kMaxCarousels];

  int contentTopY_ = 0;
  int contentBottomY_ = 0;
  int contentWidth_ = 0;
};

#include "DashboardGrid.inl"
