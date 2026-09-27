#include "Model/BuildPurse.h"

double FBuildLine::Amount() const
{
	const double Raw = Quantity * RatePerUnit
		* (Pavement.IsSet() ? Pavement::RateFactor(*Pavement) : 1.0);
	// CLAMPED AT ZERO - see this method's own header comment.
	return FMath::Max(0.0, Raw);
}

double FBuildQuote::BaseAmount() const
{
	double Sum = 0.0;
	for (const FBuildLine& Line : Lines)
	{
		Sum += Line.Amount();
	}
	return Sum;
}
