// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "PlacementDRC.h"
#include "boost/geometry/geometry.hpp"
#include "boost/random/uniform_int_distribution.hpp"
#include "dpl/Opendp.h"
#include "graphics/DplObserver.h"
#include "infrastructure/Coordinates.h"
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include "infrastructure/Padding.h"
#include "infrastructure/architecture.h"
#include "infrastructure/network.h"
#include "odb/db.h"
#include "odb/dbTransform.h"
#include "odb/geom.h"
#include "optimization/detailed_orient.h"
#include "util/journal.h"
#include "util/symmetry.h"
#include "utl/Logger.h"
// #define ODP_DEBUG

namespace dpl {
using std::max;
using std::min;
using std::numeric_limits;
using std::string;
using std::vector;

using utl::DPL;

using utl::format_as;  // NOLINT(misc-unused-using-decls)

std::string Opendp::printBgBox(
    const boost::geometry::model::box<bgPoint>& queryBox)
{
  return fmt::format("({0}, {1}) - ({2}, {3})",
                     queryBox.min_corner().x(),
                     queryBox.min_corner().y(),
                     queryBox.max_corner().x(),
                     queryBox.max_corner().y());
}

// The order below is a correctness requirement, not a convention.  Every
// obstruction has to be painted into the grid before the first
// canBePlaced() query runs, because the site search reads an unpainted
// pixel as free and will happily hand it out.  Fixed cells go down first,
// then already-legalized cells when running incrementally, then
// fence-region ownership.  Only then may cells be searched for, and
// grouped cells go before the general pass because each of them is
// confined to its own fence region while an ungrouped cell may take any
// site the region rectangles leave free.
void Opendp::diamondDPL()
{
  if (debug_observer_) {
    debug_observer_->startPlacement(block_);
  }

  placement_failures_.clear();
  initGrid();
  // Paint fixed cells.
  setFixedGridCells();
  // Paint initially place2d cells (respecting already legalized ones).
  if (incremental_) {
    logger_->report("setInitialGridCells()");
    setInitialGridCells();
  }
  // group mapping & x_axis dummycell insertion
  groupInitPixels2();
  // y axis dummycell insertion
  groupInitPixels();

  if (!arch_->getRegions().empty()) {
    placeGroups();
  }

  if (debug_observer_) {
    logger_->report("Pause before detail placement.");
    debug_observer_->redrawAndPause();
  }

  place();

  if (debug_observer_) {
    logger_->report("Pause after detail placement.");
    debug_observer_->redrawAndPause();
  }
}

////////////////////////////////////////////////////////////////

void Opendp::placeGroups()
{
  groupAssignCellRegions();

  prePlaceGroups();
  prePlace();

  // naive placement method ( multi -> single )
  placeGroups2();
  for (auto& group : arch_->getRegions()) {
    // magic number alert
    for (int pass = 0; pass < 3; pass++) {
      int refine_count = groupRefine(group);
      int anneal_count = anneal(group);
      // magic number alert
      if (refine_count < 10 || anneal_count < 100) {
        break;
      }
    }
  }
}

// An ungrouped cell straddling the left edge of a fence region is a
// conflict the general pass cannot resolve cheaply: checkPixels() rejects
// every region-owned pixel for a cell that belongs to no region, so the
// search would have to walk clear of the region before finding anything
// legal, and the displacement limits may not reach that far.  Seeding such
// cells at the region edge up front, and marking the ones that succeed
// held so later passes leave them alone, keeps that walk out of the main
// pass.
//
// Only that one straddling case is picked up here.  The predicate below
// asks two things of the cell's unpadded pre-legalization box:
// horizontally, that it start to the left of the rectangle's left edge and
// extend past it; vertically, only that it overlap the rectangle at all.
// The horizontal half is what narrows the pass -- a cell wholly inside a
// rectangle, or overlapping only its right-hand part, fails it however
// badly the region and the cell collide, and is left to the general pass
// and to the recovery path.
void Opendp::prePlace()
{
  for (auto& cell : network_->getNodes()) {
    if (cell->getType() != Node::CELL) {
      continue;
    }
    const odb::Rect* group_rect = nullptr;
    if (!cell->inGroup() && !cell->isPlaced()) {
      for (auto& group : arch_->getRegions()) {
        for (const odb::Rect& rect : group->getRects()) {
          if (checkOverlap(cell.get(), rect)) {
            group_rect = &rect;
          }
        }
      }
      if (group_rect) {
        const DbuPt nearest = nearestPt(cell.get(), *group_rect);
        const GridPt legal = legalGridPt(cell.get(), nearest);
        if (diamondMove(cell.get(), legal)) {
          cell->setHold(true);
        }
      }
    }
  }
}

bool Opendp::checkOverlap(const Node* cell, const DbuRect& rect) const
{
  const DbuPt init = initialLocation(cell, false);
  const DbuX x = init.x;
  const DbuY y = init.y;
  return x + cell->getWidth() > rect.xl && x < rect.xl
         && y + cell->getHeight() > rect.yl && y < rect.yh;
}

DbuPt Opendp::nearestPt(const Node* cell, const DbuRect& rect) const
{
  const DbuPt init = initialLocation(cell, false);
  const DbuX x = init.x;
  const DbuY y = init.y;

  DbuX temp_x = x;
  DbuY temp_y = y;

  const DbuX cell_width = cell->getWidth();
  if (checkOverlap(cell, rect)) {
    DbuX dist_x;
    DbuY dist_y;
    if (abs(x + cell_width - rect.xl) > abs(rect.xh - x)) {
      dist_x = abs(rect.xh - x);
      temp_x = rect.xh;
    } else {
      dist_x = abs(x - rect.xl);
      temp_x = rect.xl - cell_width;
    }
    if (abs(y + cell->getHeight() - rect.yl) > abs(rect.yh - y)) {
      dist_y = abs(rect.yh - y);
      temp_y = rect.yh;
    } else {
      dist_y = abs(y - rect.yl);
      temp_y = rect.yl - cell->getHeight();
    }
    if (dist_x.v < dist_y.v) {
      return {temp_x, y};
    }
    return {x, temp_y};
  }

  if (x < rect.xl) {
    temp_x = rect.xl;
  } else if (x + cell_width > rect.xh) {
    temp_x = rect.xh - cell_width;
  }

  if (y < rect.yl) {
    temp_y = rect.yl;
  } else if (y + cell->getHeight() > rect.yh) {
    temp_y = rect.yh - cell->getHeight();
  }

  return {temp_x, temp_y};
}

// The mirror image of prePlace().  A cell that belongs to a fence region
// but whose global-placement position falls outside every rectangle of
// that region starts with no legal site anywhere near it, because the
// search is additionally clipped to the region boundary.  Seeding it at
// the nearest point of the nearest rectangle gives the later search an
// origin from which legal sites are actually reachable.  Cells that
// already start inside a rectangle are left for the general ordering to
// decide.
void Opendp::prePlaceGroups()
{
  for (auto& group : arch_->getRegions()) {
    for (Node* cell : group->getCells()) {
      if (!cell->isFixed() && !cell->isPlaced()) {
        int dist = numeric_limits<int>::max();
        bool in_group = false;
        const odb::Rect* nearest_rect = nullptr;
        for (const odb::Rect& rect : group->getRects()) {
          if (isInside(cell, rect)) {
            in_group = true;
          }
          int rect_dist = distToRect(cell, rect);
          if (rect_dist < dist) {
            dist = rect_dist;
            nearest_rect = &rect;
          }
        }
        if (!nearest_rect) {
          continue;  // degenerate case of empty group.regions
        }
        if (!in_group) {
          const DbuPt nearest = nearestPt(cell, *nearest_rect);
          const GridPt legal = legalGridPt(cell, nearest);
          if (diamondMove(cell, legal)) {
            cell->setHold(true);
          }
        }
      }
    }
  }
}

bool Opendp::isInside(const Node* cell, const odb::Rect& rect) const
{
  const DbuPt init = initialLocation(cell, false);
  const DbuX x = init.x;
  const DbuY y = init.y;
  return x >= rect.xMin() && x + cell->getWidth() <= rect.xMax()
         && y >= rect.yMin() && y + cell->getHeight() <= rect.yMax();
}

int Opendp::distToRect(const Node* cell, const odb::Rect& rect) const
{
  const DbuPt init = initialLocation(cell, true);
  const DbuX x = init.x;
  const DbuY y = init.y;

  DbuX dist_x{0};
  DbuY dist_y{0};
  if (x < rect.xMin()) {
    dist_x = DbuX{rect.xMin()} - x;
  } else if (x + cell->getWidth() > rect.xMax()) {
    dist_x = x + cell->getWidth() - rect.xMax();
  }

  if (y < rect.yMin()) {
    dist_y = DbuY{rect.yMin()} - y;
  } else if (y + cell->getHeight() > rect.yMax()) {
    dist_y = y + cell->getHeight() - rect.yMax();
  }

  return sumXY(dist_x, dist_y);
}

// The order cells are legalized in is the whole of this legalizer's
// greedy heuristic: the grid is claimed first come, first served, so
// whoever sorts earlier gets the better site.  This comparator therefore
// front-loads the cells with the fewest legal options, and ends on a
// unique key so the resulting order is reproducible.  It is a friend of
// Opendp only so it can reach isMultiRow(); the core centre it measures
// against is captured once at construction rather than per comparison.
class CellPlaceOrderLess
{
 public:
  explicit CellPlaceOrderLess(const odb::Rect& core, const Opendp* opendp);
  bool operator()(const Node* cell1, const Node* cell2) const;

