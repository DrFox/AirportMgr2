#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RunwayFacts.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayEndTest,
	"Airside.Model.RunwayEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayEndTest::RunTest(const FString& Parameters)
{
	// A 1000 uu strip on a diagonal, so X-only arithmetic could not pass by accident.
	FRunwayEnd End;
	End.Threshold = FVector2D(1000.0, 2000.0);
	End.Direction = FVector2D(0.6, 0.8);   // unit length: 3-4-5 triangle scaled
	End.Length = 1000.0;
	End.Seed.Index = 7;
	End.Seed.Generation = 3;

	// 1. FarEnd is the threshold walked the whole length along the direction.
	{
		const FVector2D Expected = End.Threshold + End.Direction * End.Length;
		TestEqual(TEXT("FarEnd is Threshold + Direction * Length"), End.FarEnd(), Expected);
	}

	// 2. OffsetOf projects a point onto the strip's own axis, signed - negative short of
	//    the threshold, exactly what a query on final approach needs.
	{
		const FVector2D OnAxisPastThreshold = End.Threshold + End.Direction * 250.0;
		TestEqual(TEXT("a point on-axis, ahead, offsets positively"),
			End.OffsetOf(OnAxisPastThreshold), 250.0, 1.0e-6);

		const FVector2D OnAxisBeforeThreshold = End.Threshold - End.Direction * 400.0;
		TestEqual(TEXT("a point behind the threshold offsets negatively"),
			End.OffsetOf(OnAxisBeforeThreshold), -400.0, 1.0e-6);

		// Off-axis: only the component ALONG Direction counts, never the lateral one -
		// the same rule IsPointOnRunway uses for its own Distance.
		const FVector2D Lateral(-End.Direction.Y, End.Direction.X);   // perpendicular, unit length
		const FVector2D OffAxis = End.Threshold + End.Direction * 100.0 + Lateral * 5000.0;
		TestEqual(TEXT("a lateral offset does not change the along-strip distance"),
			End.OffsetOf(OffAxis), 100.0, 1.0e-6);
	}

	// 3. Reversed describes the SAME strip from its other end: Threshold becomes FarEnd,
	//    Direction flips, Length is unchanged, and Seed still names the one segment this
	//    was measured from - Reversed renames the end, not the segment.
	{
		const FRunwayEnd Back = End.Reversed();
		TestEqual(TEXT("Reversed threshold is the original FarEnd"), Back.Threshold, End.FarEnd());
		TestEqual(TEXT("Reversed direction points the other way"), Back.Direction, -End.Direction);
		TestEqual(TEXT("length is unchanged"), Back.Length, End.Length);
		TestEqual(TEXT("seed is unchanged - it names the segment, not the end"), Back.Seed, End.Seed);

		// Reversing twice returns to the original end - the operation is its own inverse,
		// which is what "the same strip, the other way round" has to mean.
		const FRunwayEnd ThereAndBack = Back.Reversed();
		TestEqual(TEXT("reversed twice is the threshold again"), ThereAndBack.Threshold, End.Threshold);
		TestEqual(TEXT("and the direction again"), ThereAndBack.Direction, End.Direction);
	}

	return true;
}

#endif
