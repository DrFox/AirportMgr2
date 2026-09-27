#include "Tool/GridOverlay.h"

#include "AirsideLog.h"
#include "Solve/GridSnap.h"
#include "Tool/RoadBuildTool.h"

void GridOverlay::Describe(const FToolContext& Context, IToolPreviewSink& Sink)
{
	if (Context.GridStepUu <= 0.0 || Context.GridOverlayRadiusUu <= 0.0)
	{
		return;
	}

	// Kept across frames: ~1600 pieces at 1 m is the ESTIMATE the design made (2026-09-27), and
	// a fresh array per frame would allocate that every frame for nothing. Static rather than a
	// member because this is a free function with no instance to own it - and game-thread only,
	// as both callers are.
	static TArray<GridSnap::FPiece> Pieces;
	GridSnap::PiecesInDisc(Context.GuidedCursor(), Context.GridOverlayRadiusUu, Context.GridStepUu, Pieces);

	// MEASURED, NOT ASSUMED: the piece count once per step change, so the design's estimate can
	// be read off the log rather than trusted.
	static double LoggedStep = 0.0;
	if (LoggedStep != Context.GridStepUu)
	{
		LoggedStep = Context.GridStepUu;
		UE_LOG(LogAirside, Log, TEXT("Grid overlay: %d pieces at %.0f m, radius %.0f m"),
			Pieces.Num(), Context.GridStepUu / 100.0, Context.GridOverlayRadiusUu / 100.0);
	}

	for (const GridSnap::FPiece& Piece : Pieces)
	{
		Sink.Line(Piece.From, Piece.To, Piece.bMajor ? EPreviewStyle::GridMajor : EPreviewStyle::GridMinor);
	}
}