 private:
  int centerDist(const Node* cell) const;

  const int center_x_;
  const int center_y_;
  const Opendp* opendp_;
};

CellPlaceOrderLess::CellPlaceOrderLess(const odb::Rect& core,
                                       const Opendp* opendp)
    : center_x_((core.xMin() + core.xMax()) / 2),
      center_y_((core.yMin() + core.yMax()) / 2),
      opendp_(opendp)
{
}

// Unweighted Manhattan distance in database units, and deliberately not
// the metric the site search minimizes.  calcDist() weights X by site
// width and resolves Y through the row table because it has to rank
// candidate sites physically; this measure only has to rank cells against
// each other, so an unweighted offset from the core centre is enough.
//
// What it measures is the cell's lower-left corner and not the cell's own
// centre, so the value differs from a true centre offset by half the cell's
// width and half its height.  The sign of that shift depends on where the
// cell lies rather than on how wide it is: a cell to the right of or above
// the core centre scores nearer than a measurement from its own centre
// would give, while one to the left of or below the centre scores farther.
// The difference is bounded by half the cell's own footprint, and this is
// only the third of the four keys below, so it reorders nothing the
// row-span and area keys have already separated.
int CellPlaceOrderLess::centerDist(const Node* cell) const
{
  return sumXY(abs(cell->getLeft() - center_x_),
               abs(cell->getBottom() - center_y_));
}

// Four keys, ordered by how badly a cell needs an uncontended grid.
// Multi-row cells first: a candidate site has to offer them a run of free
// rows rather than one, and checkRowPowerCompatible() has to find matching
// rail polarity across that whole span, so they generally have fewer sites
// to choose from than a single-row cell does and only an unfragmented grid
// still offers them one near where global placement wanted them.  Then
// larger area first, the same argument
// expressed by size, since a wide cell needs a long run of consecutive
// free sites and such runs only exist early.  Then nearer the core centre
// first, because the interior is the most contended region and claiming
// it early spares later cells a long walk outwards.  Finally the instance
// name, which is unique within the block and so can never tie: that key
// is what makes this comparator a strict total order rather than merely a
// weak one, and a strict total order is what an unstable sort needs to
// produce a reproducible result.
bool CellPlaceOrderLess::operator()(const Node* cell1, const Node* cell2) const
{
  const bool is_multi_row1 = opendp_->isMultiRow(cell1);
  const bool is_multi_row2 = opendp_->isMultiRow(cell2);

  if (is_multi_row1 != is_multi_row2) {
    return is_multi_row1;
  }

  const int64_t area1 = cell1->area();
  const int64_t area2 = cell2->area();
  const int dist1 = centerDist(cell1);
  const int dist2 = centerDist(cell2);
  return area1 > area2
         || (area1 == area2
             && (dist1 < dist2
                 || (dist1 == dist2
                     && strcmp(cell1->getDbInst()->getConstName(),
                               cell2->getDbInst()->getConstName())
                            < 0)));
}

// The general pass, and the only one that escalates on failure.  The
// eligibility filter admits nothing already committed elsewhere: fixed
// cells own their sites, grouped cells were settled by placeGroups(), and
// cells already placed must not be disturbed.  What is left is exactly the
// population whose position is still undecided.  A cell too large for the
// core is a hard error here rather than a counted failure because no
// amount of searching or ripping up can ever seat it, so saying so at once
// is more useful than a total at the end.  Every cell then gets one
// diamond search, and only a search that fails is worth the cost of
// evicting its neighbours.
//
// The sort that fixes that order is std::ranges::sort, which is not a
// stable sort, so equal-comparing elements may be permuted arbitrarily.
// Reproducibility here rests entirely on CellPlaceOrderLess ending in a
// strcmp of the instance name, which is unique and therefore leaves no
// equal-comparing pairs at all.  With no ties to break, the comparator is
// a strict total order and the sorted sequence is a pure function of which
// cells are in the vector, not of the order they were collected in -- so
// the sort result here does not depend on how network_ was built.  The
// name-ordered stable_sort in createNetwork() earns its keep elsewhere: it
// fixes node insertion order and node ids, and with them every traversal
// of getNodes() that is consumed without being re-sorted.
void Opendp::place()
{
  auto report_placement = [this](
                              Node* cell, bool diamond_move, bool rip_up_move) {
    if (debug_observer_) {
      const char* type = isMultiRow(cell) ? "multi-row" : "single-row";
      if (diamond_move) {
        logger_->report("Successful diamondMove(), {} cell {}, #moves: {}",
                        type,
                        cell->name(),
                        move_count_);
      } else {
        logger_->report(
            "Failed diamondMove(), {} cell {}, trying ripUpAndReplace(), "
            "#moves: {}",
            type,
            cell->name(),
            move_count_);
        if (rip_up_move) {
          logger_->report(
              "Successful ripUpAndReplace(), {} cell {}, #moves: {}",
              type,
              cell->name(),
              move_count_);
        } else {
          logger_->report("Unsuccessful placement, {} cell {}, #moves: {}",
                          type,
                          cell->name(),
                          move_count_);
        }
      }
      move_count_++;
      if (jump_moves_ > 0 && (move_count_ % jump_moves_ != 0)) {
        deep_iterative_debug_ = false;
        return;
      }
      deep_iterative_debug_ = true;
      debug_observer_->redrawAndPause();
    }
  };

  vector<Node*> sorted_cells;
  sorted_cells.reserve(network_->getNumCells());
  int failed_diamond_move = 0, failed_rip_up = 0, success_diamond_move = 0;

  for (auto& cell : network_->getNodes()) {
    if (cell->getType() != Node::CELL
        || !cell->getDbInst()->getMaster()->isCore()) {
      continue;
    }
    if (!(cell->isFixed() || cell->inGroup() || cell->isPlaced())) {
      sorted_cells.push_back(cell.get());
      if (!grid_->cellFitsInCore(cell.get())) {
        logger_->error(DPL,
                       15,
                       "instance {} does not fit inside the ROW core area.",
                       cell->name());
      }
    }
  }
  std::ranges::sort(sorted_cells, CellPlaceOrderLess(core_, this));

  int count = 0;
  for (Node* cell : sorted_cells) {
    if (iterative_debug_) {
      count++;
      logger_->report("Placing cell {}, multi-row: {}, count {}, %: {:.2f}",
                      cell->name(),
                      isMultiRow(cell),
                      count,
                      100.0 * count / sorted_cells.size());
    }

    bool diamond_move = diamondMove(cell);
    bool rip_up_move = false;

    if (!diamond_move) {
      // TODO: this is non-deteministic due to std::set<Node*>,
      // and experiments show no legalization for failed diamond searches.
      rip_up_move = ripUpAndReplace(cell);
      if (!rip_up_move) {
        failed_rip_up++;
      }
    }
    diamond_move == 1 ? success_diamond_move++ : failed_diamond_move++;

    if (iterative_debug_) {
      odb::Point initial_location = getOdbLocation(cell);
      odb::Point final_location = getDplLocation(cell);
      float len = odb::Point::squaredDistance(initial_location, final_location);
      if (len > 0) {
        report_placement(cell, diamond_move, rip_up_move);
      }
    }
  }

  const size_t total_cells = sorted_cells.size();
  const int success_rip_up = failed_diamond_move - failed_rip_up;

  logger_->report("Movements Summary");
  logger_->report("---------------------------------------");
  logger_->report("Total cells:                {:8d}", total_cells);
  logger_->report(
      "Diamond Move Success:       {:8d} ({:6.2f}%)",
      success_diamond_move,
      total_cells > 0 ? 100.0 * success_diamond_move / total_cells : 0.0);
  logger_->report("Diamond Move Failure:       {:8d}", failed_diamond_move);
  logger_->report(
      "Rip-up and replace Success: {:8d} ({:6.2f}% of diamond failures)",
      success_rip_up,
      failed_diamond_move > 0 ? 100.0 * success_rip_up / failed_diamond_move
                              : 0.0);
  logger_->report("Rip-up and replace Failure: {:8d}", failed_rip_up);
  logger_->report("Total Placement Failures:   {:8d}",
                  (int) placement_failures_.size());
  logger_->report("---------------------------------------");
}

// Grouped cells get their own pass because a fence region is effectively a
// private core: the search is clipped to the region boundary, so a grouped
// cell competes only with its own group and gains nothing from being
// interleaved with the general population.  The same comparator is reused,
// so the within-group order follows the same reasoning.  The pass is all
// or nothing: one unplaceable cell abandons the whole group to a brick
// placer, because a partially seated group leaves the region in a worse
// state than an empty one does.
void Opendp::placeGroups2()
{
  for (auto& group : arch_->getRegions()) {
    vector<Node*> group_cells;
    group_cells.reserve(network_->getNumCells());
    for (Node* cell : group->getCells()) {
      if (!cell->isFixed() && !cell->isPlaced()) {
        group_cells.push_back(cell);
      }
    }
    std::ranges::sort(group_cells, CellPlaceOrderLess(core_, this));

    bool pass = true;
    for (Node* cell : group_cells) {
      if (!cell->isFixed() && !cell->isPlaced()) {
        assert(cell->inGroup());
        pass = diamondMove(cell);
        if (!pass) {
          break;
        }
      }
    }

    if (!pass) {
      // Erase group cells
      for (Node* cell : group->getCells()) {
        unplaceCell(cell);
      }

      // Determine brick placement by utilization.
      // magic number alert
      if (group->getUtil() > 0.95) {
        brickPlace1(group);
      } else {
        brickPlace2(group);
      }
    }
  }
}

// Place cells in group toward edges.
void Opendp::brickPlace1(const Group* group)
{
  const odb::Rect& boundary = group->getBBox();
  vector<Node*> sorted_cells(group->getCells());

  std::ranges::sort(sorted_cells, [&](Node* cell1, Node* cell2) {
    return rectDist(cell1, boundary) < rectDist(cell2, boundary);
  });

  for (Node* cell : sorted_cells) {
    DbuX x;
    DbuY y;
    rectDist(cell, boundary, &x.v, &y.v);
    const GridPt legal = legalGridPt(cell, {x, y});
    // This looks for a site starting at the nearest corner in rect,
    // which seems broken. It should start looking at the nearest point
    // on the rect boundary. -cherry
    if (!diamondMove(cell, legal)) {
      logger_->error(DPL, 16, "cannot place instance {}.", cell->name());
    }
  }
}

void Opendp::rectDist(const Node* cell,
                      const odb::Rect& rect,
                      // Return values.
                      int* x,
                      int* y) const
{
  const DbuPt init = initialLocation(cell, false);
  const DbuX init_x = init.x;
  const DbuY init_y = init.y;

  if (init_x > (rect.xMin() + rect.xMax()) / 2) {
    *x = rect.xMax();
  } else {
    *x = rect.xMin();
  }

  if (init_y > (rect.yMin() + rect.yMax()) / 2) {
    *y = rect.yMax();
  } else {
    *y = rect.yMin();
  }
}

int Opendp::rectDist(const Node* cell, const odb::Rect& rect) const
{
  int x, y;
  rectDist(cell, rect, &x, &y);
  const DbuPt init = initialLocation(cell, false);
  return sumXY(abs(init.x - x), abs(init.y - y));
}

// Place group cells toward region edges.
void Opendp::brickPlace2(const Group* group)
{
  vector<Node*> sorted_cells(group->getCells());

  std::ranges::sort(sorted_cells, [&](Node* cell1, Node* cell2) {
    return rectDist(cell1, *cell1->getRegion())
           < rectDist(cell2, *cell2->getRegion());
  });

  for (Node* cell : sorted_cells) {
    if (!cell->isHold()) {
      DbuX x;
      DbuY y;
      rectDist(cell, *cell->getRegion(), &x.v, &y.v);
      const GridPt legal = legalGridPt(cell, {x, y});
      // This looks for a site starting at the nearest corner in rect,
      // which seems broken. It should start looking at the nearest point
      // on the rect boundary. -cherry
      if (!diamondMove(cell, legal)) {
        logger_->error(DPL, 17, "cannot place instance {}.", cell->name());
      }
    }
  }
}

// Refinement has a fixed budget, so it spends it where the loss is
// largest: on the cells that ended up furthest from where global placement
// wanted them.  The ranking key is disp(), unweighted Manhattan distance
// in database units from the cell's original position, and neither the
// site search's calcDist() nor CellPlaceOrderLess::centerDist().  Held
// cells are skipped because a pre-placement pass positioned them
// deliberately and refinement must not undo that.
int Opendp::groupRefine(const Group* group)
{
  vector<Node*> sort_by_disp(group->getCells());

  std::ranges::sort(sort_by_disp, [&](Node* cell1, Node* cell2) {
    return (disp(cell1) > disp(cell2));
  });

  int count = 0;
  for (int i = 0; i < sort_by_disp.size() * group_refine_percent_; i++) {
    Node* cell = sort_by_disp[i];
    if (!cell->isHold() && !cell->isFixed()) {
      if (refineMove(cell)) {
        count++;
      }
    }
  }
  return count;
}

// This is NOT annealing. It is random swapping. -cherry
int Opendp::anneal(Group* group)
{
  std::mt19937 rand_gen(rand_seed_);
  int count = 0;

  // magic number alert
  using idx_range = boost::random::uniform_int_distribution<int>;
  const size_t num_cells = group->getCells().size();
  for (int i = 0; i < 100 * num_cells; i++) {
    const auto cell1_idx = idx_range(0, num_cells - 1)(rand_gen);
    const auto cell2_idx = idx_range(0, num_cells - 1)(rand_gen);
    Node* cell1 = group->getCells()[cell1_idx];
    Node* cell2 = group->getCells()[cell2_idx];
    if (swapCells(cell1, cell2)) {
      count++;
    }
  }
  return count;
}

// Not called -cherry.
int Opendp::refine()
{
  vector<Node*> sorted;
  sorted.reserve(network_->getNumCells());

  for (auto& cell : network_->getNodes()) {
    if (cell->getType() != Node::CELL) {
      continue;
    }
    if (!(cell->isFixed() || cell->isHold() || cell->inGroup())) {
      sorted.push_back(cell.get());
    }
  }
  std::ranges::sort(sorted, [&](Node* cell1, Node* cell2) {
    return disp(cell1) > disp(cell2);
  });

  int count = 0;
  for (int i = 0; i < sorted.size() * refine_percent_; i++) {
    Node* cell = sorted[i];
    if (!cell->isHold()) {
      if (refineMove(cell)) {
        count++;
      }
    }
  }
  return count;
}

////////////////////////////////////////////////////////////////

bool Opendp::diamondMove(Node* cell)
{
  const GridPt init = legalGridPt(cell, false);
  return diamondMove(cell, init);
}

// Success is reported by the returned PixelPt carrying a non-null pixel
// pointer rather than by a status flag, because a default-constructed
// PixelPt is exactly what an exhausted search returns and the coordinates
// it also carries would then be indistinguishable from a legitimate hit at
// the grid origin.  A successful search is committed here instead of being
// handed back to the caller, so the pixels this cell now owns are visible
// to every later search in the same pass.
bool Opendp::diamondMove(Node* cell, const GridPt& grid_pt)
{
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "diamond move {} ({}, {}) to ({}, {})",
             cell->name(),
             cell->getLeft(),
             cell->getBottom(),
             grid_pt.x,
             grid_pt.y);
  const PixelPt pixel_pt = diamondSearch(cell, grid_pt.x, grid_pt.y);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "Diamond search {} ({}, {}) to ({}, {})",
             cell->name(),
             cell->getLeft(),
             cell->getBottom(),
             pixel_pt.x,
             pixel_pt.y);
  if (pixel_pt.pixel) {
    placeCell(cell, pixel_pt.x, pixel_pt.y);
    if (debug_observer_) {
      debug_observer_->drawSelected(cell->getDbInst(), false);
    }
    return true;
  }
  return false;
}

