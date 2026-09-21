#pragma once

#include "CoreMinimal.h"

/**
 * What survives of the plot's own bay-grid fit, after issue #182 retired the rest of it.
 *
 * Dependency-free, like every other Solve/ header: CoreMinimal.h and nothing else.
 *
 * FitBays, FPlotFit, FPlotBay, EPlotRefusal and this file's own file-static Contains lived
 * here until #182: a 4 m x 12 m bay grid, with its OWN winding-number point-in-polygon test,
 * that URoadEditFacade::PlaceEntityInPlot still judged a commit against while the tool's
 * ghost and readout judged the SAME plot against PlotYard::Reserve - two evaluators, with two
 * different answers for exactly the self-touching outlines this file's old Contains comment
 * said it existed to get right, and the facade discarded FitBays's own refusal besides
 * (PlaceEntityInPlot's return went unchecked in FPlotPlaceTool::OnCommit). See that spec's
 * section 8, which had already named the truncation half of this as a known second opinion:
 * docs/superpowers/specs/2026-09-16-four-point-plot-gesture-design.md.
 *
 * URoadEditFacade::PlaceEntityInPlot now runs PlotLayoutFor(Layout)->Solve(Site, Specs) - the
 * IDENTICAL call FPlotPlaceTool::ReservationFor makes for the preview - so there is one
 * evaluator, not two that might disagree. Its point-in-polygon test moved to
 * RoadGeom::PointInPolygon, which switched from crossing number to winding number for the
 * same reason this file's old Contains gave: see RoadGeom::PointInPolygon's own comment.
 *
 * DELETED RATHER THAN LEFT, the same ruling this file made once already for the grid of slots
 * that preceded FitBays (see git history if that comment is wanted verbatim) - a solver
 * nothing calls is a thing the next reader has to disprove the importance of before they can
 * change anything near it. FPlotFitBaysTest and FPlotFitFacesAwayFromRoadTest moved with it:
 * the boundary-exact, notched and either-winding cases they pinned are PlotYard's own
 * contract now (Airside.Solve.PlotYard*), proved through RoadGeom::PointInPolygon rather than
 * a second Contains that could quietly stop agreeing with it.
 */
namespace PlotFit
{
	/**
	 * How far a corner probe is pulled in from its own corner before asking whether it is
	 * inside the plot, uu. 1 cm.
	 *
	 * STILL LIVE: PlotYard::StandCorners insets every stand corner by exactly this before
	 * PlotYard and RoadGeom::PointInPolygon judge it, for the reason FitBays needed it too -
	 * NOT A TOLERANCE FUDGE. A plot exactly the width of what stands in it puts a corner
	 * exactly ON the outline, and a containment test is undefined on the boundary: it answers
	 * by floating-point coin flip, differently on another machine. Probing just inside asks
	 * the question that was actually meant - "is this corner within the plot" - rather than
	 * "is this point on its edge".
	 *
	 * KEPT IN THIS NAMESPACE rather than moved to PlotYard, which is its only remaining
	 * consumer: PlotYard::StandCorners's own doc comment already points here, and repointing
	 * every one of those citations for a constant's sake would be the tidying-for-taste
	 * CLAUDE.md asks this kind of change to leave alone.
	 */
	inline constexpr double CornerInsetUu = 1.0;
}
