#include "Model/TaxiReservations.h"

#include <cmath>

#include "Algo/BinarySearch.h"

namespace
{
	/** Half-open overlap: [10,20) and [20,30) do not overlap - see FTaxiWindow. */
	bool Overlaps(double FromA, double ToA, double FromB, double ToB)
	{
		return FromA < ToB && FromB < ToA;
	}

	/** Whether any window in Sorted overlaps [From, To), IgnoreHolder's excepted (0 ignores nobody). */
	bool AnyOverlap(TConstArrayView<FTaxiWindow> Sorted, double From, double To, int32 IgnoreHolder)
	{
		for (const FTaxiWindow& Other : Sorted)
		{
			// Sorted by From: nothing past here can start before To.
			if (Other.From >= To)
			{
				break;
			}
			if ((IgnoreHolder == 0 || Other.Holder != IgnoreHolder) && Overlaps(From, To, Other.From, Other.To))
			{
				return true;
			}
		}
		return false;
	}

	/** Whether Window may not share (FTaxiReservations::MayShare) with some window of Sorted - IgnoreHolder's excepted. */
	bool AnyConflict(TConstArrayView<FTaxiWindow> Sorted, const FTaxiWindow& Window, int32 IgnoreHolder, double Headway)
	{
		for (const FTaxiWindow& Other : Sorted)
		{
			// Sorted by From: nothing past here can start before Window ends, so nothing past here overlaps it.
			if (Other.From >= Window.To)
			{
				break;
			}
			if ((IgnoreHolder == 0 || Other.Holder != IgnoreHolder) && !FTaxiReservations::MayShare(Other, Window, Headway))
			{
				return true;
			}
		}
		return false;
	}
}

bool FTaxiReservations::MayShare(const FTaxiWindow& A, const FTaxiWindow& B, double InHeadway)
{
	if (!Overlaps(A.From, A.To, B.From, B.To))
	{
		return true;
	}
	if (A.Way == ETaxiWay::Any || A.Way != B.Way)
	{
		return false;
	}
	// FIFO, A HEADWAY APART AT BOTH ENDS: whichever enters first also leaves first. Entering first but leaving later
	// is an overtake on one centreline - and a follower that cannot overtake would have to wait inside the edge for
	// a leader the plan says has gone.
	const double H = FMath::Max(InHeadway, 0.001);
	return (B.From >= A.From + H && B.To >= A.To + H) || (A.From >= B.From + H && A.To >= B.To + H);
}

bool FTaxiReservations::BookWindow(const FTaxiResource& Resource, const FTaxiWindow& Window)
{
	if (!(Window.From < Window.To))
	{
		return false;
	}

	TArray<FTaxiWindow>& On = Windows.FindOrAdd(Resource);

	// EVERY holder, its own included (IgnoreHolder 0): see the class comment - two windows of one resource that may
	// not share have no order, and order is what the table exists to give.
	if (AnyConflict(On, Window, 0, Headway))
	{
		if (On.Num() == 0)
		{
			Windows.Remove(Resource);
		}
		return false;
	}

	const int32 At = Algo::LowerBoundBy(On, Window.From, &FTaxiWindow::From);
	On.Insert(Window, At);
	return true;
}

bool FTaxiReservations::BookPasses(TConstArrayView<FTaxiPass> Passes)
{
	// CHECK EVERYTHING FIRST, then write: the all-or-nothing the header promises. Against the table, and against the
	// passes before each one - a plan that visits a resource twice in overlapping windows is as unorderable as two
	// aircraft doing it.
	for (int32 Index = 0; Index < Passes.Num(); ++Index)
	{
		const FTaxiPass& Pass = Passes[Index];
		if (!(Pass.Window.From < Pass.Window.To))
		{
			return false;
		}
		if (const TArray<FTaxiWindow>* On = Windows.Find(Pass.Resource);
			On != nullptr && AnyConflict(*On, Pass.Window, 0, Headway))
		{
			return false;
		}
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			const FTaxiPass& Before = Passes[Earlier];
			if (Before.Resource == Pass.Resource && !MayShare(Before.Window, Pass.Window, Headway))
			{
				return false;
			}
		}
	}

	for (const FTaxiPass& Pass : Passes)
	{
		// Cannot refuse now - every pass was checked above against the same state.
		verify(BookWindow(Pass.Resource, Pass.Window));
	}
	return true;
}

