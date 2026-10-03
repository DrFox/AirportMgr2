#include "Tool/LandBuyTool.h"
#include "Model/BuildPurse.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/ToolReadout.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	const FLandGrid* LandOf(const FToolContext& Context)
	{
		const URoadNetwork* Network = Context.Network();
		return Network != nullptr && Network->GetOwnedLand().IsValid() ? &Network->GetOwnedLand() : nullptr;
	}
}

FText FLandBuyTool::GetDisplayName() const
{
	return LOCTEXT("BuyLandTool", "Buy land");
}

void FLandBuyTool::OnClick(const FToolContext& Context)
{
	const FLandGrid* Land = LandOf(Context);
	if (Land != nullptr && Context.Target != nullptr)
	{
		Context.Target->BuyLandTile(Land->TileAt(Context.Cursor));
	}
}

void FLandBuyTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const FLandGrid* Land = LandOf(Context);
	if (Land == nullptr || Context.Target == nullptr)
	{
		return;
	}
	const IBuildPurse* Purse = Context.Target->GetPurse();
	const FIntPoint Hovered = Land->TileAt(Context.Cursor);
	for (int32 Y = 0; Y < Land->Rows; ++Y)
	{
		for (int32 X = 0; X < Land->Columns; ++X)
		{
			const FIntPoint Tile(X, Y);
			if (!Land->IsBuyable(Tile))
			{
				continue;
			}
			const FString Why = Context.Target->WhyLandTileRefused(Tile);
			const EPreviewStyle Style = !Why.IsEmpty() ? EPreviewStyle::Refused
				: Tile == Hovered ? EPreviewStyle::Hover : EPreviewStyle::Pending;
			const FBox2D Box = Land->TileBox(Tile);
			const FVector2D Corners[4] = { Box.Min, FVector2D(Box.Max.X, Box.Min.Y), Box.Max, FVector2D(Box.Min.X, Box.Max.Y) };
			Sink.Polygon(Corners, Style);
			const FBuildQuote Quote = Context.Target->QuoteLandTile(Tile);
			const FString Price = Purse != nullptr ? Purse->Describe(Quote).ToString() : FString();
			Sink.Label(Box.GetCenter(), Why.IsEmpty() ? Price : Why, Style == EPreviewStyle::Hover ? EPreviewStyle::Pending : Style);
		}
	}
}

void FLandBuyTool::BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const
{
	const FLandGrid* Land = LandOf(Context);
	if (Land == nullptr)
	{
		Sink.Warning(TEXT("This map has no land to buy"));
		Sink.Committable(false);
		return;
	}
	Sink.Fact(TEXT("Owned"), FString::Printf(TEXT("%d of %d tiles"), Land->NumOwned(), Land->Columns * Land->Rows));
	const FIntPoint Tile = Land->TileAt(Context.Cursor);
	const FString Why = Context.Target != nullptr ? Context.Target->WhyLandTileRefused(Tile) : FString();
	Sink.Committable(Why.IsEmpty());
	if (!Why.IsEmpty())
	{
		Sink.Warning(Why);
	}
}

#undef LOCTEXT_NAMESPACE
