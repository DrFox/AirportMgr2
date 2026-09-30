#include "Build/AirsideDerivation.h"

#include "AirsideLog.h"
#include "Build/AnchorLink.h"
#include "Build/DepotKit.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiwayRestriction.h"
#include "Profiles/RoadDesignVehicles.h"

namespace
{
	/** Which of the sequence's passes one scope runs - a row of DerivationPassesOf below. */
	struct FDerivationPasses
	{
		/** The default profile, then FRoadNetworkSolver::SolveAll. */
		bool bSolve;
		/** TaxiwayRestriction::Apply. */
		bool bFacts;
		/** FRoadGuidelineBuilder::Build. */
		bool bGraph;
		/** FAnchorLink::Build, then DepotKit::ReportIncomplete. */
		bool bLinks;
		/** URoadNetwork::MarkGuidelinesDerived. */
		bool bStamp;
	};

	/**
	 * THE TABLE, ONE ROW PER EDeriveScope, in the enum's order - the only copy; the header points
	 * here. One table rather than a predicate per pass, so a new scope is one row a reviewer reads
	 * across, and a new pass one column every row must answer.
	 * ENFORCED BY: the static_assert below (a row per scope), Airside.Build.Derivation.ScopeTable
	 * (each row's passes run and no others)
	 */
	constexpr FDerivationPasses DerivationPassesOf[] = {
		/* Full    */ { true,  true,  true,  true,  true  },
		/* Surface */ { true,  false, false, false, false },
		/* Graph   */ { true,  true,  true,  false, true  },
		/* Links   */ { false, false, false, true,  true  },
		/* Facts   */ { false, true,  false, false, false },
	};
	static_assert(UE_ARRAY_COUNT(DerivationPassesOf) == static_cast<int32>(AirsideDerivation::EDeriveScope::Count),
		"one DerivationPassesOf row per EDeriveScope - a scope with no row would read past the table");

	const TCHAR* DerivationScopeName(AirsideDerivation::EDeriveScope Scope)
	{
		using AirsideDerivation::EDeriveScope;
		switch (Scope)
		{
		case EDeriveScope::Full:    return TEXT("Full");
		case EDeriveScope::Surface: return TEXT("Surface");
		case EDeriveScope::Graph:   return TEXT("Graph");
		case EDeriveScope::Links:   return TEXT("Links");
		case EDeriveScope::Facts:   return TEXT("Facts");
		default:                    return TEXT("?");
		}
	}
}

