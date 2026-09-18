#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/BuildCost.h"
#include "Entities/EntityDefinition.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostPerMetreTest,
	"Airside.Build.BuildCostPerMetre",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostPerMetreTest::RunTest(const FString& Parameters)
{
	URoadProfile* Profile = NewObject<URoadProfile>();
	Profile->CostPerMetre = 300.0;

	// 500 m, in uu. THE CONVERSION HAPPENS ONCE, in BuildCost, and this is what pins it: a
	// second site dividing by 100 is how a taxiway comes to cost a hundred times too much with
	// nothing to say which of the two was wrong.
	const FBuildQuote Quote = BuildCost::ForSegment(*Profile, 50000.0);

	TestEqual(TEXT("500 m of a 300-per-metre profile quotes 150,000"),
		Quote.BaseAmount, 150000.0, 1e-6);
	TestEqual(TEXT("the quote names the profile as its source, so a discount can key on the "
		"asset itself rather than on a parallel enum of build kinds"),
		Quote.Source.Get(), static_cast<const UObject*>(Profile));
	TestFalse(TEXT("and it says what it is, for the ghost to print"), Quote.What.IsEmpty());
	TestFalse(TEXT("a priced build is not free"), Quote.IsFree());

	const FBuildQuote Unpriced = BuildCost::ForSegment(*NewObject<URoadProfile>(), 50000.0);
	TestTrue(TEXT("a profile nobody has priced builds free rather than at an invented figure - "
		"an un-authored asset should fail visibly, not expensively"), Unpriced.IsFree());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostApronAreaTest,
	"Airside.Build.BuildCostApronArea",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostApronAreaTest::RunTest(const FString& Parameters)
{
	// A 100 m x 100 m square, in uu.
	TArray<FVector2D> Outline;
	Outline.Add(FVector2D(0.0, 0.0));
	Outline.Add(FVector2D(10000.0, 0.0));
	Outline.Add(FVector2D(10000.0, 10000.0));
	Outline.Add(FVector2D(0.0, 10000.0));

	TestEqual(TEXT("ten thousand square metres at 15 quotes 150,000"),
		BuildCost::ForApron(Outline, 15.0).BaseAmount, 150000.0, 1e-6);

	// WOUND THE OTHER WAY. A signed shoelace sum is negative for one winding, and both occur
	// here - CCW faces down in Unreal, so an outline drawn either way round is a real apron. A
	// negative area would quote a negative amount, and a negative charge PAYS THE PLAYER TO
	// BUILD, which is the kind of bug that funds an airport.
	Algo::Reverse(Outline);
	TestEqual(TEXT("winding does not change what an apron costs"),
		BuildCost::ForApron(Outline, 15.0).BaseAmount, 150000.0, 1e-6);

	TArray<FVector2D> Line;
	Line.Add(FVector2D(0.0, 0.0));
	Line.Add(FVector2D(10000.0, 0.0));
	TestEqual(TEXT("a degenerate outline has no area and so costs nothing"),
		BuildCost::ForApron(Line, 15.0).BaseAmount, 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostEntityTest,
	"Airside.Build.BuildCostEntity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostEntityTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>();
	Definition->PlacementCost = 40000.0;

	const FBuildQuote Quote = BuildCost::ForEntity(*Definition);
	TestEqual(TEXT("a stand costs what its definition says"), Quote.BaseAmount, 40000.0, 1e-6);
	TestEqual(TEXT("and names the definition, so a discount can key on the kind of thing"),
		Quote.Source.Get(), static_cast<const UObject*>(Definition));
	return true;
}

#endif
