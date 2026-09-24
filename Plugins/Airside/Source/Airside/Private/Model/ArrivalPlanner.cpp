#include "Model/ArrivalPlanner.h"

#include "AirsideLog.h"
#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/TrafficOccupancy.h"
#include "Solve/IcaoCode.h"

namespace
{
	/**
	 * Stands exist, and not one of them admits Airframe - size alone, reachable or not.
	 *
	 * EVERY STAND, NOT THE REACHABLE ONES ChooseStand counts: a too-small stand's lead-in
	 * carries its letter's span limit (FAnchorLink's one rule, final review I5), so to a
	 * widebody it is not reachable at all and ChooseStand's own too-small tally never sees it.
	 * The same IsStandCandidate filter and the same StandAdmits rule, so this cannot disagree
	 * with ChooseStand about what a stand is or what fits it - only about whether reach
	 * matters, which for "draw a bigger stand" it does not.
	 */
	bool EveryStandTooSmall(const URoadNetwork& Network, const FAirframe& Airframe)
	{
		int32 Stands = 0;
		for (const FEntityInstance& Stand : Network.GetEntities())
		{
			if (!Stand.IsStandCandidate()) { continue; }
			++Stands;
			if (IcaoCode::StandAdmits(Stand.DesignWingspan, Airframe.Wingspan)) { return false; }
		}
		return Stands > 0;
	}
}

namespace ArrivalPlanner
{
	FGuidelineNodeId ChooseStand(const URoadNetwork& Network, FGuidelineNodeId From,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingAgent,
		FRoutePlan* OutRoute, bool* bOutSawHeld)
	{
		// Every live STAND's pose node - never a depot's, even though a depot has a pose node
		// too (FEntityInstance::IsStandCandidate, the ONE filter UStandAllocator::Reserve also
		// calls; PoseRole captured at placement) - in
		// FEntityInstance ENUMERATION ORDER. The tie-break below ("first minimum wins")
		// depends on Candidates keeping GetEntities()'s own order, exactly as the old
		// per-stand loop implicitly did by walking it directly. CandidateSpan is kept
		// parallel by construction (same filter, same push, same index) rather than
		// recomputed from Candidates afterwards, so the two arrays cannot disagree about
		// which stand is which.
		TArray<FGuidelineNodeId> Candidates;
		TArray<double> CandidateSpan;
		Candidates.Reserve(Network.GetEntities().Num());
		CandidateSpan.Reserve(Network.GetEntities().Num());
		for (const FEntityInstance& Stand : Network.GetEntities())
		{
			if (Stand.IsStandCandidate())
			{
				Candidates.Add(Stand.PoseNode);
				CandidateSpan.Add(Stand.DesignWingspan);
			}
		}

		// ADMISSION AND RANK BOTH COME FROM IcaoCode::StandAdmits/StandRank - the ONE rule
		// UStandAllocator::Reserve also calls, so a legacy stand's captured DesignWingspan
		// and an aircraft's Wingspan are always compared by LETTER, in exactly one place, never
		// against each other as raw doubles here or anywhere else. See those functions' own
		// comments for unknown-admits-anything and the wider-than-F refusal.

		// Built by hand rather than FRouteQuery::For: that factory also takes a Goal, and
		// there isn't ONE here - FindToGoals takes the whole Candidates set instead of a
		// single Query.Goal. See RouteSearch::FindToGoals (#190, deferred from #171/#201):
		// ONE multi-goal search from From replaces the old one-Find()-per-stand loop, which
		// stayed O(exits x stands) searches per dispatch, per re-offer, per plan re-resolve
		// even after #201 made each individual search cheap.
		//
		// THE POLICY STILL COMES FROM THE TABLE, hand-built or not: the errand is set and
		// FRoutePolicy::For resolves it, so this site cannot drift from the one list even
		// though it cannot use the factory.
		FRouteQuery Query;
		Query.Start = From;
		Query.Class = ETraversalClass::Aircraft;
		Query.Wingspan = Airframe.Wingspan;
		Query.Errand = ERouteErrand::ArrivalTaxiIn;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.AvoidRunways = Query.Policy.Avoidance;

		// The wingspan retry Find() pays for on TooWide (RouteSearch::Find's own unconstrained
		// re-run) is NOT reproduced here - see FindToGoals' own comment. This loop never read
		// Result, only IsValid()/Polyline/Steps, so that second search bought it nothing then
		// either.
		TArray<FGoalReach> Reach;
		const FMultiGoalSearch Search = RouteSearch::FindToGoals(Network, Query, Candidates, Reach);

		FGuidelineNodeId Best;
		int32 BestIndex = INDEX_NONE;
		double BestLength = TNumericLimits<double>::Max();
		int32 BestRank = TNumericLimits<int32>::Max();
		bool bSawHeld = false;
		int32 TooSmallCount = 0;
		int32 HeldCount = 0;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (!Reach[Index].bReachable)
			{
				continue;
			}
			// Asked BEFORE Held: size is a property of the ground, not of the traffic on it, so
			// a stand too small for this aircraft is never counted as "held" in the sense that
			// word reports to the player (wait, or build another - see NoFreeStand's wording).
			// IcaoCode::StandAdmits is where "unknown admits anything" and "wider than F is
			// never admitted" both live - see its own comment.
			if (!IcaoCode::StandAdmits(CandidateSpan[Index], Airframe.Wingspan))
			{
				++TooSmallCount;
				continue;
			}
			// Held is asked AFTER reachability (and size), so bSawHeld means "a stand this
			// aircraft could have used" - the only reading under which NoFreeStand is the
			// right word.
			if (Occupancy != nullptr && Occupancy->IsHeld(FTrafficResource::OfNode(Candidates[Index]), ExcludingAgent))
			{
				bSawHeld = true;
				++HeldCount;
				continue;
			}
			// SMALLEST FITTING LETTER FIRST, THEN SHORTEST TAXI: BestRank starts one past F, so
			// the first admitted candidate always wins it outright, exactly as BestLength's
			// TNumericLimits::Max() start did alone before this task. Both comparisons are
			// STRICT, so a later equal rank-and-length never overtakes an earlier one - the
			// same "first minimum wins" contract ChooseStandMultiGoalTieBreakTest pins for
			// length, now the first test applied rather than the only one.
			const int32 Rank = IcaoCode::StandRank(CandidateSpan[Index]);
			if (Rank < BestRank || (Rank == BestRank && Reach[Index].Length < BestLength))
			{
				BestRank = Rank;
				BestLength = Reach[Index].Length;
				Best = Candidates[Index];
				BestIndex = Index;
			}
		}
		// The FULL route (Polyline/Steps) for the ONE winner, backtraced from the SAME search
		// above rather than re-run - see FMultiGoalSearch::BuildPlan. A default FRoutePlan
		// (Result NoStart) when nothing won, matching the old loop's untouched BestRoute.
		if (OutRoute != nullptr) { *OutRoute = Best.IsSet() ? Search.BuildPlan(Network, Best) : FRoutePlan(); }
		if (bOutSawHeld != nullptr) { *bOutSawHeld = bSawHeld; }

