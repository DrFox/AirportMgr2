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

	namespace
	{
		/**
		 * Slab test: the segment parameter in [0, 1] at which A->B enters Box, or false.
		 * Parameterised on the SEGMENT, so values from boxes in different frames compare - an
		 * affine map keeps where along a segment a point lies.
		 */
		bool EntryParameter(const FBox& Box, const FVector& A, const FVector& B, double& OutT)
		{
			double Enter = 0.0;
			double Exit = 1.0;
			const FVector D = B - A;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (FMath::IsNearlyZero(D[Axis]))
				{
					// Parallel to this slab: inside it for the whole segment, or never.
					if (A[Axis] < Box.Min[Axis] || A[Axis] > Box.Max[Axis])
					{
						return false;
					}
					continue;
				}
				double T0 = (Box.Min[Axis] - A[Axis]) / D[Axis];
				double T1 = (Box.Max[Axis] - A[Axis]) / D[Axis];
				if (T0 > T1)
				{
					Swap(T0, T1);
				}
				Enter = FMath::Max(Enter, T0);
				Exit = FMath::Min(Exit, T1);
				if (Enter > Exit)
				{
					return false;
				}
			}
			OutT = Enter;
			return true;
		}
	}

	int32 FirstBoxHit(TConstArrayView<FPickBox> Boxes, const FVector& Start, const FVector& End)
	{
		int32 Best = INDEX_NONE;
		double BestT = TNumericLimits<double>::Max();
		for (int32 I = 0; I < Boxes.Num(); ++I)
		{
			const FPickBox& Pick = Boxes[I];
			if (!Pick.LocalBox.IsValid)
			{
				continue;
			}
			// Both ENDS into the box's frame rather than a direction: a scaled frame changes a
			// direction's length, and the segment parameter must mean the same in every box.
			const FVector A = Pick.LocalToWorld.InverseTransformPosition(Start);
			const FVector B = Pick.LocalToWorld.InverseTransformPosition(End);
			double T = 0.0;
			if (EntryParameter(Pick.LocalBox, A, B, T) && T < BestT)
			{
				BestT = T;
				Best = I;
			}
		}
		return Best;
	}
}
