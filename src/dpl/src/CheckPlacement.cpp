// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2020-2025, The OpenROAD Authors

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "PlacementDRC.h"
#include "dpl/Opendp.h"
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include "infrastructure/Padding.h"
#include "infrastructure/network.h"
#include "odb/db.h"
#include "odb/isotropy.h"
#include "utl/Logger.h"
namespace dpl {

using odb::Direction2D;
using std::vector;

using utl::DPL;

using utl::format_as;  // NOLINT(misc-unused-using-decls)

// checkPlacement is this module's definition of "legal": a placement is
// legal exactly when every check below passes for every cell, so the
// ordered list that follows is the invariant detailed placement exists to
// establish rather than a mere diagnostic pass.  It is also a standalone
// verifier - importDb() rebuilds the network from the database and
// adjustNodesOrient() re-reads each instance's orientation from it, so
// nothing here trusts what this module last left in memory and a database
// produced by any tool, or simply read from disk, can be checked.
//
// The nine separate failure vectors exist so that each class of violation
// is reported under its own message identifier rather than collapsed into
// a single unattributable error, and the mapping from check to identifier
// is stable across runs.  The number alone is not unique module-wide,
// though: two of the nine are also used by informational messages emitted
// earlier in detailed placement - 5 for the displacement limits and 6 for
// the utilization report, both in Opendp.cpp - so a log search on one of
// those two numbers needs the warning level and the check name carried in
// the message text to tell a violation apart from an informational line.
// The other seven - 3, 4, 7, 8, 9, 10 and 11 - are used nowhere else in
// the module.  initGrid() and groupAssignCellRegions() must run before the
// loop because every per-cell test below but one reads either the pixel
// grid or a cell's assigned region, and neither exists until they do.  The
// placed check is that one exception: it reads the instance's own status
// and depends on neither.
//
// The order inside the per-cell loop is forced, not incidental:
//   - Site alignment comes first and is the only check that abandons the
//     rest of the cell.  A left edge that is not a multiple of the site
//     width, or a bottom edge that is not exactly some row's origin, puts
//     the cell off the site/row lattice altogether, and the tests that
//     convert the cell into grid indices derived from that lattice - the
//     in-rows scan, region placement, and the pixel claiming the overlap
//     pass performs - would then return verdicts that are meaningless
//     rather than merely negative.  Not every later test is in that class:
//     the placed test reads the instance's placement status straight from
//     the database, and the overlap test compares cell rectangles in
//     database units.  The effect of abandoning the cell is that a
//     misaligned cell is reported under site alignment and under nothing
//     else, and that its padding is never painted, so no later cell is
//     measured against a reservation it would have made.
//   - Site alignment, in-rows and region placement are gated on
//     isStdCell() because all three are assertions about the row lattice,
//     which only core and endcap instances are obliged to occupy.  What
//     that gate lets past to the ungated checks is blocks: the network
//     admits only masters OpenDB reports as core-auto-placeable, which
//     covers the core, endcap and block families and excludes every pad
//     variant outright, and this loop then skips any node not typed as a
//     cell.  So no pad is ever among the cells checked here, and the
//     ungated checks are being applied to blocks alongside standard
//     cells.
//   - The padding check deliberately precedes paintCellPadding(), so a
//     cell is never rejected against its own reservation.  Reservations
//     then accumulate as the loop advances, which suffices: a conflicting
//     pair is caught when the later of the two cells is reached.
//
// The one-site-gap check is deferred to a second loop over every cell.
// The explanation immediately above that loop is authoritative; the
// mechanism behind it is that checkOverlap() carries a side effect - it
// claims every unowned pixel it visits for the cell under test - while the
// gap probe reads neighbouring pixels' cell pointers.  Only once the first
// loop has run that side effect over every cell is the grid populated
// enough for the probe to see a neighbour, so probing earlier would
// silently under-report instead of failing.  Note also that the flag
// guarding that loop is not a user setting: importDb() derives it from the
// library, disallowing one-site gaps only when no master is exactly one
// site wide, because a gap that narrow is a violation only when nothing
// exists that could fill it.
//
// The reporting identifiers are fixed message numbers emitted in check
// order, not in numeric order - 11 (Padding) is emitted between 5
// (Overlap) and 6 (Site aligned) - so the sequence 3, 4, 5, 11, 6, 7, 8,
// 9, 10 below is not a numbering scheme and must not be read as one:
//   3 Placed, 4 Placed in rows, 5 Overlap, 11 Padding, 6 Site aligned,
//   7 One site gap, 8 Region placement,
//   9 LEF58_CELLEDGESPACINGTABLE, 10 Blocked layers.
//
// Observed characteristic: the design__violations metric sums five of the
// nine categories, whereas the aggregation that raises the terminal error
// sums all nine, one-site gaps conditionally.  A run can therefore publish
// a zero violation count and still fail here.
void Opendp::checkPlacement(const bool verbose,
                            const std::string& report_file_name)
{
  importDb();
  adjustNodesOrient();

  std::vector<Node*> placed_failures;
  std::vector<Node*> in_rows_failures;
  std::vector<Node*> overlap_failures;
  std::vector<Node*> padding_failures;
  std::vector<Node*> one_site_gap_failures;
  std::vector<Node*> site_align_failures;
  std::vector<Node*> region_placement_failures;
  std::vector<Node*> edge_spacing_failures;
  std::vector<Node*> blocked_layers_failures;

  initGrid();
  groupAssignCellRegions();
  const auto& row_coords = grid_->getRowCoordinates();
  for (auto& cell : network_->getNodes()) {
    if (cell->getType() != Node::CELL) {
      continue;
    }
    if (cell->isStdCell()) {
      // Site alignment check
      if (cell->getLeft() % grid_->getSiteWidth() != 0
          || row_coords.find(cell->getBottom().v) == row_coords.end()) {
        site_align_failures.push_back(cell.get());
        continue;
      }

      if (!checkInRows(*cell)) {
        in_rows_failures.push_back(cell.get());
      }
      if (!checkRegionPlacement(cell.get())) {
        region_placement_failures.push_back(cell.get());
      }
    }
    // Placed check
    if (!isPlaced(cell.get())) {
      placed_failures.push_back(cell.get());
    }
    // Overlap check
    if (checkOverlap(*cell)) {
      overlap_failures.push_back(cell.get());
    }
    // Padding check
    if (!drc_engine_->checkPadding(cell.get())) {
      padding_failures.emplace_back(cell.get());
    }
    grid_->paintCellPadding(cell.get());
    // EdgeSpacing check
    if (!drc_engine_->checkEdgeSpacing(cell.get())) {
      edge_spacing_failures.emplace_back(cell.get());
    }
    if (!drc_engine_->checkBlockedLayers(cell.get())) {
      blocked_layers_failures.emplace_back(cell.get());
    }
  }
  // This loop is separate because it needs to be done after the overlap check
  // The overlap check assigns the overlap cell to its pixel
  // Thus, the one site gap check needs to be done after the overlap check
  // Otherwise, this check will miss the pixels that could have resulted in
  // one-site gap violations as null
  if (disallow_one_site_gaps_) {
    for (auto& cell : network_->getNodes()) {
      // One site gap check
      if (cell->getType() == Node::CELL && checkOneSiteGaps(*cell)) {
        one_site_gap_failures.push_back(cell.get());
      }
    }
  }
  saveFailures(placed_failures,
               in_rows_failures,
               overlap_failures,
               padding_failures,
               one_site_gap_failures,
               site_align_failures,
               region_placement_failures,
               {},
               edge_spacing_failures,
               blocked_layers_failures);
  if (!report_file_name.empty()) {
    writeJsonReport(report_file_name);
  }
  reportFailures(placed_failures, 3, "Placed", verbose);
  reportFailures(in_rows_failures, 4, "Placed in rows", verbose);
  reportFailures(
      overlap_failures, 5, "Overlap", verbose, [&](Node* cell) -> void {
        reportOverlapFailure(cell);
      });
  reportFailures(padding_failures, 11, "Padding", verbose);
  reportFailures(site_align_failures, 6, "Site aligned", verbose);
  reportFailures(one_site_gap_failures, 7, "One site gap", verbose);
  reportFailures(region_placement_failures, 8, "Region placement", verbose);
  reportFailures(
      edge_spacing_failures, 9, "LEF58_CELLEDGESPACINGTABLE", verbose);
  reportFailures(blocked_layers_failures, 10, "Blocked layers", verbose);
  logger_->metric("design__violations",
                  placed_failures.size() + in_rows_failures.size()
                      + overlap_failures.size() + padding_failures.size()
                      + site_align_failures.size());

  if (placed_failures.size() + in_rows_failures.size() + overlap_failures.size()
          + padding_failures.size() + site_align_failures.size()
          + (disallow_one_site_gaps_ ? one_site_gap_failures.size() : 0)
          + region_placement_failures.size() + edge_spacing_failures.size()
          + blocked_layers_failures.size()
      > 0) {
    logger_->error(
        DPL, 33, "detailed placement checks failed during check placement.");
  }
}

void Opendp::saveViolations(const std::vector<Node*>& failures,
                            odb::dbMarkerCategory* category,
                            const std::string& violation_type) const
{
  for (auto failure : failures) {
    odb::dbMarker* marker = odb::dbMarker::create(category);
    if (!marker) {
      break;
    }
    int xMin = (failure->getLeft() + core_.xMin()).v;
    int yMin = (failure->getBottom() + core_.yMin()).v;
    int xMax = (failure->getLeft() + failure->getWidth() + core_.xMin()).v;
    int yMax = (failure->getBottom() + failure->getHeight() + core_.yMin()).v;

    if (violation_type == "overlap") {
      const Node* o_cell = checkOverlap(*failure);
      if (!o_cell) {
        logger_->error(DPL,
                       48,
                       "Could not find overlapping cell for cell {}",
                       failure->name());
      }
      odb::Rect o_rect(o_cell->getLeft().v,
                       o_cell->getBottom().v,
                       o_cell->getLeft().v + o_cell->getWidth().v,
                       o_cell->getBottom().v + o_cell->getHeight().v);
      odb::Rect f_rect(failure->getLeft().v,
                       failure->getBottom().v,
                       failure->getLeft().v + failure->getWidth().v,
                       failure->getBottom().v + failure->getHeight().v);

      odb::Rect overlap_rect;
      o_rect.intersection(f_rect, overlap_rect);

      xMin = overlap_rect.xMin() + core_.xMin();
      yMin = overlap_rect.yMin() + core_.yMin();
      xMax = overlap_rect.xMax() + core_.xMin();
      yMax = overlap_rect.yMax() + core_.yMin();

      marker->addSource(o_cell->getDbInst());
    }
    marker->addShape(odb::Rect{xMin, yMin, xMax, yMax});
    marker->addSource(failure->getDbInst());
  }
}

void Opendp::saveFailures(const vector<Node*>& placed_failures,
                          const vector<Node*>& in_rows_failures,
                          const vector<Node*>& overlap_failures,
                          const vector<Node*>& padding_failures,
                          const vector<Node*>& one_site_gap_failures,
                          const vector<Node*>& site_align_failures,
                          const vector<Node*>& region_placement_failures,
                          const vector<Node*>& placement_failures,
                          const vector<Node*>& edge_spacing_failures,
                          const vector<Node*>& blocked_layers_failures)
{
  if (placed_failures.empty() && in_rows_failures.empty()
      && overlap_failures.empty() && padding_failures.empty()
      && one_site_gap_failures.empty() && site_align_failures.empty()
      && region_placement_failures.empty() && placement_failures.empty()
      && edge_spacing_failures.empty() && blocked_layers_failures.empty()) {
    return;
  }

  auto* tool_category = odb::dbMarkerCategory::createOrReplace(block_, "DPL");
  if (!placed_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "Placement failures");
    category->setDescription("Cells that were not placed.");
    saveViolations(placed_failures, category);
  }
  if (!in_rows_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(tool_category,
                                                           "In_rows_failures");
    category->setDescription(
        "Cells that were not assigned to rows in the grid.");
    saveViolations(in_rows_failures, category);
  }
  if (!overlap_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(tool_category,
                                                           "Overlap_failures");
    category->setDescription("Cells that are overlapping with other cells.");
    saveViolations(overlap_failures, category, "overlap");
  }
  if (!padding_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(tool_category,
                                                           "Padding_failures");
    category->setDescription("Cells that violate the padding rules.");
    saveViolations(padding_failures, category);
  }
  if (!one_site_gap_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "One_site_gap_failures");
    category->setDescription(
        "Cells that violate the one site gap spacing rules.");
    saveViolations(one_site_gap_failures, category);
  }
  if (!site_align_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "Site_alignment_failures");
    category->setDescription(
        "Cells that are not aligned with placement sites.");
    saveViolations(site_align_failures, category);
  }
  if (!region_placement_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "Region_placement_failures");
    category->setDescription(
        "Cells that violate the region placement constraints.");
    saveViolations(region_placement_failures, category);
  }
  if (!placement_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "Placement_failures");
    category->setDescription("Cells that DPL failed to place.");
    saveViolations(placement_failures, category);
  }
  if (!edge_spacing_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "Cell_edge_spacing_failures");
    category->setDescription(
        "Cells that violate the LEF58_CELLEDGESPACINGTABLE.");
    saveViolations(edge_spacing_failures, category);
  }
  if (!blocked_layers_failures.empty()) {
    auto category = odb::dbMarkerCategory::createOrReplace(
        tool_category, "Blocked_layers_failures");
    category->setDescription("Cells that violate the blocked layers.");
    saveViolations(blocked_layers_failures, category);
  }
}

