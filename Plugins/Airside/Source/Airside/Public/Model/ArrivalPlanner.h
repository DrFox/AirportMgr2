#pragma once

#include "CoreMinimal.h"
#include "Model/Airframe.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayAdmission.h"
#include "Model/StandAdmission.h"
#include "ArrivalPlanner.generated.h"

class URoadNetwork;
struct FTrafficOccupancy;

/**
 * Why ArrivalPlanner::Plan could not produce a plan.
 *
 * One enum rather than a bare false, for the reason FRouteQuery's ERouteResult exists: a
 * refusal that does not say which of "no runway", "too short" or "no exit" applied is a
 * feature that "does nothing" - see CLAUDE.md and the log line "pressing 7 does nothing"
 * this project has already shipped once.
 */
UENUM()
enum class EArrivalRefusal : uint8
{
	None,

	/** Near is not on, or near enough to be answered by, any runway. */
	NoRunway,

	/** The runway exists but is shorter than this airframe needs to stop. */
	RunwayTooShort,

	/** The runway can be stopped on, but nothing joins it far enough down to be usable. */
	NoExit,

	/** At least one usable exit, but no route from any of them reaches a stand. */
	NoRouteToStand,

	/**
	 * The runway exists and would do, but someone holds it - a landing rolling out, a
	 * departure lining up, or an aircraft crossing at a holding-position. The one refusal that
	 * clears on its own; M3's sequencer queues on it.
	 */
	RunwayOccupied,

	/**
	 * The runway exists but this aircraft may not use it: surface, approach, published
	 * field length or width - FArrivalPlan::Admission says which. Asked BEFORE occupancy,
	 * because a refusal that never clears must not be reported as one that will.
	 */
	NotAdmitted,

	/** Stands are reachable, but every one of them is held by another aircraft. Distinct from
	 *  NoRouteToStand because the player's fix differs: wait (or build a stand), not a taxiway. */
	NoFreeStand,

	/** Stands exist, but every one of them is too SMALL for this aircraft - StandAdmits refused
	 *  them all, reachable or not. Distinct from NoRouteToStand because the player's fix
	 *  differs: draw a bigger stand, not a taxiway. FArrivalPlan::AircraftWingspan names the
	 *  letter needed. */
	NoStandBigEnough,

	/** Stands exist and at least one is big enough, but every big-enough one is paved too
	 *  weakly for this aircraft. The player's fix: pave a stand, not draw a bigger one. */
	NoStandPavedEnough,

	/** A stand is big enough and paved enough, but a service this aircraft needs cannot work on
	 *  its pavement. Unreachable until StandAdmission::PavementAdmitsRole restricts a role. */
	NoStandServiceable,

	/**
	 * The guideline graph is behind the road it was derived from - a node is mid-drag, and a
	 * drag rebuilds geometry only (URoadSurfacePresenter, #165), deriving the graph on release.
	 * A route searched now would follow lines the player has already moved (2026-09-27: an
	 * SR22 joined the runway where its end used to be). TRANSIENT: it clears on release, and
	 * a retry then plans on the graph the player sees. Appended LAST so saved values keep
	 * their meaning. See URoadNetwork::AreGuidelinesBehindRoad.
	 */
	GraphBeingEdited,

	/**
	 * A stand would fit and is paved enough, but it sits inside a taxiway's clearance strip -
	 * drawn before the strip existed, or its taxiway upgraded since (strip spec 2026-09-28).
	 * The player's fix: redraw it further back. PERMANENT to the offer generator, since it needs
	 * building. Appended LAST, as GraphBeingEdited was, so saved values keep their meaning.
	 */
	NoStandClearOfStrip,

	/**
	 * A stand is reachable, but only over a taxiway too narrow for this aircraft - its own
	 * letter, or lower where something in its strip restricts it (strip stage 6: every taxiway
	 * limits wingspan to its letter). Distinct from NoRouteToStand because the taxiways ARE
	 * joined up; the fix is to upgrade the taxiway or clear what restricts it, and
	 * FArrivalPlan::NarrowTaxiway says which. PERMANENT to the offer generator (it needs
	 * building). Appended LAST, NoStandClearOfStrip's reason.
	 */
	TaxiwayTooNarrow,

