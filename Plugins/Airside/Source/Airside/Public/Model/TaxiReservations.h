#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

/**
 * What a reservation window is ON: a guideline edge or a guideline node (spec 2026-10-02 §1).
 * An enum, not a bool bEdge, per this codebase's rule - and because a third kind (a runway
 * chain, a stand) is the obvious next one and a bool would have to be unpicked to add it.
 */
enum class ETaxiResourceKind : uint8
{
	Edge,
	Node,
};

/**
 * One thing an aircraft's plan occupies for a while.
 *
 * NO DIRECTION, deliberately: a two-way edge is ONE resource, both ways (spec §1). That is the
 * whole point of the table - the head-on of 2026-10-02 was two aircraft holding one stretch from
 * opposite ends, and a key that carried a direction would book them both. Two PARALLEL lines are
 * two edges and so two resources, which is what lets the planner take the other one.
 *
 * THE HANDLES THEMSELVES, not a copied {Index, Generation}: a key built by hand from a handle's
 * fields is a second place that knows a handle's layout (#79), and a recycled slot must not
 * match a dead edge's windows - the handles' own operator== compares the generation.
 */
struct AIRSIDE_API FTaxiResource
{
	ETaxiResourceKind Kind = ETaxiResourceKind::Node;

	/** Set when Kind is Edge; unset otherwise. */
	FGuidelineEdgeId EdgeId;

	/** Set when Kind is Node; unset otherwise. */
	FGuidelineNodeId NodeId;

	static FTaxiResource Edge(FGuidelineEdgeId Id)
	{
		FTaxiResource Out;
		Out.Kind = ETaxiResourceKind::Edge;
		Out.EdgeId = Id;
		return Out;
	}

	static FTaxiResource Node(FGuidelineNodeId Id)
	{
		FTaxiResource Out;
		Out.Kind = ETaxiResourceKind::Node;
		Out.NodeId = Id;
		return Out;
	}

	bool operator==(const FTaxiResource& Other) const
	{
		return Kind == Other.Kind && EdgeId == Other.EdgeId && NodeId == Other.NodeId;
	}
	bool operator!=(const FTaxiResource& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FTaxiResource& Resource)
{
	return HashCombine(::GetTypeHash(static_cast<uint8>(Resource.Kind)),
		Resource.Kind == ETaxiResourceKind::Edge ? GetTypeHash(Resource.EdgeId) : GetTypeHash(Resource.NodeId));
}

/**
 * Which way a window drives along its EDGE - A to B, B to A - or Any: a node, a push, or a window that
 * is both ways at once (a push and the taxi back over the same line, merged). Any shares with nothing.
 */
enum class ETaxiWay : uint8
{
	Any,
	AToB,
	BToA,
};

/**
 * Who holds a resource, and when: [From, To) in seconds on the caller's clock.
 *
 * HALF-OPEN, so a window that ends at 20 and one that starts at 20 do not conflict - the second
 * aircraft enters the instant the first has left, and the safety margin (FTrafficRules::
 * TaxiPlanMargin), not the interval arithmetic, is what keeps them apart. Closed intervals would
 * make every back-to-back pair a conflict and force a zero-width gap nobody could plan into.
 */
struct FTaxiWindow
{
	/** The agent id holding it. 0 is "nobody" and is what FreeIntervals' IgnoreHolder defaults to. */
	int32 Holder = 0;
	double From = 0.0;
	double To = 0.0;

	/** Which way it drives - see ETaxiWay, and FTaxiReservations::MayShare for what it buys. */
	ETaxiWay Way = ETaxiWay::Any;
};

/** A stretch of time when a resource is free: [Start, End). End == FTaxiReservations::Forever is open-ended. */
struct FTaxiInterval
{
	double Start = 0.0;
	double End = 0.0;
};

/** One step of a re-time (FTaxiReservations::ShiftLater): Holder's windows still held after Since, moved Delta later. */
struct FTaxiShift
{
	int32 Holder = 0;
	double Since = 0.0;
	double Delta = 0.0;
};

/** One window a plan wants, on one resource - what FTaxiReservations::BookPasses takes. */
struct FTaxiPass
{
	FTaxiResource Resource;
	FTaxiWindow Window;
};

/**
 * The space-time reservation table: per resource, the ordered windows during which someone holds
 * it (spec 2026-10-02 §1). PLAIN DATA - book, release, ask what is free - with no idea what a
 * plan, an aircraft or a priority is; FTaxiPlanner reads it, UTaxiPlanning alone writes it.
 *
 * WINDOWS ON ONE RESOURCE NEVER CONFLICT (MayShare), whoever holds them, the holder's own included:
 * the table is what order enforcement is derived from (FPassingOrder), and two windows that may not
 * share have no order. Two windows OVERLAP only when they drive one edge the same way in FIFO order
 * (PR 2's correction: PR 1 made an edge one aircraft's at a time, so nobody could follow anybody
 * along a taxiway). A refused booking leaves the table untouched.
 *
 * A LINEAR SCAN PER RESOURCE, not an interval tree: a resource carries one window per aircraft
 * due through it, and on M_ScaleGatwick at 80 mov/h the spike saw at most a few dozen aircraft
 * on the ground at once (2026-10-02) - a sorted TArray of that size beats a tree's constant.
 */
class AIRSIDE_API FTaxiReservations
{
public:
	/** The end of an open-ended window or interval: held, or free, for ever. */
	static constexpr double Forever = TNumericLimits<double>::Max();