int32 FTaxiReservations::ReleaseHolder(int32 Holder)
{
	int32 Removed = 0;
	for (auto It = Windows.CreateIterator(); It; ++It)
	{
		Removed += It.Value().RemoveAll([Holder](const FTaxiWindow& Window) { return Window.Holder == Holder; });
		if (It.Value().Num() == 0)
		{
			It.RemoveCurrent();
		}
	}
	return Removed;
}

int32 FTaxiReservations::ReleaseHolderOn(const FTaxiResource& Resource, int32 Holder)
{
	TArray<FTaxiWindow>* On = Windows.Find(Resource);
	if (On == nullptr)
	{
		return 0;
	}
	const int32 Removed = On->RemoveAll([Holder](const FTaxiWindow& Window) { return Window.Holder == Holder; });
	if (On->Num() == 0)
	{
		Windows.Remove(Resource);
	}
	return Removed;
}

bool FTaxiReservations::ReleaseFirstOn(const FTaxiResource& Resource, int32 Holder)
{
	TArray<FTaxiWindow>* On = Windows.Find(Resource);
	const int32 First = On != nullptr ? On->IndexOfByPredicate([Holder](const FTaxiWindow& Window) { return Window.Holder == Holder; })
		: INDEX_NONE;
	if (First == INDEX_NONE)
	{
		return false;
	}
	On->RemoveAt(First);
	if (On->Num() == 0)
	{
		Windows.Remove(Resource);
	}
	return true;
}

bool FTaxiReservations::PullForward(const FTaxiResource& Resource, int32 Holder, double Earliest)
{
	TArray<FTaxiWindow>* On = Windows.Find(Resource);
	const int32 Mine = On != nullptr ? On->IndexOfByPredicate([Holder](const FTaxiWindow& Window) { return Window.Holder == Holder; })
		: INDEX_NONE;
	if (Mine == INDEX_NONE)
	{
		return false;
	}
	FTaxiWindow& Own = (*On)[Mine];
	const double H = FMath::Max(Headway, 0.001);
	// NO EARLIER THAN THE WINDOWS AHEAD ALLOW - so MayShare still holds with each, and the array stays sorted: every bound
	// below is at or after the From of the window it comes from.
	double From = Earliest;
	for (int32 Index = 0; Index < Mine; ++Index)
	{
		const FTaxiWindow& Ahead = (*On)[Index];
		const bool bFollowing = Own.Way != ETaxiWay::Any && Ahead.Way == Own.Way;
		From = FMath::Max(From, bFollowing ? Ahead.From + H : Ahead.To);
	}
	if (!(From < Own.From) || !(From < Own.To))
	{
		return false;
	}
	Own.From = From;
	return true;
}

namespace
{
	/** Holder's windows still held after Since, moved Delta later: those starting after it shifted, one straddling it stretched. */
	void ShiftOne(TMap<FTaxiResource, TArray<FTaxiWindow>>& Table, int32 Holder, double Since, double Delta)
	{
		for (TPair<FTaxiResource, TArray<FTaxiWindow>>& Each : Table)
		{
			for (FTaxiWindow& Window : Each.Value)
			{
				if (Window.Holder != Holder || !(Window.To > Since))
				{
					continue;
				}
				if (Window.From > Since)
				{
					Window.From += Delta;
				}
				if (Window.To < FTaxiReservations::Forever)
				{
					Window.To += Delta;
				}
			}
		}
	}

	/**
	 * How much later Behind must go to stay behind Ahead: a headway after it the same way along an edge (entering and
	 * leaving), after its end otherwise. Forever: it cannot - Ahead holds for ever and they may not share.
	 */
	double NeedBehind(const FTaxiWindow& Ahead, const FTaxiWindow& Behind, double H)
	{
		constexpr double Forever = FTaxiReservations::Forever;
		if (Ahead.Way != ETaxiWay::Any && Behind.Way == Ahead.Way)
		{
			double Need = Ahead.From + H - Behind.From;
			if (Behind.To < Forever)
			{
				Need = FMath::Max(Need, Ahead.To >= Forever ? Forever : Ahead.To + H - Behind.To);
			}
			return Need;
		}
		return Ahead.To >= Forever ? (Behind.From < Forever ? Forever : 0.0) : Ahead.To - Behind.From;
	}
}

bool FTaxiReservations::ShiftLater(int32 Holder, double Since, double Delta, double Now, TArray<FTaxiShift>& OutShifts)
{
	return ShiftLater(Holder, Since, Delta, Now, [](int32) { return false; }, OutShifts);
}

