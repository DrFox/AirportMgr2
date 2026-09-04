#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RunwayDesignator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** A unit direction at a compass bearing, with +X north and +Y east. */
	FVector2D DesignatorAtBearing(double Degrees)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayDesignatorTest,
	"Airside.Solve.RunwayDesignator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayDesignatorTest::RunTest(const FString& Parameters)
{
	// 1. THE CARDINALS, which are the ones a reader can check against a compass without
	//    trusting the arithmetic below them.
	{
		TestEqual(TEXT("north is 36, not 0"), RunwayDesignator::Designate(FVector2D(1.0, 0.0)), 36);
		TestEqual(TEXT("east is 09"),  RunwayDesignator::Designate(FVector2D(0.0, 1.0)), 9);
		TestEqual(TEXT("south is 18"), RunwayDesignator::Designate(FVector2D(-1.0, 0.0)), 18);
		TestEqual(TEXT("west is 27"),  RunwayDesignator::Designate(FVector2D(0.0, -1.0)), 27);
	}

	// 2. ROUNDING TO THE NEAREST TEN. A runway on 093 is 09 and one on 087 is also 09: the
	//    designator is the bearing rounded, not truncated, which is why a chart shows 09 for
	//    a strip that is not quite east.
	{
		TestEqual(TEXT("093 rounds down to 09"), RunwayDesignator::Designate(DesignatorAtBearing(93.0)), 9);
		TestEqual(TEXT("087 rounds up to 09"),   RunwayDesignator::Designate(DesignatorAtBearing(87.0)), 9);
		TestEqual(TEXT("095 rounds up to 10"),   RunwayDesignator::Designate(DesignatorAtBearing(95.0)), 10);
	}

	// 3. EITHER SIDE OF NORTH, the case that catches an implementation that forgot 0 is not
	//    a runway number. Both of these are runway 36.
	{
		TestEqual(TEXT("355 is 36"), RunwayDesignator::Designate(DesignatorAtBearing(355.0)), 36);
		TestEqual(TEXT("004 is 36"), RunwayDesignator::Designate(DesignatorAtBearing(4.0)), 36);
		TestEqual(TEXT("005 is 01"), RunwayDesignator::Designate(DesignatorAtBearing(5.0)), 1);
	}

	// 4. THE OTHER THRESHOLD is always half a turn away, and wraps rather than reaching 0.
	{
		TestEqual(TEXT("09 pairs with 27"), RunwayDesignator::Reciprocal(9), 27);
		TestEqual(TEXT("27 pairs back with 09"), RunwayDesignator::Reciprocal(27), 9);
		TestEqual(TEXT("36 pairs with 18"), RunwayDesignator::Reciprocal(36), 18);
		TestEqual(TEXT("18 pairs back with 36"), RunwayDesignator::Reciprocal(18), 36);
		TestEqual(TEXT("01 pairs with 19"), RunwayDesignator::Reciprocal(1), 19);

		// Walking the strip the other way must agree with the reciprocal, or the two ends of
		// one runway could disagree about what runway they are.
		for (double Bearing = 0.0; Bearing < 360.0; Bearing += 7.0)
		{
			const FVector2D Forward = DesignatorAtBearing(Bearing);
			const int32 Ahead = RunwayDesignator::Designate(Forward);
			const int32 Behind = RunwayDesignator::Designate(-Forward);

			TestEqual(FString::Printf(
				TEXT("at %.0f, the far threshold agrees with the reciprocal"), Bearing),
				Behind, RunwayDesignator::Reciprocal(Ahead));
		}
	}

	// 5. HOW IT IS PAINTED. Two digits, and the pair reads low first.
	{
		TestEqual(TEXT("single digits are padded"), RunwayDesignator::ToText(9), FString(TEXT("09")));
		TestEqual(TEXT("and 36 is not"), RunwayDesignator::ToText(36), FString(TEXT("36")));

		// LOW FIRST, both ways round. Dragging a node must not rename 09/27 to 27/09.
		TestEqual(TEXT("east reads 09/27"),
			RunwayDesignator::ToPairText(FVector2D(0.0, 1.0)), FString(TEXT("09/27")));
		TestEqual(TEXT("and west reads 09/27 as well"),
			RunwayDesignator::ToPairText(FVector2D(0.0, -1.0)), FString(TEXT("09/27")));
	}

	// 6. NO BEARING IS NOT NORTH. A zero-length runway must not be called 36 - it must be
	//    reportable as "not a runway", or a degenerate segment gets a name and looks real.
	{
		TestEqual(TEXT("a zero direction has no designator"),
			RunwayDesignator::Designate(FVector2D::ZeroVector), 0);
		TestEqual(TEXT("which prints as nothing"), RunwayDesignator::ToText(0), FString());
		TestEqual(TEXT("and pairs as nothing"),
			RunwayDesignator::ToPairText(FVector2D::ZeroVector), FString());
	}

	return true;
}

#endif
