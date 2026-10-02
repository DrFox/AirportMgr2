#include "Model/FuelSupply.h"

#include "Model/ServiceRolePolicy.h"

double UFuelSupply::Capacity() const
{
	return CapacityOf ? FMath::Max(CapacityOf(), 0.0) : TNumericLimits<double>::Max();
}

bool UFuelSupply::IsDry() const
{
	return Available() < FFuelRolePolicy::FuelledWithinLitres;
}

double UFuelSupply::Draw(double Litres)
{
	const double Granted = FMath::Clamp(Litres, 0.0, Available());
	StockLitres -= Granted;
	return Granted;
}

double UFuelSupply::Receive(double Litres)
{
	const double Added = FMath::Clamp(Litres, 0.0, FreeSpace());
	StockLitres += Added;
	return Added;
}
