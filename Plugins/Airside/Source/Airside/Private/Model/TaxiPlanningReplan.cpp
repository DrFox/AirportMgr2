// UTaxiPlanning's half for a plan that the world changed (taxi planning PR 3, spec 2026-10-02 §2): a rebuild's take-out
// with its order kept, a plan booked along a route already being driven, the unplanned record and its retry dates, and
// the re-time. The owner itself - table, clearances, order - is TaxiPlanning.cpp; split for rule 77's 800-line budget,
// one class's two concerns (#427's pattern).

#include "Model/TaxiPlanning.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"

void UTaxiPlanning::TakeAllForRebuild(TArray<int32>& OutOrder, TMap<int32, FTaxiClearance>& OutCleared)
{
	OutOrder.Reset();
	OutCleared = Clearances;

	// THE ORDER, TOPOLOGICALLY (Kahn): every holder after every holder booked ahead of it on any resource. The table is one
	// timeline, so the pairs have no cycle; ties - and any holder a broken table left in one - go by earliest window.
	TArray<TPair<int32, int32>> Pairs;
	Table.OrderPairs(Pairs);
	TMap<int32, double> Earliest;
	for (const TPair<int32, FTaxiClearance>& Each : Clearances)
	{
		double First = FTaxiReservations::Forever;
		for (const FTaxiPass& Pass : Each.Value.Plan.Passes)
		{
			for (const FTaxiWindow& Window : Table.WindowsOn(Pass.Resource))
			{
				First = Window.Holder == Each.Key ? FMath::Min(First, Window.From) : First;
			}
		}
		Earliest.Add(Each.Key, First);
	}
	TMap<int32, int32> Ahead;
	TMultiMap<int32, int32> Behind;
	for (const TPair<int32, double>& Each : Earliest)
	{
		Ahead.Add(Each.Key, 0);
	}
	for (const TPair<int32, int32>& Pair : Pairs)
	{
		if (Ahead.Contains(Pair.Key) && Ahead.Contains(Pair.Value))
		{
			Behind.Add(Pair.Key, Pair.Value);
			++Ahead[Pair.Value];
		}
	}
	while (Ahead.Num() > 0)
	{
		int32 Next = 0;
		double NextAt = TNumericLimits<double>::Max();
		bool bFree = false;
		for (const TPair<int32, int32>& Each : Ahead)
		{
			const bool bThisFree = Each.Value == 0;
			const double At = Earliest.FindRef(Each.Key);
			if ((bThisFree && !bFree) || (bThisFree == bFree && At < NextAt))
			{
				Next = Each.Key;
				NextAt = At;
				bFree = bThisFree;
			}
		}
		OutOrder.Add(Next);
		Ahead.Remove(Next);
		TArray<int32> After;
		Behind.MultiFind(Next, After);
		for (const int32 Holder : After)
		{
			if (int32* Count = Ahead.Find(Holder))
			{
				--*Count;
			}
		}
	}

	// TAKEN OUT SILENTLY: each is said again as it is re-planned, or as it falls to unplanned.
	Clearances.Reset();
	Table = FTaxiReservations();
	Bump(true);
}

bool UTaxiPlanning::BookAlong(const URoadNetwork& Network, int32 Holder, ETaxiClearanceKind Kind, ETaxiClearanceStage Stage,
	const FRoutePlan& Live, int32 Prefix, const FTaxiPlan& Tail, TConstArrayView<FTaxiPass> PrefixPasses,
	const FRoutePlan& PushRoute, TConstArrayView<FTaxiResource> PushOnly, double Now)
{
	if (bRefuseReplansForTest || Prefix < 0 || Prefix > Live.Steps.Num() || Tail.Legs.Num() != Live.Steps.Num() - Prefix)
	{
		return false;
	}
	// THE WHOLE ROUTE, as the agent drives it: the steps before the one it is on are behind it (no windows, legs at now),
	// the one it is on is the prefix's, the rest the tail's - so SameRoute holds and Track's indices are the agent's.
	FTaxiPlan Held = Tail;
	Held.Route = Live;
	Held.Legs.Reset();
	for (int32 Index = 0; Index < Prefix; ++Index)
	{
		const bool bOn = Index == Prefix - 1;
		Held.Legs.Add({ Live.Steps[Index].Edge, Live.Steps[Index].To, Now, bOn ? FMath::Max(Now, Tail.PushAt) : Now });
	}
	Held.Legs.Append(Tail.Legs);
	Held.MoveStarts.Reset();
	if (Prefix > 0)
	{
		Held.MoveStarts.Add(Prefix - 1);
	}
	for (const int32 Start : Tail.MoveStarts)
	{
		Held.MoveStarts.Add(Prefix + Start);
	}
	// ONE WINDOW PER RESOURCE: a prefix window that touches the tail's on the same resource joins it, as the planner's own
	// passes do (AddPass) - two windows of one holder that may not share have no order.
	Held.Passes.Reset();
	auto Join = [&Held](const FTaxiPass& Pass)
	{
		for (FTaxiPass& Mine : Held.Passes)
		{
			if (Mine.Resource == Pass.Resource && Mine.Window.From <= Pass.Window.To && Pass.Window.From <= Mine.Window.To)
			{
				Mine.Window.From = FMath::Min(Mine.Window.From, Pass.Window.From);
				Mine.Window.To = FMath::Max(Mine.Window.To, Pass.Window.To);
				Mine.Window.Way = Mine.Window.Way == Pass.Window.Way ? Pass.Window.Way : ETaxiWay::Any;
				return;
			}
		}
		Held.Passes.Add(Pass);
	};
	for (const FTaxiPass& Pass : PrefixPasses)
	{
		Join(Pass);
	}
	for (const FTaxiPass& Pass : Tail.Passes)
	{
		Join(Pass);
	}
	Held.PushWindows = TArray<FTaxiResource>(PushOnly);
	if (!Book(Network, Holder, Kind, Held, Stage, PushRoute, Now))
	{
		return false;
	}
	FTaxiClearance& Clearance = Clearances[Holder];
	// BEHIND IT ALREADY: nothing booked there to release, nor its start.
	Clearance.ReleasedThrough = Prefix - 2;
	Clearance.bStartReleased = Prefix > 0;
	if (Prefix > 0)
	{
		Clearance.Entered.Add(FTaxiResource::Edge(Live.Steps[Prefix - 1].Edge));
	}
	return true;
}

