#include "Model/TaxiReservations.h"

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
}

bool FTaxiReservations::BookWindow(const FTaxiResource& Resource, const FTaxiWindow& Window)
{
	if (!(Window.From < Window.To))
	{
		return false;
	}

	TArray<FTaxiWindow>& On = Windows.FindOrAdd(Resource);

	// EVERY holder, its own included (IgnoreHolder 0): see the class comment - two windows of one resource that
	// overlap have no order, and order is what the table exists to give.
	if (AnyOverlap(On, Window.From, Window.To, 0))
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
			On != nullptr && AnyOverlap(*On, Pass.Window.From, Pass.Window.To, 0))
		{
			return false;
		}
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			const FTaxiPass& Before = Passes[Earlier];
			if (Before.Resource == Pass.Resource
				&& Overlaps(Pass.Window.From, Pass.Window.To, Before.Window.From, Before.Window.To))
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

// ---- PR 2 STUBS (red batch) ----
bool FTaxiReservations::MayShare(const FTaxiWindow& A, const FTaxiWindow& B, double InHeadway)
{
	return !(A.From < B.To && B.From < A.To);
}

bool FTaxiReservations::EarliestFit(const FTaxiResource& Resource, ETaxiWay Way, double From, double To, int32 IgnoreHolder,
	double& OutShift) const
{
	OutShift = 0.0;
	return false;
}

double FTaxiReservations::LatestEnd(const FTaxiResource& Resource, ETaxiWay Way, double From, int32 IgnoreHolder) const
{
	return Forever;
}

int32 FTaxiReservations::PlaceAt(const FTaxiResource& Resource, double At, int32 IgnoreHolder) const
{
	return 0;
}

double FTaxiReservations::NextPlaceAfter(const FTaxiResource& Resource, ETaxiWay Way, double From, int32 IgnoreHolder) const
{
	return Forever;
}
