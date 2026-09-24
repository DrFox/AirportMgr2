#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadProfileBands.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadProfileBandsTest,
	"Airside.Build.ProfileBands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadProfileBandsTest::RunTest(const FString& Parameters)
{
	// A single-band profile has no interior boundary: just the two outer edges, and no
	// shoulder, so nothing fades. This is what MakeTransient produced before this task
	// and it must keep working - the ribbon still has to build from it.
	{
		URoadProfile* Plain = URoadProfile::MakeTransient(200.0, 100.0);
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Plain);

		TestEqual(TEXT("a single band gives two boundaries"), Bands.Alphas.Num(), 2);
		TestEqual(TEXT("alpha starts at the right edge"), Bands.Alphas[0], 0.0);
		TestEqual(TEXT("alpha ends at the left edge"), Bands.Alphas[1], 1.0);
		TestEqual(TEXT("right edge lateral is -HalfWidthRight"), Bands.Laterals[0], -100.0f);
		TestEqual(TEXT("left edge lateral is +HalfWidthLeft"), Bands.Laterals[1], 100.0f);
	}

	// Shoulder | lane | shoulder. Four boundaries, so the two shoulders are separable from
	// the lane and can carry their own material.
	{
		URoadProfile* Shouldered = URoadProfile::MakeTransient(200.0, 100.0, 30.0);
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Shouldered);

		TestEqual(TEXT("three bands give four boundaries"), Bands.Alphas.Num(), 4);

		// Ascending from the right edge to the left, always starting at 0 and ending at 1.
		TestEqual(TEXT("first alpha is 0"), Bands.Alphas[0], 0.0);
		TestEqual(TEXT("last alpha is 1"), Bands.Alphas[3], 1.0);
		for (int32 Index = 1; Index < Bands.Alphas.Num(); ++Index)
		{
			TestTrue(TEXT("alphas ascend"), Bands.Alphas[Index] > Bands.Alphas[Index - 1]);
		}

		// Total width 200 with 30 uu shoulders: boundaries at -100, -70, +70, +100.
		TestEqual(TEXT("right edge"), Bands.Laterals[0], -100.0f);
		TestEqual(TEXT("right shoulder inner edge"), Bands.Laterals[1], -70.0f);
		TestEqual(TEXT("left shoulder inner edge"), Bands.Laterals[2], 70.0f);
		TestEqual(TEXT("left edge"), Bands.Laterals[3], 100.0f);
	}

	// A null profile must not crash the builder; it yields the degenerate two-boundary
	// case at zero width, which produces no geometry rather than an exception.
	{
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(nullptr);
		TestEqual(TEXT("null profile still gives two boundaries"), Bands.Alphas.Num(), 2);
		TestEqual(TEXT("null profile has zero width"), Bands.Laterals[0], 0.0f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

// A TWO-WAY ROAD'S CENTRE IS PAINTED BY FRoadLaneMarkingBuilder (white dashes), not by the
// material. M_RoadSurface paints a solid line wherever |UV1 lateral| < CentrelineWidth, which
// is right for a taxiway and put a yellow line under the dashes on every road (review of
// 2026-09-23). So a profile that opts out keeps every lateral far from zero.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTwoWayRoadHasNoMaterialCentrelineTest,
	"Airside.Build.TwoWayRoadHasNoMaterialCentreline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTwoWayRoadHasNoMaterialCentrelineTest::RunTest(const FString& Parameters)
{
	const FRoadProfileBands Road = FRoadProfileBands::FromProfile(URoadProfile::MakeServiceRoadTransient());
	float Nearest = TNumericLimits<float>::Max();
	for (const float Lateral : Road.Laterals)
	{
		Nearest = FMath::Min(Nearest, FMath::Abs(Lateral));
	}
	// 1000 uu is far beyond any CentrelineWidth a road material could be given.
	TestTrue(TEXT("no band boundary of a two-way road lies near a material centreline"), Nearest > 1000.0f);
	TestTrue(TEXT("and the laterals still ascend, so the ribbon's interpolation is unchanged in shape"),
		Road.Laterals.Num() >= 2 && Road.Laterals[0] < Road.Laterals.Last());

	const FRoadProfileBands Taxiway = FRoadProfileBands::FromProfile(URoadProfile::MakeTransient(2300.0, 1500.0));
	TestTrue(TEXT("a taxiway keeps its painted centreline: its laterals straddle zero"),
		Taxiway.Laterals.Num() >= 2 && Taxiway.Laterals[0] < 0.0f && Taxiway.Laterals.Last() > 0.0f);
	return true;
}

#endif
