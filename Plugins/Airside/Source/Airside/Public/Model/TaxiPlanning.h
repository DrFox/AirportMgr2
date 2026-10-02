#pragma once

#include "CoreMinimal.h"
#include "Model/TaxiPlanner.h"
#include "Model/TaxiReservations.h"
#include "Model/TrafficOccupancy.h"
#include "UObject/Object.h"
#include "TaxiPlanning.generated.h"

class URoadNetwork;
struct FAirframe;
struct FRoadAgent;
struct FTrafficRules;

/** What a clearance is for: the taxi in from a runway exit, or the taxi out (with its push) to a runway. */
enum class ETaxiClearanceKind : uint8
{
	TaxiIn,
	TaxiOut,
};

/**
 * Where a cleared aircraft is in its plan. AN ENUM, NOT bStarted + bDone: the three cannot overlap, and what each
 * permits is different - only Booked may be revoked (spec ruling 2), only Moving is ordered, Done holds its goal.
 */
enum class ETaxiClearanceStage : uint8
{
	/** Booked and not moving: a departure on its stand, waiting for the push its plan times. The one stage an arrival may revoke. */
	Booked,
	/** Moving on its plan - on final to its exit, pushing, taxiing. Never revoked (ruling 2). */
	Moving,
	/** At its goal and parked: its stand stays booked for ever, nothing is ordered any more. */
	Done,
};

/** Why an aircraft lost its plan - the alert speaks only of a layout edit (spec §3); the inspector of either. */
enum class ETaxiUnplanned : uint8
{
	/** Its route was changed under it (the resolver's replan, a redirect, a re-offer) and no plan fits the new one. */
	RouteChanged,
	/** A layout edit rebuilt the graph under it and no plan fits the route it was re-resolved onto. */
	LayoutEdit,
};

/**
 * An aircraft taxiing UNPLANNED (spec §2): no windows - today's claims and the resolver keep it apart - until a plan
 * along its route fits again (retried when the table moves) or it parks, departs or goes.
 */
struct FTaxiUnplanned
{
	ETaxiUnplanned Cause = ETaxiUnplanned::RouteChanged;
	FString Why;
	ETaxiClearanceKind Kind = ETaxiClearanceKind::TaxiIn;
	ERouteErrand Errand = ERouteErrand::Unset;

	/** When a re-plan was last tried, and the table's revision then - tried again only once both have moved on. */
	double TriedAt = -1.0;
	uint32 TriedRevision = 0;
};

/** One cleared aircraft: its plan, and how far through it it is. */
struct FTaxiClearance
{
	int32 Holder = 0;
	ETaxiClearanceKind Kind = ETaxiClearanceKind::TaxiIn;
	ETaxiClearanceStage Stage = ETaxiClearanceStage::Moving;

	/** The booked plan. Plan.Route is the route the agent TAXIES - its TaxiInPlan, TaxiOutPlan or straight-out route. */
	FTaxiPlan Plan;

	/** The push off the stand, for a departure that pushes; invalid for one that drives straight out. */
	FRoutePlan PushRoute;

	/** The push's own windows (FTaxiPlan::PushWindows) - released, one each, when the push hands over to the taxi. */
	TArray<FTaxiResource> PushOnly;

	/** Edges whose start its NOSE has passed - what a follower booked the same way behind it waits for. */
	TSet<FTaxiResource> Entered;

	/**
	 * The last move (index into Plan.MoveStarts) it was let into. A move let into is never asked again: it is let in as
	 * soon as its claim window reaches the move - within its stopping distance - and its windows there are pulled forward
	 * to now (FTaxiReservations::PullForward), so nothing booked later can be placed ahead of it (review of #528 finding 2).
	 */
	int32 GrantedMove = INDEX_NONE;

	/** The last move whose END NODE - a lane's, asked on its own (OrderHold) - it was let onto, committed likewise. */
	int32 GrantedEnd = INDEX_NONE;

	/** Route steps whose edge and end node are released, through this index. */
	int32 ReleasedThrough = INDEX_NONE;

	/** Whether its route's start node has been released. */
	bool bStartReleased = false;

	/**
	 * A DEPARTURE QUEUEING: its plan ends at a holding node short of the runway entry (QueueFor), because the entry is
	 * booked for ever by the departure ahead of it until that one lines up. It is held at that node until the rest is
	 * planned and booked onto the end of this plan (UTaxiPlanning::Extend) and driven (UGroundTraffic::ExtendRoute).
	 */
	FGuidelineNodeId QueueFor;
	ERouteErrand QueueErrand = ERouteErrand::Unset;