void Opendp::deepIterativePause(const std::string& message, bool only_print)
{
  if (deep_iterative_debug_ && debug_observer_) {
    logger_->report(message);
    if (!only_print) {
      debug_observer_->redrawAndPause();
    }
  }
}

// The recovery path, reached only after a diamond search has exhausted its
// entire displacement window.  It evicts a bounded neighbourhood instead
// of re-solving the placement, so the cost of one failure stays roughly
// constant and the work already committed elsewhere survives; the price is
// a bounded amount of local quality.  The window reaches three rows above
// and below the target and, horizontally, four times the target's padded
// width in sites to either side, that factor being derived from the row
// margin so the two extents stay coupled rather than independently tuned.
// Eviction is filtered on fence-region membership as a boolean, not on
// region identity: the test asks only whether target and neighbour are
// both in some region or both in none, so a grouped target may well evict
// a cell belonging to a different region, while a grouped and an ungrouped
// cell never disturb each other.  Nothing is lost by that, because each
// evicted cell is re-placed by its own diamondMove(), whose window is
// clipped to that cell's own region and whose legality predicate confines
// it there as well, so a neighbour pulled out of a different region is
// still put back inside that region.  Note that the collection is a
// std::set of pointers, so eviction and re-placement order follows heap
// addresses and may differ between runs; the note above its call site in
// place() already records this.  A neighbour that cannot be put back fails
// the call just as the target cell does.
bool Opendp::ripUpAndReplace(Node* target_cell)
{
  const GridPt taget_cell_pixel = legalGridPt(target_cell, true);
  // magic number alert
  const GridY boundary_margin{3};
  const GridX margin_width{grid_->gridPaddedWidth(target_cell).v
                           * (1 + boundary_margin.v)};
  std::set<Node*> region_cells;
  for (GridX x = taget_cell_pixel.x - margin_width;
       x <= (taget_cell_pixel.x + margin_width);
       x++) {
    for (GridY y = taget_cell_pixel.y - boundary_margin;
         y <= (taget_cell_pixel.y + boundary_margin);
         y++) {
      Pixel* pixel = grid_->gridPixel(x, y);
      if (pixel) {
        Node* cell_in_pixel = pixel->cell;
        if (cell_in_pixel && !cell_in_pixel->isFixed()) {
          region_cells.insert(cell_in_pixel);
        }
      }
    }
  }

  deepIterativePause("pause after legalGridPt() inside ripUpAndReplace(), cell "
                     + target_cell->name());

  // erase region cells
  for (Node* around_cell : region_cells) {
    if (target_cell->inGroup() == around_cell->inGroup()) {
      unplaceCell(around_cell);
    }
  }

  deepIterativePause("pause after unplacing cells inside ripUpAndReplace()");

  // place target cell
  bool success = true;
  if (!diamondMove(target_cell)) {
    deepIterativePause(
        "failed diamondMove() inside ripUpAndReplace() for target cell "
            + target_cell->name(),
        /*only_print=*/true);
    placement_failures_.push_back(target_cell);
    success = false;
  }

  deepIterativePause(
      "pause after placing target cell inside ripUpAndReplace()");

  // re-place erased cells
  for (Node* around_cell : region_cells) {
    deepIterativePause(
        "pause before diamondMove() inside ripUpAndReplace() for surrounding "
        "cell "
        + around_cell->name());

    if (target_cell->inGroup() == around_cell->inGroup()
        && !diamondMove(around_cell)) {
      deepIterativePause(
          "failed diamondMove() inside ripUpAndReplace() for surrounding cell "
              + around_cell->name(),
          /*only_print=*/true);
      placement_failures_.push_back(around_cell);
      success = false;
    }
  }

  deepIterativePause(
      "pause after placing surrounding cells inside ripUpAndReplace()");

  return success;
}

