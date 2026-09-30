#pragma once

#include "CoreMinimal.h"
#include "Solve/PlotYard.h"

/**
 * Two depot-yard reservations compared AS VALUES (#450).
 *
 * A reservation has no operator==, and the tests that stood beside one asked whether each evaluator RAN (GetSolveCountForTest,
 * PlotEvaluatorCountForTest) - which passes just as well when they disagree. The yard the tool previews, the yard the facade
 * judges at commit and the yard the presenter rebuilds from the built entity are one function over one set of floats, so the
 * comparison is EXACT: a tolerance here could only hide a real disagreement, the preview-versus-built split the design exists to
 * prevent.
 *
 * A HEADER OF INLINE FUNCTIONS, prefixed by namespace, because the test module is a unity build and two .cpp files each wanting this
 * would otherwise collide in an anonymous namespace.
 */
namespace YardAgreement
{
	/** Empty when A and B are the same reservation, stand for stand; otherwise the first difference, named. */
	inline FString Difference(const PlotYard::FReservation& A, const PlotYard::FReservation& B)
	{
		if (A.bStandsIncludeApron != B.bStandsIncludeApron)
		{
			return TEXT("bStandsIncludeApron differs");
		}
		if (A.Stands.Num() != B.Stands.Num())
		{
			return FString::Printf(TEXT("%d stand(s) against %d"), A.Stands.Num(), B.Stands.Num());
		}
		for (int32 Index = 0; Index < A.Stands.Num(); ++Index)
		{
			const PlotYard::FReservedStand& X = A.Stands[Index];
			const PlotYard::FReservedStand& Y = B.Stands[Index];
			if (X.Centre != Y.Centre)
			{
				return FString::Printf(TEXT("stand %d centre (%.3f, %.3f) against (%.3f, %.3f)"),
					Index, X.Centre.X, X.Centre.Y, Y.Centre.X, Y.Centre.Y);
			}
			if (X.Heading != Y.Heading)
			{
				return FString::Printf(TEXT("stand %d heading %.6f against %.6f"), Index, X.Heading, Y.Heading);
			}
			if (X.bPlaced != Y.bPlaced || X.KitIndex != Y.KitIndex || X.RunLength != Y.RunLength)
			{
				return FString::Printf(TEXT("stand %d kit %d x%d against kit %d x%d"), Index, X.KitIndex, X.RunLength, Y.KitIndex, Y.RunLength);
			}
		}
		return FString();
	}
}