	/**
	 * Runways exist, but every one is set to departures only (ERunwayUse). The player's fix is
	 * the runway card's setting, not building. Appended LAST, NoStandClearOfStrip's reason.
	 */
	NoArrivalRunway,
};

/**
 * The answer to one arrival query: which runway, which exit, which stand, and why not.
 *
 * Pulled out of ARoadNetworkActor::DispatchArrival (issue #29) because choosing all three
 * is a pure function of the graph and the airframe - nothing in it needs a world - and it
 * was the reason Airside.Present.ArrivalDispatch had to UWorld::CreateWorld just to reach
 * three decisions with no view in them at all.
 */
USTRUCT()
struct AIRSIDE_API FArrivalPlan
{
	GENERATED_BODY()

	/** The runway threshold nearest the query point, its heading and the strip's own seed
	 *  segment - see #88. */
	UPROPERTY() FRunwayEnd End;

	/** Every segment continuous with End.Seed. What the landing holds in the occupancy
	 *  table from StartArrival until Vacated. */
	UPROPERTY() TArray<FRoadSegmentId> RunwayChain;

	/** The admission decision for this runway and airframe; Why == NotAdmitted when refused. */
	UPROPERTY() FRunwayAdmission Admission;

	/** Runway needed past Threshold to stop, uu - see FLandingRun::RequiredLandingDistance. */
	UPROPERTY() double Needed = 0.0;

	/** The chosen exit: the earliest one that reaches any stand at all. */
	UPROPERTY() FGuidelineNodeId Exit;

	/** Exit's own position in the ordered exit list, 1-based, for the "vacating at exit N of M" log. */
	UPROPERTY() int32 ExitOrdinal = 0;

	/** How many usable exits the runway offered. */
	UPROPERTY() int32 ExitCount = 0;

	/** Distance along the runway centreline from Threshold at which the aircraft leaves it, uu. */
	UPROPERTY() double VacateAt = 0.0;

	/** Shortest taxi from Exit to a stand. */
	UPROPERTY() FRoutePlan TaxiIn;

	/**
	 * The planned airframe's wingspan, uu - carried so DescribeRefusal can name the stand
	 * letter a NoStandBigEnough refusal needs ("needs a Code F stand") from the plan alone,
	 * the same reason Needed is carried for RunwayTooShort's figures.
	 */
	UPROPERTY() double AircraftWingspan = 0.0;

	/**
	 * The one stand admission a stand refusal (NoStandBigEnough, NoStandPavedEnough,
	 * NoStandServiceable) is worded from - its pavement check names what to pave with. Carried
	 * as Admission is for NotAdmitted, so DescribeRefusal writes from the decision instead of
	 * re-deriving it. Default (admitted) for every other Why; see WhyEveryStandRefused.
	 */
	UPROPERTY() FStandAdmission StandRefusal;

	/**
	 * TaxiwayTooNarrow only: the too-narrow piece and its fix, as a clause - "a taxiway
	 * restricted to Code C by a service road - move it clear of the strip" or "a Code C taxiway
	 * - upgrade it to Code D". Words, not ids, because DescribeRefusal has no network to read
	 * the restriction back from; written once, where the route that found it is in hand.
	 */
	UPROPERTY() FString NarrowTaxiway;

	/**
	 * NoExit or NoRouteToStand only: landing the OTHER way would have reached a stand. The
	 * refusal is still made - a flip is the player's call, never the planner's (ruling 3,
	 * spec 2026-09-28-runway-in-use) - but the sentence says so, because the likeliest reason
	 * a layout that worked yesterday refuses today is that its runway in use was flipped, and
	 * "check the taxiway reaches the stands" sends the player to a taxiway that does.
	 */
	UPROPERTY() bool bOtherEndWouldServe = false;