// Swapping is the one move that needs no free space, which is why the
// random-swap pass over a fence region's cells is built on it.  The
// identical width and height requirement is what keeps it cheap: each cell
// inherits the other's occupied footprint exactly, so the set of pixels
// recorded as holding a cell is the same after the exchange as before, and
// neither cell can come to overlap anything a search would otherwise have
// had to find room around.
//
// That accounts for occupancy only.  Padding lives in its own pixel field
// and paintPixel() finishes by calling paintCellPadding(), which reserves
// from a per-cell left and right pad, so two masters of equal width and
// height can still reserve different extents and each move can mark pixels
// the counterpart had never reserved at that site.  Padding conflicts are
// therefore left to the design-rule re-check below rather than excluded by
// the dimension test.
//
// Preserving the footprints is not the same as preserving legality, and the
// re-check here is narrower than the one a search runs.  What is rerun is
// the design-rule engine for both cells, which covers four things: edge
// spacing, padding, blocked layers and the one-site gap.  The
// equal-dimension requirement bounds which pixels the exchange marks as
// occupied; it says nothing about the properties that depend on the master
// or on the destination row, and two masters of equal width and height can
// still declare different symmetry and still present different rail
// polarity.
// None of site availability, fence-region containment, master symmetry
// against the site orientation, or power-rail parity across the span of a
// multi-row cell is rerun here, so on those counts each cell carries over
// the verdict its previous site earned instead of one re-established at its
// new one.  Held and fixed cells are excluded because their positions were
// decided deliberately.  The exchange is recorded in a journal local to
// this call and undone when the design-rule check fails, because by that
// point both cells have already been moved.
bool Opendp::swapCells(Node* cell1, Node* cell2)
{
  if (cell1 != cell2 && !cell1->isHold() && !cell2->isHold()
      && cell1->getWidth() == cell2->getWidth()
      && cell1->getHeight() == cell2->getHeight() && !cell1->isFixed()
      && !cell2->isFixed()) {
    const int dist_change
        = distChange(cell1, cell2->getLeft(), cell2->getBottom())
          + distChange(cell2, cell1->getLeft(), cell1->getBottom());

    if (dist_change < 0) {
      Journal journal(grid_.get(), nullptr);
      MoveCellAction action1(cell1,
                             cell1->getLeft(),
                             cell1->getBottom(),
                             cell2->getLeft(),
                             cell2->getBottom(),
                             cell1->isPlaced());
      journal.addAction(action1);

      MoveCellAction action2(cell2,
                             cell2->getLeft(),
                             cell2->getBottom(),
                             cell1->getLeft(),
                             cell1->getBottom(),
                             cell2->isPlaced());
      journal.addAction(action2);

      const GridX grid_x1 = grid_->gridX(cell2);
      const GridY grid_y1 = grid_->gridSnapDownY(cell2);
      const GridX grid_x2 = grid_->gridX(cell1);
      const GridY grid_y2 = grid_->gridSnapDownY(cell1);

      unplaceCell(cell1);
      unplaceCell(cell2);
      placeCell(cell1, grid_x1, grid_y1);
      placeCell(cell2, grid_x2, grid_y2);
      // Check if placement is valid
      if (drc_engine_->checkDRC(cell1) && drc_engine_->checkDRC(cell2)) {
        return true;
      }
      journal.undo();
    }
  }
  return false;
}

// A cell that is already legal is re-searched here, so unlike the
// placement passes this one can refuse a legal result.  The search is free
// to return any site inside the clipped window, but refinement exists to
// reduce displacement, so a site further from the cell's original position
// than its current one, or outside the displacement limits measured from
// its own start point, is discarded and the cell stays where it is.
bool Opendp::refineMove(Node* cell)
{
  const GridPt grid_pt = legalGridPt(cell, false);
  const PixelPt pixel_pt = diamondSearch(cell, grid_pt.x, grid_pt.y);

  if (pixel_pt.pixel) {
    if (abs(grid_pt.x - pixel_pt.x) > max_displacement_x_
        || abs(grid_pt.y - pixel_pt.y) > max_displacement_y_) {
      return false;
    }

    const int dist_change
        = distChange(cell,
                     gridToDbu(pixel_pt.x, grid_->getSiteWidth()),
                     grid_->gridYToDbu(pixel_pt.y));

    if (dist_change < 0) {
      unplaceCell(cell);
      placeCell(cell, pixel_pt.x, pixel_pt.y);
      return true;
    }
  }
  return false;
}

