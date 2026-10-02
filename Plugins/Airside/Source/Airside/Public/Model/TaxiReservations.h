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
};

/** A stretch of time when a resource is free: [Start, End). End == FTaxiReservations::Forever is open-ended. */
struct FTaxiInterval
{
	double Start = 0.0;
	double End = 0.0;
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
 * plan, an aircraft or a priority is; FTaxiPlanner reads it, the planning owner (UTaxiPlanning,
 * PR 2 of the spec's four) writes it.
 *
 * WINDOWS ON ONE RESOURCE NEVER OVERLAP, whoever holds them, the holder's own included: the table
 * is what order enforcement is derived from (FPassingOrder, PR 2), and two windows of one
 * resource that overlap have no order. A refused booking leaves the table untouched.
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
	 * Books Window on Resource. False, and nothing changed, when the window has no length
	 * (From >= To) or overlaps any window already there.
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

	/** Removes Holder's windows on Resource only - the tail clearing one resource. Returns how many went. */
	int32 ReleaseHolderOn(const FTaxiResource& Resource, int32 Holder);

	/**
	 * Resource's free intervals, earliest first, treating IgnoreHolder's own windows as free -
	 * so a holder re-planning is not blocked by the plan it is replacing. Never empty: a
	 * resource nobody holds is one interval [Always, Forever). Touching windows leave no
	 * zero-length gap between them.
	 */
	void FreeIntervals(const FTaxiResource& Resource, int32 IgnoreHolder, TArray<FTaxiInterval>& Out) const;

	/** Whether no window other than IgnoreHolder's overlaps [From, To). */
	bool IsFree(const FTaxiResource& Resource, double From, double To, int32 IgnoreHolder) const;

	/** Resource's windows, sorted by From. Empty when nobody holds it. */
	TConstArrayView<FTaxiWindow> WindowsOn(const FTaxiResource& Resource) const;

private:
	/** Per resource, sorted by From, never overlapping. A resource with no windows has no entry. */
	TMap<FTaxiResource, TArray<FTaxiWindow>> Windows;
};
