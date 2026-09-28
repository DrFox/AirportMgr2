#include "Model/ArrivalPlanner.h"

#include "AirsideLog.h"
#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/StandAdmission.h"
#include "Model/TaxiwayRestriction.h"
#include "Model/TrafficOccupancy.h"
#include "Solve/IcaoCode.h"
#include "Solve/RunwayDesignator.h"

namespace
{
	/**
	 * Stands exist, and not one of them admits Airframe - surface, size or service, reachable
	 * or not - and if any refusal is fixable, which ONE fix to report.
	 *
	 * EVERY STAND, NOT THE REACHABLE ONES ChooseStand counts: a too-small (or unpaved) stand's
	 * lead-in carries its letter's span limit (FAnchorLink's one rule, final review I5), so to
	 * a widebody it is not reachable at all and ChooseStand's own tallies never see it. The
	 * same IsStandCandidate filter and the same StandAdmission::Judge rule, so this cannot
	 * disagree with ChooseStand about what a stand is or what fits it - only about whether
	 * reach matters, which for "draw a bigger stand" or "pave a stand" it does not.
	 *
	 * PRIORITY ACROSS STANDS, not per-stand precedence (Judge already orders Strip, Surface,
	 * TooSmall, Service within one stand): a stand refused ONLY for sitting in a taxiway's
	 * clearance strip wins first - it fits and is paved, so redrawing it back is the one fix
	 * (strip spec 2026-09-28); then Service wins if ANY stand was refused only for a service - the
	 * more specific, more actionable report; else Surface if ANY stand is big enough at all
	 * (Admission.bPassesSize, regardless of its own pavement) - paving one would fix it; else
	 * every stand really is too small, whatever it is paved with.
	 *
	 * READS Admission.bPassesSize RATHER THAN CALLING IcaoCode::StandAdmits ITSELF: the
	 * call-site rule (Check-Architecture rule 4 row 'IcaoCode::StandAdmits', StandAdmission.cpp's
	 * own ENFORCED BY comment) confines that call to IcaoCode.cpp and StandAdmission.cpp, so a
	 * second opinion here would both violate it and risk drifting from Judge's own answer.
	 *
	 * OutSpeaker is the ONE stand admission the refusal is worded from (R13, final review):
	 * the first service-only refusal for Service, the first big-enough stand for Surface (the
	 * one paving would fix), and for TooSmall the first stand whose pavement passes - else the
	 * first stand, so "draw a bigger stand" adds "on tarmac" only when no stand's pavement
	 * would have done either. Untouched (admitted) when this returns None.
	 */
	EStandRefusal WhyEveryStandRefused(const URoadNetwork& Network, const FAirframe& Airframe,
		FStandAdmission& OutSpeaker)
	{
		int32 Stands = 0;
		TOptional<FStandAdmission> FirstStripOnly;
		TOptional<FStandAdmission> FirstServiceOnly;
		TOptional<FStandAdmission> FirstBigEnough;
		TOptional<FStandAdmission> FirstPavedEnough;
		TOptional<FStandAdmission> First;
		for (const FEntityInstance& Stand : Network.GetEntities())
		{
			if (!Stand.IsStandCandidate()) { continue; }
			++Stands;
			const FStandAdmission Admission = StandAdmission::Judge(Network, Stand, Airframe);
			if (Admission.IsAdmitted()) { return EStandRefusal::None; }
			if (!First.IsSet()) { First = Admission; }
			if (Admission.Why == EStandRefusal::InsideStrip && Admission.bPassesSize && Admission.Pavement.Passes()
				&& !FirstStripOnly.IsSet()) { FirstStripOnly = Admission; }
			if (Admission.Why == EStandRefusal::Service && !FirstServiceOnly.IsSet()) { FirstServiceOnly = Admission; }
			if (Admission.bPassesSize && !FirstBigEnough.IsSet()) { FirstBigEnough = Admission; }
			if (Admission.Pavement.Passes() && !FirstPavedEnough.IsSet()) { FirstPavedEnough = Admission; }
		}
		if (Stands == 0)
		{
			// No stands at all is not "every stand refused" - the caller's NoRouteToStand/
			// NoFreeStand fallback is the right word for that, exactly as EveryStandTooSmall's
			// Stands > 0 guard read before this generalised it.
			return EStandRefusal::None;
		}
		if (FirstStripOnly.IsSet()) { OutSpeaker = *FirstStripOnly; return EStandRefusal::InsideStrip; }
		if (FirstServiceOnly.IsSet()) { OutSpeaker = *FirstServiceOnly; return EStandRefusal::Service; }
		if (FirstBigEnough.IsSet()) { OutSpeaker = *FirstBigEnough; return EStandRefusal::Surface; }
		OutSpeaker = FirstPavedEnough.IsSet() ? *FirstPavedEnough : *First;
		return EStandRefusal::TooSmall;
	}

