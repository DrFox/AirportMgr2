#include "Model/PassingOrder.h"

int32 FPassingOrder::WaitingFor(const FTaxiReservations& Table, int32 Holder, const FTaxiResource& Resource,
	TFunctionRef<bool(int32 Other)> HasEntered)
{
	const TConstArrayView<FTaxiWindow> Windows = Table.WindowsOn(Resource);
	const int32 Mine = Windows.IndexOfByPredicate([Holder](const FTaxiWindow& Window) { return Window.Holder == Holder; });
	if (Mine == INDEX_NONE)
	{
		return 0;
	}

	// AHEAD = BOOKED EARLIER: the table keeps a resource's windows sorted by From, so the order IS the array. A window
	// still in the table is one whose holder has not left - release removes it as the tail clears (UTaxiPlanning::Track).
	const FTaxiWindow& Own = Windows[Mine];
	for (int32 Index = 0; Index < Mine; ++Index)
	{
		const FTaxiWindow& Ahead = Windows[Index];
		if (Ahead.Holder == Holder)
		{
			continue;
		}
		// THE SAME WAY DOWN AN EDGE, the leader need only have ENTERED: the two may share it in FIFO order (MayShare),
		// and the claim pass keeps them a gap apart. Any other window ahead must have LEFT.
		const bool bFollowing = Own.Way != ETaxiWay::Any && Ahead.Way == Own.Way;
		if (!bFollowing || !HasEntered(Ahead.Holder))
		{
			return Ahead.Holder;
		}
	}
	return 0;
}