void UTaxiPlanning::MarkUnplanned(int32 Holder, ETaxiUnplanned Cause, const FString& Why, ETaxiClearanceKind Kind, ERouteErrand Errand)
{
	// ITS WINDOWS GO, silently - the line below is the event.
	Drop(Holder, nullptr);
	FTaxiUnplanned* Was = Unplanned.Find(Holder);
	if (Was == nullptr || Was->Why != Why || Was->Cause != Cause)
	{
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d unplanned - %s"), Holder, *Why);
	}
	FTaxiUnplanned& Now = Unplanned.FindOrAdd(Holder);
	bUnplannedChanged |= Was == nullptr || Now.Cause != Cause;
	Now.Cause = Cause;
	Now.Why = Why;
	Now.Kind = Kind;
	Now.Errand = Errand;
}

void UTaxiPlanning::ClearUnplanned(int32 Holder)
{
	bUnplannedChanged |= Unplanned.Remove(Holder) > 0;
}

TArray<int32> UTaxiPlanning::UnplannedHolders() const
{
	TArray<int32> Out;
	Unplanned.GetKeys(Out);
	return Out;
}

TArray<int32> UTaxiPlanning::UnplannedDueRetry(double Now) const
{
	// EVENT-DRIVEN, COALESCED - QueuedDueAsk's rule: when the table has moved since the last try, at most once a sim second.
	TArray<int32> Due;
	for (const TPair<int32, FTaxiUnplanned>& Each : Unplanned)
	{
		if (Each.Value.TriedAt < 0.0 || (Each.Value.TriedRevision != RevisionCount && Now - Each.Value.TriedAt >= 1.0))
		{
			Due.Add(Each.Key);
		}
	}
	return Due;
}

void UTaxiPlanning::NoteUnplannedTried(int32 Holder, double Now)
{
	if (FTaxiUnplanned* Each = Unplanned.Find(Holder))
	{
		Each->TriedAt = Now;
		Each->TriedRevision = RevisionCount;
	}
}

bool UTaxiPlanning::TakeUnplannedChanged()
{
	const bool bWas = bUnplannedChanged;
	bUnplannedChanged = false;
	return bWas;
}

bool UTaxiPlanning::Retime(int32 Holder, double Since, double Lag)
{
	TArray<FTaxiShift> Shifts;
	if (!(Lag > 0.0) || !Clearances.Contains(Holder) || !Table.ShiftLater(Holder, Since, Lag, Shifts))
	{
		NoteRefused(Holder, FString::Printf(TEXT("agent %d's re-time"), Holder),
			TEXT("the windows behind it cannot all move (one is held for ever)"));
		return false;
	}
	// THE PLANS CARRY THE TIMES TOO: legs, holds, arrival, push - moved exactly as their windows were, in the same order.
	TSet<int32> Moved;
	for (const FTaxiShift& Shift : Shifts)
	{
		FTaxiClearance* Clearance = Clearances.Find(Shift.Holder);
		if (Clearance == nullptr)
		{
			continue;
		}
		Moved.Add(Shift.Holder);
		auto Later = [&Shift](double& Time)
		{
			Time += Time > Shift.Since ? Shift.Delta : 0.0;
		};
		for (FTaxiLeg& Leg : Clearance->Plan.Legs)
		{
			Later(Leg.Leave);
			Later(Leg.Reach);
		}
		for (FTaxiHold& Hold : Clearance->Plan.Holds)
		{
			Later(Hold.From);
			Later(Hold.To);
		}
		Later(Clearance->Plan.Arrival);
		Later(Clearance->Plan.PushAt);
	}
	UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d re-timed +%.0f s (%d other plan(s) moved behind it)"), Holder, Lag,
		FMath::Max(0, Moved.Num() - 1));
	// A WAKE-UP (spec §2): time ahead of the late one's old windows is free now.
	Bump(true);
	return true;
}
