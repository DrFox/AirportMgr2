#pragma once

#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadNetworkSolver.h"
#include "Solve/PlotYard.h"

class URoadNetwork;
class URoadProfile;
struct FRoadDesignVehicles;

/**
 * THE ROUTING GRAPH'S DERIVATION, ONE FUNCTION (#438): the default profile, the solve, the taxiway
 * restriction, the guideline graph, the anchor links, the depot census and the Derived stamp, in
 * that order, from the road the player drew.
 *
 * WHY HERE AND NOT IN THE PRESENTER, where it lived until #438: URoadSurfacePresenter::RebuildInternal
 * owned this sequence outright, behind its render-component check, so everything that needed the
 * graph - traffic, the ops load, the tests - either went through a mesh rebuild or re-typed the
 * sequence. TestGraph, the upgrade tool's what-if and three ops tests each did, and each drifted
 * (closed #101 and #311 were both "fixed" by a test-side copy production never ran; the strip
 * restriction and #324's &Solved each had to be added twice). CLAUDE.md gives Build/ the job of
 * deriving the guideline graph; the presenter now calls this and meshes from what it returns.
 *
 * A PASS ADDED TO THE DERIVATION GOES IN Derive, and every door reaches it: the presenter, TestGraph,
 * the what-if and the ops tests. Its production callers are pinned by Check-Architecture's
 * allowed-callers rows for FRoadGuidelineBuilder::Build, FAnchorLink::Build,
 * TaxiwayRestriction::Apply, MarkGuidelinesDerived and FRoadNetworkSolver::SolveAll.
 * ENFORCED BY: Airside.Build.Derivation.TestGraphMatchesTheActor, Check-Architecture rule 4
 */
namespace AirsideDerivation
{
	/**
	 * HOW MUCH OF THE ONE SEQUENCE A CALLER NEEDS - never a set of skip-this bools, which would let a
	 * caller ask for the links without the graph they join. Each scope is a ROW of ONE table in the
	 * .cpp (DerivationPassesOf: solve, restriction, guidelines, links and census, stamp), read there
	 * rather than redrawn here, where a second copy could drift; every pass runs in Full's order
	 * whichever rows include it. Each value's own comment says who asks for it and why.
	 * ENFORCED BY: Airside.Build.Derivation.ScopeTable
	 */
	enum class EDeriveScope : uint8
	{
		/** Every pass - an EChangeKind::Topology rebuild, and TestGraph::Rebuild. */
		Full,

		/**
		 * The default profile and the solve only, READING the bends' widening the last Full traced
		 * (EWideningTrace::ReadCached) - an EChangeKind::Geometry rebuild, a drag frame. A drag moved
		 * positions and nothing else, so the graph's SHAPE is what it was; re-deriving it every frame
		 * is the cost issue #165 is about. No stamp, deliberately: the graph IS behind the road for the
		 * drag's duration, and URoadNetwork::AreGuidelinesBehindRoad saying so is what stops the
		 * planners searching lines the player has already moved.
		 */
		Surface,

		/**
		 * Full without the links and the census: the solve, the restriction, the guideline graph and
		 * the stamp. TestGraph::Derive - a fixture that places or links its entities itself, after.
		 */
		Graph,

		/**
		 * Full's passes AFTER the guideline graph, over the graph as it stands: the anchor links, the
		 * census and the stamp. No solve, so no turn path to re-measure (FAnchorLink::Build gets no
		 * Solved). TestGraph::Link - a fixture whose guideline graph is hand-laid, the ops tests'.
		 */
		Links,

		/**
		 * The facts the road alone determines, which admission and the inspector read: the taxiway
		 * restriction today. QUIET - asked of a copy (the upgrade hover's ghost network), which is
		 * asked what WOULD happen; a `Restriction:` line from it would describe a road nobody built.
		 * No solve and no graph: nothing a what-if asks reads either, and the guideline builder has
		 * no quiet mode to run it in.
		 */
		Facts,

		Count
	};

	/**
	 * What the derivation reads beyond the network, RESOLVED BY THE CALLER - this layer decides no
	 * content default (Check-Architecture's Build->Content rule): the presenter passes
	 * ARoadNetworkActor::MakeSurfaceSettings' answers, TestGraph the content set's. A VIEW, not a
	 * copy: DesignVehicles and DepotKits point at the caller's own resolved values, which outlive
	 * the call; a copy per drag frame would be a copy of a table nothing changed.
	 */
	struct FDeriveInputs
	{
		EDeriveScope Scope = EDeriveScope::Full;

		/**
		 * Written to URoadNetwork::DefaultProfile before the solve, by every scope that solves - see
		 * Derive for why. Null writes null, as the presenter always did; a caller with no profile of
		 * its own to hand (TestGraph) passes the network's current one.
		 */
		URoadProfile* DefaultProfile = nullptr;

		/**
		 * What each road's turns are sized for - see URoadSurfacePresenter::FSurfaceSettings::
		 * DesignVehicles, which the presenter points this at. REQUIRED by every scope but Facts:
		 * Derive refuses, with an Error, rather than size fillets for a vehicle nobody chose.
		 */
		const FRoadDesignVehicles* DesignVehicles = nullptr;

		/** How far a SERVICE link may reach - see ARoadNetworkActor::ServiceLinkRadius. */
		double ServiceLinkRadius = FAnchorLink::DefaultServiceLinkRadius;

		/**
		 * The depot kit table the census seats a depot's modules against (ARoadNetworkActor::
		 * ResolveDepotKits, through FSurfaceSettings::DepotKits). Empty: no plot to seat against, and
		 * the owned list stands - see DepotKit::ReportIncomplete.
		 */
		TArrayView<const PlotYard::FKitSpec> DepotKits;
	};

	/**
	 * Runs Inputs.Scope's passes over Network, in order, and returns the solve they used - empty for a
	 * scope that does not solve (Links, Facts). Refuses a scope that needs Inputs.DesignVehicles
	 * without one: logs an Error, derives nothing, returns an empty result.
	 * ENFORCED BY: Airside.Build.Derivation.ScopeTable
	 */
	AIRSIDE_API FRoadSolveResult Derive(URoadNetwork& Network, const FDeriveInputs& Inputs);
}
