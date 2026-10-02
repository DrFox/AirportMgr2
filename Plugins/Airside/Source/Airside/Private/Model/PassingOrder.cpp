#include "Model/PassingOrder.h"

int32 FPassingOrder::WaitingFor(const FTaxiReservations& Table, int32 Holder, const FTaxiResource& Resource,
	TFunctionRef<bool(int32 Other)> HasEntered)
{
	return 0;
}