// Returns the signed change in displacement, so a negative result means
// the candidate is an improvement and callers can test a move with one
// comparison against zero.  The quantity is unweighted Manhattan distance
// in database units from the cell's original position, the same measure
// disp() reports, and not the site search's calcDist().  Refinement
// therefore optimizes a different objective from the search that produced
// the candidate in the first place.
int Opendp::distChange(const Node* cell, const DbuX x, const DbuY y) const
{
  const DbuPt init = initialLocation(cell, false);
  const int cell_dist
      = sumXY(abs(cell->getLeft() - init.x), abs(cell->getBottom() - init.y));
  const int pt_dist = sumXY(abs(init.x - x), abs(init.y - y));
  return pt_dist - cell_dist;
}

////////////////////////////////////////////////////////////////

PixelPt Opendp::diamondSearch(const Node* cell,
                              const GridX x,
                              const GridY y) const
{
  // Diamond search limits.
  GridX x_min = x - max_displacement_x_;
  GridX x_max = x + max_displacement_x_;
  GridY y_min = y - max_displacement_y_;
  GridY y_max = y + max_displacement_y_;

  // Restrict search to group boundary.
  Group* group = cell->getGroup();
  if (group) {
    // Boundary to grid staying inside.
    const GridRect grid_boundary = grid_->gridWithin(group->getBBox());
    const GridPt min = grid_boundary.closestPtInside({x_min, y_min});
    const GridPt max = grid_boundary.closestPtInside({x_max, y_max});
    x_min = min.x;
    y_min = min.y;
    x_max = max.x;
    y_max = max.y;
  }

  // Clip limits to grid bounds.
  x_min = max(GridX{0}, x_min);
  y_min = max(GridY{0}, y_min);
  x_max = min(grid_->getRowSiteCount(), x_max);
  y_max = min(grid_->getRowCount(), y_max);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "Diamond search {} ({}, {}) bounds ({}-{}, {}-{})",
             cell->name(),
             x,
             y,
             x_min,
             x_max - 1,
             y_min,
             y_max - 1);

  // The search frontier.  Ordering on a true distance rather than a hop
  // count is what makes this a best-first, uniform-cost traversal and not a
  // breadth-first one: a hop count would price one row of vertical travel
  // the same as one site of horizontal travel, which is physically wrong
  // because the two axes differ by a large factor in database units.  The
  // key is the distance from the search origin, not an accumulated path
  // cost, so the first legal site popped is the nearest legal site under
  // calcDist(), the metric this module actually cares about.
  // The sequence number is the tie-break.  Without it, entries of equal
  // distance would pop in whatever order the heap happened to hold them,
  // which is unspecified and varies between standard library
  // implementations; ordering ties by insertion makes them first in, first
  // out, which is reproducible across compilers and platforms.
  // Marking a point visited when it is pushed rather than when it is popped
  // is safe for the same reason the key works: with a consistent metric and
  // non-negative steps, the distance first computed for a point is already
  // its minimum, so no later path can improve it, and marking early keeps
  // the heap small.
  struct PQ_entry
  {
    int manhattan_distance;
    GridPt p;
    int sequence;
    bool operator>(const PQ_entry& other) const
    {
      return std::tie(manhattan_distance, sequence)
             > std::tie(other.manhattan_distance, other.sequence);
    }
  };
  std::priority_queue<PQ_entry, std::vector<PQ_entry>, std::greater<PQ_entry>>
      positionsHeap;
  std::unordered_set<GridPt> visited;
  int sequence = 0;
  GridPt center{x, y};
  positionsHeap.push(
      {.manhattan_distance = 0, .p = center, .sequence = sequence++});
  visited.insert(center);

  const vector<GridPt> neighbors = {{GridX(-1), GridY(0)},
                                    {GridX(1), GridY(0)},
                                    {GridX(0), GridY(-1)},
                                    {GridX(0), GridY(1)}};
  while (!positionsHeap.empty()) {
    const GridPt nearest = positionsHeap.top().p;
    positionsHeap.pop();

    if (canBePlaced(cell, nearest.x, nearest.y)) {
      return PixelPt(
          grid_->gridPixel(nearest.x, nearest.y), nearest.x, nearest.y);
    }

    // Put neighbors in the queue
    for (GridPt offset : neighbors) {
      GridPt neighbor = {nearest.x + offset.x, nearest.y + offset.y};
      // Check if it was already put in the queue
      if (visited.contains(neighbor)) {
        continue;
      }
      // Check limits
      if (neighbor.x < x_min || neighbor.x > x_max || neighbor.y < y_min
          || neighbor.y > y_max) {
        continue;
      }

      visited.insert(neighbor);
      positionsHeap.push({.manhattan_distance = calcDist(center, neighbor),
                          .p = neighbor,
                          .sequence = sequence++});
    }
  }
  return PixelPt();
}

// The metric the site search minimizes, and the reason the search is only
// diamond-shaped in database units.  Both axes are converted to database
// units before they are summed: X is a count of sites scaled by the site
// width, Y is a row index resolved through the grid's row table.  The
// typed coordinate wrappers in Coordinates.h enforce that, since sumXY()
// is the only way to combine an X and a Y component and it accepts only
// database-unit types; summing raw grid indices would silently add two
// incommensurable quantities.
// The vertical term has to consult the row table rather than multiply by a
// constant because rows are not required to share a height.  gridYToDbu()
// resolves a row index through row_index_to_y_dbu_ on every design, hybrid
// or not, so the conversion never depends on a design-wide pitch existing;
// the grid's uniform_row_height_ optional is a separate quantity that
// feeds height classification in gridHeight() and isMultiHeight(), not
// this conversion.
// The consequence is an asymmetry worth knowing about: one row of vertical
// travel costs as much as row_height / site_width sites of horizontal
// travel, a large ratio for an ordinary single-height row.  The search
// therefore exhausts a wide horizontal band before it will move a cell to
// another row, and the equal-cost contour, a true diamond in database
// units, is a strongly flattened one in grid indices.
int Opendp::calcDist(GridPt p0, GridPt p1) const
{
  DbuY y_dist = abs(grid_->gridYToDbu(p0.y) - grid_->gridYToDbu(p1.y));
  DbuX x_dist = gridToDbu(abs(p0.x - p1.x), grid_->getSiteWidth());
  return sumXY(x_dist, y_dist);
}

// The row guard runs before the extent arithmetic because that arithmetic
// converts a row index to database units through the grid's row table, and
// a bottom row at or past the end of that table names no row a cell could
// occupy.  The top edge is then derived by converting the cell's height
// back to a row index rather than by adding a row count, because rows may
// have differing heights and a count of rows would not locate the correct
// top row in a hybrid design.  Everything that decides legality is
// delegated to checkPixels(); this function only settles the rectangle to
// be tested and lets the debug observer draw it.
bool Opendp::canBePlaced(const Node* cell, GridX bin_x, GridY bin_y) const
{
  debugPrint(logger_,
             DPL,
             "place",
             3,
             " canBePlaced {} ({:4},{:4})",
             cell->name(),
             bin_x,
             bin_y);

  if (bin_y >= grid_->getRowCount()) {
    return false;
  }

  const GridX x_end = bin_x + grid_->gridWidth(cell);
  const GridY y_end
      = grid_->gridEndY(grid_->gridYToDbu(bin_y) + cell->getHeight());

  if (debug_observer_) {
    debug_observer_->binSearch(cell, bin_x, bin_y, x_end, y_end);
  }
  return checkPixels(cell, bin_x, bin_y, x_end, y_end);
}

