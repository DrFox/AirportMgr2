#include "Tool/ScreenPick.h"

namespace ScreenPick
{
	int32 NearestWithin(TConstArrayView<FVector2D> Points, const FVector2D& Cursor, double Radius)
	{
		int32 Best = INDEX_NONE;
		double BestSq = Radius * Radius;
		for (int32 I = 0; I < Points.Num(); ++I)
		{
			const double DSq = FVector2D::DistSquared(Points[I], Cursor);
			if (DSq <= BestSq)
			{
				BestSq = DSq;
				Best = I;
			}
		}
		return Best;
	}
}