	/**
	 * Every runway Plan found this airframe COULD land on, one seed per strip, the best among them or not - whatever
	 * Plan's own verdict. Under ERunwayBusy::Queue that is "usable once free", which is what the arrival queue caches
	 * so its live gate asks about THESE strips only (RunwayQuery::AreRunwaysHeld). Asking whether ANY arrival runway was free cleared
	 * a flight onto a free strip it could not use, the dispatch refused it RunwayOccupied, and it went round again every
	 * frame: about 200 refusals and toasts a second (samples/refused.png, 2026-10-01).
	 * ENFORCED BY: AirportOps.Model.ArrivalQueue.FreeRunwayItCannotUseIsNoClearance
	 */
	UPROPERTY() TArray<FRoadSegmentId> UsableRunways;

	/**
	 * None means every step above succeeded and every other field is meaningful.
	 *
	 * DEFAULTS TO NoRunway, not None - fail closed. A default-constructed plan (one nobody
	 * has run Plan() over yet) must read as refused, never as an arrival some caller could
	 * mistake for valid and act on.
	 */
	UPROPERTY() EArrivalRefusal Why = EArrivalRefusal::NoRunway;

	bool IsValid() const { return Why == EArrivalRefusal::None; }

	/**
	 * The stand this plan taxis to - the pose node its taxi-in ends at - or unset when it reaches none. THE STAND AN
	 * ACCEPT HOLDS (#431): UFlightBoard::TryAccept holds exactly this one, so the hold comes from the same REACHABLE set
	 * the plan chose from (ChooseStand), never the smallest-fitting stand on the field whether or not anything can
	 * taxi to it. The route's last step is its goal: RouteSearch backtraces from the stand it settled.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.AcceptHoldsTheReachableStand (the held stand is the connected one)
	 */
	FGuidelineNodeId StandNode() const { return TaxiIn.Steps.Num() > 0 ? TaxiIn.Steps.Last().To : FGuidelineNodeId(); }
};

/**
 * What Plan does about a runway someone else holds.
 *
 * Refuse is a landing NOW - the dispatch - which must not be cleared onto an occupied strip.
 * Queue is an ACCEPT: the flight will wait its turn (UArrivalSequencer), so a busy runway is not
 * a reason to turn it away - and Plan must still run every later step, the stand above all.
 * AN ENUM, not a bool, so a call site says which question it is asking.
 */
enum class ERunwayBusy : uint8
{
	Refuse,
	Queue
};

/**
 * Chooses a runway, an exit and a stand for an arrival - the model half of a landing.
 *
 * Free functions over a const URoadNetwork&, matching RouteSearch's shape rather than a
 * class: there is no state to own between calls, only a graph to read and an airframe to
 * read it against.
 */
namespace ArrivalPlanner
{
	/**
	 * The best FREE stand reachable from From that Airframe is ADMITTED to: the SMALLEST ICAO
	 * letter stand that is Airframe's own letter or wider (never smaller - GDD's "smallest
	 * free stand that fits", so a widebody never parks an A320 out of the one stand it needs),
	 * then the shortest taxi among ties, with runway edges excluded and any stand whose pose
	 * node Occupancy says is held by an agent other than ExcludingAgent skipped. Unset when
	 * none. An airframe with no known wingspan (Wingspan <= 0) or a stand nobody measured
	 * (DesignWingspan == 0) is admitted to anything, as it always was before letter admission
	 * existed - there is nothing to compare a size against.
	 *
	 * OutRoute receives the winning route; bOutSawHeld reports that at least one reachable,
	 * ADMITTED stand was skipped for being held, which is how Plan tells NoFreeStand from
	 * NoRouteToStand - a stand skipped for being too SMALL is neither, and is not counted here.
	 * Plan tells NoStandBigEnough apart on its own, by asking every stand on the field rather
	 * than the reachable ones: a too-small stand's lead-in carries its letter's span limit
	 * (FAnchorLink), so to a widebody it is not even REACHABLE, and a count taken here would
	 * never see it.
	 * Factored out of Plan so the rebuild can ask it from a node that is not a runway exit
	 * (UGroundTraffic::ReResolvePlan) and the re-offer from wherever a waiter stopped.
	 */
	AIRSIDE_API FGuidelineNodeId ChooseStand(const URoadNetwork& Network, FGuidelineNodeId From,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingAgent,
		FRoutePlan* OutRoute = nullptr, bool* bOutSawHeld = nullptr);