void Opendp::writeJsonReport(const std::string& filename)
{
  auto* tool_category = block_->findMarkerCategory("DPL");
  if (tool_category) {
    tool_category->writeJSON(filename);
  }
}

void Opendp::reportFailures(const vector<Node*>& failures,
                            const int msg_id,
                            const char* msg,
                            const bool verbose) const
{
  reportFailures(failures, msg_id, msg, verbose, [&](Node* cell) -> void {
    logger_->report(" {}", cell->name());
  });
}

void Opendp::reportFailures(
    const vector<Node*>& failures,
    const int msg_id,
    const char* msg,
    const bool verbose,
    const std::function<void(Node* cell)>& report_failure) const
{
  if (!failures.empty()) {
    logger_->warn(DPL, msg_id, "{} check failed ({}).", msg, failures.size());
    if (verbose) {
      for (Node* cell : failures) {
        report_failure(cell);
      }
    }
  }
}

void Opendp::reportOverlapFailure(Node* cell) const
{
  const Node* overlap = checkOverlap(*cell);
  logger_->report(" {} ({}) overlaps {} ({})",
                  cell->name(),
                  cell->getDbInst()->getMaster()->getName(),
                  overlap->name(),
                  overlap->getDbInst()->getMaster()->getName());
}

/* static */
bool Opendp::isPlaced(const Node* cell)
{
  return cell->getDbInst()->isPlaced();
}

