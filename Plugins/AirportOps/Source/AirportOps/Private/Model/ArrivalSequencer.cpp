#include "Model/ArrivalSequencer.h"

#include "Model/Flight.h"

UFlight* UArrivalSequencer::Next(TArrayView<UFlight* const> Queue,
	TFunctionRef<bool(const UFlight&)> IsRunwayBusy) const
{
	for (UFlight* Each : Queue)
	{
		if (Each != nullptr && !IsRunwayBusy(*Each))
		{
			return Each;
		}
	}
	return nullptr;
}
