#include "Model/TaxiReservations.h"

// RED-PHASE STUBS (batched testing): compile, and fail each test on its own assertion.

bool FTaxiReservations::BookWindow(const FTaxiResource& Resource, const FTaxiWindow& Window)
{
	return false;
}

bool FTaxiReservations::BookPasses(TConstArrayView<FTaxiPass> Passes)
{
	return false;
}

int32 FTaxiReservations::ReleaseHolder(int32 Holder)
{
	return 0;
}

int32 FTaxiReservations::ReleaseHolderOn(const FTaxiResource& Resource, int32 Holder)
{
	return 0;
}

void FTaxiReservations::FreeIntervals(const FTaxiResource& Resource, int32 IgnoreHolder, TArray<FTaxiInterval>& Out) const
{
	Out.Reset();
}

bool FTaxiReservations::IsFree(const FTaxiResource& Resource, double From, double To, int32 IgnoreHolder) const
{
	return false;
}

TConstArrayView<FTaxiWindow> FTaxiReservations::WindowsOn(const FTaxiResource& Resource) const
{
	return TConstArrayView<FTaxiWindow>();
}