// Fence-region containment, with three distinct verdicts.  If the cell
// belongs to a region and the R-tree returns exactly one overlapping
// region rectangle, the placement is legal only when the cell's own box is
// covered by that rectangle: containment runs cell inside region, not the
// other way round.  If the cell belongs to a region and the overlap count
// is zero or greater than one it is illegal: the implementation admits only
// a single-rectangle answer, so anything else is treated as unresolved
// rather than examined further.  If the cell belongs
// to no region it is legal only when it overlaps no region rectangle at
// all, since an unconstrained cell may not intrude on a fence region.
// That last case is the one the final return below reaches; the comment
// beside it describes neither that branch nor the direction of the test
// the region branch performs.
bool Opendp::checkRegionOverlap(const Node* cell,
                                const GridX x,
                                const GridY y,
                                const GridX x_end,
                                const GridY y_end) const
{
  // TODO: Investigate the caching of this function
  // it is called with the same cell and x,y,x_end,y_end multiple times
  debugPrint(logger_,
             DPL,
             "region",
             1,
             "Checking region overlap for cell {} at x[{} {}] and y[{} {}]",
             cell->name(),
             x,
             x_end,
             y,
             y_end);
  const DbuX site_width = grid_->getSiteWidth();
  const bgBox queryBox(
      {gridToDbu(x, site_width).v, grid_->gridYToDbu(y).v},
      {gridToDbu(x_end, site_width).v - 1, grid_->gridYToDbu(y_end).v - 1});

  std::vector<bgBox> result;
  findOverlapInRtree(queryBox, result);

  if (cell->getRegion()) {
    if (result.size() == 1) {
      // the queryBox must be fully contained in the region or else there
      // might be a part of the cell outside of any region
      return boost::geometry::covered_by(queryBox, result[0]);
    }
    // if we are here, then the overlap size is either 0 or > 1
    // both are invalid. The overlap size should be 1
    return false;
  }
  // If the cell has a region, then the region's bounding box must
  // be fully contained by the cell's bounding box.
  return result.empty();
}

// Check all pixels are empty.
// The line above understates this.  It is the module's full legality
// predicate, seven stages deep, and every stage can reject.  One, the cell
// must not run past the right-hand end of the row.  Two, fence-region
// containment.  Three, a per-pixel scan whose six independent rejections
// are a missing pixel, an occupied one, an invalid one, a region-owned one
// the cell does not belong to, any region-owned one at all when the cell
// belongs to none, and a bottom row that offers no site orientation.
// Four, the optional one-site-gap probe.  Five, master symmetry.  Six,
// power-rail parity for multi-row cells.  Seven, the design rules.
// Orientation is a legality question here, not a cosmetic one: the supply
// rails run along a cell's top and bottom edges, so which rails a cell
// meets follows from the orientation of the row it lands in, and a cell
// landed against the wrong rails is an electrical error rather than an
// untidy one.  That orientation is a property of the individual row: the
// grid records whatever orientation each database row declared, alongside
// the span of sites it declared it for, and this stage reads it back per
// pixel, so neighbouring rows may agree or differ and nothing in this
// predicate treats them as alternating.  That is why the orientation the
// row actually offers is queried inside the placement predicate instead of
// being fixed up afterwards.
bool Opendp::checkPixels(const Node* cell,
                         const GridX x,
                         const GridY y,
                         const GridX x_end,
                         const GridY y_end) const
{
  if (x_end > grid_->getRowSiteCount()) {
    return false;
  }
  if (!checkRegionOverlap(cell, x, y, x_end, y_end)) {
    return false;
  }

  odb::dbSite* site = cell->getSite();
  for (GridY y1 = y; y1 < y_end; y1++) {
    const bool first_row = (y1 == y);
    for (GridX x1 = x; x1 < x_end; x1++) {
      const Pixel* pixel = grid_->gridPixel(x1, y1);
      if (pixel == nullptr || pixel->cell || !pixel->is_valid
          || (cell->inGroup() && pixel->group != cell->getGroup())
          || (!cell->inGroup() && pixel->group)
          || (first_row && !grid_->getSiteOrientation(x1, y1, site))) {
        return false;
      }
    }
  }

  if (disallow_one_site_gaps_) {
    // here we need to check for abutting first, if there is an abutting
    // cell then we continue as there is nothing wrong with it if there is
    // no abutting cell, we will then check cells at 1+ distances we only
    // need to check on the left and right sides
    const GridX x_begin = max(GridX{0}, x - 1);
    const GridY y_begin = max(GridY{0}, y);
    // inclusive search, so we don't add 1 to the end
    const GridX x_finish = min(x_end, grid_->getRowSiteCount() - 1);
    const GridY y_finish = min(y_end, grid_->getRowCount() - 1);

    auto isAbutted = [this](const GridX x, const GridY y) {
      const Pixel* pixel = grid_->gridPixel(x, y);
      return (pixel == nullptr || pixel->cell);
    };

    auto cellAtSite = [this](const GridX x, const GridY y) {
      const Pixel* pixel = grid_->gridPixel(x, y);
      return (pixel != nullptr && pixel->cell);
    };
    for (GridY y = y_begin; y < y_finish; ++y) {
      // left side
      if (!isAbutted(x_begin, y) && cellAtSite(x_begin - 1, y)) {
        debugPrint(logger_,
                   DPL,
                   "one_site_gap",
                   1,
                   "One site gap left of {}  at ({}, {})",
                   cell->name(),
                   x,
                   y);
        return false;
      }
      // right side
      if (!isAbutted(x_finish, y) && cellAtSite(x_finish + 1, y)) {
        debugPrint(logger_,
                   DPL,
                   "one_site_gap",
                   1,
                   "One site gap right of {} at ({}, {})",
                   cell->name(),
                   x,
                   y);
        return false;
      }
    }
  }

  const auto orient = grid_->getSiteOrientation(x, y, site).value();

  // Check for symmetry
  auto* dbMaster = cell->getDbInst()->getMaster();
  unsigned masterSym = dpl::DetailedOrient::getMasterSymmetry(dbMaster);
  if (!checkMasterSym(masterSym, orient)) {
    return false;
  }

  // For multi-row cells, the bottom-row site/orient check above only covers
  // the bottom row; it doesn't ensure the master's power pin stack lines up
  // with the PDN rail stack across the span.  Reject wrong-parity landings.
  if (cell->getMaster()->isMultiRow() && !checkRowPowerCompatible(cell, y)) {
    return false;
  }

  return drc_engine_->checkDRC(cell, x, y, orient);
}

// The per-pixel scan can only query the orientation of the bottom row, so
// a multi-row cell still needs its whole span checked against the
// architecture's row table, which records the rail polarity each row
// actually presents.
//
// This runs on a design whose rows were imported successfully: a block with
// no usable row at all is rejected earlier, by DPL 12 in
// Grid::examineRows(), and find_closest_row() reads the first row
// unconditionally, so it presupposes a populated table rather than guarding
// against an empty one.  For a populated table it clamps instead of
// failing, handing back an index inside the table for any coordinate, so a
// candidate bottom edge above the topmost row comes back as the topmost row
// and not as an overrun.  The bounds test on that index is therefore
// defensive rather than an ordinary case - unreachable once the table is
// populated - and it answers incompatible rather than raising, so a
// candidate that somehow reached it would be rejected instead of the pass
// being aborted.
bool Opendp::checkRowPowerCompatible(const Node* cell, const GridY y) const
{
  const int row_idx = arch_->find_closest_row(grid_->gridYToDbu(y));
  if (row_idx >= arch_->getNumRows()) {
    return false;
  }
  bool flip = false;
  return arch_->powerCompatible(cell, arch_->getRow(row_idx), flip);
}

// A site's orientation says how a cell must be flipped to sit in that row,
// but only the master's declared symmetry says whether that flip is
// meaningful for this cell: reflecting a master that is not symmetric
// about the axis in question yields a different physical layout, not the
// same one turned around.  Each orientation therefore maps to the symmetry
// bits it presupposes, and anything the master does not declare is
// refused.  R0 presupposes nothing, which is why it always passes.
bool Opendp::checkMasterSym(unsigned masterSym, unsigned cellOri) const
{
  using odb::dbOrientType;
  switch (cellOri) {
    case dbOrientType::R0:
      return true;
    case dbOrientType::MX:
      return (masterSym & Symmetry_X) != 0;
    case dbOrientType::MY:
      return (masterSym & Symmetry_Y) != 0;
    case dbOrientType::R180:
      return (masterSym & Symmetry_X) && (masterSym & Symmetry_Y);
    case dbOrientType::R90:
    case dbOrientType::R270:
      return (masterSym & Symmetry_ROT90) != 0;
    case dbOrientType::MXR90:
    case dbOrientType::MYR90:
      return (masterSym & Symmetry_ROT90) && (masterSym & Symmetry_X)
             && (masterSym & Symmetry_Y);
    default:
      return false;
  }
}

////////////////////////////////////////////////////////////////

