#include "Solve/GridSnap.h"

namespace GridSnap
{
	FVector2D Quantise(const FVector2D& Point, double StepUu)
	{
		if (StepUu <= 0.0)
		{
			return Point;
		}
		return FVector2D(
			FMath::RoundToDouble(Point.X / StepUu) * StepUu,
			FMath::RoundToDouble(Point.Y / StepUu) * StepUu);
	}

	bool NearestCrossingAlong(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, double StepUu, FVector2D& Out)
	{
		const FVector2D Unit = Direction.GetSafeNormal();
		if (StepUu <= 0.0 || Unit.IsZero())
		{
			return false;
		}

		// The foot of Near on the line, as a parameter: the crossing chosen is the one nearest
		// to where the player is pointing ALONG the line, never across it.
		const double Foot = FVector2D::DotProduct(Near - Origin, Unit);

		// A component this small crosses the family so rarely that the "nearest" crossing is
		// kilometres away; treating the line as parallel to it is the honest answer.
		constexpr double ParallelEpsilon = 1e-9;

		bool bFound = false;
		double BestT = 0.0;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const double Component = Axis == 0 ? Unit.X : Unit.Y;
			if (FMath::Abs(Component) <= ParallelEpsilon)
			{
				continue;
			}
			const double Start = Axis == 0 ? Origin.X : Origin.Y;
			const double AtFoot = Start + Foot * Component;
			const double Line = FMath::RoundToDouble(AtFoot / StepUu) * StepUu;
			const double T = (Line - Start) / Component;
			if (!bFound || FMath::Abs(T - Foot) < FMath::Abs(BestT - Foot))
			{
				BestT = T;
				bFound = true;
			}
		}

		if (!bFound)
		{
			return false;
		}
		Out = Origin + Unit * BestT;
		return true;
	}

	bool NearestCrossingInRange(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, double StepUu, double TMin, double TMax, FVector2D& Out)
	{
		const FVector2D Unit = Direction.GetSafeNormal();
		if (StepUu <= 0.0 || Unit.IsZero() || TMin > TMax)
		{
			return false;
		}

		const double Foot = FVector2D::DotProduct(Near - Origin, Unit);
		const double Clamped = FMath::Clamp(Foot, TMin, TMax);
		constexpr double ParallelEpsilon = 1e-9;

		// PER FAMILY, THE TWO LINES EITHER SIDE OF THE CLAMPED FOOT. If the clamped foot is in
		// range, the nearest in-range crossing of a family is one of those two; if neither is in
		// range, the whole range lies between two consecutive lines of that family and it has
		// none to offer.
		bool bFound = false;
		double BestT = 0.0;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const double Component = Axis == 0 ? Unit.X : Unit.Y;
			if (FMath::Abs(Component) <= ParallelEpsilon)
			{
				continue;
			}
			const double Start = Axis == 0 ? Origin.X : Origin.Y;
			const double K = (Start + Clamped * Component) / StepUu;
			for (const double Line : { FMath::FloorToDouble(K), FMath::CeilToDouble(K) })
			{
				const double T = (Line * StepUu - Start) / Component;
				if (T < TMin || T > TMax)
				{
					continue;
				}
				if (!bFound || FMath::Abs(T - Foot) < FMath::Abs(BestT - Foot))
				{
					BestT = T;
					bFound = true;
				}
			}
		}

		if (!bFound)
		{
			return false;
		}
		Out = Origin + Unit * BestT;
		return true;
	}

	void PiecesInDisc(const FVector2D& Centre, double RadiusUu, double StepUu, TArray<FPiece>& Out)
	{
		Out.Reset();
		if (StepUu <= 0.0 || RadiusUu <= 0.0)
		{
			return;
		}

		// Axis 0: lines X = k * Step, running along Y. Axis 1: lines Y = k * Step, along X.
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const double C = Axis == 0 ? Centre.X : Centre.Y;
			const double Across = Axis == 0 ? Centre.Y : Centre.X;
			const int64 First = static_cast<int64>(FMath::CeilToDouble((C - RadiusUu) / StepUu));
			const int64 Last = static_cast<int64>(FMath::FloorToDouble((C + RadiusUu) / StepUu));
			for (int64 K = First; K <= Last; ++K)
			{
				const double At = static_cast<double>(K) * StepUu;
				const double Offset = At - C;
				const double HalfChordSquared = RadiusUu * RadiusUu - Offset * Offset;
				// A tangent line touches the disc at one point: nothing to draw.
				if (HalfChordSquared <= 0.0)
				{
					continue;
				}
				const double HalfChord = FMath::Sqrt(HalfChordSquared);
				const int32 Count = FMath::Max(1, FMath::CeilToInt(2.0 * HalfChord / StepUu));
				const double Length = 2.0 * HalfChord / Count;
				// MODULO ON THE INTEGER K, sign-safe: -5 % 5 is 0 in C++, and so is 5 % 5.
				const bool bMajor = K % 5 == 0;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const double S0 = Across - HalfChord + Length * Index;
					const double S1 = S0 + Length;
					FPiece Piece;
					Piece.From = Axis == 0 ? FVector2D(At, S0) : FVector2D(S0, At);
					Piece.To = Axis == 0 ? FVector2D(At, S1) : FVector2D(S1, At);
					Piece.bMajor = bMajor;
					Out.Add(Piece);
				}
			}
		}
	}
}