	/**
	 * "tarmac or stronger", or just the name at the top of the scale - "reinforced or
	 * stronger" would promise a pavement that does not exist.
	 */
	FString PavementOrStronger(EPavement Need)
	{
		return Need == EPavement::Reinforced
			? FString(Pavement::Name(Need))
			: FString::Printf(TEXT("%s or stronger"), Pavement::Name(Need));
	}

	/**
	 * NoStandBigEnough's sentence, with or without the pavement the new stand must be drawn
	 * on. ONE BODY for both DescribeRefusal overloads, so the plan's figures only ever ADD a
	 * clause to the reason-only wording rather than retyping it. OnPavement empty: none.
	 */
	FString BiggerStandSentence(double AircraftWingspan, const FString& OnPavement)
	{
		// THE LETTER IS THE LEVER - the player draws stands by letter, so "too small" alone
		// leaves them guessing how big. Named only for a span the table can name: wider
		// than Code F is a span no stand can ever be drawn for, and saying "Code F" there
		// would send them to build one that still refuses it - nor is its pavement worth naming.
		if (AircraftWingspan > IcaoCode::MaxWingspanForLetter(EIcaoCode::F))
		{
			return FString::Printf(
				TEXT("Arrival refused: a %.1f m wingspan is wider than any stand can be built for."),
				AircraftWingspan / 100.0);
		}
		const FString On = OnPavement.IsEmpty() ? FString() : FString::Printf(TEXT(" on %s"), *OnPavement);
		if (AircraftWingspan > 0.0)
		{
			return FString::Printf(
				TEXT("Arrival refused: this aircraft needs a Code %s stand, and none on the field is ")
				TEXT("big enough. Draw a bigger stand%s."),
				*IcaoCode::LetterForWingspan(AircraftWingspan), *On);
		}
		return FString::Printf(
			TEXT("Arrival refused: every stand is too small for this aircraft. Draw a bigger stand%s."), *On);
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
		// per-stand loop implicitly did by walking it directly. CandidateSpan and CandidateStand
		// are kept parallel by construction (same filter, same push, same index) rather than
		// recomputed from Candidates afterwards, so the arrays cannot disagree about which stand
		// is which. CandidateStand is StandAdmission::Judge's own argument - added beside
		// CandidateSpan rather than replacing it, since the log line below still reads the span
		// directly for its Code-letter text.
		TArray<FGuidelineNodeId> Candidates;
		TArray<double> CandidateSpan;
		TArray<const FEntityInstance*> CandidateStand;
		Candidates.Reserve(Network.GetEntities().Num());
		CandidateSpan.Reserve(Network.GetEntities().Num());
		CandidateStand.Reserve(Network.GetEntities().Num());
		for (const FEntityInstance& Stand : Network.GetEntities())
		{
			if (Stand.IsStandCandidate())
			{
				Candidates.Add(Stand.PoseNode);
				CandidateSpan.Add(Stand.DesignWingspan);
				CandidateStand.Add(&Stand);
			}
		}

		// RANK STILL COMES FROM IcaoCode::StandRank - the ONE rule UStandAllocator::Reserve
		// also calls, so a legacy stand's captured DesignWingspan and an aircraft's Wingspan
		// are always compared by LETTER, in exactly one place, never against each other as raw
		// doubles here or anywhere else. ADMISSION now comes from StandAdmission::Judge, the
		// one rule that also asks pavement and service - see its own comment for why surface
		// beats size.

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
		Query.NeedsPavement(Airframe.MinimumPavement);
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
		// INDEXED BY EStandRefusal - None's slot never increments. SIZED FROM THE ENUM'S LAST
		// VALUE, not a literal: this was [4] until InsideStrip (value 4) was appended on
		// 2026-09-28, and a literal would have been written one past its end.
		int32 RefusedCount[static_cast<uint8>(EStandRefusal::InsideStrip) + 1] = {};
		int32 HeldCount = 0;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (!Reach[Index].bReachable)
			{
				continue;
			}
			// Asked BEFORE Held: surface, size and service are properties of the ground, not of
			// the traffic on it, so a stand this aircraft cannot use at all is never counted as
			// "held" in the sense that word reports to the player (wait, or build another - see
			// NoFreeStand's wording). StandAdmission::Judge is the ONE rule - surface, then
			// size (IcaoCode::StandAdmits' "unknown admits anything" and "wider than F is never
			// admitted"), then service - UStandAllocator::Reserve also calls.
			const FStandAdmission Admission = StandAdmission::Judge(Network, *CandidateStand[Index], Airframe);
			if (!Admission.IsAdmitted())
			{
				++RefusedCount[static_cast<uint8>(Admission.Why)];
				continue;
			}
			// Held is asked AFTER reachability (and admission), so bSawHeld means "a stand this
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
		// (Code %s); %d too small, %d held, %d unpaved" is CLAUDE.md's own "pressing 7 does
		// nothing" discipline applied here: a refusal with no line saying WHY (too small vs.
		// unpaved vs. held vs. simply unreachable) is what cost the runway-length bug a whole
		// session. Code %s is the WINNING stand's own letter when one was found (a known span -
		// an unmeasured winner reports "?", since 0 has no letter), else the aircraft's OWN
		// letter for context on a refusal (also "?" for an airframe with no known span, or one
		// wider than MaxWingspanForLetter(F) admits - LetterForWingspan has nothing useful to
		// say about either). Reading the letter back off LetterForWingspan rather than the Rank
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
		const int32 TooSmallCount = RefusedCount[static_cast<uint8>(EStandRefusal::TooSmall)];
		const int32 UnpavedCount = RefusedCount[static_cast<uint8>(EStandRefusal::Surface)];
		const int32 InStripCount = RefusedCount[static_cast<uint8>(EStandRefusal::InsideStrip)];
		UE_LOG(LogAirside, Log,
			TEXT("ChooseStand: span %.1f m -> node %d (Code %s); %d too small, %d held, %d unpaved, %d in a taxiway strip"),
			Airframe.Wingspan / 100.0, Best.Index, *CodeText, TooSmallCount, HeldCount, UnpavedCount, InStripCount);

		return Best;
	}