	/** Where QueueFor is - re-found by position after a rebuild, which kills the handle (UGroundTraffic::ReplanTaxi). */
	FVector2D QueueAt = FVector2D::ZeroVector;

	/** When a re-time was last tried, and how long to wait before the next: a sim second, doubled on each refusal up to 30 s
	 *  and reset by a success (review of #534 finding 9 - a refused cascade copies the table and walks every pair, and was
	 *  asked again every second for as long as the aircraft stayed late). */
	double RetimeTriedAt = -1.0;
	double RetimeBackoff = 1.0;

	/**
	 * Whether this plan STARTS AT A RUNWAY EXIT - an arrival's, booked as it is cleared to land (BookArrival) or re-made from
	 * the exit while it is still on final. Its first move is then never refused by the order (OrderHold). A FACT OF THE
	 * CLEARANCE, not "move 0 of a taxi in" (review of #534 finding 3): a re-plan along the live route makes move 0 the step
	 * the aircraft is on, anywhere on its way, and its next node was entered unordered.
	 */
	bool bFromExit = false;

	/** Whether the aircraft is still ON FINAL (Arriving): its landing is flown, so no re-time may move its windows (finding 6). */
	bool bOnFinal = false;

	/** When the rest was last asked for, and the table's revision then - asked again only once both have moved on. */
	double ExtendAskedAt = -1.0;
	uint32 ExtendRevision = 0;
};

/** The claim pass's one more refusal: where the agent stops, and for whom. Blocker 0: none. */
struct FTaxiOrderHold
{
	/** The first step of the move it may not enter yet - what FRoadAgent::BlockedStep names. */
	int32 Step = INDEX_NONE;

	/** Where that step starts, in route distance. */
	double StepStart = 0.0;

	/**
	 * Held at the END NODE of Step (its turn on the node has not come) rather than at the start of a move: it stops a gap
	 * short of the node, on the lane it is driving - StepEnd says where the node is. False: a gap short of StepStart.
	 */
	bool bAtNode = false;
	double StepEnd = 0.0;

	/** Who it waits for, and on what. */
	int32 Blocker = 0;
	FTrafficResource Resource;

	bool IsSet() const { return Blocker != 0; }
};

/**
 * THE OWNER OF TAXI PLANNING (spec 2026-10-02 §1): the reservation table, every cleared aircraft's plan, the
 * order the claim pass enforces, and the release as tails clear. A UGroundTraffic subobject (made in its
 * PostInitProperties), driven by it: UGroundTraffic decides WHEN an aircraft is cleared (dispatch, pushback) and
 * asks this for the plan; this decides nothing about traffic on its own.
 *
 * THE ONLY WRITER OF FTaxiReservations: one owner, so the table's order is one
 * timeline and every wait it orders points at something booked earlier on it (spec §2's acyclicity).
 * ENFORCED BY: Check-Architecture rule 104 (taxi reservation table written)
 *
 * PATTERN: a Mediator's colleague, as FDeadlockResolver and FPlanReResolver are - but a UObject, unlike them, so
 * PR 4's inspector and overlay can hold it weakly. Its state is all session state: nothing is saved (agents are not).
 *
 * LOGS ON LogAirsideTaxiPlan, ONCE PER EVENT, never per frame: planned, refused (once per reason), revoked,
 * unplanned.
 */
UCLASS(Transient)
class AIRSIDE_API UTaxiPlanning : public UObject
{
	GENERATED_BODY()

public:
	const FTaxiReservations& GetTable() const { return Table; }

	/** Moves on every write to the table - a clearance retry dates itself by it (FArrivalQueue::ClearanceFor). */
	uint32 Revision() const { return RevisionCount; }

	const FTaxiClearance* Find(int32 Holder) const { return Clearances.Find(Holder); }
	int32 ClearanceCount() const { return Clearances.Num(); }

	/** The headway the table holds followers to - FTrafficRules::TaxiPlanMargin, set by the owner every Advance. */
	void SetHeadway(double Seconds);

	/** A plan on the table as it stands. Nothing booked. A planner per call: see FTaxiPlanner's "one per burst". */
	FTaxiPlan Plan(const URoadNetwork& Network, const FAirframe& Airframe, const FTrafficRules& Rules,
		const FTaxiRequest& Request) const;

	/** A plan made round On instead of the table - a copy TableWithout gave. Nothing booked. */
	static FTaxiPlan PlanOn(const FTaxiReservations& On, const URoadNetwork& Network, const FAirframe& Airframe,
		const FTrafficRules& Rules, const FTaxiRequest& Request);

