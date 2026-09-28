#include "UI/WindowSnap.h"

namespace
{
	/** The smallest signed shift, at most Distance, that lands one of Edges on one of Targets; 0 if none. */
	double Shift(std::initializer_list<double> Edges, const TArray<double>& Targets, double Distance)
	{
		double Best = 0.0;
		double BestAbs = TNumericLimits<double>::Max();
		for (const double T : Targets)
		{
			for (const double E : Edges)
			{
				const double D = T - E;
				if (FMath::Abs(D) <= Distance && FMath::Abs(D) < BestAbs)
				{
					Best = D;
					BestAbs = FMath::Abs(D);
				}
			}
		}
		return Best;
	}

	/** Bounds' edges plus the edges of every Other that faces a window at TopLeft/Size. */
	void Targets(FVector2D TopLeft, FVector2D Size, const FBox2D& Bounds, TConstArrayView<FBox2D> Others,
		double Distance, TArray<double>& OutX, TArray<double>& OutY)
	{
		OutX = { Bounds.Min.X, Bounds.Max.X };
		OutY = { Bounds.Min.Y, Bounds.Max.Y };
		for (const FBox2D& O : Others)
		{
			const bool bRowsMeet = O.Min.Y <= TopLeft.Y + Size.Y + Distance && O.Max.Y >= TopLeft.Y - Distance;
			const bool bColumnsMeet = O.Min.X <= TopLeft.X + Size.X + Distance && O.Max.X >= TopLeft.X - Distance;
			if (bRowsMeet) { OutX.Add(O.Min.X); OutX.Add(O.Max.X); }
			if (bColumnsMeet) { OutY.Add(O.Min.Y); OutY.Add(O.Max.Y); }
		}
	}

	FVector2D Clamp(FVector2D P, FVector2D Size, const FBox2D& Bounds)
	{
		return FVector2D(
			FMath::Clamp(P.X, Bounds.Min.X, FMath::Max(Bounds.Min.X, Bounds.Max.X - Size.X)),
			FMath::Clamp(P.Y, Bounds.Min.Y, FMath::Max(Bounds.Min.Y, Bounds.Max.Y - Size.Y)));
	}
}

FVector2D WindowSnap::Place(FVector2D Proposed, FVector2D Size, const FBox2D& Bounds,
	TConstArrayView<FBox2D> Others, double Distance)
{
	FVector2D P = Clamp(Proposed, Size, Bounds);
	TArray<double> Xs, Ys;
	Targets(P, Size, Bounds, Others, Distance, Xs, Ys);
	P.X += Shift({ P.X, P.X + Size.X }, Xs, Distance);
	P.Y += Shift({ P.Y, P.Y + Size.Y }, Ys, Distance);
	// Clamped again: a neighbour's edge can sit outside the bounds after the viewport shrank.
	return Clamp(P, Size, Bounds);
}

FVector2D WindowSnap::Resize(FVector2D TopLeft, FVector2D Proposed, FVector2D MinSize,
	const FBox2D& Bounds, TConstArrayView<FBox2D> Others, double Distance)
{
	auto Fit = [&](FVector2D S)
	{
		return FVector2D(
			FMath::Max(MinSize.X, FMath::Min(S.X, Bounds.Max.X - TopLeft.X)),
			FMath::Max(MinSize.Y, FMath::Min(S.Y, Bounds.Max.Y - TopLeft.Y)));
	};
	FVector2D S = Fit(Proposed);
	TArray<double> Xs, Ys;
	Targets(TopLeft, S, Bounds, Others, Distance, Xs, Ys);
	S.X += Shift({ TopLeft.X + S.X }, Xs, Distance);
	S.Y += Shift({ TopLeft.Y + S.Y }, Ys, Distance);
	return Fit(S);
}