	/** The start of the first free interval of a resource nobody has ever held. */
	static constexpr double Always = TNumericLimits<double>::Lowest();

	/**
	 * Whether B may hold its resource while A does. Always when they do not overlap in time. When they do, only
	 * when both drive the edge the SAME WAY (neither Any) and in FIFO order - one enters at least Headway after
	 * the other AND leaves at least Headway after it: entry order is exit order, so a follower never has to pass
	 * its leader, and the gap between them in time is at least Headway at both ends. Opposite ways, or Any, never
	 * share - that is the head-on the table exists to prevent.
	 */
	static bool MayShare(const FTaxiWindow& A, const FTaxiWindow& B, double Headway);

	/**
	 * The headway MayShare asks of two windows following each other along one edge, seconds. Set by the owner from
	 * FTrafficRules::TaxiPlanMargin - the time twin of the gap, as the margin is. Floored at a millisecond: with no
	 * headway two identical windows would be "FIFO" and two aircraft would be booked onto one spot.
	 */
	void SetHeadway(double InHeadway) { Headway = FMath::Max(InHeadway, 0.001); }
	double GetHeadway() const { return Headway; }

	/**
	 * Books Window on Resource. False, and nothing changed, when the window has no length
	 * (From >= To) or may not share (MayShare) with any window already there.
	 */
	bool BookWindow(const FTaxiResource& Resource, const FTaxiWindow& Window);

	/**
	 * Books every pass, or none: false, and nothing changed, if any pass would be refused by
	 * BookWindow - against the table or against an earlier pass in the same list. ALL OR NOTHING
	 * because a plan half-booked is a plan whose order other aircraft will wait on and which the
	 * aircraft itself can never fly.
	 */
	bool BookPasses(TConstArrayView<FTaxiPass> Passes);

	/** Removes every window Holder has, on every resource. Returns how many went. */
	int32 ReleaseHolder(int32 Holder);

	/** Removes Holder's windows on Resource only. Returns how many went. */
	int32 ReleaseHolderOn(const FTaxiResource& Resource, int32 Holder);

	/**
	 * Removes Holder's EARLIEST window on Resource - the tail clearing it ONCE. A route may pass one resource twice (a
	 * turnaround loop at a dead end, out along an edge and back along it), and the second pass's window is still due:
	 * releasing all of them on the first let the aircraft back onto it unordered (measured on M_ScaleGatwick, 2026-10-02 -
	 * two departures through one loop, the second out first). True when one went.
	 */
	bool ReleaseFirstOn(const FTaxiResource& Resource, int32 Holder);

	/**
	 * Holder's EARLIEST window on Resource made to start no later than Earliest - as early as the windows ahead of it
	 * allow (past a non-sharing one's end; a headway behind a same-way one's start), never later than it was, its end
	 * unchanged. A COMMITMENT (UTaxiPlanning::OrderHold, review of #528 finding 2): an aircraft early on its plan that the
	 * order has let into a move can no longer stop short of it, and a gap left before its window would let a plan made
	 * now be booked AHEAD of it there - an order it could not obey. True when the window moved.
	 */
	bool PullForward(const FTaxiResource& Resource, int32 Holder, double Earliest);

	/**
	 * A RE-TIME (spec 2026-10-02 §2): Holder's windows still held after Since moved Delta later - one that starts after
	 * Since shifted, one that straddles it stretched - and EVERY window booked behind one of them that would then overlap
	 * or overtake it moved on by the least that keeps it behind, recursively: same order, later times.
	 *
	 * NOTHING THAT HAS STARTED MOVES ITS START (review of #534 finding 2): a window with From at or before Now - ground its
	 * holder is on, or a move the order committed it to - is only stretched, and one that waited for the late one is
	 * moved up to ABUT it. A start moved later left a gap ahead of an aircraft that could not stop, and a plan was booked
	 * into it ahead of that aircraft.
	 *
	 * AN IMMOVABLE HOLDER'S WINDOWS DO NOT MOVE AT ALL (finding 6 - an arrival on final: its landing is flown): one that
	 * would be overrun stays, and the late window ahead of it goes behind it instead.
	 *
	 * All or nothing: false, and the table untouched, when a window would have to move behind one held for ever that it may
	 * not share with, or a late one already there would overrun an immovable one. OutShifts: every move made, the first
	 * Holder's, for the plans that carry the times (UTaxiPlanning::Retime).
	 */
	bool ShiftLater(int32 Holder, double Since, double Delta, double Now, TFunctionRef<bool(int32 Holder)> Immovable,
		TArray<FTaxiShift>& OutShifts);