// Legalize cell origin
//  inside the core
//  row site
DbuPt Opendp::legalPt(const Node* cell, const DbuPt& pt) const
{
  // Move inside core.
  const DbuX site_width = grid_->getSiteWidth();
  const DbuX core_x = std::clamp(
      pt.x,
      DbuX{0},
      gridToDbu(grid_->getRowSiteCount(), site_width) - cell->getWidth());
  // Align with row site.
  const GridX grid_x{divRound(core_x.v, site_width.v)};
  const DbuX legal_x{gridToDbu(grid_x, site_width)};
  // Align to row
  const DbuY core_y
      = std::clamp(pt.y, DbuY{0}, DbuY{core_.yMax()} - cell->getHeight());
  const GridY grid_y = grid_->gridRoundY(core_y);
  DbuY legal_y = grid_->gridYToDbu(grid_y);

  return {legal_x, legal_y};
}

// The search works in grid indices, so a caller holding a database-unit
// point has to convert, and the conversion is not a plain division: the
// point is first pulled inside the core and onto a row by legalPt(),
// because an index derived from an out-of-core or inter-row coordinate
// would name a pixel that does not exist.  Y snaps down rather than
// rounding, so the index always names the row the cell's bottom edge would
// actually occupy.
//
// Two overloads share that reasoning.  This one starts from a point the
// caller chose.  The other, further below, is the one the placement passes
// use: it legalizes the cell's own pre-legalization position, so a search
// starts as close to where global placement wanted the cell as the core,
// the rows, the macros and the hopeless map allow, and its padded flag
// selects whether that origin is the cell's own left edge or its padded
// one -- which matters because the rip-up window is sized in padded
// widths.
GridPt Opendp::legalGridPt(const Node* cell, const DbuPt& pt) const
{
  const DbuPt legal = legalPt(cell, pt);
  return GridPt(grid_->gridX(legal.x), grid_->gridSnapDownY(legal.y));
}

// A cell whose start point falls inside a macro is relocated to the
// nearest edge of that macro rather than rejected, because the start point
// only seeds a search and the displacement limits are measured from it: a
// point buried in a blockage would spend its whole search window walking
// out of the macro and would often find nothing at all.  All four edges
// are measured and the closest wins, ties resolving in the order tested.
// The result is passed back through legalPt(), since an edge of a macro is
// not necessarily on a site boundary or even inside the core.
DbuPt Opendp::nearestBlockEdge(const Node* cell,
                               const DbuPt& legal_pt,
                               const odb::Rect& block_bbox) const
{
  const DbuX legal_x = legal_pt.x;
  const DbuY legal_y = legal_pt.y;

  const DbuX x_min_dist = abs(legal_x - block_bbox.xMin());
  const DbuX x_max_dist
      = abs(DbuX{block_bbox.xMax()} - (legal_x + cell->getWidth()));
  const DbuY y_min_dist = abs(legal_y - block_bbox.yMin());
  const DbuY y_max_dist
      = abs(DbuY{block_bbox.yMax()} - (legal_y + cell->getHeight()));

  const int min_dist
      = std::min({x_min_dist.v, x_max_dist.v, y_min_dist.v, y_max_dist.v});

  if (min_dist == x_min_dist) {  // left of block
    return legalPt(cell,
                   {DbuX{block_bbox.xMin()} - cell->getWidth(), legal_pt.y});
  }
  if (min_dist == x_max_dist) {  // right of block
    return legalPt(cell, {DbuX{block_bbox.xMax()}, legal_pt.y});
  }
  if (min_dist == y_min_dist) {  // below block
    return legalPt(cell,
                   {legal_pt.x, DbuY{block_bbox.yMin() - cell->getHeight().v}});
  }
  // above block
  return legalPt(cell, {legal_pt.x, DbuY{block_bbox.yMax()}});
}

// Find the nearest valid site left/right/above/below, if any.
// The site doesn't need to be empty but mearly valid.  That should
// be a reasonable place to start the search.  Returns true if any
// site can be found.
bool Opendp::moveHopeless(const Node* cell, GridX& grid_x, GridY& grid_y) const
{
  GridX best_x = grid_x;
  GridY best_y = grid_y;
  int best_dist = std::numeric_limits<int>::max();
  const GridX site_count = grid_->getRowSiteCount();
  const GridY row_count = grid_->getRowCount();
  const DbuX site_width = grid_->getSiteWidth();

  for (GridX x = grid_x - 1; x >= 0; --x) {  // left
    const Pixel& p = grid_->pixel(grid_y, x);
    if (p.is_valid && !p.is_hopeless) {
      best_dist = gridToDbu(grid_x - x - 1, site_width).v;
      best_x = x;
      best_y = grid_y;
      break;
    }
  }
  for (GridX x = grid_x + 1; x < site_count; ++x) {  // right
    const Pixel& p = grid_->pixel(grid_y, x);
    if (p.is_valid && !p.is_hopeless) {
      const int dist = gridToDbu(x - grid_x, site_width).v - cell->getWidth().v;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = x;
        best_y = grid_y;
      }
      break;
    }
  }
  for (GridY y = grid_y - 1; y >= 0; --y) {  // below
    const Pixel& p = grid_->pixel(y, grid_x);
    if (p.is_valid && !p.is_hopeless) {
      const int dist = (grid_->gridYToDbu(grid_y) - grid_->gridYToDbu(y)).v;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = grid_x;
        best_y = y;
      }
      break;
    }
  }
  for (GridY y = grid_y + 1; y < row_count; ++y) {  // above
    const Pixel& p = grid_->pixel(y, grid_x);
    if (p.is_valid && !p.is_hopeless) {
      const int dist = (grid_->gridYToDbu(y) - grid_->gridYToDbu(grid_y)).v;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = grid_x;
        best_y = y;
      }
      break;
    }
  }
  if (best_dist != std::numeric_limits<int>::max()) {
    grid_x = best_x;
    grid_y = best_y;
    return true;
  }
  return false;
}

void Opendp::initMacrosAndGrid()
{
  importDb();
  adjustNodesOrient();
  initGrid();
  setFixedGridCells();
}

void Opendp::convertDbToCell(odb::dbInst* db_inst, Node& cell)
{
  cell.setType(Node::CELL);
  cell.setDbInst(db_inst);
  odb::Rect bbox = getBbox(db_inst);
  cell.setWidth(DbuX{bbox.dx()});
  cell.setHeight(DbuY{bbox.dy()});
  cell.setLeft(DbuX{bbox.xMin()});
  cell.setBottom(DbuY{bbox.yMin()});
  cell.setOrient(db_inst->getOrient());
}

DbuPt Opendp::pointOffMacro(const Node& cell)
{
  // Get cell position
  const DbuPt init = initialLocation(&cell, false);
  const odb::Rect bbox(init.x.v,
                       init.y.v,
                       init.x.v + cell.getWidth().v,
                       init.y.v + cell.getHeight().v);

  const GridRect grid_box = grid_->gridCovering(bbox);

  Pixel* pixel1 = grid_->gridPixel(grid_box.xlo, grid_box.ylo);
  Pixel* pixel2 = grid_->gridPixel(grid_box.xhi, grid_box.ylo);
  Pixel* pixel3 = grid_->gridPixel(grid_box.xlo, grid_box.yhi);
  Pixel* pixel4 = grid_->gridPixel(grid_box.xhi, grid_box.yhi);

  Node* block = nullptr;
  if (pixel1 && pixel1->cell && pixel1->cell->isBlock()) {
    block = pixel1->cell;
  } else if (pixel2 && pixel2->cell && pixel2->cell->isBlock()) {
    block = pixel2->cell;
  } else if (pixel3 && pixel3->cell && pixel3->cell->isBlock()) {
    block = pixel3->cell;
  } else if (pixel4 && pixel4->cell && pixel4->cell->isBlock()) {
    block = pixel4->cell;
  }

  if (block && block->isBlock()) {
    // Get new legal position
    const odb::Rect block_bbox(block->getLeft().v,
                               block->getBottom().v,
                               block->getLeft().v + block->getWidth().v,
                               block->getBottom().v + block->getHeight().v);
    return nearestBlockEdge(&cell, init, block_bbox);
  }
  return init;
}

