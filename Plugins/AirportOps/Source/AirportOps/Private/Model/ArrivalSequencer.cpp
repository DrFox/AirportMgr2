#include "Model/ArrivalSequencer.h"

#include "Model/Flight.h"

UFlight* UArrivalSequencer::Next(TArrayView<UFlight* const> Queue,
	TFunctionRef<bool(const UFlight&)> CanClear) const
{
	for (UFlight* Each : Queue)
	{
		if (Each != nullptr && CanClear(*Each))
		{
			return Each;
		}
	}
	return nullptr;
}
