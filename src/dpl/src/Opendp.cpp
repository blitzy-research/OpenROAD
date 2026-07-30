// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2018-2025, The OpenROAD Authors

#include "dpl/Opendp.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "NegotiationLegalizer.h"
#include "PlacementDRC.h"
#include "boost/geometry/index/predicates.hpp"
#include "dpl/OptMirror.h"
#include "graphics/DplObserver.h"
#include "infrastructure/Coordinates.h"
#include "infrastructure/DecapObjects.h"  // NOLINT(misc-include-cleaner) Needed for DecapCell/GapInfo completeness in ~Opendp()
#include "infrastructure/Grid.h"
#include "infrastructure/Objects.h"
#include "infrastructure/Padding.h"
#include "infrastructure/network.h"
#include "odb/db.h"
#include "odb/geom.h"
#include "odb/util.h"
#include "util/journal.h"
#include "utl/Logger.h"

namespace dpl {

using std::round;
using std::string;

using utl::DPL;

using odb::dbInst;
using odb::Rect;

////////////////////////////////////////////////////////////////

bool Opendp::isMultiRow(const Node* cell) const
{
  return network_->getMaster(cell->getDbInst()->getMaster())->isMultiRow();
}

////////////////////////////////////////////////////////////////

Opendp::Opendp(odb::dbDatabase* db, utl::Logger* logger)
    : logger_(logger), db_(db)
{
  dummy_cell_ = std::make_unique<Node>();
  dummy_cell_->setPlaced(true);
  dummy_cell_->setFixed(true);
  padding_ = std::make_shared<Padding>();
  grid_ = std::make_unique<Grid>();
  grid_->init(logger);
  network_ = std::make_unique<Network>();
  arch_ = std::make_unique<Architecture>();
}

Opendp::~Opendp() = default;

void Opendp::setPaddingGlobal(const int left, const int right)
{
  padding_->setPaddingGlobal(GridX{left}, GridX{right});
}

void Opendp::setPadding(odb::dbInst* inst, const int left, const int right)
{
  padding_->setPadding(inst, GridX{left}, GridX{right});
}

void Opendp::setPadding(odb::dbMaster* master, const int left, const int right)
{
  padding_->setPadding(master, GridX{left}, GridX{right});
}

void Opendp::setDebug(std::unique_ptr<DplObserver>& observer)
{
  debug_observer_ = std::move(observer);
}

void Opendp::setJumpMoves(const int jump_moves)
{
  jump_moves_ = jump_moves;
}

void Opendp::setIterativePlacement(const bool iterative)
{
  iterative_debug_ = iterative;
}

void Opendp::setDeepIterativePlacement(const bool deep_iterative)
{
  deep_iterative_debug_ = deep_iterative;
  if (deep_iterative) {
    iterative_debug_ = true;
  }
}

void Opendp::setJournal(Journal* journal)
{
  journal_ = journal;
}

Journal* Opendp::getJournal() const
{
  return journal_;
}

// Entry point for the detailed_placement command and the single point at
// which the two legalization engines are chosen. Dispatch is on one flag
// rather than two commands so that both engines share an identical
// preamble - import, utilization screening, HPWL baseline and displacement
// limit resolution all complete before the branch - and so that both close
// with the same statistics-then-write-back pair.
//
// The displacement limits are all-or-nothing rather than per-axis. The
// guard below tests max_displacement_x == 0 || max_displacement_y == 0, so
// a caller passing a real X limit together with a zero Y limit does not
// keep its X value: both arguments are discarded and both built-in
// defaults are installed. Zero means unspecified for the pair as a whole.
//
// The two limits carry different units, and the DPL 5 message below is the
// authoritative statement of which: the horizontal limit counts sites and
// the vertical limit counts rows. The defaults are therefore 500 sites and
// 100 rows. diamondSearch in Place.cpp applies the vertical limit to a row
// index, so rows is the operative unit for it.
//
// The limits are resolved before any grid exists because Grid::initGrid
// hands both of them to Grid::markHopeless, which precomputes how far a
// cell can reach; a grid built from stale limits would prune sites the
// search is still entitled to visit.
//
// findDisplacementStats runs before updateDbInstLocations in both
// branches. The displacement measure recovers each cell's start position
// by reading the database instance (see disp below), so writing legalized
// coordinates first would make every cell appear not to have moved.
//
// The diamond branch names every offending instance under DPL 34 and 35
// and persists the JSON report before raising DPL 36, so a failed run
// tells the user which cells could not be legalized instead of only that
// legalization failed. DPL 36 is an error rather than a warning: an
// unlegalizable placement stops the flow instead of handing downstream
// stages a database with overlapping cells.
void Opendp::detailedPlacement(const int max_displacement_x,
                               const int max_displacement_y,
                               const std::string& report_file_name,
                               bool incremental,
                               const bool use_negotiation,
                               const bool run_abacus)
{
  incremental_ = incremental;
  use_negotiation_ |= use_negotiation;
  importDb();
  adjustNodesOrient();
  if (!incremental_) {
    for (const auto& node : network_->getNodes()) {
      if (node->getType() == Node::CELL && !node->isFixed()) {
        node->setPlaced(false);
      }
    }
  }

  if (have_fillers_) {
    logger_->warn(DPL, 37, "Use remove_fillers before detailed placement.");
  }

  {
    const int64_t core_area
        = static_cast<int64_t>(core_.dx()) * static_cast<int64_t>(core_.dy());
    int64_t inst_area = 0;
    for (const auto& node : network_->getNodes()) {
      if (node->getType() == Node::CELL) {
        inst_area += static_cast<int64_t>(node->getWidth().v)
                     * static_cast<int64_t>(node->getHeight().v);
      }
    }
    const double utilization = core_area > 0
                                   ? (static_cast<double>(inst_area)
                                      / static_cast<double>(core_area))
                                         * 100.0
                                   : 0.0;
    logger_->info(DPL,
                  6,
                  "Core area: {:.2f} um^2, Instances area: {:.2f} um^2, "
                  "Utilization: {:.1f}%",
                  block_->dbuAreaToMicrons(core_area),
                  block_->dbuAreaToMicrons(inst_area),
                  utilization);
    logger_->metric("utilizatin__before__dpl", utilization);
    if (utilization > 100.0) {
      logger_->error(
          DPL, 38, "Utilization greater than 100%, impossible to legalize");
    }
  }

  odb::WireLengthEvaluator eval(block_);
  hpwl_before_ = eval.hpwl();

  if (max_displacement_x == 0 || max_displacement_y == 0) {
    max_displacement_x_ = 500;
    max_displacement_y_ = 100;
  } else {
    max_displacement_x_ = max_displacement_x;
    max_displacement_y_ = max_displacement_y;
  }

  logger_->info(DPL,
                5,
                "Diamond search max displacement: +/- {} sites horizontally, "
                "+/- {} rows vertically.",
                max_displacement_x_,
                max_displacement_y_);

  if (!use_negotiation_) {
    logger_->info(DPL, 1101, "Legalizing using diamond search.");
    diamondDPL();
    findDisplacementStats();
    updateDbInstLocations();
    if (!placement_failures_.empty()) {
      logger_->info(DPL,
                    34,
                    "Detailed placement failed on the following {} instances:",
                    placement_failures_.size());
      for (auto cell : placement_failures_) {
        logger_->info(DPL, 35, " {}", cell->name());
      }

      saveFailures({}, {}, {}, {}, {}, {}, {}, placement_failures_, {}, {});
      if (!report_file_name.empty()) {
        writeJsonReport(report_file_name);
      }
      logger_->error(DPL, 36, "Detailed placement failed inside DPL.");
    }
  } else {
    initGrid();
    setFixedGridCells();
    // Populate pixel->group for each fence region so diamondRecovery's
    // underlying diamondSearch correctly enforces region constraints.
    if (!arch_->getRegions().empty()) {
      groupInitPixels2();
      groupInitPixels();
    }
    logger_->info(DPL, 1102, "Legalizing using negotiation legalizer.");

    NegotiationLegalizer negotiation(this,
                                     db_,
                                     logger_,
                                     padding_.get(),
                                     debug_observer_.get(),
                                     network_.get());
    negotiation.setRunAbacus(run_abacus);
    negotiation.legalize();
    negotiation.setDplPositions();

    if (negotiation.numViolations() > 0) {
      logger_->warn(DPL,
                    701,
                    "NegotiationLegalizer did not fully converge. "
                    "Violations remain: {}",
                    negotiation.numViolations());
      logger_->metric("NL__no__converge__final_violations",
                      negotiation.numViolations());
    }

    findDisplacementStats();
    updateDbInstLocations();
  }
}

// Publishes legalized positions back to OpenDB. This is the only writer of
// instance locations on the legalization path, which is what lets every
// other routine here treat the database as still holding pre-legalization
// state; disp and initialLocation both depend on that.
//
// DPL works in core-relative coordinates so that grid indices and cell
// offsets share one origin, so the core's lower-left corner has to be
// added back on the way out: what a cell stores is an offset, not a chip
// coordinate.
//
// Both writes are guarded by an inequality test, and the guard is not a
// micro-optimization. Instance setters fire OpenDB callbacks, and those
// callbacks propagate into the incremental state other tools keep about
// this block, so rewriting an unchanged value would invalidate analysis
// that nothing actually disturbed.
void Opendp::updateDbInstLocations()
{
  for (auto& cell : network_->getNodes()) {
    if (!cell->isFixed() && cell->isStdCell()) {
      odb::dbInst* db_inst_ = cell->getDbInst();
      // Only move the instance if necessary to avoid triggering callbacks.
      if (db_inst_->getOrient() != cell->getOrient()) {
        db_inst_->setOrient(cell->getOrient());
      }
      const DbuX x = core_.xMin() + cell->getLeft();
      const DbuY y = core_.yMin() + cell->getBottom();
      int inst_x, inst_y;
      db_inst_->getLocation(inst_x, inst_y);
      if (x != inst_x || y != inst_y) {
        db_inst_->setLocation(x.v, y.v);
      }
    }
  }
}

// Reports the placement analysis table. Every quantity DPL computes is in
// database units; this routine is the boundary at which those integers
// become microns, so the figures a user reads are physical lengths rather
// than raw database-unit counts. Nothing downstream consumes the converted
// values, so the conversion stays confined here.
//
// The metric keys emitted below are an observable output contract rather
// than an internal detail: regression and reporting tooling outside this
// module keys off their exact spelling, so a key's name is part of the
// tool's behaviour however that name happens to read.
//
// The delta is guarded because its baseline is a measured quantity and not
// a constant - a design whose nets contribute no wirelength leaves the
// pre-legalization baseline at zero, and the percentage would divide by it.
void Opendp::reportLegalizationStats() const
{
  logger_->report("Placement Analysis");
  logger_->report("---------------------------------");
  logger_->report("total displacement   {:10.1f} u",
                  block_->dbuToMicrons(displacement_sum_));
  logger_->metric("design__instance__displacement__total",
                  block_->dbuToMicrons(displacement_sum_));
  logger_->report("average displacement {:10.1f} u",
                  block_->dbuToMicrons(displacement_avg_));
  logger_->metric("design__instance__displacement__mean",
                  block_->dbuToMicrons(displacement_avg_));
  logger_->report("max displacement     {:10.1f} u",
                  block_->dbuToMicrons(displacement_max_));
  logger_->metric("design__instance__displacement__max",
                  block_->dbuToMicrons(displacement_max_));
  logger_->report("original HPWL        {:10.1f} u",
                  block_->dbuToMicrons(hpwl_before_));
  odb::WireLengthEvaluator eval(block_);
  const double hpwl_legal = eval.hpwl();
  logger_->report("legalized HPWL       {:10.1f} u",
                  block_->dbuToMicrons(hpwl_legal));
  logger_->metric("route__wirelength__estimated",
                  block_->dbuToMicrons(hpwl_legal));
  const int hpwl_delta
      = (hpwl_before_ == 0.0)
            ? 0.0
            : round((hpwl_legal - hpwl_before_) / hpwl_before_ * 100);
  logger_->report("delta HPWL           {:10} %", hpwl_delta);
  logger_->report("");
  logger_->metric("dpl__hpwl__delta", hpwl_legal - hpwl_before_);
  logger_->metric("dpl__hpwl__delta__percent", hpwl_delta);
}

////////////////////////////////////////////////////////////////

void Opendp::findDisplacementStats()
{
  displacement_avg_ = 0;
  displacement_sum_ = 0;
  displacement_max_ = 0;
  for (auto& cell : network_->getNodes()) {
    if (cell->getType() != Node::CELL) {
      continue;
    }
    const int displacement = disp(cell.get());
    displacement_sum_ += displacement;
    displacement_max_ = std::max<int64_t>(displacement, displacement_max_);
  }
  if (network_->getNumCells() != 0) {
    displacement_avg_ = displacement_sum_ / network_->getNumCells();
  } else {
    displacement_avg_ = 0.0;
  }
}

////////////////////////////////////////////////////////////////

void Opendp::optimizeMirroring()
{
  OptimizeMirroring opt(logger_, db_);
  opt.run();
}

void Opendp::resetGlobalSwapParams()
{
  global_swap_params_ = GlobalSwapParams();
}

void Opendp::configureGlobalSwapParams(
    int passes,
    double tolerance,
    double tradeoff,
    double area_weight,
    double pin_weight,
    double user_weight,
    int sampling_moves,
    int normalization_interval,
    double profiling_excess,
    const std::vector<double>& budget_multipliers)
{
  if (passes > 0) {
    global_swap_params_.passes = passes;
  }
  if (tolerance > 0.0) {
    global_swap_params_.tolerance = tolerance;
  }
  if (tradeoff >= 0.0) {
    global_swap_params_.tradeoff = std::max(0.0, std::min(1.0, tradeoff));
  }
  if (area_weight >= 0.0) {
    global_swap_params_.area_weight = area_weight;
  }
  if (pin_weight >= 0.0) {
    global_swap_params_.pin_weight = pin_weight;
  }
  if (user_weight > 0.0) {
    global_swap_params_.user_congestion_weight = user_weight;
  }
  if (sampling_moves > 0) {
    global_swap_params_.sampling_moves = sampling_moves;
  }
  if (normalization_interval > 0) {
    global_swap_params_.normalization_interval = normalization_interval;
  }
  if (profiling_excess > 0.0) {
    global_swap_params_.profiling_excess = profiling_excess;
  }
  if (!budget_multipliers.empty()) {
    global_swap_params_.budget_multipliers = budget_multipliers;
  }
  if (global_swap_params_.budget_multipliers.empty()) {
    global_swap_params_.budget_multipliers = {1.0};
  }
  if (global_swap_params_.area_weight < 0.0
      || global_swap_params_.pin_weight < 0.0) {
    logger_->error(DPL, 1280, "Utilization weights must be non-negative.");
  }
  if (global_swap_params_.area_weight == 0.0
      && global_swap_params_.pin_weight == 0.0) {
    logger_->error(
        DPL, 1281, "At least one utilization weight must be greater than 0.");
  }
}

// How far one cell sits from where global placement left it, as Manhattan
// distance in database units. The start position is recovered from the
// database instance, so this reports the pre-legalization position only
// while updateDbInstLocations has not yet run.
//
// The measure is deliberately unweighted: both axes are summed in raw
// database units. It answers how far a cell physically moved, which is why
// it is the right input for the reported displacement statistics and for
// the displacement-ordered refinement sorts in Place.cpp.
//
// It is not the site-search metric. calcDist in Place.cpp measures a
// different quantity from a different origin - distance from the current
// search centre - and does weight the axes, scaling X by the site width
// and resolving Y through the row table. Conflating the two would be
// wrong: they agree on neither origin nor weighting.
int Opendp::disp(const Node* cell) const
{
  const DbuPt init = initialLocation(cell, false);
  return sumXY(abs(init.x - cell->getLeft()), abs(init.y - cell->getBottom()));
}

int Opendp::padGlobalLeft() const
{
  return padding_->padGlobalLeft().v;
}

int Opendp::padGlobalRight() const
{
  return padding_->padGlobalRight().v;
}

int Opendp::padLeft(odb::dbInst* inst) const
{
  return padding_->padLeft(inst).v;
}

int Opendp::padRight(odb::dbInst* inst) const
{
  return padding_->padRight(inst).v;
}

// Builds the site grid. It is a thin forwarder rather than part of
// diamondDPL because every DPL entry point that touches pixels needs the
// same grid - legalization, placement checking, filler and decap placement
// all route through here.
//
// The displacement limits are handed down because reachability is baked
// into the grid instead of being tested per search: markHopeless uses them
// to precompute which pixels no cell could ever reach. They must therefore
// already be resolved before this runs.
//
// Each call rebuilds pixel state from scratch, so callers are responsible
// for repainting whatever ownership they rely on afterwards. Fixed cells,
// any already-legal cells and fence-region ownership are all discarded
// here and restored by separate passes.
void Opendp::initGrid()
{
  grid_->initGrid(
      db_, block_, padding_, max_displacement_x_, max_displacement_y_);
}

void Opendp::deleteGrid()
{
  grid_->clear();
}

// Answers which fence regions a box touches. The answer comes from an
// R-tree because this sits on the innermost path of the site search: the
// legality predicate consults checkRegionOverlap for every candidate site
// it examines, so a linear scan over regions would cost the search a
// factor of the region count. The tree is built once when the network is
// created and is queried nowhere else.
//
// The caller owns the result vector and it is cleared on entry, so one
// buffer can be reused across the many queries a search performs instead
// of reallocating per call.
void Opendp::findOverlapInRtree(const bgBox& queryBox,
                                std::vector<bgBox>& overlaps) const
{
  overlaps.clear();
  regions_rtree_.query(boost::geometry::index::intersects(queryBox),
                       std::back_inserter(overlaps));
}

// Incremental-mode pass deciding which already-placed cells may keep the
// positions they arrived with. It exists because incremental legalization
// must not disturb work an earlier run already legalized, yet it cannot
// take the incoming placement on trust either, so every placed cell has to
// earn its position.
//
// The two screens below are independent and both are required: a cell can
// sit exactly on a site and still overlap a neighbour, and a cell can be
// entirely alone yet straddle a row boundary. Failing either disqualifies.
//
// The overlap screen borrows pixel->cell as scratch space to detect
// collisions, which is why the sweep that follows clears every non-fixed
// stamp before the final paint. Without that reset a disqualified cell
// would leave its footprint behind and block sites the search is entitled
// to use. Fixed stamps survive because an earlier pass painted them and
// they are not what this pass is adjudicating.
//
// Both parties to a collision are disqualified rather than just the second
// one encountered: nothing here justifies preferring either, and keeping
// one would make the outcome depend on traversal order.
void Opendp::setInitialGridCells()
{
  std::unordered_set<Node*> conflicted;
  const DbuX site_width = grid_->getSiteWidth();

  // Check which cells are missaligned with rows
  for (auto& node : network_->getNodes()) {
    if (node->getType() == Node::CELL && !node->isFixed() && node->isPlaced()) {
      const GridX x = grid_->gridX(node.get());
      const GridY y = grid_->gridSnapDownY(node.get());
      if (node->getLeft() != gridToDbu(x, site_width)
          || node->getBottom() != grid_->gridYToDbu(y)
          || !canBePlaced(node.get(), x, y)) {
        conflicted.insert(node.get());
      }
    }
  }

  // Check which cells are overlapping with other cells
  for (auto& node : network_->getNodes()) {
    if (node->getType() == Node::CELL && !node->isFixed() && node->isPlaced()) {
      if (conflicted.contains(node.get())) {
        continue;
      }

      bool node_conflicted = false;
      grid_->visitCellPixels(
          *node, false, [&](Pixel* pixel, [[maybe_unused]] bool padded) {
            if (pixel->cell != nullptr && pixel->cell != node.get()) {
              node_conflicted = true;
              if (!pixel->cell->isFixed()) {
                conflicted.insert(pixel->cell);
              }
            } else {
              pixel->cell = node.get();
            }
          });

      if (node_conflicted) {
        conflicted.insert(node.get());
      }
    }
  }

  for (GridY y{0}; y < grid_->getRowCount(); y++) {
    for (GridX x{0}; x < grid_->getRowSiteCount(); x++) {
      Pixel& pixel = grid_->pixel(y, x);
      if (pixel.cell != nullptr && !pixel.cell->isFixed()) {
        pixel.cell = nullptr;
      }
    }
  }

  for (auto& node : network_->getNodes()) {
    if (node->getType() == Node::CELL && !node->isFixed() && node->isPlaced()) {
      if (conflicted.find(node.get()) == conflicted.end()) {
        // This cell is perfectly legal and has no conflicts.
        grid_->visitCellPixels(
            *node, false, [&](Pixel* pixel, [[maybe_unused]] bool padded) {
              pixel->cell = node.get();
              pixel->util = 1.0;
            });
        grid_->paintCellPadding(node.get());
      } else {
        // This cell is either illegal or was part of an overlap conflict.
        // Unplace it.
        unplaceCell(node.get());
      }
    }
  }
}

// Paints everything the legalizer is not allowed to move. This runs before
// any search so that fixed obstructions already occupy their pixels when
// the first candidate site is tested; the search has no notion of
// fixedness and depends entirely on finding those pixels taken.
//
// Padding is recorded in its own pixel field instead of as occupancy. A
// padding column is a real site that a compatible neighbour may still
// legally use, so marking it occupied would sterilize usable area. The
// DRC engine instead consults the reservation and decides whether one
// specific cell conflicts with it.
//
// Masters that carry an overlap obstruction layer are painted from that
// geometry rather than from their bounding box, so a notched or L-shaped
// macro does not block sites it never covers. Blocks additionally get the
// hopeless mark, which the recovery path reads as a cue to walk a cell off
// the macro rather than as a statement about reachability.
void Opendp::setFixedGridCells()
{
  for (auto& cell : network_->getNodes()) {
    if (cell->getType() == Node::CELL && cell->isFixed()) {
      grid_->visitCellPixels(*cell, true, [&](Pixel* pixel, bool padded) {
        if (padded) {
          pixel->padding_reserved_by = cell.get();
        } else {
          setGridCell(*cell, pixel);
        }
      });
    }
  }
}

void Opendp::setGridCell(Node& cell, Pixel* pixel)
{
  pixel->cell = &cell;
  pixel->util = 1.0;
  if (cell.isBlock()) {
    // Try the is_hopeless strategy to get off of a block
    pixel->is_hopeless = true;
  }
}

// Binds every cell of a fence region to one specific rectangle of that
// region and records how full the region is.
//
// The binding has to be single-valued because its consumers dereference it
// unconditionally: the group pre-placement passes in Place.cpp both sort
// and aim by the bound rectangle, and the legality predicate's region
// stage demands that the cell be covered by exactly one region box. That
// is why a cell currently inside none of its region's rectangles is still
// handed the first one instead of being left unbound - an arbitrary but
// valid target beats a null pointer, and the search will carry the cell
// inside.
//
// The utilization tally reads group ownership straight out of the pixels,
// so this necessarily runs after region ownership has been painted rather
// than before it. Each row contributes its own height instead of a shared
// one, because hybrid-row designs have no single row height to assume.
void Opendp::groupAssignCellRegions()
{
  const int64_t site_width = grid_->getSiteWidth().v;
  const GridX row_site_count = grid_->getRowSiteCount();
  const GridY row_count = grid_->getRowCount();

  for (auto& group : arch_->getRegions()) {
    int64_t total_site_area = 0;
    if (!group->getCells().empty()) {
      for (GridX x{0}; x < row_site_count; x++) {
        for (GridY y{0}; y < row_count; y++) {
          const Pixel* pixel = grid_->gridPixel(x, y);
          if (pixel->is_valid && pixel->group == group) {
            total_site_area += grid_->rowHeight(y).v * site_width;
          }
        }
      }
    }

    double cell_area = 0;
    for (Node* cell : group->getCells()) {
      cell_area += cell->area();

      for (const auto& rect : group->getRects()) {
        if (isInside(cell, rect)) {
          cell->setRegion(&rect);
        }
      }
      if (cell->getRegion() == nullptr) {
        cell->setRegion(group->getRects().data());
      }
    }
    group->setUtil(total_site_area ? cell_area / total_site_area : 0.0);
  }
}

// Retires every site that straddles a fence-region boundary. A site only
// partly inside a region cannot host a cell that must lie wholly within
// it, and neither the search nor the legality predicate has any way to
// express partial legality, so such sites leave consideration altogether.
//
// They are retired by parking the shared dummy cell on them rather than by
// clearing a flag, because the legality predicate rejects outright any
// pixel that already has an occupant. The dummy is constructed placed and
// fixed, so no later pass tries to move it off. Invalidating the pixel as
// well closes the same door from the other side.
void Opendp::groupInitPixels2()
{
  for (GridX x{0}; x < grid_->getRowSiteCount(); x++) {
    for (GridY y{0}; y < grid_->getRowCount(); y++) {
      const Rect sub(x.v * grid_->getSiteWidth().v,
                     grid_->gridYToDbu(y).v,
                     (x + 1).v * grid_->getSiteWidth().v,
                     grid_->gridYToDbu(y + 1).v);
      Pixel* pixel = grid_->gridPixel(x, y);
      for (auto& group : arch_->getRegions()) {
        for (const Rect& rect : group->getRects()) {
          if (!isInside(sub, rect) && checkOverlap(sub, rect)) {
            pixel->util = 0.0;
            pixel->cell = dummy_cell_.get();
            pixel->is_valid = false;
            debugPrint(logger_,
                       DPL,
                       "group",
                       1,
                       "Block pixel [({}, {}) on region boundary",
                       x.v,
                       y.v);
          }
        }
      }
    }
  }
}

odb::dbInst* Opendp::getAdjacentInstance(odb::dbInst* inst, bool left) const
{
  const Rect inst_rect = inst->getBBox()->getBox();
  DbuX x_dbu = left ? DbuX{inst_rect.xMin() - 1} : DbuX{inst_rect.xMax() + 1};
  x_dbu -= core_.xMin();
  GridX x = grid_->gridX(x_dbu);

  GridY y = grid_->gridSnapDownY(DbuY{inst_rect.yMin() - core_.yMin()});

  Pixel* pixel = grid_->gridPixel(x, y);

  odb::dbInst* adjacent_inst = nullptr;

  // do not return macros, endcaps and tapcells
  if (pixel != nullptr && pixel->cell && pixel->cell->getDbInst()->isCore()) {
    adjacent_inst = pixel->cell->getDbInst();
  }

  return adjacent_inst;
}

std::vector<dbInst*> Opendp::getAdjacentInstancesCluster(dbInst* inst) const
{
  const bool left = true;
  const bool right = false;
  std::vector<odb::dbInst*> adj_inst_cluster;

  odb::dbInst* left_inst = getAdjacentInstance(inst, left);
  while (left_inst != nullptr) {
    adj_inst_cluster.push_back(left_inst);
    // the right instance can be ignored, since it was added in the line above
    left_inst = getAdjacentInstance(left_inst, left);
  }

  std::ranges::reverse(adj_inst_cluster);
  adj_inst_cluster.push_back(inst);

  odb::dbInst* right_inst = getAdjacentInstance(inst, right);
  while (right_inst != nullptr) {
    adj_inst_cluster.push_back(right_inst);
    // the left instance can be ignored, since it was added in the line above
    right_inst = getAdjacentInstance(right_inst, right);
  }

  return adj_inst_cluster;
}

/* static */
bool Opendp::isInside(const Rect& cell, const Rect& box)
{
  return cell.xMin() >= box.xMin() && cell.xMax() <= box.xMax()
         && cell.yMin() >= box.yMin() && cell.yMax() <= box.yMax();
}

bool Opendp::checkOverlap(const Rect& cell, const Rect& box)
{
  return box.xMin() < cell.xMax() && box.xMax() > cell.xMin()
         && box.yMin() < cell.yMax() && box.yMax() > cell.yMin();
}

// Establishes the pixel-level ownership that actually enforces fence
// regions. The site search never consults the region list: it rejects a
// grouped cell whose pixel belongs to a different group, and rejects an
// ungrouped cell on any group-owned pixel. Stamping the group onto pixels
// here is therefore what turns a fence into a constraint instead of an
// annotation.
//
// The pass reuses pixel->util as a coverage accumulator, which is why it
// begins by zeroing that field everywhere. A rectangle adds one unit to
// each pixel it covers, and an edge that does not land on a site boundary
// subtracts back the fraction of the site it leaves outside. For the
// duration of this pass the field means how much of a site its region
// covers, not the occupancy fraction it carries elsewhere.
//
// Classification needs a second sweep over the same rectangles because
// accumulation has to finish first. A pixel shared by two rectangles of
// one region still looks fractional until both have been added, and
// classifying it early would retire a site the region fully covers. Only
// exactly covered pixels take ownership; partly covered ones are retired
// the same way boundary sites are.
void Opendp::groupInitPixels()
{
  for (GridX x{0}; x < grid_->getRowSiteCount(); x++) {
    for (GridY y{0}; y < grid_->getRowCount(); y++) {
      Pixel* pixel = grid_->gridPixel(x, y);
      pixel->util = 0.0;
    }
  }
  for (auto& group : arch_->getRegions()) {
    if (group->getCells().empty()) {
      if (group->getId() != 0) {
        logger_->warn(
            DPL, 42, "No cells found in group {}. ", group->getName());
      }
      continue;
    }
    const DbuX site_width = grid_->getSiteWidth();
    for (const DbuRect rect : group->getRects()) {
      debugPrint(logger_,
                 DPL,
                 "detailed",
                 1,
                 "Group {} region [x{} y{}] [x{} y{}]",
                 group->getName(),
                 rect.xl.v,
                 rect.yl.v,
                 rect.xh.v,
                 rect.yh.v);
      const GridRect grid_rect{grid_->gridWithin(rect)};

      for (GridY k{grid_rect.ylo}; k < grid_rect.yhi; k++) {
        for (GridX l{grid_rect.xlo}; l < grid_rect.xhi; l++) {
          Pixel* pixel = grid_->gridPixel(l, k);
          pixel->util += 1.0;
        }
        if (rect.xl % site_width != 0) {
          Pixel* pixel = grid_->gridPixel(grid_rect.xlo, k);
          pixel->util
              -= (rect.xl % site_width).v / static_cast<double>(site_width.v);
        }
        if (rect.xh % site_width != 0) {
          Pixel* pixel = grid_->gridPixel(grid_rect.xhi - 1, k);
          pixel->util -= ((site_width - rect.xh) % site_width).v
                         / static_cast<double>(site_width.v);
        }
      }
    }
    for (const DbuRect rect : group->getRects()) {
      const GridRect grid_rect{grid_->gridWithin(rect)};

      for (GridY k{grid_rect.ylo}; k < grid_rect.yhi; k++) {
        for (GridX l{grid_rect.xlo}; l < grid_rect.xhi; l++) {
          // Assign group to each pixel.
          Pixel* pixel = grid_->gridPixel(l, k);
          if (pixel->util == 1.0) {
            pixel->group = group;
            pixel->is_valid = true;
            pixel->util = 1.0;
          } else if (pixel->util > 0.0 && pixel->util < 1.0) {
            pixel->cell = dummy_cell_.get();
            pixel->util = 0.0;
            pixel->is_valid = false;
          }
        }
      }
    }
  }
}

odb::Point Opendp::getOdbLocation(const Node* cell) const
{
  odb::dbBox* odb_bbox = cell->getDbInst()->getBBox();
  return {odb_bbox->xMin(), odb_bbox->yMin()};
}

odb::Point Opendp::getDplLocation(const Node* cell) const
{
  DbuX final_x{core_.xMin() + cell->getLeft()};
  DbuY final_y{core_.yMin() + cell->getBottom()};
  return {final_x.v, final_y.v};
}

int divRound(const int dividend, const int divisor)
{
  return round(static_cast<double>(dividend) / divisor);
}

int divCeil(const int dividend, const int divisor)
{
  return ceil(static_cast<double>(dividend) / divisor);
}

int divFloor(const int dividend, const int divisor)
{
  return dividend / divisor;
}

}  // namespace dpl
