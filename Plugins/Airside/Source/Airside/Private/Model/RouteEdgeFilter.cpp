#include "Model/RouteEdgeFilter.h"

#include "Model/Pavement.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"

// MOVED FROM RouteSearch.cpp's ExpandNode on 2026-10-02 with every comment, in the same order of tests - see
// FRouteEdgeFilter's header comment for why the filter is shared.

namespace
{
	/**
	 * See RouteSearch::RunwaySeedResolveCountForTest. Bumped once per seed actually resolved -
	 * a memo hit does not count, which is what makes the count measure the memo rather than
	 * the traffic through it.
	 */
	int32 GRunwaySeedResolveCountForTest = 0;

	/**
	 * The strongest pavement any road or taxiway profile may offer - the ceiling the taxiing
	 * gate clamps an aircraft's need to (see the gate). Tarmac on 2026-09-27: every road and
	 * taxiway profile carries { Tarmac, Grass }. A profile that one day offers concrete raises
	 * this with it, or a concrete taxiway would be judged as tarmac.
	 * ENFORCED BY: Airside.Content.RoadProfilesOfferTarmacAndGrass (goes red if any road or
	 * taxiway profile offers more), Airside.Model.RoutePavementGate
	 */
	constexpr EPavement TaxiwayPavementCeiling = EPavement::Tarmac;

	/**
	 * Is this edge too narrow for the query's wingspan?
	 *
	 * MaxWingspan of 0 means UNLIMITED, so this is not a plain greater-than. Written once,
	 * here, because the same test read backwards routes a widebody onto a link built for a
	 * regional jet - and it would still find a route, which is the failure that never
	 * reports itself.
	 */
	bool ExceedsWingspan(const FGuidelineEdge& Edge, double Wingspan)
	{
		return Edge.MaxWingspan > 0.0 && Wingspan > Edge.MaxWingspan;
	}
}

namespace RouteSearch
{
	int32 RunwaySeedResolveCountForTest() { return GRunwaySeedResolveCountForTest; }
	void ResetRunwaySeedResolveCountForTest() { GRunwaySeedResolveCountForTest = 0; }
}

FRouteEdgeFilter::FRouteEdgeFilter(const URoadNetwork& InNetwork, const FRouteQuery& InQuery, bool bInIgnoreSize,
	const TSet<FGuidelineEdgeId>* InExcluded, TMap<FGuidelineEdgeId, bool>* InFitMemo)
	: Network(InNetwork)
	, Query(InQuery)
	, bIgnoreSize(bInIgnoreSize)
	, Excluded(InExcluded)
	, FitMemo(InFitMemo)
{
}

bool FRouteEdgeFilter::IsRunwayEdge(FRoadSegmentId Seed)
{
	// Whether an edge's source segment IS a runway, once per segment seen. A slot lookup
	// plus a profile resolve, which the avoidance test below used to pay on EVERY relaxation
	// of every edge - a node is relaxed several times before Closed catches it, so a long
	// strip paid for the same unchanged answer again and again. This is #171's complaint
	// about EdgeCost's re-sampling, in the one place that survived it.
	//
	// PER SEARCH, NOT CACHED ON THE EDGE: runway-ness depends on the PROFILE, which changes
	// when a road is re-profiled - an open invalidation trigger with no writer positioned to
	// catch it, unlike FGuidelineEdge::Length's closed set of three writers. A stale true
	// would refuse taxiways for the rest of the session.
	if (!Seed.IsSet())
	{
		return false;
	}
	if (const bool* Known = RunwaySeeds.Find(Seed.Index))
	{
		return *Known;
	}
	++GRunwaySeedResolveCountForTest;
	const bool bRunway = Network.IsRunwaySegment(Seed);
	RunwaySeeds.Add(Seed.Index, bRunway);
	return bRunway;
}

bool FRouteEdgeFilter::IsRunwayHeld(FRoadSegmentId Seed)
{
	// Whether a runway's chain is in use - held by somebody other than the querier, or
	// occupied by the querier's own body (ERunwayAvoidance::Held says why both) - once
	// per runway segment seen: the chain walk and the table scan are not free, and every
	// edge along a long strip would otherwise pay for both.
	if (const bool* Known = RunwayInUse.Find(Seed.Index))
	{
		return *Known;
	}
	// bCountOwnOccupied true: ERunwayAvoidance::Held's own comment (FRouteQuery)
	// says why the querier's OWN occupied claim counts too.
	const bool bHeld = Query.Occupancy != nullptr
		&& Query.Occupancy->IsAnyHeld(Network.RunwaySurfaces(Seed), Query.QueryingAgent, /*bCountOwnOccupied*/ true);
	RunwayInUse.Add(Seed.Index, bHeld);
	return bHeld;
}

bool FRouteEdgeFilter::VehicleFits(FGuidelineEdgeId EdgeId, const FGuidelineEdge& Edge)
{
	// MEMOISED PER Find (review of aa90eec2): VehicleFit::Fits traces the vehicle round the
	// curve (VehicleSweep::Trace), and an edge is relaxed from every node that reaches it,
	// in every search one Find runs - the constrained pass, each tow retry. The answer
	// depends on the edge and the vehicle alone, both fixed for the Find. Measured
	// 2026-09-25: the rig course's cold plan spent ~1.9 s of 2.4 s re-tracing edges.
	if (FitMemo != nullptr)
	{
		if (const bool* Known = FitMemo->Find(EdgeId))
		{
			return *Known;
		}
	}
	const bool bFits = VehicleFit::Fits(Edge, *Query.Vehicle, Network);
	if (FitMemo != nullptr)
	{
		FitMemo->Add(EdgeId, bFits);
	}
	return bFits;
}

