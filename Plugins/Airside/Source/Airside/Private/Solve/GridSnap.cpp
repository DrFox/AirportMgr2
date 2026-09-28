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

	namespace
	{
		FVector2D PerpOf(const FVector2D& Axis) { return FVector2D(-Axis.Y, Axis.X); }

		FVector2D ToLocal(const FGridFrame& Frame, const FVector2D& Q)
		{
			const FVector2D D = Q - Frame.Origin;
			return FVector2D(FVector2D::DotProduct(D, Frame.Axis), FVector2D::DotProduct(D, PerpOf(Frame.Axis)));
		}

		/** A direction, not a point: rotated, never shifted by the origin. */
		FVector2D DirToLocal(const FGridFrame& Frame, const FVector2D& V)
		{
			return FVector2D(FVector2D::DotProduct(V, Frame.Axis), FVector2D::DotProduct(V, PerpOf(Frame.Axis)));
		}

		FVector2D ToWorld(const FGridFrame& Frame, const FVector2D& L)
		{
			return Frame.Origin + Frame.Axis * L.X + PerpOf(Frame.Axis) * L.Y;
		}

		/** The world functions answer every frame that needs no map - see IsWorldAligned. */
		bool NeedsNoMap(const FGridFrame& Frame) { return Frame.IsWorldAligned() || !Frame.IsOn(); }
	}

	FGridFrame FGridFrame::World(double StepUu)
	{
		FGridFrame Frame;
		Frame.StepUu = StepUu;
		return Frame;
	}

	FGridFrame FGridFrame::Along(const FVector2D& Point, const FVector2D& Direction, double StepUu)
	{
		const FVector2D Unit = Direction.GetSafeNormal();
		if (Unit.IsZero())
		{
			return World(StepUu);
		}

		// THE FOLD BY QUARTER TURNS, (x, y) -> (y, -x): a swap and a negation, exact in IEEE
		// doubles, so the same line reached from any of its four directions folds to the same
		// bits. A trig fold (atan2, fmod, cos/sin) would not, and SameLines would then see four
		// grids where the player sees one.
		FVector2D Axis = Unit;
		for (int32 Turn = 0; Turn < 4 && !(Axis.X > 0.0 && Axis.Y >= 0.0); ++Turn)
		{
			Axis = FVector2D(Axis.Y, -Axis.X);
		}

		FGridFrame Frame;
		// -0 FOLDED TO +0 (x + 0.0 is +0 for x = -0), so an axis-aligned road gets the world's
		// own (1, 0) bits and, through y = 0, IsWorldAligned.
		Frame.Axis = FVector2D(Axis.X + 0.0, Axis.Y + 0.0);
		// THE FOOT OF THE WORLD ORIGIN ON THE LINE - see the header: both families pass through it.
		const FVector2D Foot = Point - Unit * FVector2D::DotProduct(Point, Unit);
		Frame.Origin = FVector2D(Foot.X + 0.0, Foot.Y + 0.0);
		Frame.StepUu = StepUu;
		return Frame;
	}

	bool FGridFrame::IsWorldAligned() const
	{
		return Origin.X == 0.0 && Origin.Y == 0.0 && Axis.X == 1.0 && Axis.Y == 0.0;
	}

	bool FGridFrame::SameLines(const FGridFrame& Other) const
	{
		return Origin == Other.Origin && Axis == Other.Axis && StepUu == Other.StepUu;
	}

	bool FGridFrame::SameGrid(const FGridFrame& Other) const
	{
		if (StepUu != Other.StepUu)
		{
			return false;
		}
		if (!IsOn())
		{
			return true;
		}
		constexpr double AxisTolerance = 1e-9;
		constexpr double LineToleranceUu = 1e-3;
		if (FMath::Abs(Axis.X - Other.Axis.X) > AxisTolerance || FMath::Abs(Axis.Y - Other.Axis.Y) > AxisTolerance)
		{
			return false;
		}
		// THE OTHER ORIGIN ON THIS FRAME'S LINES - both families - is the same grid: every line of
		// one passes through a grid point of the other.
		const FVector2D L = ToLocal(*this, Other.Origin);
		auto OnLine = [this](double V)
		{
			return FMath::Abs(V - FMath::RoundToDouble(V / StepUu) * StepUu) <= LineToleranceUu;
		};
		return OnLine(L.X) && OnLine(L.Y);
	}

	double FGridFrame::AxisDegrees() const
	{
		return FMath::RadiansToDegrees(FMath::Atan2(Axis.Y, Axis.X));
	}

	FVector2D Quantise(const FVector2D& Point, const FGridFrame& Frame)
	{
		if (NeedsNoMap(Frame))
		{
			return Quantise(Point, Frame.StepUu);
		}
		return ToWorld(Frame, Quantise(ToLocal(Frame, Point), Frame.StepUu));
	}

	bool NearestCrossingAlong(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, const FGridFrame& Frame, FVector2D& Out)
	{
		if (NeedsNoMap(Frame))
		{
			return NearestCrossingAlong(Origin, Direction, Near, Frame.StepUu, Out);
		}
		FVector2D Local;
		if (!NearestCrossingAlong(ToLocal(Frame, Origin), DirToLocal(Frame, Direction), ToLocal(Frame, Near),
			Frame.StepUu, Local))
		{
			return false;
		}
		Out = ToWorld(Frame, Local);
		return true;
	}

	bool NearestCrossingInRange(const FVector2D& Origin, const FVector2D& Direction,
		const FVector2D& Near, const FGridFrame& Frame, double TMin, double TMax, FVector2D& Out)
	{
		if (NeedsNoMap(Frame))
		{
			return NearestCrossingInRange(Origin, Direction, Near, Frame.StepUu, TMin, TMax, Out);
		}
		// T SURVIVES THE MAP: a rotation keeps lengths, so a range along the unit direction in the
		// world is the same range along its rotated image.
		FVector2D Local;
		if (!NearestCrossingInRange(ToLocal(Frame, Origin), DirToLocal(Frame, Direction), ToLocal(Frame, Near),
			Frame.StepUu, TMin, TMax, Local))
		{
			return false;
		}
		Out = ToWorld(Frame, Local);
		return true;
	}

	void PiecesInDisc(const FVector2D& Centre, double RadiusUu, const FGridFrame& Frame, TArray<FPiece>& Out)
	{
		if (NeedsNoMap(Frame))
		{
			PiecesInDisc(Centre, RadiusUu, Frame.StepUu, Out);
			return;
		}
		PiecesInDisc(ToLocal(Frame, Centre), RadiusUu, Frame.StepUu, Out);
		for (FPiece& Piece : Out)
		{
			Piece.From = ToWorld(Frame, Piece.From);
			Piece.To = ToWorld(Frame, Piece.To);
		}
	}
}