	namespace
	{
		/**
		 * Would an arrival landing over End's OTHER threshold reach a stand? Asked only on a
		 * NoExit/NoRouteToStand refusal, to word it (FArrivalPlan::bOtherEndWouldServe) - never
		 * to land that way. The same exit list and the same ChooseStand the plan itself uses,
		 * from the reversed end: a second opinion built from different rules would be a
		 * sentence about an airport the planner does not see.
		 */
		bool OtherEndServes(const URoadNetwork& Network, const FRunwayEnd& End, double SlowedBy,
			const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingHolder)
		{
			const FRunwayEnd Other = End.Reversed();
			for (const FGuidelineNodeId& Exit : Network.RunwayExitNodes(Other.Seed, Other.Threshold, Other.Direction, SlowedBy))
			{
				FRoutePlan Route;
				ChooseStand(Network, Exit, Airframe, Occupancy, ExcludingHolder, &Route);
				if (Route.IsValid())
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * The TaxiwayTooNarrow clause, or empty when no stand is reachable even ignoring size
		 * (then it IS NoRouteToStand). Asked only on a NoRouteToStand refusal, with the same exit
		 * list and the same ChooseStand as the plan - at span 0, which no taxiway limits (a 0 span
		 * is "unknown", admitted everywhere, IcaoCode::StandAdmits' rule) - then walks the route
		 * it WOULD have taken to the first edge this aircraft's wings do not fit: the one a pilot
		 * would meet first, not necessarily the only one. FindToGoals has no TooWide retry of its
		 * own (RouteSearch::Find's), which is why this second search lives here.
		 */
		FString NarrowTaxiwayOnRoute(const URoadNetwork& Network, const FRunwayEnd& End, double SlowedBy,
			const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingHolder)
		{
			if (Airframe.Wingspan <= 0.0)
			{
				return FString();
			}
			FAirframe AnySize = Airframe;
			AnySize.Wingspan = 0.0;
			for (const FGuidelineNodeId& Exit : Network.RunwayExitNodes(End.Seed, End.Threshold, End.Direction, SlowedBy))
			{
				FRoutePlan Route;
				ChooseStand(Network, Exit, AnySize, Occupancy, ExcludingHolder, &Route);
				if (!Route.IsValid())
				{
					continue;
				}
				for (int32 Index = 0; Index < Route.Steps.Num(); ++Index)
				{
					const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Route.Steps[Index].Edge);
					// RouteSearch's own ExceedsWingspan: 0 unlimited, otherwise strictly wider.
					if (Edge == nullptr || Edge->MaxWingspan <= 0.0 || Airframe.Wingspan <= Edge->MaxWingspan)
					{
						continue;
					}
					// THE PIECE: a straight edge names its segment; a turn path names none, so take
					// the neighbouring derived edge carrying the same limit - the turn's Min came
					// from one of its two arms (RoadGuidelineBuilder's turn rule).
					FRoadSegmentId Piece = Edge->DerivedFrom;
					for (const int32 Near : { Index - 1, Index + 1 })
					{
						const FGuidelineEdge* Arm = Route.Steps.IsValidIndex(Near)
							? Network.GetGuidelineEdge(Route.Steps[Near].Edge) : nullptr;
						if (!Piece.IsSet() && Arm != nullptr && Arm->DerivedFrom.IsSet() && Arm->MaxWingspan == Edge->MaxWingspan)
						{
							Piece = Arm->DerivedFrom;
						}
					}
					const FString Need = IcaoCode::LetterForWingspan(Airframe.Wingspan);
					TaxiwayRestriction::FObstruction Worst;
					const TOptional<EIcaoCode> Restricted = Piece.IsSet()
						? TaxiwayRestriction::RestrictionOf(Network, Piece, &Worst) : TOptional<EIcaoCode>();
					if (Restricted.IsSet())
					{
						return FString::Printf(TEXT("a taxiway restricted to Code %s by %s - move it clear of the strip"),
							IcaoCode::ToLetter(Restricted.GetValue()), *TaxiwayRestriction::Describe(Worst));
					}
					const TOptional<EIcaoCode> Letter = Piece.IsSet()
						? TaxiwayRestriction::EffectiveLetterOf(Network, Piece) : TOptional<EIcaoCode>();
					return Letter.IsSet()
						? FString::Printf(TEXT("a Code %s taxiway - upgrade it to Code %s"), IcaoCode::ToLetter(Letter.GetValue()), *Need)
						: FString::Printf(TEXT("a taxiway too narrow for it - upgrade it to Code %s"), *Need);
				}
			}
			return FString();
		}
	}

	bool IsRunwayBusy(const URoadNetwork& Network, const FVector2D& Near, const FTrafficOccupancy* Occupancy)
	{
		// InUseRunwayNearest, not NearestRunwayThreshold, although only the CHAIN is read here:
		// the planners choose ends through the in-use resolver only (Check-Architecture rule 28),
		// and the chain is the same whichever end is returned.
		FRunwayEnd End;
		if (Occupancy == nullptr || !Network.InUseRunwayNearest(Near, End))
		{
			return false;
		}
		// No agent of our own to be occupying anything: nothing has been dispatched yet, so
		// bCountOwnOccupied is false - see FTrafficOccupancy::IsAnyHeld.
		return Occupancy->IsAnyHeld(Network.RunwaySurfaces(End.Seed), 0, false);
	}

	FArrivalPlan Plan(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe,
		const FTrafficOccupancy* Occupancy, ERunwayBusy RunwayBusy, int32 ExcludingHolder)
	{
		FArrivalPlan Out;
		Out.AircraftWingspan = Airframe.Wingspan;

		// 0. NOT ON A GRAPH MID-EDIT - see EArrivalRefusal::GraphBeingEdited. First, because
		//    every step below reads the guideline graph, and the one it would read is stale.
		if (Network.AreGuidelinesBehindRoad())
		{
			Out.Why = EArrivalRefusal::GraphBeingEdited;
			return Out;
		}

		// 1. WHICH RUNWAY, AND WHICH END. The runway nearest the query point, which is the
		//    user's own choice of rule; the END is the runway in use, never the one nearest the
		//    approach focus (spec 2026-09-28-runway-in-use). Nearest-end put a landing and a
		//    shortest-taxi departure head to head on one strip (samples/deadlock.png): with no
		//    wind model, the player's choice of direction is what stands in for one.
		if (!Network.InUseRunwayNearest(Near, Out.End))
		{
			Out.Why = EArrivalRefusal::NoRunway;
			return Out;
		}

		Out.RunwayChain = Network.RunwayChain(Out.End.Seed);

		// 1a. MAY IT USE THIS RUNWAY AT ALL. Surface, approach, published field length and
		//     width, in that order - before occupancy, because occupancy clears on its own
		//     and this never does: M3's sequencer will queue on RunwayOccupied, and it must
		//     not queue an airliner behind a Piper for a grass strip it can never land on.
		//
		//     AND MAY IT LEAVE AGAIN - CheckArrival, not Check (2026-09-27): a landing that fits
		//     on a strip no runway can take the departure from strands the aircraft on its stand.
		Out.Admission = RunwayAdmission::CheckArrival(Network, Out.End.Seed, Airframe);
		if (!Out.Admission.IsAdmitted())
		{
			Out.Why = EArrivalRefusal::NotAdmitted;
			return Out;
		}

		// Asked before the length and exit steps, because those cannot change while the
		// runway is busy and this can: a refusal that clears on its own is reported as
		// itself, not as whichever later step happened to fail too.
		// SKIPPED UNDER Queue: an accept waits its turn for the strip - see ERunwayBusy. The
		// test itself is IsRunwayBusy, the same one the sequencer asks.
		if (RunwayBusy == ERunwayBusy::Refuse && IsRunwayBusy(Network, Near, Occupancy))
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
			Out.bOtherEndWouldServe = OtherEndServes(Network, Out.End, SlowedBy, Airframe, Occupancy, ExcludingHolder);
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
			ChooseStand(Network, Candidate, Airframe, Occupancy, ExcludingHolder, &BestForExit, &bHeldHere);
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
			// Four refusals for four fixes: every stand too small means draw a bigger one; every
			// stand paved too weakly means pave one; every stand serviceable-blocked means fix
			// the service, not the stand; no stand reachable at all means build a taxiway; every
			// reachable stand held means wait, or build a stand. The three stand refusals are
			// asked BEFORE reach and held, and among themselves SERVICE, then SURFACE, then SIZE
			// (WhyEveryStandRefused's priority ACROSS stands - see its comment; Judge's own
			// per-stand order is the other way round), because each is a fact about the ground
			// that no taxiway or waiting changes - and admission used to fall through to
			// NoRouteToStand, sending the player to build a taxiway that already reached every
			// stand (final review I6). Held cannot also be true then: a stand is only counted
			// held once it has admitted.
			switch (WhyEveryStandRefused(Network, Airframe, Out.StandRefusal))
			{
			case EStandRefusal::TooSmall:
				Out.Why = EArrivalRefusal::NoStandBigEnough;
				return Out;
			case EStandRefusal::Surface:
				Out.Why = EArrivalRefusal::NoStandPavedEnough;
				return Out;
			case EStandRefusal::Service:
				Out.Why = EArrivalRefusal::NoStandServiceable;
				return Out;
			case EStandRefusal::InsideStrip:
				Out.Why = EArrivalRefusal::NoStandClearOfStrip;
				return Out;
			case EStandRefusal::None:
			default:
				break;
			}
			Out.Why = bSawHeldStand ? EArrivalRefusal::NoFreeStand : EArrivalRefusal::NoRouteToStand;
			if (Out.Why == EArrivalRefusal::NoRouteToStand)
			{
				// THE SHAPE samples/deadlock.png's field refuses in when flipped: its one connector
				// is behind the touchdown, the strip's own dead-end node is the only "exit" left,
				// and no stand is reachable from that.
				Out.bOtherEndWouldServe = OtherEndServes(Network, Out.End, SlowedBy, Airframe, Occupancy, ExcludingHolder);

				// JOINED UP BUT TOO NARROW (strip stage 6): every taxiway limits aircraft to its
				// letter, so "no route" may only mean "not for wings this wide". The player's fix
				// is an upgrade or a cleared strip, not a new taxiway.
				Out.NarrowTaxiway = NarrowTaxiwayOnRoute(Network, Out.End, SlowedBy, Airframe, Occupancy, ExcludingHolder);
				if (!Out.NarrowTaxiway.IsEmpty())
				{
					Out.Why = EArrivalRefusal::TaxiwayTooNarrow;
				}
			}
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
			// THE DIRECTION IS A FIX TOO (ruling 3, 2026-09-28): an exit behind the touchdown is
			// no exit, and flipping the runway in use turns it into one ahead.
			return TEXT("Arrival refused: nothing joins the runway far enough down to be an exit. ")
				TEXT("Connect a taxiway further down it, or change the runway in use.");

		case EArrivalRefusal::NoRouteToStand:
			return TEXT("Arrival refused: no route from any usable exit to a stand.");

		case EArrivalRefusal::RunwayOccupied:
			return TEXT("Arrival refused: the runway is in use. Wait for it to clear.");

		case EArrivalRefusal::NoFreeStand:
			return TEXT("Arrival refused: every stand it could reach is taken. Wait for one to free, or build another.");

		case EArrivalRefusal::NotAdmitted:
			return TEXT("Arrival refused: this aircraft is not admitted to that runway.");

		case EArrivalRefusal::NoStandBigEnough:
			// BiggerStandSentence carries the letter rule. No pavement clause here: the reason
			// alone does not say what the stands are paved with.
			return BiggerStandSentence(AircraftWingspan, FString());

		case EArrivalRefusal::NoStandPavedEnough:
			return TEXT("Arrival refused: no stand is paved for this aircraft - pave one.");

		case EArrivalRefusal::NoStandServiceable:
			return TEXT("Arrival refused: no stand can be serviced on its pavement.");

		case EArrivalRefusal::GraphBeingEdited:
			return TEXT("Arrival waiting: the airport is being edited.");

		case EArrivalRefusal::NoStandClearOfStrip:
			return TEXT("Arrival refused: every stand that fits sits inside a taxiway's clearance strip - redraw one further back.");

		case EArrivalRefusal::TaxiwayTooNarrow:
			return TEXT("Arrival refused: the taxiways to the stands are too narrow for this aircraft - upgrade them, or clear what restricts them.");

		case EArrivalRefusal::None:
		default:
			return FString();
		}
	}

	namespace
	{
		/** " Landing 27 would reach a stand - change the runway in use." or empty. */
		FString OtherEndSentence(const FArrivalPlan& Plan)
		{
			if (!Plan.bOtherEndWouldServe)
			{
				return FString();
			}
			return FString::Printf(TEXT(" Landing %s would reach a stand - change the runway in use."),
				*RunwayDesignator::ToText(RunwayDesignator::Designate(-Plan.End.Direction)));
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
				TEXT("Arrival refused: landing %s, nothing joins the runway beyond %.0f uu, so ")
				TEXT("there is no exit this aircraft could take. Connect a taxiway further down it, ")
				TEXT("or change the runway in use.%s"),
				*RunwayDesignator::ToText(RunwayDesignator::Designate(Plan.End.Direction)), Plan.Needed,
				*OtherEndSentence(Plan));

		case EArrivalRefusal::NoRouteToStand:
			return FString::Printf(
				TEXT("Arrival refused: landing %s, %d usable exit(s), but no route from any of them ")
				TEXT("to a stand. Check the taxiway reaches the stands.%s"),
				*RunwayDesignator::ToText(RunwayDesignator::Designate(Plan.End.Direction)), Plan.ExitCount,
				*OtherEndSentence(Plan));

		case EArrivalRefusal::NotAdmitted:
			return FString::Printf(TEXT("Arrival refused: %s."), *RunwayAdmission::Describe(Plan.Admission));

		case EArrivalRefusal::TaxiwayTooNarrow:
			return FString::Printf(TEXT("Arrival refused: landing %s, the only route to a stand is over %s."),
				*RunwayDesignator::ToText(RunwayDesignator::Designate(Plan.End.Direction)), *Plan.NarrowTaxiway);

		// THE STAND REFUSALS NAME THE PAVEMENT from the admission that spoke for them (R13,
		// spec 2026-09-27 §3: the plan's refusal text reaches the stand's own decision) - "pave
		// one" alone left the player guessing with what. The reason-only overload keeps the
		// pavement-free wording: a listener on the bus has no admission to read.
		case EArrivalRefusal::NoStandPavedEnough:
			return FString::Printf(
				TEXT("Arrival refused: no stand is paved for this aircraft - it needs %s; draw one on %s."),
				*PavementOrStronger(Plan.StandRefusal.Pavement.Need), Pavement::Name(Plan.StandRefusal.Pavement.Need));

		case EArrivalRefusal::NoStandBigEnough:
			// A stand too small AND too soft: the bigger one must be drawn on the stronger
			// pavement too, or it refuses again for its surface. Only when no stand's pavement
			// would have done (WhyEveryStandRefused's speaker choice); the span names the letter.
			return BiggerStandSentence(Plan.AircraftWingspan,
				Plan.StandRefusal.Why != EStandRefusal::None && !Plan.StandRefusal.Pavement.Passes()
					? PavementOrStronger(Plan.StandRefusal.Pavement.Need) : FString());

		case EArrivalRefusal::NoStandServiceable:
			return FString::Printf(TEXT("Arrival refused: no stand can be serviced on its pavement - %s."),
				*StandAdmission::Describe(Plan.StandRefusal));

		default:
			// NoRunway, RunwayOccupied, NoFreeStand and None carry no figures, so their wording
			// is the reason-only overload's and is not repeated here.
			return DescribeRefusal(Plan.Why, Plan.AircraftWingspan);
		}
	}
}