	/**
	 * The table without Holders' windows - a COPY, nothing changed: what a re-plan is made round when those holders are to be
	 * re-planned behind it (UGroundTraffic::ReplanTaxi, review of #534 finding 1).
	 */
	FTaxiReservations TableWithout(TConstArrayView<int32> Holders) const;

	/**
	 * A departure's plan to its runway entry holds the entry, and the edge onto it, for Hold seconds after it arrives rather
	 * than for ever (FTrafficRules::TaxiPlanEntryHold). 0 leaves them for ever. Every booking of a plan that ends AT an entry
	 * applies it - the first, the queue's rest, and a re-plan (review of #534 finding 5: a re-plan held the entry for ever
	 * again, and every taxi-in through it waited for the departure to take off).
	 * ENFORCED BY: Airside.Model.TaxiPlan.ReplannedDepartureFreesItsEntry
	 */
	static void CapEntryHold(FTaxiPlan& Plan, double Hold);

	/**
	 * AN ARRIVAL'S PLAN, ARRIVALS FIRST (spec ruling 2): on the table as it stands, or failing that on the table
	 * without the Booked departures - those not yet pushing - in which case OutRevoke names the ones whose windows
	 * the plan needs. A Moving plan is never in OutRevoke. Nothing booked or revoked here.
	 */
	FTaxiPlan PlanArrival(const URoadNetwork& Network, const FAirframe& Airframe, const FTrafficRules& Rules,
		const FTaxiRequest& Request, TArray<int32>& OutRevoke) const;

	/**
	 * Books Plan for Holder - every pass re-held under Holder, since a plan made before the agent existed had none -
	 * replacing whatever Holder had. Logs "planned". False, and nothing changed, when the table refuses it.
	 */
	bool Book(const URoadNetwork& Network, int32 Holder, ETaxiClearanceKind Kind, const FTaxiPlan& Plan,
		ETaxiClearanceStage Stage, const FRoutePlan& PushRoute = FRoutePlan(), double Now = 0.0);

	/**
	 * Books Plan as Book does, giving up each of Revoke's Booked departures' plans for it in the same swap - none of them
	 * if the plan cannot be booked, and the arrival is not cleared (review of #528 finding 6: revoked first, they were lost
	 * to a booking that then failed). OutRevoked: those given up, each logged "revoked by arrival".
	 */
	bool BookArrival(const URoadNetwork& Network, int32 Holder, const FTaxiPlan& Plan, TConstArrayView<int32> Revoke,
		double Now, TArray<int32>& OutRevoked);

	/** A Booked departure's plan given up for arrival ByArrival - logged "revoked by arrival". False for any other stage. */
	bool Revoke(int32 Holder, int32 ByArrival);

	/**
	 * Holder's push (or straight-out taxi) has STARTED: Moving from now, so no arrival may revoke it (ruling 2). Called
	 * where the motion starts (UGroundTraffic::StartPlannedDeparture), not left to the next tick's Track - the arrival
	 * queue dispatches between ticks (review of #528 finding 1).
	 */
	void MarkMoving(int32 Holder);

	/** Holder's clearance and every window gone. Why non-null: logged "unplanned - Why"; null: a completion, silent. */
	void Drop(int32 Holder, const TCHAR* Why);

	/** Every clearance dropped - a rebuild. Logged once. */
	void DropAll(const TCHAR* Why);

	/**
	 * A REBUILD'S FIRST HALF (taxi planning PR 3): every clearance taken out of the table - whose handles the rebuild is
	 * about to kill - with the table's ORDER as a list of holders, ahead first (topological over OrderPairs; ties by
	 * earliest window). UGroundTraffic::ReplanAfterRebuild books them again, along their re-resolved routes, in that order.
	 */
	void TakeAllForRebuild(TArray<int32>& OutOrder, TMap<int32, FTaxiClearance>& OutCleared);

	/**
	 * A plan ALONG A ROUTE ALREADY BEING DRIVEN booked for Holder: Live is that whole route, Tail the planner's plan for
	 * its steps from Prefix on (FTaxiRequest::Along), PrefixPasses the windows of what it is on before them (the step it
	 * is driving, a push under way). The clearance's route is Live, its legs and moves offset by Prefix, the steps before
	 * the one it is on counted released, and the step it is ON let in (GrantedMove): it is there, and nothing is asked of
	 * it again. bFromExit: the plan starts at a runway exit (FTaxiClearance::bFromExit). EntryHold: CapEntryHold's, 0 for a
	 * plan that does not end at a runway entry. Replaces any plan Holder had; clears it as unplanned. False: nothing changed.
	 */
	bool BookAlong(const URoadNetwork& Network, int32 Holder, ETaxiClearanceKind Kind, ETaxiClearanceStage Stage,
		const FRoutePlan& Live, int32 Prefix, const FTaxiPlan& Tail, TConstArrayView<FTaxiPass> PrefixPasses,
		const FRoutePlan& PushRoute, TConstArrayView<FTaxiResource> PushOnly, double Now, bool bFromExit, double EntryHold);