// "In rows" is a stronger claim than "inside the core area".  The core
// rectangle is not guaranteed to be paved with sites: rows can be
// fragmented or absent over parts of it, and the pixels standing in for
// those places exist but are flagged invalid, so a cell can sit well
// inside the core and still be in no row at all.  That is why the scan
// rejects a present-but-invalid pixel as firmly as a missing one.
//
// The site-orientation query is applied only to the cell's first row
// because that row is what fixes the cell's site and the orientation it
// must take.  The query asks whether the row offers this cell's site at
// that column at all, and in which orientation - ultimately a power-rail
// question, since the supply rails run along a cell's top and bottom
// edges, so the polarity a cell meets follows from the orientation of its
// row.  Nothing here is deduced from a row's position in the stack: that
// orientation is a property of the individual row, the grid records
// whatever each database row declared alongside the span of sites it
// declared it for, this query reads it back, and powerCompatible() consults
// that row's own rail assignment - so neighbouring rows may agree or differ
// and nothing here treats them as alternating.  A cell whose site is
// unavailable there is not in that row however empty its pixels are.
//
// For a multi-row master the first row settles nothing on its own: the
// cell spans several rows and must present compatible rail polarity to
// each of them, which is why checkRowPowerCompatible is required across
// the whole span.  A single-row master has no span to reconcile, so the
// check is conditional rather than unconditional.
bool Opendp::checkInRows(const Node& cell) const
{
  const auto grid_rect = grid_->gridCovering(&cell);
  debugPrint(logger_,
             DPL,
             "hybrid",
             1,
             "Checking cell {} with site {} and "
             "height {} in rows. Y start {} y end {}",
             cell.name(),
             cell.getSite()->getName(),
             cell.getHeight(),
             grid_rect.ylo,
             grid_rect.yhi);

  for (GridY y = grid_rect.ylo; y < grid_rect.yhi; y++) {
    const bool first_row = (y == grid_rect.ylo);
    for (GridX x = grid_rect.xlo; x < grid_rect.xhi; x++) {
      const Pixel* pixel = grid_->gridPixel(x, y);
      // outside core or invalid
      if (pixel == nullptr || !pixel->is_valid) {
        return false;
      }
      if (first_row && !grid_->getSiteOrientation(x, y, cell.getSite())) {
        return false;
      }
    }
  }
  return !cell.getMaster()->isMultiRow()
         || checkRowPowerCompatible(&cell, grid_rect.ylo);
}