	/** ShiftLater with no holder immovable. */
	bool ShiftLater(int32 Holder, double Since, double Delta, double Now, TArray<FTaxiShift>& OutShifts);

	/**
	 * Holder's windows that have started (From at or before Now) and end before Until stretched towards Until - each only as
	 * far as the next window behind it allows (LatestEnd): no cascade, nobody moved. A RE-TIME'S FALLBACK (review of #534
	 * finding 6): refused, the late aircraft's windows would LAPSE while it was still on the ground they hold, and plans be
	 * made through it. True when anything was stretched.
	 */
	bool StretchHeld(int32 Holder, double Now, double Until);

	/**
	 * The ORDER the table holds, as (ahead, behind) holder pairs - consecutive windows of two holders on one resource.
	 * What survives a rebuild that kills every handle (UGroundTraffic::ReplanAfterRebuild re-plans in this order).
	 */
	void OrderPairs(TArray<TPair<int32, int32>>& Out) const;

	/**
	 * Resource's free intervals, earliest first, treating IgnoreHolder's own windows as free -
	 * so a holder re-planning is not blocked by the plan it is replacing. Never empty: a
	 * resource nobody holds is one interval [Always, Forever). Touching windows leave no
	 * zero-length gap between them. BLIND TO WAY: every window blocks - the conservative answer,
	 * for a reader that does not drive anywhere (the planner asks EarliestFit).
	 */
	void FreeIntervals(const FTaxiResource& Resource, int32 IgnoreHolder, TArray<FTaxiInterval>& Out) const;

	/** Whether no window other than IgnoreHolder's overlaps [From, To). Blind to way, as FreeIntervals is. */
	bool IsFree(const FTaxiResource& Resource, double From, double To, int32 IgnoreHolder) const;

	/**
	 * The least Shift >= 0 such that a window [From + Shift, To + Shift) going Way may share (MayShare) with every
	 * window on Resource but IgnoreHolder's. To == Forever stays Forever. False when no shift will do - something
	 * that may not share holds the resource for ever after From. THE PLANNER'S ONE QUESTION of the table: it
	 * shifts its departure by the answer and asks again, until every resource of a move answers 0.
	 */
	bool EarliestFit(const FTaxiResource& Resource, ETaxiWay Way, double From, double To, int32 IgnoreHolder,
		double& OutShift) const;

	/**
	 * How late a window that starts at From going Way may END and still share with every window on Resource but
	 * IgnoreHolder's: the start of the next one that may not overlap it, or the end (less Headway) of the next one
	 * following it the same way, which it must leave before. Forever when nothing lies ahead. How long an aircraft
	 * may wait on a node, with its tail on the edge behind it.
	 */
	double LatestEnd(const FTaxiResource& Resource, ETaxiWay Way, double From, int32 IgnoreHolder) const;

	/**
	 * How many of Resource's windows (IgnoreHolder's excepted) start before At - the place a window starting at At
	 * takes in the resource's order. SIPP keys a search state on it, as textbook SIPP keys on a free interval's
	 * index: two arrivals in one place have the same aircraft ahead and behind.
	 */
	int32 PlaceAt(const FTaxiResource& Resource, double At, int32 IgnoreHolder) const;

	/**
	 * The least start after From at which a window going Way would take a LATER place: past the first window that
	 * starts at or after From - after its end, or, the same way along an edge, a headway after its start. Forever
	 * when no window starts at or after From. The planner's "next safe interval".
	 */
	double NextPlaceAfter(const FTaxiResource& Resource, ETaxiWay Way, double From, int32 IgnoreHolder) const;

	/** Resource's windows, sorted by From. Empty when nobody holds it. */
	TConstArrayView<FTaxiWindow> WindowsOn(const FTaxiResource& Resource) const;

private:
	/** Per resource, sorted by From, never conflicting. A resource with no windows has no entry. */
	TMap<FTaxiResource, TArray<FTaxiWindow>> Windows;

	/** See SetHeadway. 5 s by default: FTrafficRules::TaxiPlanMargin's default, so a table nobody configured agrees with one that was. */
	double Headway = 5.0;
};