	/**
	 * Holder's clearance and windows taken out, silently, into Out (false and Out untouched when it had no clearance - its
	 * windows go either way): to be re-planned BEHIND an aircraft physically ahead of it (UGroundTraffic::ReplanTaxi). Unlike
	 * Drop, it keeps the holder's test delay.
	 */
	bool TakeOut(int32 Holder, FTaxiClearance& Out);

	/** Holder taxis on with no plan - see FTaxiUnplanned. Logged "unplanned - Why" once per reason. Its windows go. */
	void MarkUnplanned(int32 Holder, ETaxiUnplanned Cause, const FString& Why, ETaxiClearanceKind Kind, ERouteErrand Errand);

	/** Holder's unplanned record, or null when it has a plan or never had one. */
	const FTaxiUnplanned* FindUnplanned(int32 Holder) const { return Unplanned.Find(Holder); }

	/** Holder no longer counts as unplanned - it parked, departed or went (UGroundTraffic::RetryUnplanned). */
	void ClearUnplanned(int32 Holder);

	/** Every unplanned holder. */
	TArray<int32> UnplannedHolders() const;

	/**
	 * The unplanned holders due a retry: the table moved since the last try and a sim second has passed, or UnplannedRetryPeriod
	 * has passed regardless - a window that runs out moves nothing, and the plan it blocked fits from then on (review of #534
	 * finding 10).
	 */
	TArray<int32> UnplannedDueRetry(double Now) const;

	/** A retry was made at Now on the table as it is - UnplannedDueRetry's date. */
	void NoteUnplannedTried(int32 Holder, double Now);

	/** True once after the unplanned set changed since it was last asked - DiffFreedom's question (OnTaxiUnplannedChanged). */
	bool TakeUnplannedChanged();

	/**
	 * RE-TIME (spec §2): Holder's windows still held after Since moved Lag later, everyone booked behind them with them,
	 * same order (FTaxiReservations::ShiftLater); every shifted plan's legs, holds, arrival and push time moved alike.
	 * Since is the moment it is late FOR, so that overdue moment moves too; Now is the clock, and nothing started by it has its
	 * start moved (ShiftLater). An arrival still on final is immovable (bOnFinal). Logged "re-timed +X s". Wakes waiters.
	 * False when the cascade cannot be made: then the late one's started windows are stretched as far as nobody behind is
	 * overrun (StretchHeld, review of #534 finding 6), so they do not lapse under it.
	 */
	bool Retime(int32 Holder, double Since, double Lag, double Now);

	/** The retry period of an unplanned aircraft when the table has not moved - a window that merely runs out moves nothing. */
	static constexpr double UnplannedRetryPeriod = 10.0;

	/** Test only: every re-plan refused - the unplanned fallback, made on demand. */
	bool bRefuseReplansForTest = false;

	/** A refusal, said once per holder per reason (0 holder: an arrival not yet admitted - said per reason). */
	void NoteRefused(int32 Holder, const FString& What, const FString& Why);

	/**
	 * Who Holder waits for before entering Resource: FPassingOrder over the table and every clearance's Entered.
	 * 0 when its turn - or when bEnforceOrderForTest is off.
	 */
	int32 WaitingFor(int32 Holder, const FTaxiResource& Resource) const;

	/**
	 * Who is booked ahead of Holder on Edge COMING THE OTHER WAY (or both ways, a push) and has not left - what an arrival's
	 * exempt first move still waits for (review of #534 finding 7): one going its way may share the edge behind it, one
	 * coming at it would meet it head-on mid-edge. 0: nobody, or bEnforceOrderForTest off.
	 */
	int32 OpposingAhead(int32 Holder, const FTaxiResource& Edge) const;

	/** Marks Holder's departure as QUEUEING for QueueFor - see FTaxiClearance::QueueFor. */
	void SetQueued(int32 Holder, FGuidelineNodeId QueueFor, ERouteErrand Errand, const FVector2D& QueueAt);

	/** The queueing departures not asked in the last second, or since the table last moved - ExtendQueuedDepartures' list. */
	TArray<int32> QueuedDueAsk(double Now) const;