		// ONE LINE PER CALL, winner or refusal alike - "ChooseStand: span %.1f m -> node %d
		// (Code %s); %d too small, %d held" is CLAUDE.md's own "pressing 7 does nothing"
		// discipline applied here: a refusal with no line saying WHY (too small vs. held vs.
		// simply unreachable) is what cost the runway-length bug a whole session. Code %s is
		// the WINNING stand's own letter when one was found (a known span - an unmeasured
		// winner reports "?", since 0 has no letter), else the aircraft's OWN letter for
		// context on a refusal (also "?" for an airframe with no known span, or one wider
		// than MaxWingspanForLetter(F) admits - LetterForWingspan has nothing useful to say
		// about either). Reading the letter back off LetterForWingspan rather than the Rank
		// ordinal above keeps this presentation-only - it duplicates no admission decision.
		FString CodeText = TEXT("?");
		if (BestIndex != INDEX_NONE && CandidateSpan[BestIndex] > 0.0)
		{
			CodeText = IcaoCode::LetterForWingspan(CandidateSpan[BestIndex]);
		}
		else if (!Best.IsSet() && Airframe.Wingspan > 0.0
			&& Airframe.Wingspan <= IcaoCode::MaxWingspanForLetter(EIcaoCode::F))
		{
			CodeText = IcaoCode::LetterForWingspan(Airframe.Wingspan);
		}
		UE_LOG(LogAirside, Log,
			TEXT("ChooseStand: span %.1f m -> node %d (Code %s); %d too small, %d held"),
			Airframe.Wingspan / 100.0, Best.Index, *CodeText, TooSmallCount, HeldCount);