// Return the cell this cell overlaps.
const Node* Opendp::checkOverlap(Node& cell) const
{
  debugPrint(
      logger_, DPL, "grid", 2, "checking overlap for cell {}", cell.name());
  const Node* overlap_cell = nullptr;
  grid_->visitCellPixels(cell, false, [&](Pixel* pixel, bool padded) {
    const Node* pixel_cell = pixel->cell;
    if (pixel_cell) {
      if (pixel_cell != &cell && overlap(&cell, pixel_cell)) {
        overlap_cell = pixel_cell;
      }
    } else {
      pixel->cell = &cell;
    }
  });
  return overlap_cell;
}

bool Opendp::overlap(const Node* cell1, const Node* cell2) const
{
  // BLOCK/BLOCK overlaps allowed
  if (cell1->isBlock() && cell2->isBlock()) {
    return false;
  }

  const DbuPt ll1 = initialLocation(cell1, false);
  const DbuPt ll2 = initialLocation(cell2, false);
  DbuPt ur1, ur2;
  ur1 = DbuPt(ll1.x + cell1->getWidth().v, ll1.y + cell1->getHeight().v);
  ur2 = DbuPt(ll2.x + cell2->getWidth().v, ll2.y + cell2->getHeight().v);
  return ll1.x < ur2.x && ur1.x > ll2.x && ll1.y < ur2.y && ur1.y > ll2.y;
}

