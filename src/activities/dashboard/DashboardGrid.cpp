#include "DashboardGrid.h"

namespace {
constexpr int kCellPad = 8;
constexpr int kCellInnerPad = 4;
// Carved off the bottom of a carousel's own region so its cards shrink to make
// room rather than the page dots overlapping them.
constexpr int kDotsReserve = 18;
}  // namespace

void DashboardGrid::SetContentBounds(const int topY, const int bottomY, const int width) {
  contentTopY_ = topY;
  contentBottomY_ = bottomY > topY ? bottomY : topY + 1;
  contentWidth_ = width;
  if (hasData_) {
    RecomputePixelLayout();
  }
}

void DashboardGrid::ApplyCards(const dashboard::DashboardPageCards& cards) {
  count_ = cards.count > kMaxCards ? kMaxCards : cards.count;
  for (int i = 0; i < count_; ++i) {
    cards_[i].data = cards.items[i];
  }
  hasData_ = true;
  RebuildCarousels();
  RecomputePixelLayout();
}

bool DashboardGrid::ApplyStatePatch(const dashboard::DashboardCardStatePatch* patches, const int count) {
  if (patches == nullptr || count <= 0) {
    return false;
  }

  bool changed = false;
  for (int p = 0; p < count; ++p) {
    for (int i = 0; i < count_; ++i) {
      if (cards_[i].data.id != patches[p].id) {
        continue;
      }
      if (cards_[i].data.type != dashboard::DashboardCardType::kButton &&
          cards_[i].data.type != dashboard::DashboardCardType::kToggle) {
        continue;
      }
      if (cards_[i].data.is_on != patches[p].is_on) {
        cards_[i].data.is_on = patches[p].is_on;
        changed = true;
      }
      break;
    }
  }
  return changed;
}

bool DashboardGrid::ToggleLocally(const int cardIndex) {
  if (cardIndex < 0 || cardIndex >= count_) {
    return false;
  }
  if (cards_[cardIndex].data.type != dashboard::DashboardCardType::kToggle) {
    return false;
  }
  cards_[cardIndex].data.is_on = !cards_[cardIndex].data.is_on;
  return true;
}

bool DashboardGrid::SetCarouselPage(const int slot, const int page) {
  if (slot < 0 || slot >= kMaxCarousels || !carousels_[slot].used) {
    return false;
  }
  if (page < 0 || page >= carousels_[slot].pageCount || page == carousels_[slot].activePage) {
    return false;
  }
  carousels_[slot].activePage = page;
  return true;
}

const dashboard::DashboardCard* DashboardGrid::GetCard(const int index) const {
  if (index < 0 || index >= count_) {
    return nullptr;
  }
  return &cards_[index].data;
}

const DashboardGrid::CarouselState* DashboardGrid::FindCarousel(const int index) const {
  for (int c = 0; c < kMaxCarousels; ++c) {
    if (carousels_[c].used && carousels_[c].index == index) {
      return &carousels_[c];
    }
  }
  return nullptr;
}

DashboardGrid::CarouselState* DashboardGrid::FindCarousel(const int index) {
  for (int c = 0; c < kMaxCarousels; ++c) {
    if (carousels_[c].used && carousels_[c].index == index) {
      return &carousels_[c];
    }
  }
  return nullptr;
}

bool DashboardGrid::IsHiddenByCarousel(const GridCard& card) const {
  if (!card.data.in_carousel) {
    return false;
  }
  const CarouselState* state = FindCarousel(card.data.carousel_index);
  if (state == nullptr) {
    return true;  // More carousels than kMaxCarousels; extras stay hidden.
  }
  return card.data.carousel_page_index != state->activePage;
}

void DashboardGrid::RebuildCarousels() {
  // Remember which page each carousel was on so a re-published layout (which
  // arrives whole, not as a diff) doesn't yank the user back to page 0 while
  // they're looking at page 2.
  CarouselState previous[kMaxCarousels];
  for (int i = 0; i < kMaxCarousels; ++i) {
    previous[i] = carousels_[i];
    carousels_[i] = CarouselState{};
  }

  for (int i = 0; i < count_; ++i) {
    const dashboard::DashboardCard& data = cards_[i].data;
    if (!data.in_carousel || data.carousel_index < 0) {
      continue;
    }
    CarouselState* state = FindCarousel(data.carousel_index);
    if (state == nullptr) {
      for (int c = 0; c < kMaxCarousels; ++c) {
        if (!carousels_[c].used) {
          state = &carousels_[c];
          state->used = true;
          state->index = data.carousel_index;
          state->pageCount = 1;
          state->activePage = 0;
          for (int p = 0; p < kMaxCarousels; ++p) {
            if (previous[p].used && previous[p].index == data.carousel_index) {
              state->activePage = previous[p].activePage;
              break;
            }
          }
          break;
        }
      }
    }
    if (state == nullptr) {
      continue;  // More carousels than kMaxCarousels; extra ones stay hidden.
    }
    if (data.carousel_page_count > state->pageCount) {
      state->pageCount = data.carousel_page_count;
    }
  }

  // Clamp restored pages against the new page counts.
  for (int c = 0; c < kMaxCarousels; ++c) {
    if (carousels_[c].used && carousels_[c].activePage >= carousels_[c].pageCount) {
      carousels_[c].activePage = 0;
    }
  }
}