void Opendp::legalCellPos(odb::dbInst* db_inst)
{
  Node cell;
  convertDbToCell(db_inst, cell);
  // returns the initial position of the cell
  const DbuPt init_pos = initialLocation(&cell, false);
  // returns the modified position if the cell is in a macro
  const DbuPt legal_pt = pointOffMacro(cell);
  // return the modified position if the cell is outside the die
  const DbuPt new_pos = legalPt(&cell, legal_pt);

  if (init_pos == new_pos) {
    return;
  }

  // transform to grid Pos for align
  const GridPt legal_grid_pt{grid_->gridX(DbuX{new_pos.x}),
                             grid_->gridSnapDownY(DbuY{new_pos.y})};
  // Transform position on real position
  setGridLoc(&cell, legal_grid_pt.x, legal_grid_pt.y);
  // Set position of cell on db
  db_inst->setLocation(core_.xMin() + cell.getLeft().v,
                       core_.yMin() + cell.getBottom().v);
}

// The current location of the cell's database instance, expressed relative
// to the core origin and, when asked for, shifted for padding.  Reading the
// database rather than the node is deliberate: a node's own coordinates are
// overwritten the moment a cell is committed, so after the first move they
// would no longer say where the cell started out.
//
// How far back that start position reaches depends on the engine, and only
// the diamond path makes it a pre-legalization reading.  There the sole
// writer of instance locations is updateDbInstLocations(), which runs after
// the whole pass and after the displacement statistics, so every call here
// still sees the coordinates the database held when the pass began - for a
// run that follows global placement, the position global placement left
// behind.  The negotiation path has an earlier writer:
// NegotiationLegalizer::flushToDb() sets locations from inside legalize(),
// so a call made after that point reads whatever that pass last wrote.
//
// It reaches a search only through legalPt(cell, padded) and the
// legalGridPt() overload that wraps it, which is the origin used by the
// one-argument diamondMove() - the default case, and the only one on the
// general pass - by refineMove(), and by the rip-up window.  legalGridPt()
// nudges it into the die, off any macro it landed on and out of a flagged
// origin before the search begins.
//
// It is not the origin of every search, though.  prePlace(),
// prePlaceGroups(), brickPlace1() and brickPlace2() all hand diamondMove()
// an origin they picked themselves -- the nearest point of a fence-region
// rectangle, or the nearest corner of the region a grouped cell belongs to
// -- and the negotiation pass enters diamondSearch() at negotiation
// coordinates.  Those points are computed from this position, so it remains
// upstream of them, but the search they start does not begin here.
//
// It is also the reference distChange() and the displacement statistics
// measure against.  The padded form shifts left by the cell's left padding,
// so that callers reasoning about a padded footprint and callers reasoning
// about the cell itself both get an origin in the frame they expect.
DbuPt Opendp::initialLocation(const Node* cell, const bool padded) const
{
  DbuPt loc;
  cell->getDbInst()->getLocation(loc.x.v, loc.y.v);
  loc.x -= core_.xMin();
  if (padded) {
    loc.x -= gridToDbu(padding_->padLeft(cell), grid_->getSiteWidth());
  }
  loc.y -= core_.yMin();
  return loc;
}

// Legalize pt origin for cell
//  inside the core
//  row site
//  not on top of a macro
//  not in a hopeless site
DbuPt Opendp::legalPt(const Node* cell, const bool padded) const
{
  if (cell->isFixed()) {
    logger_->critical(
        DPL, 26, "legalPt called on fixed cell {}.", cell->name());
  }

  const DbuPt init = initialLocation(cell, padded);
  DbuPt legal_pt = legalPt(cell, init);
  GridX grid_x = grid_->gridX(legal_pt.x);
  GridY grid_y = grid_->gridSnapDownY(legal_pt.y);

  Pixel* pixel = grid_->gridPixel(grid_x, grid_y);
  if (pixel) {
    // Move std cells off of macros.  First try the is_hopeless strategy
    if (pixel->is_hopeless && moveHopeless(cell, grid_x, grid_y)) {
      legal_pt = DbuPt(gridToDbu(grid_x, grid_->getSiteWidth()),
                       grid_->gridYToDbu(grid_y));
      pixel = grid_->gridPixel(grid_x, grid_y);
    }

    const Node* block = pixel->cell;

    // If that didn't do the job fall back on the old move to nearest
    // edge strategy.  This doesn't consider site availability at the
    // end used so it is secondary.
    if (block && block->isBlock()) {
      const odb::Rect block_bbox(block->getLeft().v,
                                 block->getBottom().v,
                                 block->getLeft().v + block->getWidth().v,
                                 block->getBottom().v + block->getHeight().v);
      if ((legal_pt.x + cell->getWidth()) >= block_bbox.xMin()
          && legal_pt.x <= block_bbox.xMax()
          && (legal_pt.y + cell->getHeight()) >= block_bbox.yMin()
          && legal_pt.y <= block_bbox.yMax()) {
        legal_pt = nearestBlockEdge(cell, legal_pt, block_bbox);
      }
    }
  }

  return legal_pt;
}

GridPt Opendp::legalGridPt(const Node* cell, const bool padded) const
{
  const DbuPt pt = legalPt(cell, padded);
  return GridPt(grid_->gridX(pt.x), grid_->gridSnapDownY(pt.y));
}

// The one place in this file where a grid index becomes a cell coordinate,
// factored out so that every path which moves a cell converts the same
// way.  X is a plain scale by site width; Y has to go through the grid's
// row table, because rows may have differing heights and in a hybrid
// design there is no constant to multiply by.
void Opendp::setGridLoc(Node* cell, const GridX x, const GridY y)
{
  cell->setLeft(gridToDbu(x, grid_->getSiteWidth()));
  cell->setBottom(grid_->gridYToDbu(y));
}
// Commit.  The pixel painting, the cell coordinates, the placed flag and
// the orientation have to move together: the search reads pixels to decide
// legality and reads coordinates to measure distance, so a cell that owned
// pixels while reporting stale coordinates would make later searches wrong
// rather than merely worse.  The orientation is taken from the site the
// cell actually landed on instead of being carried over, which is what
// makes the parity checks upstream mean anything.  The journal entry is
// written only when a caller has installed one through setJournal(), and it
// records where the cell came from so that caller can reverse the move.
// Nothing in this module installs a journal on the placer, so on the
// legalization path the entry is not written at all; a caller wanting the
// guarantee supplies its own, as swapCells() above does around its trial
// exchange, rolling back through a journal it creates for itself.  In
// particular ripUpAndReplace() does not rewind - it evicts, then calls
// diamondMove() on each evicted cell again.
void Opendp::placeCell(Node* cell, const GridX x, const GridY y)
{
  const DbuX original_x = cell->getLeft();
  const DbuY original_y = cell->getBottom();
  const bool was_placed = cell->isPlaced();
  setGridLoc(cell, x, y);
  grid_->paintPixel(cell);
  cell->setPlaced(true);
  odb::dbSite* site = cell->getDbInst()->getMaster()->getSite();
  cell->setOrient(grid_->getSiteOrientation(x, y, site).value());
  if (journal_) {
    MoveCellAction action(cell,
                          original_x,
                          original_y,
                          cell->getLeft(),
                          cell->getBottom(),
                          was_placed);
    journal_->addAction(action);
  }
}

// Undo, and deliberately tolerant of cells that were never placed: the
// group fallback in placeGroups2() clears a whole group without tracking
// which of its cells had been seated, so returning quietly for the rest
// keeps that caller simple.  Fixed cells are refused outright because
// their pixels are the obstruction map every search depends on.  The held
// flag is cleared alongside the placed flag, so a cell a pre-placement
// pass had pinned becomes an ordinary candidate again once it is lifted.
// The journal entry, when a caller has installed one, is written before the
// pixels are erased, so it still carries the hold state the erase is about
// to discard.  Like the commit above it is bookkeeping for that caller
// rather than a mechanism the fallback relies on: ripUpAndReplace() calls
// this and then searches again, and it does so with no journal installed.
// What actually lets that path evict and still recover is
// Grid::erasePixel() clearing only the entries this cell owns, which leaves
// every neighbour's pixels intact for the re-placement attempt that
// follows.
void Opendp::unplaceCell(Node* cell)
{
  if (cell->isFixed() || !cell->isPlaced()) {
    return;
  }
  if (journal_) {
    UnplaceCellAction action(cell, cell->isHold());
    journal_->addAction(action);
  }
  grid_->erasePixel(cell);
  cell->setPlaced(false);
  cell->setHold(false);
}

}  // namespace dpl