// Only the West and East edges are probed; North and South return at once.
// A one-site gap is a problem because it cannot be filled, and filler
// cells are placed along rows, so only a horizontal gap is unfillable -
// vertical clearance of a single row is not a gap in anything that gets
// filled.  This mirrors the placement-time test in checkPixels, which
// likewise looks left and right only.
//
// Abutment is tested first and the wider probe runs only when no abutting
// cell was found: if a neighbour touches this edge there is no gap here to
// measure, so looking past it would answer a question nobody asked.  The
// wider probe sits two sites out from the cell's own boundary column - one
// site for the gap itself, plus one more to reach the site where the next
// cell would begin - which is why the offset is twice the unit step and
// not the step itself.  The unit is sites, and its sign carries the
// direction: negative to the West, positive to the East.
//
// The returned pointer names a cell found across such a gap.  The callback
// keeps whatever the most recently probed edge yielded rather than
// accumulating every offender, and its only caller consumes the result as
// a presence test, so a violation is attributed to this cell without
// recording which of its two sides produced it.
Node* Opendp::checkOneSiteGaps(Node& cell) const
{
  Node* gap_cell = nullptr;
  grid_->visitCellBoundaryPixels(
      cell, [&](Pixel* pixel, const Direction2D& edge, GridX x, GridY y) {
        GridX abut_x{0};

        switch (static_cast<Direction2D::Value>(edge)) {
          case Direction2D::West:
            abut_x = GridX{-1};
            break;
          case Direction2D::East:
            abut_x = GridX{1};
            break;
          case Direction2D::North:
          case Direction2D::South:
            return;
        }
        // check the abutting pixel
        const Pixel* abut_pixel = grid_->gridPixel(x + abut_x, y);
        const bool abuttment_exists = (abut_pixel && abut_pixel->cell);
        if (!abuttment_exists) {
          // check the 1 site gap pixel
          const Pixel* gap_pixel = grid_->gridPixel(x + GridX{2 * abut_x.v}, y);
          if (gap_pixel) {
            gap_cell = gap_pixel->cell;
          }
        }
      });
  return gap_cell;
}