		return Best;
	}

	FArrivalPlan Plan(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe,
		const FTrafficOccupancy* Occupancy)
	{
		FArrivalPlan Out;
		Out.AircraftWingspan = Airframe.Wingspan;

		// 1. WHICH RUNWAY. Nearest threshold to the query point, which is the user's own choice
		//    of rule - there is no wind model, so nothing else could decide it.
		if (!Network.NearestRunwayThreshold(Near, Out.End))
		{
			Out.Why = EArrivalRefusal::NoRunway;
			return Out;
		}

		Out.RunwayChain = Network.RunwayChain(Out.End.Seed);

		// 1a. MAY IT USE THIS RUNWAY AT ALL. Surface, approach, published field length and
		//     width, in that order - before occupancy, because occupancy clears on its own
		//     and this never does: M3's sequencer will queue on RunwayOccupied, and it must
		//     not queue an airliner behind a Piper for a grass strip it can never land on.
		Out.Admission = RunwayAdmission::Check(Network, Out.End.Seed, Airframe, true);
		if (!Out.Admission.IsAdmitted())
		{
			Out.Why = EArrivalRefusal::NotAdmitted;
			return Out;
		}

		// Asked before the length and exit steps, because those cannot change while the
		// runway is busy and this can: a refusal that clears on its own is reported as
		// itself, not as whichever later step happened to fail too.
		// No agent of our own to be occupying anything: nothing has been dispatched yet, so
		// bCountOwnOccupied is false - see FTrafficOccupancy::IsAnyHeld.
		if (Occupancy != nullptr && Occupancy->IsAnyHeld(Network.RunwaySurfaces(Out.End.Seed), 0, false))
		{
			Out.Why = EArrivalRefusal::RunwayOccupied;
			return Out;
		}

		// The distance the model actually flies, plus its margin - see FLandingRun. The closed
		// form this replaced demanded 649 m of a 297 m landing and refused every runway on the
		// field, which is what "pressing 7 does nothing" turned out to be.
		Out.Needed = FLandingRun::RequiredLandingDistance(Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach)
			* FLandingRun::LandingMargin;

		// 2. THE EARLIEST EXIT IT COULD TAKE, asked before anything is armed - the same
		//    discipline as a departure refusing a strip it cannot leave.
		//
		//    CALLED ONCE, and ALWAYS - even when the runway is already too short to matter -
		//    so ExitCount is populated on every path DispatchArrival logs from, RunwayTooShort
		//    included (it comes back 0 there: MinDistance Needed exceeds a too-short runway,
		//    so nothing qualifies, which is the right answer to report). DispatchArrival used
		//    to call this twice, the second time with MinDistance 0 purely to log how many
		//    nodes sat on the strip at all versus how many were far enough down to use. That
		//    count served a diagnostic log line, not a decision - Plan makes no decision from
		//    it - so it is dropped rather than paid for on every dispatch; a caller that wants
		//    it back can run the MinDistance-0 query itself, the same cheap filter this used
		//    to duplicate.
		//
		//    From the UNMARGINED distance since the exit arcs (2026-09-06). Needed carries
		//    FLandingRun::LandingMargin, which is a refusal margin on the STRIP - a runway a
		//    quarter shorter than the roll is refused - not a statement about which turn-off
		//    is takeable: the aircraft is at taxi speed by the raw figure (measured: 29632 on
		//    an unbounded strip against 37039 needed), and an arc whose start lay between
		//    the two was skipped for the junction node behind it, which has no turn-off at
		//    all. The player watched the aircraft roll straight past the exit it had built.
		// RunwayExitNodes tests each chain segment against its OWN width (#87) - no HalfWidth
		// measured here, and no risk of a wider runway elsewhere loosening this strip's test.
		// Threshold/Direction are OUR OWN (the end this arrival is actually at), not
		// re-derived from the seed: a seed has two ends and only the caller knows which one
		// is meant (fixed 2026-09-13, see RunwayExitNodes's own comment).
		const double SlowedBy = Out.Needed / FLandingRun::LandingMargin;
		const TArray<FGuidelineNodeId> Exits =
			Network.RunwayExitNodes(Out.End.Seed, Out.End.Threshold, Out.End.Direction, SlowedBy);
		Out.ExitCount = Exits.Num();

		if (Out.End.Length < Out.Needed)
		{
			Out.Why = EArrivalRefusal::RunwayTooShort;
			return Out;
		}
		if (Exits.Num() == 0)
		{
			Out.Why = EArrivalRefusal::NoExit;
			return Out;
		}

		// 3. WHICH STAND. Shortest route, the user's rule - and taken from the FIRST exit that
		//    reaches anything, because an aircraft takes the earliest turn-off it can rather
		//    than rolling to the end in search of a marginally shorter taxi.
		//
		//    AN EXIT IS WHERE THE AIRCRAFT LEAVES THE RUNWAY, so the taxi-in is searched with
		//    the runway's own edges off limits - the rule the deadlock replan already lives
		//    by. Without it a junction's own node-end, which has no turn-off since the exit
		//    arcs (the turns attach at the split nodes either side), "exited" by rolling on
		//    to the downstream split and hairpinning back - the aircraft the player saw roll
		//    straight past its exit - and an early exit lost to a later one because the
		//    SHORTEST route from it ran down the strip. With the strip excluded a node-end
		//    has no route at all and the earliest arc wins on its taxiways, as the rule says.
		//    A FORWARD turn-off (the first span of the route heading down the runway) beats
		//    a backtrack at any distance: an aircraft turns off ahead of itself if it can.
		FGuidelineNodeId FirstForward, FirstBacktrack;
		bool bSawHeldStand = false;
		FRoutePlan ForwardRoute, BacktrackRoute;
		int32 ForwardOrdinal = 0, BacktrackOrdinal = 0;
		for (int32 Index = 0; Index < Exits.Num() && !FirstForward.IsSet(); ++Index)
		{
			const FGuidelineNodeId& Candidate = Exits[Index];
			// The stand choice itself is ChooseStand's - one rule for the dispatch, the rebuild
			// and the re-offer - asked here per exit with this aircraft excluded from nothing
			// (0: it does not exist yet).
			FRoutePlan BestForExit;
			bool bHeldHere = false;
			ChooseStand(Network, Candidate, Airframe, Occupancy, 0, &BestForExit, &bHeldHere);
			bSawHeldStand = bSawHeldStand || bHeldHere;
			if (!BestForExit.IsValid())
			{
				continue;
			}
			const bool bForward =
				FVector2D::DotProduct(BestForExit.Polyline[1] - BestForExit.Polyline[0], Out.End.Direction) > 0.0;
			if (bForward)
			{
				FirstForward = Candidate;
				ForwardRoute = BestForExit;
				ForwardOrdinal = Index + 1;
			}
			else if (!FirstBacktrack.IsSet())
			{
				FirstBacktrack = Candidate;
				BacktrackRoute = BestForExit;
				BacktrackOrdinal = Index + 1;
			}
		}
		if (FirstForward.IsSet())
		{
			Out.TaxiIn = ForwardRoute;
			Out.Exit = FirstForward;
			Out.ExitOrdinal = ForwardOrdinal;
		}
		else if (FirstBacktrack.IsSet())
		{
			Out.TaxiIn = BacktrackRoute;
			Out.Exit = FirstBacktrack;
			Out.ExitOrdinal = BacktrackOrdinal;
		}

		if (!Out.TaxiIn.IsValid())
		{
			// Three refusals for three fixes: every stand too small means draw a bigger one; no
			// stand reachable at all means build a taxiway; every reachable stand held means
			// wait, or build a stand. SIZE FIRST, because it is a fact about the ground that no
			// taxiway or waiting changes - and it used to fall through to NoRouteToStand, sending
			// the player to build a taxiway that already reached every stand (final review I6).
			// Held cannot also be true then: a stand is only counted held once it has admitted.
			if (EveryStandTooSmall(Network, Airframe))
			{
				Out.Why = EArrivalRefusal::NoStandBigEnough;
				return Out;
			}
			Out.Why = bSawHeldStand ? EArrivalRefusal::NoFreeStand : EArrivalRefusal::NoRouteToStand;
			return Out;
		}

		// WHERE IT LEAVES THE RUNWAY, handed to the landing so the rollout carries on to the
		// taxiway at taxi speed instead of stopping wherever the braking ran out.
		Out.VacateAt = Out.End.Length;
		if (const FGuidelineNode* ExitNode = Network.GetGuidelineNode(Out.Exit))
		{
			Out.VacateAt = Out.End.OffsetOf(ExitNode->Position);
		}

		Out.Why = EArrivalRefusal::None;
		return Out;
	}

	FString DescribeRefusal(EArrivalRefusal Why, double AircraftWingspan)
	{
		// The figure-free wording, for a listener that has the reason and not the plan. The
		// plan overload below defers to this wherever it has no figures to add, so one
		// refusal cannot end up with two different sentences.
		switch (Why)
		{
		case EArrivalRefusal::NoRunway:
			return TEXT("No runway to land on - draw one first.");

		case EArrivalRefusal::RunwayTooShort:
			return TEXT("Arrival refused: the runway is too short for this aircraft to stop.");

		case EArrivalRefusal::NoExit:
			return TEXT("Arrival refused: nothing joins the runway far enough down to be an exit.");

		case EArrivalRefusal::NoRouteToStand:
			return TEXT("Arrival refused: no route from any usable exit to a stand.");

		case EArrivalRefusal::RunwayOccupied:
			return TEXT("Arrival refused: the runway is in use. Wait for it to clear.");

		case EArrivalRefusal::NoFreeStand:
			return TEXT("Arrival refused: every stand it could reach is taken. Wait for one to free, or build another.");

		case EArrivalRefusal::NotAdmitted:
			return TEXT("Arrival refused: this aircraft is not admitted to that runway.");

		case EArrivalRefusal::NoStandBigEnough:
			// THE LETTER IS THE LEVER - the player draws stands by letter, so "too small" alone
			// leaves them guessing how big. Named only for a span the table can name: wider
			// than Code F is a span no stand can ever be drawn for, and saying "Code F" there
			// would send them to build one that still refuses it.
			if (AircraftWingspan > IcaoCode::MaxWingspanForLetter(EIcaoCode::F))
			{
				return FString::Printf(
					TEXT("Arrival refused: a %.1f m wingspan is wider than any stand can be built for."),
					AircraftWingspan / 100.0);
			}
			if (AircraftWingspan > 0.0)
			{
				return FString::Printf(
					TEXT("Arrival refused: this aircraft needs a Code %s stand, and none on the field is ")
					TEXT("big enough. Draw a bigger stand."),
					*IcaoCode::LetterForWingspan(AircraftWingspan));
			}
			return TEXT("Arrival refused: every stand is too small for this aircraft. Draw a bigger stand.");

		case EArrivalRefusal::None:
		default:
			return FString();
		}
	}

	FString DescribeRefusal(const FArrivalPlan& Plan)
	{
		// Exactly the three refusal branches DispatchArrival used to choose between inline,
		// moved here so the actor logs from the plan it acted on rather than re-deriving why.
		//
		// Only the branches that have FIGURES are spelled out here; the rest defer to the
		// reason-only overload above, which is the one source for that wording.
		switch (Plan.Why)
		{
		case EArrivalRefusal::RunwayTooShort:
			return FString::Printf(
				TEXT("Arrival refused: the runway is %.0f uu and this aircraft needs %.0f to ")
				TEXT("stop. Draw a longer runway."),
				Plan.End.Length, Plan.Needed);

		case EArrivalRefusal::NoExit:
			return FString::Printf(
				TEXT("Arrival refused: nothing joins the runway beyond %.0f uu, so there is ")
				TEXT("no exit this aircraft could take. Connect a taxiway further down it."),
				Plan.Needed);

		case EArrivalRefusal::NoRouteToStand:
			return FString::Printf(
				TEXT("Arrival refused: %d usable exit(s), but no route from any of them to a ")
				TEXT("stand. Check the taxiway reaches the stands."),
				Plan.ExitCount);

		case EArrivalRefusal::NotAdmitted:
			return FString::Printf(TEXT("Arrival refused: %s."), *RunwayAdmission::Describe(Plan.Admission));

		default:
			// NoRunway, RunwayOccupied, NoFreeStand and None carry no figures, so their
			// wording is the reason-only overload's and is not repeated here. NoStandBigEnough
			// is worded there too, handed the plan's own span so it can name the letter.
			return DescribeRefusal(Plan.Why, Plan.AircraftWingspan);
		}
	}
}