void DashboardGrid::RecomputePixelLayout() {
  const int contentH = contentBottomY_ - contentTopY_;
  if (contentH <= 0 || contentWidth_ <= 0) {
    return;
  }

  const int gridX = kCellPad;
  const int gridY = contentTopY_ + kCellPad;
  const int gridW = contentWidth_ - (2 * kCellPad);
  const int gridH = contentH - (2 * kCellPad);
  if (gridW <= 0 || gridH <= 0) {
    return;
  }

  const int cellW = gridW / dashboard::DashboardPageCards::kGridCols;
  const int cellH = gridH / dashboard::DashboardPageCards::kGridRows;
  if (cellW <= 0 || cellH <= 0) {
    return;
  }

  // Each carousel's own region in pixels, and the dot strip along its bottom.
  for (int c = 0; c < kMaxCarousels; ++c) {
    carousels_[c].regionRect = freeink::ui::Rect{0, 0, 0, 0};
    carousels_[c].dotsRect = freeink::ui::Rect{0, 0, 0, 0};
  }
  for (int i = 0; i < count_; ++i) {
    const dashboard::DashboardCard& data = cards_[i].data;
    if (!data.in_carousel) {
      continue;
    }
    CarouselState* state = FindCarousel(data.carousel_index);
    if (state == nullptr || state->regionRect.height > 0) {
      continue;  // Already computed from an earlier card in the same carousel.
    }
    const int rRow0 = data.carousel_row - 1 < 0 ? 0 : data.carousel_row - 1;
    const int rCol0 = data.carousel_col - 1 < 0 ? 0 : data.carousel_col - 1;
    const int rRows = data.carousel_row_span < 1 ? 1 : data.carousel_row_span;
    const int rCols = data.carousel_col_span < 1 ? 1 : data.carousel_col_span;
    const int regionH = rRows * cellH;
    state->regionRect = freeink::ui::Rect{static_cast<int16_t>(gridX + (rCol0 * cellW)),
                                          static_cast<int16_t>(gridY + (rRow0 * cellH)),
                                          static_cast<int16_t>(rCols * cellW), static_cast<int16_t>(regionH)};
    state->dotsRect = freeink::ui::Rect{state->regionRect.x,
                                        static_cast<int16_t>(state->regionRect.y + regionH - kDotsReserve),
                                        static_cast<int16_t>(rCols * cellW), static_cast<int16_t>(kDotsReserve)};
  }

  for (int i = 0; i < count_; ++i) {
    GridCard& card = cards_[i];
    const int row0 = card.data.row - 1 < 0 ? 0 : card.data.row - 1;
    const int col0 = card.data.col - 1 < 0 ? 0 : card.data.col - 1;
    const int rspan = card.data.row_span < 1 ? 1 : card.data.row_span;
    const int cspan = card.data.col_span < 1 ? 1 : card.data.col_span;

    int x = gridX + (col0 * cellW) + kCellInnerPad;
    int y = gridY + (row0 * cellH) + kCellInnerPad;
    int w = (cspan * cellW) - (2 * kCellInnerPad);
    int h = (rspan * cellH) - (2 * kCellInnerPad);

    // Cards inside a carousel get squeezed vertically into the region above the
    // dot strip, so the bottom row of a full-height carousel page doesn't sit
    // underneath its own dots. A linear remap keeps relative row proportions
    // intact, which matters because pages in the same carousel often have
    // different row counts.
    if (card.data.in_carousel) {
      const int rRow0 = card.data.carousel_row - 1 < 0 ? 0 : card.data.carousel_row - 1;
      const int rRows = card.data.carousel_row_span < 1 ? 1 : card.data.carousel_row_span;
      const int regionY = gridY + (rRow0 * cellH);
      const int regionH = rRows * cellH;
      const int usableH = regionH - kDotsReserve;
      if (regionH > 0 && usableH > 0) {
        y = regionY + (((y - regionY) * usableH) / regionH);
        h = (h * usableH) / regionH;
      }
    }

    card.pixelRect = freeink::ui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y),
                                       static_cast<int16_t>(w > 1 ? w : 1), static_cast<int16_t>(h > 1 ? h : 1)};
  }
}