// Both conditions are required and neither implies the other.  contains()
// asks only whether the rectangle of the region this cell was assigned to
// covers the cell, which says nothing about any other region; the R-tree
// test inside checkRegionOverlap additionally demands that the cell's box
// overlap exactly one region rectangle and be covered by it, and that is
// what rules out a cell straddling two adjacent regions or protruding from
// its own region into open core.  A cell can satisfy either test alone and
// still be illegally placed.
//
// The extents computed here are in database units, while the four
// arguments handed to checkRegionOverlap are grid indices.  Observed
// characteristic: the two Y indices are formed by dividing by the cell's
// own height rather than by consulting the row table, so they agree with
// Grid::gridYToDbu - the hybrid-row-aware conversion checkRegionOverlap
// uses to turn them back into database units - only for a single-row cell
// in a design whose rows all share one height.
//
// A cell with no region is legal here unconditionally, because region
// placement constrains only cells that were assigned one.  The converse
// direction, an unassigned cell intruding into somebody's region, is not
// this function's concern: it is refused during placement by the
// pixel-group conditions in checkPixels, which deny a pixel owned by a
// group to any cell outside that group.
bool Opendp::checkRegionPlacement(const Node* cell) const
{
  const DbuX x_begin = cell->getLeft();
  const DbuX x_end = x_begin + cell->getWidth();
  const DbuY y_begin = cell->getBottom();
  const DbuY y_end = y_begin + cell->getHeight();

  if (cell->getRegion()) {
    const DbuX site_width = grid_->getSiteWidth();
    return cell->getRegion()->contains(
               odb::Rect(x_begin.v, y_begin.v, x_end.v, y_end.v))
           && checkRegionOverlap(cell,
                                 GridX{x_begin.v / site_width.v},
                                 GridY{y_begin.v / cell->getHeight().v},
                                 GridX{x_end.v / site_width.v},
                                 GridY{y_end.v / cell->getHeight().v});
  }
  return true;
}

}  // namespace dpl