bool FTaxiReservations::ShiftLater(int32 Holder, double Since, double Delta, double Now, TFunctionRef<bool(int32 Holder)> Immovable,
	TArray<FTaxiShift>& OutShifts)
{
	OutShifts.Reset();
	if (!(Delta > 0.0))
	{
		return true;
	}
	const double H = FMath::Max(Headway, 0.001);

	// ON A COPY, ALL OR NOTHING: a cascade that meets a window it cannot move - one behind a for-ever window it may not share
	// with - leaves the table as it was.
	TMap<FTaxiResource, TArray<FTaxiWindow>> Trial = Windows;
	TArray<FTaxiShift> Shifts;
	ShiftOne(Trial, Holder, Since, Delta);
	Shifts.Add({ Holder, Since, Delta });

	// TO A FIXED POINT, IN THE ORDER AS BOOKED: each resource's array keeps its booked order (it is re-sorted only at the
	// end), so "behind" is the array's own order. The first window found that a shift has put level with or ahead of one it
	// was behind moves its holder on by exactly the least that restores it - asked again from the moved state, so nothing is
	// pushed twice for one cause. The order is one timeline, so this ends; the bound guards a table that broke that.
	int32 Budget = 64;
	for (const TPair<FTaxiResource, TArray<FTaxiWindow>>& Each : Trial)
	{
		Budget += 4 * Each.Value.Num();
	}
	for (bool bMoved = true; bMoved;)
	{
		bMoved = false;
		for (const TPair<FTaxiResource, TArray<FTaxiWindow>>& Each : Trial)
		{
			const TArray<FTaxiWindow>& On = Each.Value;
			for (int32 A = 0; A < On.Num() && !bMoved; ++A)
			{
				for (int32 B = A + 1; B < On.Num() && !bMoved; ++B)
				{
					if (On[A].Holder == On[B].Holder)
					{
						continue;
					}
					const double Need = NeedBehind(On[A], On[B], H);
					if (Need <= 0.0)
					{
						continue;
					}
					if (Need >= Forever || --Budget < 0)
					{
						return false;
					}
					const FTaxiShift Cascade{ On[B].Holder, std::nextafter(On[B].From, Always), Need };
					ShiftOne(Trial, Cascade.Holder, Cascade.Since, Cascade.Delta);
					Shifts.Add(Cascade);
					bMoved = true;
				}
			}
			if (bMoved)
			{
				break;
			}
		}
	}

	for (TPair<FTaxiResource, TArray<FTaxiWindow>>& Each : Trial)
	{
		Each.Value.StableSort([](const FTaxiWindow& A, const FTaxiWindow& B) { return A.From < B.From; });
	}
	Windows = MoveTemp(Trial);
	OutShifts = MoveTemp(Shifts);
	return true;
}

void FTaxiReservations::OrderPairs(TArray<TPair<int32, int32>>& Out) const
{
	Out.Reset();
	for (const TPair<FTaxiResource, TArray<FTaxiWindow>>& Each : Windows)
	{
		for (int32 Index = 1; Index < Each.Value.Num(); ++Index)
		{
			if (Each.Value[Index - 1].Holder != Each.Value[Index].Holder)
			{
				Out.Emplace(Each.Value[Index - 1].Holder, Each.Value[Index].Holder);
			}
		}
	}
}

void FTaxiReservations::FreeIntervals(const FTaxiResource& Resource, int32 IgnoreHolder, TArray<FTaxiInterval>& Out) const
{
	Out.Reset();
	double Cursor = Always;
	if (const TArray<FTaxiWindow>* On = Windows.Find(Resource))
	{
		for (const FTaxiWindow& Window : *On)
		{
			if (IgnoreHolder != 0 && Window.Holder == IgnoreHolder)
			{
				continue;
			}
			// STRICTLY after the cursor: a window that starts where the last ended leaves no gap to plan into.
			if (Window.From > Cursor)
			{
				Out.Add({ Cursor, Window.From });
			}
			Cursor = FMath::Max(Cursor, Window.To);
		}
	}
	if (Cursor < Forever)
	{
		Out.Add({ Cursor, Forever });
	}
}

bool FTaxiReservations::IsFree(const FTaxiResource& Resource, double From, double To, int32 IgnoreHolder) const
{
	const TArray<FTaxiWindow>* On = Windows.Find(Resource);
	return On == nullptr || !AnyOverlap(*On, From, To, IgnoreHolder);
}

TConstArrayView<FTaxiWindow> FTaxiReservations::WindowsOn(const FTaxiResource& Resource) const
{
	const TArray<FTaxiWindow>* On = Windows.Find(Resource);
	return On != nullptr ? TConstArrayView<FTaxiWindow>(*On) : TConstArrayView<FTaxiWindow>();
}