	/**
	 * THE TAXI-IN QUERY - the one statement of what route an arrival taxis to a stand by: the ArrivalTaxiIn errand
	 * (its policy and runway avoidance, through FRouteQuery::For), the aircraft class, EdgeSpan as the wingspan the
	 * edges must take, and Airframe's pavement need. No goal: ChooseStand hands the query every candidate stand at once
	 * (RouteSearch::FindToGoals), and a caller that has chosen one sets it.
	 *
	 * ONE FACTORY BECAUSE TWO SEARCHES MUST AGREE (#429 review): ChooseStand chooses a stand by this query, and
	 * UGroundTraffic::ReofferStand then drives the waiter there by a route SendAgentTo searches with it. Built twice,
	 * the two drift - and a stand chosen by one search and unreachable by the other leaves the waiter offered a stand,
	 * refused the route, every pass: an aircraft stuck for good with a free stand in sight.
	 * ENFORCED BY: Check-Architecture rule 4 ('taxi-in query built': ERouteErrand::ArrivalTaxiIn is named in
	 * ArrivalPlanner.cpp and the policy table only)
	 */
	AIRSIDE_API FRouteQuery TaxiInQuery(FGuidelineNodeId From, const FAirframe& Airframe, double EdgeSpan);

	/**
	 * Plans an arrival at the runway nearest Near, for an airframe with Airframe's
	 * performance and wingspan.
	 *
	 * EVERY RUNWAY THAT TAKES ARRIVALS (ERunwayUse) is planned, and the best kept: a free one
	 * before a held one, one SET to arrivals before a mixed one, then the shorter taxi in
	 * (2026-09-29 - it used to be the runway nearest Near alone, and a second runway sat empty).
	 * Near now only orders the runways, which decides whose refusal is reported when none will
	 * do. SHORTEST TAXI FROM THE EARLIEST USABLE EXIT is still the rule on each runway: an
	 * aircraft takes the earliest turn-off it can rather than rolling to the end in search of a
	 * marginally shorter taxi.
	 *
	 * Occupancy, when given, refuses RunwayOccupied while any segment of the chain is held -
	 * unless RunwayBusy is Queue, which skips that one step and carries on (see ERunwayBusy).
	 * ExcludingHolder is a stand hold that does not count as taken - a holding flight's OWN
	 * hold, when the queue asks whether it could land now (UFlightBoard's clearance gate).
	 * Null is the pre-traffic answer, which is what a tool that only asks "could this land
	 * here" still wants.
	 */
	AIRSIDE_API FArrivalPlan Plan(const URoadNetwork& Network, const FVector2D& Near,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy = nullptr,
		ERunwayBusy RunwayBusy = ERunwayBusy::Refuse, int32 ExcludingHolder = 0);

	/**
	 * The runway Plan asks FIRST from Near - the landing runway nearest it, whose refusal Plan reports when none will do
	 * and none clears on its own - or unset when no runway takes arrivals. Near also orders the other candidates and
	 * breaks ties between them, but it changes nothing else a quote renders (the verdict and its sentence) while this
	 * stays put, so a view that caches quotes keys on this rather than on every camera move (the Land panel, #432) - and
	 * does not choose a runway by proximity itself, which Check-Architecture rule 28 forbids outside the planners.
	 * ENFORCED BY: AirportMgr.UI.LandPanelBuildsOnlyOnChange (a pan along one runway re-plans nothing, onto another does)
	 */
	AIRSIDE_API FRoadSegmentId FirstLandingRunway(const URoadNetwork& Network, const FVector2D& Near);