	/** Holder's rest was asked for at Now, on the table as it is - QueuedDueAsk's date. Only an ask that was made is noted. */
	void NoteExtendAsked(int32 Holder, double Now);

	/**
	 * A queueing departure's REST booked onto its plan: Ext runs from where the plan ends (its holding node) to the entry.
	 * Its windows on that node and the edge into it - held for ever while it queued - end as Ext leaves; Ext's passes join
	 * them; the whole lot is re-booked at once. False, nothing changed, when the table refuses it. Logged.
	 */
	bool Extend(const URoadNetwork& Network, int32 Holder, const FTaxiPlan& Ext, double Now);

	/** The route the agent now drives, adopted as its plan's after ExtendRoute spliced the rest on. */
	void AdoptRoute(int32 Holder, const FRoutePlan& Live);

	/** Who a Booked departure waits for before it may start its push: the order over every window its plan holds from the push. 0: its turn. */
	int32 PushWaitingFor(int32 Holder) const;

	/** Booked departures whose push is due by Now. */
	TArray<int32> DuePushes(double Now) const;

	/** Every holder with a clearance. */
	TArray<int32> Holders() const;

	/**
	 * THE CLAIM PASS'S QUESTION (FClaimPass::Run): may Agent enter each MOVE of its plan that its claim window reaches
	 * (T is its centre, Head the window's far end, both route distance; Now the sim clock)? The first move it may not enter - any of its
	 * resources not its turn - fills Out; a move it may enter, once its window reaches it, is granted for good (committed: GrantedMove). False
	 * when the agent has no Moving clearance or nothing holds it.
	 */
	bool OrderHold(const FRoadAgent& Agent, double Now, double T, double Head, FTaxiOrderHold& Out);

	/**
	 * After the claim pass, per agent: release what its TAIL has cleared (behind it, and no claim of its left on it),
	 * note what its NOSE has entered, follow its phase - parked is Done, lined up drops the plan (the runway is the
	 * runway's own authority from there) - and re-time it when it runs later than its plan by more than the knob
	 * (FTrafficRules::TaxiPlanRetimeLag). False: its route is no longer the plan's - the caller re-plans it along the new
	 * one (UGroundTraffic::ReplanTaxi), which PR 2 dropped instead.
	 */
	bool Track(const FRoadAgent& Agent, const FTrafficOccupancy& Occupancy, const FTrafficRules& Rules, double Now);

	/** True once after anything was released since it was last asked - UGroundTraffic::DiffFreedom's question. */
	bool TakeReleased();

	/** Test only: windows booked straight into the table with no clearance - a stand-in for traffic a test does not fly. */
	bool BookPassesForTest(TConstArrayView<FTaxiPass> Passes);

	/** Test only: order enforcement off - the headline test's mutation proof. Plans are still booked and released. */
	bool bEnforceOrderForTest = true;

	/**
	 * Test only: Holder is held STILL, wherever it is, until the sim clock reaches Until - a pilot slow off the mark, a
	 * stop for no reason the plan knew of. The headline test's seeded delays: a plan's TIMES go wrong, and only its
	 * ORDER is left to keep the field moving (spec §2). Through OrderHold, so the claim pass stops it as for any wait;
	 * it names DelayBlocker, nobody, so the resolver sees no edge.
	 */
	void DelayForTest(int32 Holder, double Until) { DelayedUntilForTest.Add(Holder, Until); }

	/** What a delayed agent waits on: an id no agent has, so the wait-for graph gets no edge from it. */
	static constexpr int32 DelayBlocker = MAX_int32;

	/**
	 * What a QUEUEING departure waits on at its holding node when nobody holds the entry it queues for - the entry has
	 * just freed and its rest is not booked yet (ExtendQueuedDepartures books it within a sim second). Its own sentinel,
	 * not DelayBlocker: the log said "a test's delay" for it (review of #528 finding 5). No agent has it either.
	 */
	static constexpr int32 QueueBlocker = MAX_int32 - 1;

private:
	void Bump(bool bReleasedSomething);

	FTaxiReservations Table;
	TMap<int32, FTaxiClearance> Clearances;
	uint32 RevisionCount = 0;
	bool bReleasedSinceAsked = false;

	/** The last refusal said per holder, so a refusal is said once per reason - see NoteRefused. */
	TMap<int32, FString> LastRefusal;

	/** See DelayForTest. */
	TMap<int32, double> DelayedUntilForTest;

	/** See FTaxiUnplanned. */
	TMap<int32, FTaxiUnplanned> Unplanned;
	bool bUnplannedChanged = false;
};