bool FTaxiReservations::EarliestFit(const FTaxiResource& Resource, ETaxiWay Way, double From, double To, int32 IgnoreHolder,
	double& OutShift) const
{
	OutShift = 0.0;
	const TArray<FTaxiWindow>* On = Windows.Find(Resource);
	if (On == nullptr)
	{
		return true;
	}
	const bool bForever = To >= Forever;
	const double Length = bForever ? 0.0 : To - From;
	const double H = FMath::Max(Headway, 0.001);

	// EACH CONFLICT RESOLVED FORWARD, IN ABSOLUTE TIME: the least start that clears the first conflicting window, then look
	// again. Once the window is after another one - FIFO behind it, or past its end - every later start keeps it there, so
	// each window is resolved once; the guard's slack is for a start that lands an ulp short of a boundary, which is
	// pushed on by one representable step rather than trusted (TouchingBoundaryFractionalReach's sweep found them).
	FTaxiWindow Probe;
	Probe.From = From;
	Probe.Way = Way;
	for (int32 Guard = 0; Guard <= 4 * (On->Num() + 1); ++Guard)
	{
		Probe.To = bForever ? Forever : Probe.From + Length;
		const FTaxiWindow* Blocking = nullptr;
		for (const FTaxiWindow& Other : *On)
		{
			if (Other.From >= Probe.To)
			{
				break;
			}
			if ((IgnoreHolder == 0 || Other.Holder != IgnoreHolder) && !MayShare(Other, Probe, Headway))
			{
				Blocking = &Other;
				break;
			}
		}
		if (Blocking == nullptr)
		{
			// THE SHIFT AS THE CALLER WILL ADD IT: at least one representable step whenever the start moved at all.
			OutShift = Probe.From > From ? FMath::Max(Probe.From - From, std::nextafter(0.0, 1.0)) : 0.0;
			return true;
		}
		if (Blocking->To >= Forever)
		{
			return false;
		}
		// Past its end - or, the same way, FIFO behind it: in a headway after it, and out a headway after it.
		double Start = Blocking->To;
		if (Way != ETaxiWay::Any && Blocking->Way == Way)
		{
			Start = FMath::Min(Start, bForever ? Blocking->From + H : FMath::Max(Blocking->From + H, Blocking->To + H - Length));
		}
		Probe.From = Start > Probe.From ? Start : std::nextafter(Probe.From, Forever);
	}
	return false;
}

double FTaxiReservations::LatestEnd(const FTaxiResource& Resource, ETaxiWay Way, double From, int32 IgnoreHolder) const
{
	const TArray<FTaxiWindow>* On = Windows.Find(Resource);
	if (On == nullptr)
	{
		return Forever;
	}
	const double H = FMath::Max(Headway, 0.001);
	double Latest = Forever;
	for (const FTaxiWindow& Other : *On)
	{
		if ((IgnoreHolder != 0 && Other.Holder == IgnoreHolder) || Other.From < From)
		{
			// Started before ours: it either has gone by From, or is one we follow the same way - which bounds how EARLY
			// ours may end, never how late.
			continue;
		}
		const bool bFollowsUs = Way != ETaxiWay::Any && Other.Way == Way && Other.From >= From + H;
		Latest = FMath::Min(Latest, bFollowsUs ? Other.To - H : Other.From);
	}
	return Latest;
}

int32 FTaxiReservations::PlaceAt(const FTaxiResource& Resource, double At, int32 IgnoreHolder) const
{
	int32 Place = 0;
	if (const TArray<FTaxiWindow>* On = Windows.Find(Resource))
	{
		for (const FTaxiWindow& Other : *On)
		{
			if (Other.From >= At)
			{
				break;
			}
			Place += (IgnoreHolder != 0 && Other.Holder == IgnoreHolder) ? 0 : 1;
		}
	}
	return Place;
}

double FTaxiReservations::NextPlaceAfter(const FTaxiResource& Resource, ETaxiWay Way, double From, int32 IgnoreHolder) const
{
	if (const TArray<FTaxiWindow>* On = Windows.Find(Resource))
	{
		for (const FTaxiWindow& Other : *On)
		{
			if (Other.From < From || (IgnoreHolder != 0 && Other.Holder == IgnoreHolder))
			{
				continue;
			}
			const bool bSameWay = Way != ETaxiWay::Any && Other.Way == Way;
			return bSameWay ? Other.From + FMath::Max(Headway, 0.001) : Other.To;
		}
	}
	return Forever;
}