	/**
	 * Is EVERY runway that takes arrivals held - when Plan, refusing on a busy strip, would find
	 * none free. False with no runway or no occupancy to ask. NOT the arrival queue's gate since
	 * 2026-10-01: a free strip the flight cannot use passed it and the dispatch refused every frame
	 * (samples/refused.png); the queue asks RunwayQuery::AreRunwaysHeld over FArrivalPlan::UsableRunways,
	 * the same loop this runs over every arrival runway.
	 * ENFORCED BY: Airside.Model.ArrivalQueue.IsRunwayBusyAgreesWithPlan
	 */
	AIRSIDE_API bool IsRunwayBusy(const URoadNetwork& Network, const FVector2D& Near,
		const FTrafficOccupancy* Occupancy);

	/**
	 * Is any segment of Seed's strip held - by anyone, a reservation included. False with no occupancy. Asked by
	 * IsRunwayBusy and by UGroundTraffic's OnRunwayFreed diff (ops batch 3 §5): public since 2026-09-30 so the
	 * diff can call this function rather than carry a copy of it, and "freed" means "what the queue asks just
	 * turned false". The tests measure the diff against this function's answer tick by tick
	 * (Airside.Model.Traffic.RunwayFreed.*); no test can see a second copy that happens to agree.
	 *
	 * A FORWARDER to RunwayQuery::IsChainHeld since 2026-09-30 (#433), where the implementation moved so the
	 * departure planner's ranking could ask it too - the diff's and the tests' call sites keep this name.
	 */
	AIRSIDE_API bool IsChainHeld(const URoadNetwork& Network, FRoadSegmentId Seed, const FTrafficOccupancy* Occupancy);

	/**
	 * The user-facing sentence for a refused plan - the same wording DispatchArrival used to
	 * log inline, now read off the plan instead of re-derived from it, so the actor logs
	 * from the SAME decision it acted on rather than a second opinion about why.
	 */
	AIRSIDE_API FString DescribeRefusal(const FArrivalPlan& Plan);

	/**
	 * The same sentence for a reason with no plan behind it - what a listener on the outcome
	 * bus has, since that bus carries the reason and not the plan.
	 *
	 * FIGURE-FREE, and that is the difference. The plan overload can say "the runway is 900
	 * uu and this aircraft needs 1200"; this one has neither number, and printing a zero
	 * where a measurement belongs would be worse than omitting it. The wording still lives
	 * in ONE switch - the plan overload defers to this and then adds its figures - so the
	 * two cannot drift into describing the same refusal differently.
	 *
	 * AircraftWingspan, when a caller has one (the inbox has the flight's airframe; the toast
	 * does not), lets NoStandBigEnough name the letter to build. It is not a figure off a plan
	 * - it is the aircraft's own span, which the inbox knows as surely as the plan does - and
	 * 0 falls back to the letter-free sentence.
	 */
	AIRSIDE_API FString DescribeRefusal(EArrivalRefusal Why, double AircraftWingspan = 0.0);

	/**
	 * Does this refusal need the PLAYER TO BUILD OR CHANGE SOMETHING before it can clear - false for one that clears on its
	 * own (a runway empties, an aeroplane leaves a stand, a node is let go) and for None. BESIDE EArrivalRefusal, where a
	 * new reason has to be classified, since #442: it lived in UOfferGenerator, and the question it answers is asked twice
	 * - at the offer ("could this field EVER take this aeroplane", so no airline is offered an A380 until an F stand
	 * exists) and of a flight already holding ("can it ever land", so the player is told and can cancel it). Two askers,
	 * and only one of them used to know the rule.
	 *
	 * EVERY REASON BY NAME, NO default, inside AIRSIDE_EXHAUSTIVE_SWITCH: before, `default: return true` made a reason added
	 * to the enum permanent without anyone deciding so - an offer filtered out for ever, a holding flight told it could
	 * never land. A new reason is a build error at this switch.
	 * ENFORCED BY: C4062 as an error around the body (AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN), AirportOps.Model.OfferGenerator.ATransientRefusalStillGetsOffered (names every EArrivalRefusal and its class)
	 */
	AIRSIDE_API bool IsPermanentRefusal(EArrivalRefusal Why);
}