void FRouteEdgeFilter::ForEachAdmitted(FGuidelineNodeId At, TFunctionRef<void(const FAdmittedEdge&)> Visit)
{
	// Traffic class and one-way direction are already applied here - this is the
	// network's own answer to "what may leave this node", so the search never
	// re-implements the rule and cannot drift from it.
	//
	// ForEachOutgoingGuideline, not GetOutgoingGuidelines (#171): the array
	// GetOutgoingGuidelines built was a fresh TArray thrown away at the end of every
	// one of these node expansions, and a route search over a real airport expands
	// many nodes. Each `continue` below becomes a `return` from this visitor - the
	// same "skip this edge" the loop meant, since there is no outer loop left to
	// continue.
	Network.ForEachOutgoingGuideline(At, Query.Class, [&](FGuidelineEdgeId EdgeId)
	{
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
		if (Edge == nullptr || Edge->A == Edge->B)
		{
			return;
		}

		if (Query.BannedEdge.IsSet() && EdgeId == Query.BannedEdge)
		{
			return;
		}

		// A TOW'S FOLD EXCLUSIONS, Find's own and per call - NOT BannedEdge, which is the
		// deadlock resolver's one edge and must survive a tow retry untouched. A set, because
		// each retry adds one and the earlier ones must stay out.
		if (Excluded != nullptr && Excluded->Contains(EdgeId))
		{
			return;
		}

		// A banned NODE bans every edge INTO it, whichever arm - the deadlock replan's
		// blocker is an aircraft standing on the node, and an edge-only ban lets the
		// search re-enter round the back. See FRouteQuery::BannedNode.
		if (Query.BannedNode.IsSet()
			&& ((Edge->B == At ? Edge->A : Edge->B) == Query.BannedNode))
		{
			return;
		}

		// GROUND TOO WEAK for the traveller, by the SAME comparison runway and stand admission
		// use (FPavementCheck) - not a grass test: #356 gated grass only, which would have
		// passed a jet needing concrete down a tarmac taxiway. Asked only when the query needs
		// more than grass, so a vehicle's or a grass-capable aircraft's search never pays the
		// lookup. A turn path has no DerivedFrom and is not judged; the lanes either side of
		// it are. RUNWAYS ARE NOT JUDGED HERE (!IsRunwaySegment) - a strip's surface is
		// RunwayAdmission's. See FRouteQuery::MinimumPavement on why Find's size retry does
		// not lift this.
		//
		// THE NEED IS CLAMPED TO WHAT A TAXIWAY CAN OFFER (R12, final review), TaxiwayPavementCeiling:
		// real heavies taxi on asphalt, and RoadProfile.h's AllowedPavements says why no road
		// or taxiway offers concrete - nothing rolls on one that grass and tarmac do not
		// already tell apart. Unclamped, a Concrete-needing type could never reach a stand
		// and was told to "build a taxiway" that no tool can build. Runway and stand
		// admission keep the full need; only taxiing is clamped.
		if (Query.MinimumPavement > EPavement::Grass && Edge->DerivedFrom.IsSet()
			&& !Network.IsRunwaySegment(Edge->DerivedFrom)
			&& !Pavement::Judge(Network.PavementOf(Edge->DerivedFrom),
				FMath::Min(Query.MinimumPavement, TaxiwayPavementCeiling)).Passes())
		{
			return;
		}

		// ONE ANSWER, TWO READERS: the filter just below and the cost term inside
		// EdgeCost. Asking the memo twice would be cheap, but reading it once is what
		// guarantees the edge the filter judged is the edge the cost charged for.
		const bool bRunwayEdge = IsRunwayEdge(Edge->DerivedFrom);

		// Runway-derived edges are the strip itself. See ERunwayAvoidance for who may
		// taxi along one and when.
		if (Query.AvoidRunways != ERunwayAvoidance::None
			&& bRunwayEdge
			&& (Query.AvoidRunways == ERunwayAvoidance::All || IsRunwayHeld(Edge->DerivedFrom)))
		{
			return;
		}

		// SIZE: an aircraft's wingspan, and since 2026-09-23 a vehicle's body (VehicleFit).
		// One flag for both, because Find's unconstrained retry lifts both to tell "too big"
		// from "not connected".
		if (!bIgnoreSize && (ExceedsWingspan(*Edge, Query.Wingspan)
			|| (Query.Vehicle != nullptr && !VehicleFits(EdgeId, *Edge))))
		{
			return;
		}

		FAdmittedEdge Admitted;
		Admitted.Id = EdgeId;
		Admitted.Edge = Edge;
		Admitted.bReversed = (Edge->B == At);
		Admitted.Next = Admitted.bReversed ? Edge->A : Edge->B;
		Admitted.bRunwayEdge = bRunwayEdge;
		Visit(Admitted);
	});
}
