#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/BuildPurse.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A purse that records rather than banks - BuildPurseTest's FRecordingPurse, Land-prefixed for the unity build. */
	class FLandRecordingPurse : public IBuildPurse
	{
	public:
		double Funds = 1000000.0;
		TArray<double> Charges;
		int32 NextId = 1;
		virtual double Balance() const override { return Funds; }
		virtual bool CanAfford(const FBuildQuote& Quote) const override { return Quote.BaseAmount() <= Funds; }
		virtual int32 Charge(const FBuildQuote& Quote) override
		{
			Funds -= Quote.BaseAmount();
			Charges.Add(Quote.BaseAmount());
			return NextId++;
		}
		virtual void Reverse(int32) override {}
		virtual void Credit(const FBuildQuote&) override {}
		virtual FText Describe(const FBuildQuote& Quote) const override { return FText::AsNumber(Quote.BaseAmount()); }
	};

	FLandGrid LandBuyStart()
	{
		const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
		return FLandGrid::Make(FVector2D(-30000.0, -240000.0), 60000.0, 8, 8, Start);
	}
}

/**
 * BUYING A TILE (land purchase spec R4, R5): priced Base x (1 + Growth x owned) - 150,000 x (1 + 0.5 x 2) = 300,000 with
 * the 1x2 start - refused with a reason for every tile that cannot be bought, and charged once, announced once, when
 * one is. A refusal charges nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandPurchaseBuyRefusals, "Airside.Present.OwnedLand.BuyRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandPurchaseBuyRefusals::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	URoadEditFacade* Facade = Fixture.Actor->GetEditFacade();
	FLandRecordingPurse Purse;
	Facade->SetPurse(&Purse);

	TestEqual(TEXT("with no grid there is no land to buy"), Facade->WhyLandTileRefused(FIntPoint(1, 3)), FString(TEXT("No land here")));
	TestFalse(TEXT("and nothing is bought"), Facade->BuyLandTile(FIntPoint(1, 3)));

	Facade->AuthorOwnedLand(LandBuyStart());
	TestEqual(TEXT("the price with two tiles owned"), Facade->QuoteLandTile(FIntPoint(1, 3)).BaseAmount(), 300000.0, 1e-6);
	TestEqual(TEXT("an owned tile"), Facade->WhyLandTileRefused(FIntPoint(0, 3)), FString(TEXT("Already yours")));
	TestEqual(TEXT("a diagonal tile"), Facade->WhyLandTileRefused(FIntPoint(1, 2)), FString(TEXT("Not next to your land")));
	TestEqual(TEXT("off the map"), Facade->WhyLandTileRefused(FIntPoint(-1, 3)), FString(TEXT("Off the map")));

	Purse.Funds = 100000.0;
	TestTrue(TEXT("too poor says so"), Facade->WhyLandTileRefused(FIntPoint(1, 3)).StartsWith(TEXT("Can't afford")));
	TestFalse(TEXT("and does not buy"), Facade->BuyLandTile(FIntPoint(1, 3)));
	TestEqual(TEXT("and charges nothing"), Purse.Charges.Num(), 0);

	Purse.Funds = 1000000.0;
	int32 Changed = 0, Bought = 0;
	FIntPoint BoughtTile(-1, -1);
	double BoughtFor = 0.0;
	Facade->OnOwnedLandChanged.AddLambda([&Changed](const FLandGrid&) { ++Changed; });
	Facade->OnLandBought.AddLambda([&](FIntPoint Tile, const FBuildQuote& Quote) { ++Bought; BoughtTile = Tile; BoughtFor = Quote.BaseAmount(); });
	TestTrue(TEXT("an adjacent, affordable tile is bought"), Facade->BuyLandTile(FIntPoint(1, 3)));
	TestTrue(TEXT("and owned"), Fixture.Actor->Network->GetOwnedLand().IsTileOwned(FIntPoint(1, 3)));
	TestEqual(TEXT("charged once"), Purse.Charges.Num(), 1);
	TestEqual(TEXT("at the quote"), Purse.Charges.Num() > 0 ? Purse.Charges[0] : 0.0, 300000.0, 1e-6);
	TestEqual(TEXT("the land change announced once"), Changed, 1);
	TestEqual(TEXT("the purchase announced once"), Bought, 1);
	TestEqual(TEXT("for that tile"), BoughtTile, FIntPoint(1, 3));
	TestEqual(TEXT("at that price"), BoughtFor, 300000.0, 1e-6);

	TestFalse(TEXT("the same tile again is refused"), Facade->BuyLandTile(FIntPoint(1, 3)));
	TestEqual(TEXT("without a second charge"), Purse.Charges.Num(), 1);
	TestEqual(TEXT("the next tile costs more: three owned"), Facade->QuoteLandTile(FIntPoint(2, 3)).BaseAmount(), 375000.0, 1e-6);
	Facade->SetPurse(nullptr);
	return true;
}

#endif