FRoadSolveResult AirsideDerivation::Derive(URoadNetwork& Network, const FDeriveInputs& Inputs)
{
	// EDeriveScope::Count is the table's size, not a scope - refused rather than read past the end.
	const int32 Row = static_cast<int32>(Inputs.Scope);
	if (Row >= static_cast<int32>(UE_ARRAY_COUNT(DerivationPassesOf)))
	{
		UE_LOG(LogAirside, Error, TEXT("AirsideDerivation::Derive: scope %d is not a row of the table - nothing derived"), Row);
		return FRoadSolveResult();
	}
	const FDerivationPasses& Passes = DerivationPassesOf[Row];

	// REFUSED, NOT GUESSED: a solve without design vehicles would size every fillet for whatever
	// each profile resolved alone, and the builder and the linker take them by reference - there is
	// no honest default to fall back to at this layer (FSurfaceSettings::DesignVehicles' own comment:
	// a caller that forgets gets a failure, not a plausible-looking wrong radius).
	if (Inputs.DesignVehicles == nullptr && (Passes.bSolve || Passes.bGraph || Passes.bLinks))
	{
		UE_LOG(LogAirside, Error, TEXT("AirsideDerivation::Derive(%s): no design vehicles given - nothing derived"),
			DerivationScopeName(Inputs.Scope));
		return FRoadSolveResult();
	}

	FRoadSolveResult Solved;
	if (Passes.bSolve)
	{
		// BEFORE the solve, because the solver reads it. Segments made in this session already
		// carry this profile; segments reloaded from a saved level carry null, because the
		// profile they were given lived in the transient package and never survived the save.
		// Handing it to the network repairs both cases through one accessor - see
		// URoadNetwork::ProfileFor, and Airside.Build.ProfileFallback for what it is worth.
		// NULL KEEPS the network's own - see FDeriveInputs::DefaultProfile for why it no longer writes null.
		if (Inputs.DefaultProfile != nullptr)
		{
			Network.DefaultProfile = Inputs.DefaultProfile;
		}

		// RESOLVED ONCE, BY THE CALLER, AND PASSED DOWN (issue #190) - see FSurfaceSettings::
		// DesignVehicles' own comment. SolveAll's BuildNodeInput asks a profile's
		// ResolvedFilletRadius per arm of every node it visits; without this, that ran
		// UAirsideSettings::ResolveLargestServiceVehicle fresh each time, on every Geometry
		// rebuild a drag frame produces as well as every Topology one.
		// A TOPOLOGY REBUILD TRACES the bends' widening (EWideningTrace); a drag's Geometry frame
		// reads what the last one traced, so a drag never drives a vehicle per frame.
		Solved = FRoadNetworkSolver::SolveAll(Network, 12, Inputs.DesignVehicles,
			Inputs.Scope == EDeriveScope::Surface ? EWideningTrace::ReadCached : EWideningTrace::Trace);
	}

	// THE RESTRICTION PASS BEFORE THE BUILDER (strip stage 6): the builder writes every taxiway
	// edge's MaxWingspan from its EFFECTIVE letter, which reads the RestrictedLetter this writes -
	// run after, it would route on the previous edit's restriction. After SolveAll, which it
	// does not read, so the order against the solve is free. Quiet only for Facts, the what-if on a
	// copy - see EDeriveScope::Facts.
	if (Passes.bFacts)
	{
		TaxiwayRestriction::Apply(Network, /*bLog=*/Inputs.Scope != EDeriveScope::Facts);
	}

	if (Passes.bGraph)
	{
		// The guideline graph is derived from the same solve, and until this call existed it
		// was derived NOWHERE outside the tests - so every route query at runtime ran against
		// an empty graph and correctly reported that nothing was connected.
		//
		// Anchor lead-ins go second and must: they join stands to guidelines that only exist
		// once the line below has run, and both are swept and rebuilt together. Both take the
		// SAME resolved vehicle SolveAll just used, rather than resolving their own (#190).
		FRoadGuidelineBuilder::Build(Network, Solved, *Inputs.DesignVehicles);
	}

	if (Passes.bLinks)
	{
		// THE SERVICE RADIUS COMES DOWN FROM THE LEVEL - see ARoadNetworkActor::ServiceLinkRadius.
		// The aircraft cap keeps FAnchorLink's own default beside it, deliberately: one is
		// per-airport gameplay tuning and the other is a fact about a painted line.
		// THE DEFAULT, NOT PER TIER: a stand or depot link is driven by the rigid trucks that
		// service stands and live in depots. The rig has no stand or depot to go to yet (spec
		// §"Out of this step", step 3); sizing every link's lane radius for it would widen every
		// yard approach for a vehicle that never uses one.
		// SOLVED, PASSED DOWN (issue #324): the same solve FRoadGuidelineBuilder::Build just
		// derived the turn paths from, so Join can re-measure a piece it splits off one
		// instead of leaving it unmeasured for every rebuild's lifetime (#288's own gap). None
		// for Links, which did not solve: a hand-laid graph has no turn path the solve measured.
		FAnchorLink::Build(Network, Inputs.DesignVehicles->Default.Chassis, FAnchorLink::DefaultMaxLeadIn,
			Inputs.ServiceLinkRadius, Passes.bSolve ? &Solved : nullptr);

		// THE FUEL-DEPOT MODULE CENSUS (#306): moved out of FAnchorLink::Build, which a depot's
		// missing shed or pump has nothing to do with, and run from here instead - the same
		// Topology census the anchor links above are part of, and for the same reason: it must
		// say so again after every edit, not just once at placement (DepotKit::ReportIncomplete's
		// own header comment).
		DepotKit::ReportIncomplete(Network, Inputs.DepotKits);
	}

	// LAST, after every mutation above: the graph now matches the road as of this revision, and
	// the planners may search it again - see URoadNetwork::AreGuidelinesBehindRoad. THE DERIVATION'S
	// STAMP, not the builder's (#438): it moved here from the end of FRoadGuidelineBuilder::Build,
	// so "derived" means every pass this scope runs finished, links included, rather than one pass.
	// A SCOPE THAT DERIVES NO GRAPH (Links) STAMPS ONLY ONE THAT WAS DERIVED (#472 review): a hand-laid
	// graph never derived stays never-derived - see URoadNetwork::WereGuidelinesEverDerived.
	if (Passes.bStamp && (Passes.bGraph || Network.WereGuidelinesEverDerived()))
	{
		Network.MarkGuidelinesDerived();
	}
	return Solved;
}
