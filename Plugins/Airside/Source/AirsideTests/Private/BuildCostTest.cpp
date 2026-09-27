#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/BuildCost.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
#include "Testing/AirsideTestGraph.h"

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
		Quote.BaseAmount(), 150000.0, 1e-6);
	TestEqual(TEXT("the quote names the profile as its source, so a discount can key on the "
		"asset itself rather than on a parallel enum of build kinds"),
		Quote.Lines.IsValidIndex(0) ? Quote.Lines[0].Source.Get() : nullptr,
		static_cast<const UObject*>(Profile));
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
		BuildCost::ForApron(Outline, 15.0, {}).BaseAmount(), 150000.0, 1e-6);

	// WOUND THE OTHER WAY. A signed shoelace sum is negative for one winding, and both occur
	// here - CCW faces down in Unreal, so an outline drawn either way round is a real apron. A
	// negative area would quote a negative amount, and a negative charge PAYS THE PLAYER TO
	// BUILD, which is the kind of bug that funds an airport.
	Algo::Reverse(Outline);
	TestEqual(TEXT("winding does not change what an apron costs"),
		BuildCost::ForApron(Outline, 15.0, {}).BaseAmount(), 150000.0, 1e-6);

	TArray<FVector2D> Line;
	Line.Add(FVector2D(0.0, 0.0));
	Line.Add(FVector2D(10000.0, 0.0));
	TestEqual(TEXT("a degenerate outline has no area and so costs nothing"),
		BuildCost::ForApron(Line, 15.0, {}).BaseAmount(), 0.0, 1e-9);
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
	TestEqual(TEXT("a stand costs what its definition says"), Quote.BaseAmount(), 40000.0, 1e-6);
	TestEqual(TEXT("and names the definition, so a discount can key on the kind of thing"),
		Quote.Lines.IsValidIndex(0) ? Quote.Lines[0].Source.Get() : nullptr,
		static_cast<const UObject*>(Definition));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostTarmacPricesUnchangedTest,
	"Airside.Build.BuildCostTarmacPricesUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostTarmacPricesUnchangedTest::RunTest(const FString& Parameters)
{
	// FIGURES CAPTURED FROM MAIN before quotes became lines (Task 0). Tarmac is factor 1, so
	// every price a player could see before this change must be the same after it.
	URoadProfile* Taxi = TestProfiles::Taxiway();
	Taxi->CostPerMetre = 12.5;
	const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	TestEqual(TEXT("a 500 m taxiway"),
		BuildCost::ForSegment(*Taxi, 50000.0, EPavement::Tarmac).BaseAmount(), 6250.0, 1e-6);
	TestEqual(TEXT("a B-sized pad"),
		BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac).BaseAmount(), 19750.0, 1e-6);

	UEntityDefinition* BStand = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
	BStand->PlacementCost = 5000.0;
	TestEqual(TEXT("a B stand's equipment"),
		BuildCost::ForEntity(*BStand).BaseAmount(), 5000.0, 1e-6);

	// A RESOLVED SHIPPING TAXIWAY PROFILE, not a transient stand-in - ForSegment's per-metre
	// conversion has to hold for the asset the game actually ships, not only for a fixture.
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (TestTrue(TEXT("the content set has a shipping taxiway profile"),
		Content != nullptr && Content->TaxiwayProfiles.Num() > 0))
	{
		const URoadProfile* Shipping = Content->TaxiwayProfiles[0].LoadSynchronous();
		if (TestNotNull(TEXT("it loads"), Shipping))
		{
			constexpr double LengthUu = 50000.0;
			TestEqual(TEXT("500 m of the shipping taxiway profile on tarmac, at its own rate"),
				BuildCost::ForSegment(*Shipping, LengthUu, EPavement::Tarmac).BaseAmount(),
				Shipping->CostPerMetre * (LengthUu / 100.0), 1e-6);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostQuoteIsItsLinesTest,
	"Airside.Build.BuildCostQuoteIsItsLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostQuoteIsItsLinesTest::RunTest(const FString& Parameters)
{
	// A STAND IS TWO LINES, so pricing can key a discount on the definition without also
	// discounting the ground under it - the reason the pad's line keeps its own null source.
	const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	const FBuildQuote Entity = BuildCost::ForEntity(*UEntityDefinition::MakeStandTransient(EIcaoCode::B));
	const FBuildQuote Ground = BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac);
	const FBuildQuote Stand = BuildCost::Combine(Entity, Ground);
	TestEqual(TEXT("two lines"), Stand.Lines.Num(), 2);
	TestEqual(TEXT("summing to both parts"), Stand.BaseAmount(), Entity.BaseAmount() + Ground.BaseAmount());
	TestTrue(TEXT("the pad line has no source asset"), Stand.Lines[1].Source.Get() == nullptr);
	TestFalse(TEXT("and a building line has no pavement"), Stand.Lines[0].Pavement.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostFactorOnEveryKindTest,
	"Airside.Build.BuildCostFactorOnEveryKind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostFactorOnEveryKindTest::RunTest(const FString& Parameters)
{
	// EVERY KIND THAT LIES ON GROUND takes the factor, and a building does not - the point of
	// applying it in FBuildLine::Amount rather than per kind.
	const URoadProfile& Taxi = *TestProfiles::Taxiway();
	const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	TestEqual(TEXT("grass taxiway is 0.4 of tarmac"),
		BuildCost::ForSegment(Taxi, 50000.0, EPavement::Grass).BaseAmount(),
		0.4 * BuildCost::ForSegment(Taxi, 50000.0, EPavement::Tarmac).BaseAmount(), 1e-6);
	TestEqual(TEXT("a grass stand pad is 0.4 of a tarmac one"),
		BuildCost::ForApron(Pad, 10.0, EPavement::Grass).BaseAmount(),
		0.4 * BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac).BaseAmount(), 1e-6);
	TestEqual(TEXT("a bare apron has no pavement and bills at the rate itself"),
		BuildCost::ForApron(Pad, 10.0, {}).BaseAmount(), BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac).BaseAmount(), 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostUpkeepUsesBuildFactorTest,
	"Airside.Build.BuildCostUpkeepUsesBuildFactor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostUpkeepUsesBuildFactorTest::RunTest(const FString& Parameters)
{
	// A RUNWAY'S PAVEMENT IS ITS FACTS', a road's is its segment's (Review Focus 2). Two
	// networks, identical but for one fact each: upkeep must move by exactly the factor.
	auto UpkeepOf = [](bool bRunway, EPavement P)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = bRunway ? TestProfiles::Runway() : TestProfiles::Taxiway();
		Profile->UpkeepPerMetrePerDay = 1.0;              // non-zero, or the ratio proves nothing
		const FRoadSegmentId S = Net->AddStraightSegment(Net->AddNode({0,0}), Net->AddNode({100000,0}), Profile);
		if (bRunway) { FRunwayFacts F; F.Surface = P; Net->SetRunwayFacts(S, F); }
		else         { Net->SetSegmentSurface(S, P); }
		return BuildCost::DailyUpkeep(*Net, 0.0);
	};
	TestEqual(TEXT("grass runway upkeep is 0.4 of tarmac"), UpkeepOf(true, EPavement::Grass), 0.4 * UpkeepOf(true, EPavement::Tarmac), 1e-6);
	TestEqual(TEXT("grass taxiway upkeep is 0.4 of tarmac"), UpkeepOf(false, EPavement::Grass), 0.4 * UpkeepOf(false, EPavement::Tarmac), 1e-6);
	TestTrue(TEXT("and upkeep is not zero, or the ratio proves nothing"), UpkeepOf(true, EPavement::Tarmac) > 0.0);
	return true;
}

#endif
