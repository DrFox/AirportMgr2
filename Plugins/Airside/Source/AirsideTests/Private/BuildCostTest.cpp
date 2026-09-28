#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/BuildCost.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Profiles/RoadProfile.h"
#include "Model/RoadNetwork.h"
#include "Solve/IcaoCode.h"
#include "StandFixture.h"
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

/**
 * EVERY PROFILE THE CONTENT SET OFFERS IS PRICED, AT ONE RATE PER LIST (2026-09-28).
 *
 * The Standard and Wide road tiers shipped at CostPerMetre 0 for four days: build_cost_rates.py
 * priced a typed list of assets, and the two new tiers were never added to it. It now walks the
 * content set's lists; this is what goes red if a profile in one of them is unpriced, or priced
 * at a rate per square metre other than its siblings' - a wider tier must cost more per metre
 * in proportion to its pavement, which is the property the script's docstring defends.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryProfileIsPricedTest,
	"Airside.Content.EveryProfileIsPriced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryProfileIsPricedTest::RunTest(const FString& Parameters)
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (!TestNotNull(TEXT("the content set loads"), Content)) { return false; }

	const TPair<const TCHAR*, const TArray<TSoftObjectPtr<URoadProfile>>*> Lists[] = {
		{ TEXT("taxiway"), &Content->TaxiwayProfiles },
		{ TEXT("runway"), &Content->RunwayProfiles },
		{ TEXT("service road"), &Content->ServiceRoadProfiles },
	};
	for (const auto& [Kind, List] : Lists)
	{
		if (!TestTrue(FString::Printf(TEXT("the content set offers %s profiles"), Kind), List->Num() > 0)) { continue; }
		double FirstRate = -1.0;
		for (int32 Index = 0; Index < List->Num(); ++Index)
		{
			const URoadProfile* Profile = (*List)[Index].LoadSynchronous();
			if (!TestNotNull(FString::Printf(TEXT("%s %d loads"), Kind, Index), Profile)) { continue; }
			TestTrue(FString::Printf(TEXT("%s %d costs something to lay"), Kind, Index), Profile->CostPerMetre > 0.0);
			TestTrue(FString::Printf(TEXT("%s %d costs something to keep"), Kind, Index), Profile->UpkeepPerMetrePerDay > 0.0);

			const double Rate = Profile->CostPerMetre / (Profile->GetTotalWidth() / 100.0);
			if (FirstRate < 0.0) { FirstRate = Rate; }
			TestEqual(FString::Printf(TEXT("%s %d is priced at its list's rate per square metre"), Kind, Index),
				Rate, FirstRate, 1e-6);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCostStandPadUpkeepByAreaTest,
	"Airside.Build.BuildCostStandPadUpkeepByArea",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCostStandPadUpkeepByAreaTest::RunTest(const FString& Parameters)
{
	// A STAND'S GROUND IS BILLED BY AREA AND PAVEMENT (user, 2026-09-27) - before, only its
	// definition's flat UpkeepPerDay, so a grass stand was cheaper to build and the same to own.
	auto PadUpkeep = [](double Depth, EPavement P)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
		Def->UpkeepPerDay = 0.0;   // isolate the pad's term
		const FEntityInstanceId Id = ServiceLinkFixture::PlaceStand(*Net, *Def, FVector2D::ZeroVector, 0.0);
		FRoadNetworkTestAccess Access(*Net);
		Access.SetEntityOutlineForTest(Id, { {0,0}, {5000,0}, {5000,Depth}, {0,Depth} });
		Access.SetEntityPavementForTest(Id, P);
		return BuildCost::DailyUpkeep(*Net, 1.0);
	};
	TestEqual(TEXT("grass pad upkeep is 0.4 of tarmac"), PadUpkeep(3950.0, EPavement::Grass), 0.4 * PadUpkeep(3950.0, EPavement::Tarmac), 1e-6);
	TestEqual(TEXT("double the area, double the upkeep"), PadUpkeep(7900.0, EPavement::Tarmac), 2.0 * PadUpkeep(3950.0, EPavement::Tarmac), 1e-6);
	TestEqual(TEXT("1975 m2 at rate 1 and factor 1"), PadUpkeep(3950.0, EPavement::Tarmac), 50.0 * 39.5, 1e-6);

	// A DEPOT'S PLOT IS NOT A STAND'S PAD - its ground is billed through its kit's own upkeep,
	// so the same outline on a depot adds nothing here.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Depot->UpkeepPerDay = 0.0;
		const FEntityInstanceId Id = Net->PlaceEntity(Depot, Depot->Anchors, FVector2D::ZeroVector, 0.0,
			0.0, Depot->PoseRole, Depot->Trucks);
		FRoadNetworkTestAccess(*Net).SetEntityOutlineForTest(Id, { {0,0}, {5000,0}, {5000,3950}, {0,3950} });
		TestEqual(TEXT("a depot's plot bills no pad upkeep"), BuildCost::DailyUpkeep(*Net, 1.0), 0.0, 1e-9);
	}
	return true;
}

#endif
