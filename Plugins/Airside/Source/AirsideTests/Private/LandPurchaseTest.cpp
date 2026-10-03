#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/BuildPurse.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/LandBuyTool.h"

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


namespace
{
	/** Records the labels and line styles a tool drew. LandBuy-prefixed: unity build. */
	struct FLandBuySink : public IToolPreviewSink
	{
		TArray<FString> Labels;
		TArray<FVector2D> LabelAt;
		TArray<EPreviewStyle> LineStyles;
		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle Style) override { LineStyles.Add(Style); }
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle) override
		{
			Labels.Add(Text);
			LabelAt.Add(At);
		}
	};
}

/**
 * THE BUY LAND TOOL (land purchase spec R6): a priced ghost on every buyable tile - and only those; the void beyond stays
 * void - the hovered one lit, and a click buying the tile under the cursor through the target.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandBuyToolGhostsAndClick, "Airside.Tool.BuyLand.GhostsAndClick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandBuyToolGhostsAndClick::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	ARoadNetworkActor* Actor = Fixture.Actor;
	FLandRecordingPurse Purse;
	Actor->GetEditFacade()->SetPurse(&Purse);
	const FLandGrid Land = LandBuyStart();
	Actor->GetEditFacade()->AuthorOwnedLand(Land);

	FLandBuyTool Tool;
	TestEqual(TEXT("named as the registry names it"), Tool.GetDisplayName().ToString(), FString(TEXT("Buy land")));
	const FVector2D OverTile13 = Land.TileBox(FIntPoint(1, 3)).GetCenter();
	FLandBuySink Sink;
	Tool.BuildPreview(TestTool::ContextAt(*Actor, OverTile13), Sink);
	TestEqual(TEXT("four buyable tiles, four price labels"), Sink.Labels.Num(), 4);
	for (const FVector2D& At : Sink.LabelAt)
	{
		TestTrue(TEXT("each label sits on a buyable tile's centre"), Land.IsBuyable(Land.TileAt(At)) && At.Equals(Land.TileBox(Land.TileAt(At)).GetCenter(), 1e-6));
	}
	TestTrue(TEXT("each is priced"), Sink.Labels.Num() > 0 && Sink.Labels[0] == FText::AsNumber(300000.0).ToString());
	TestTrue(TEXT("the hovered tile is lit"), Sink.LineStyles.Contains(EPreviewStyle::Hover));

	Tool.OnClick(TestTool::ContextAt(*Actor, Land.TileBox(FIntPoint(5, 5)).GetCenter()));
	TestEqual(TEXT("a click on an unbuyable tile buys nothing"), Actor->Network->GetOwnedLand().NumOwned(), 2);
	Tool.OnClick(TestTool::ContextAt(*Actor, OverTile13));
	TestTrue(TEXT("a click on a buyable tile buys it"), Actor->Network->GetOwnedLand().IsTileOwned(FIntPoint(1, 3)));
	TestEqual(TEXT("charged once"), Purse.Charges.Num(), 1);
	Actor->GetEditFacade()->SetPurse(nullptr);
	return true;
}

#endif
