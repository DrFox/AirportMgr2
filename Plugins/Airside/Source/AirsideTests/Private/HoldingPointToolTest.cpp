#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/GuidelineOverlay.h"
#include "Tool/HoldingPointTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Counts what a holding-point gesture emits. NOT a HUD: the styles are meanings, so a
	 * test is exactly as valid a sink as a canvas - see IToolPreviewSink.
	 *
	 * The M2Hold prefix is deliberate. AirsideTests is a UNITY build, so a bare FHoldSink
	 * here would collide with any other file's own.
	 */
	struct FM2HoldSink : IToolPreviewSink
	{
		int32 RunwayBars = 0;
		int32 IntermediateBars = 0;
		int32 Refused = 0;
		int32 RefusedMarkers = 0;
		int32 Doomed = 0;
		FString LastLabel;
		/** The last runway bar's direction of travel, so "across the taxiway" can be MEASURED. */
		FVector2D LastAlong = FVector2D::ZeroVector;

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::Doomed) { ++Doomed; }
			if (Style == EPreviewStyle::Refused) { ++RefusedMarkers; }
		}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D& Along, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::RunwayHoldingPosition) { ++RunwayBars; LastAlong = Along; }
			if (Style == EPreviewStyle::IntermediateHoldingPosition) { ++IntermediateBars; }
		}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::Refused) { ++Refused; LastLabel = Text; }
		}
	};

	/** The guideline node derived for one end of Segment, or unset. */
	FGuidelineNodeId M2HoldToolNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment
				&& Nodes[Index].Origin.bEndA == bEndA)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	EHoldingPositionKind M2HoldToolKind(const URoadNetwork& Net, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Net.GetGuidelineNode(Node);
		return Found ? Found->HoldingPosition : EHoldingPositionKind::None;
	}
}

/**
 * THE HOLDING POINT TOOL (key 8) places INTERMEDIATE holding positions and nothing else:
 * a runway-holding position is derived at every taxiway end on a runway and the tool says
 * so when clicked. Spec 2026-09-07.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingPointToolTest,
	"Airside.Tool.HoldingPoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingPointToolTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD AND A REAL ACTOR, not the model alone: the claim under test reaches
	// through the facade to the undo stack, and a position placed on a URoadNetwork by
	// hand would prove nothing about whether the player can take it back.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	// Far from everything the test then builds, so it snaps to nothing: it exists only to
	// make the facade construct its network before the model edits below.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;

	//   T ======= E ======= F        runway
	//             |
	//             X ----- Y          taxiway E-X, taxiway X-Y: X is a taxiway junction
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FRoadNodeId T = Net.AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net.AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net.AddNode(FVector2D(100000.0, 0.0));
	Net.AddStraightSegment(T, E, Runway);
	Net.AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net.AddNode(FVector2D(60000.0, -20000.0));
	const FRoadNodeId Y = Net.AddNode(FVector2D(80000.0, -20000.0));
	const FRoadSegmentId Tx = Net.AddStraightSegment(E, X, Taxiway);
	Net.AddStraightSegment(X, Y, Taxiway);
	Actor->RebuildMesh();

	const FGuidelineNodeId RunwayEnd = M2HoldToolNodeFor(Net, Tx, true);
	const FGuidelineNodeId Junction = M2HoldToolNodeFor(Net, Tx, false);
	if (!TestTrue(TEXT("both taxiway end nodes exist"), RunwayEnd.IsSet() && Junction.IsSet())) { return false; }
	TestTrue(TEXT("the runway end is a derived runway-holding position before any click"),
		M2HoldToolKind(Net, RunwayEnd) == EHoldingPositionKind::Runway);

	// The registry knows the tool, under key 8, and the session builds it.
	TestTrue(TEXT("registered under Eight"), ToolRegistry().Last().Key == EKeys::Eight);
	FBuildSession Session;
	TestEqual(TEXT("the session holds every registered tool"), Session.NumTools(), ToolRegistry().Num());

	FHoldingPointTool Tool;
	FToolContext Ctx;
	Ctx.Target = Actor;
	Ctx.SnapRadius = 400.0;

	// 1. A CLICK ON THE RUNWAY END IS REFUSED, with the reason, and changes nothing - not
	//    even the undo stack (the fixture's own PlaceNode is already on it, so the measure
	//    is "unchanged", not "empty").
	const bool bCouldUndoBefore = Actor->CanUndo();
	Ctx.Cursor = Net.GetGuidelineNode(RunwayEnd)->Position;
	Tool.OnClick(Ctx);
	TestTrue(TEXT("still a runway-holding position"), M2HoldToolKind(Net, RunwayEnd) == EHoldingPositionKind::Runway);
	TestFalse(TEXT("with a reason"), Tool.LastRefusal.IsEmpty());
	TestTrue(TEXT("and no undo step pushed for a refusal"), Actor->CanUndo() == bCouldUndoBefore);
	{
		FM2HoldSink Sink;
		Tool.BuildPreview(Ctx, Sink);
		TestEqual(TEXT("the preview shows the refusal"), Sink.Refused, 1);
		TestEqual(TEXT("and marks the runway end as refused, not doomed"), Sink.RefusedMarkers, 1);
	}
	Tool.OnCancel(Ctx);
	TestTrue(TEXT("cancel clears the refusal"), Tool.LastRefusal.IsEmpty());

	// 2. A CLICK ON THE TAXIWAY JUNCTION PLACES AN INTERMEDIATE POSITION.
	Ctx.Cursor = Net.GetGuidelineNode(Junction)->Position;
	Tool.OnClick(Ctx);
	TestTrue(TEXT("click sets an intermediate position at the junction"),
		M2HoldToolKind(Net, Junction) == EHoldingPositionKind::Intermediate);
	TestTrue(TEXT("with no refusal"), Tool.LastRefusal.IsEmpty());

	{
		FM2HoldSink Sink;
		GuidelineOverlay::Draw(Net, Sink);
		TestEqual(TEXT("the overlay draws the one derived runway bar"), Sink.RunwayBars, 1);
		TestEqual(TEXT("and the one intermediate bar"), Sink.IntermediateBars, 1);
		// ACROSS THE TAXIWAY, measured rather than asserted by name. CrossMark is handed the
		// direction of TRAVEL and the HUD draws perpendicular to it, so a bar that reads
		// correctly is one whose Along runs down the taxiway. This fixture's taxiway goes
		// E(60000,0) -> X(60000,-20000), which is the -Y axis - and the node also carries
		// turn paths onto the runway, so picking the wrong incident edge would show up here
		// as an Along with an X component.
		TestTrue(TEXT("the runway bar's Along runs down the taxiway, not off along a turn path"),
			FMath::Abs(Sink.LastAlong.Y) > 0.99);
	}

	// The hover on a set node warns that a click would REMOVE the position.
	{
		FM2HoldSink Flagged;
		Tool.BuildPreview(Ctx, Flagged);
		TestEqual(TEXT("hovering a set node marks it Doomed - the click would clear it"), Flagged.Doomed, 1);
	}

	// 3. UNDO AND REDO hand back FRESH network objects, so the node is re-found through
	//    Actor->Network each time rather than through a handle captured before the snapshot.
	TestTrue(TEXT("undoable"), Actor->Undo());
	TestTrue(TEXT("undo clears it"),
		M2HoldToolKind(*Actor->Network, M2HoldToolNodeFor(*Actor->Network, Tx, false)) == EHoldingPositionKind::None);
	TestTrue(TEXT("and undo never touched the derived runway end"),
		M2HoldToolKind(*Actor->Network, M2HoldToolNodeFor(*Actor->Network, Tx, true)) == EHoldingPositionKind::Runway);
	TestTrue(TEXT("redo"), Actor->Redo());
	TestTrue(TEXT("redo restores it"),
		M2HoldToolKind(*Actor->Network, M2HoldToolNodeFor(*Actor->Network, Tx, false)) == EHoldingPositionKind::Intermediate);

	// 4. A SECOND CLICK CLEARS.
	Ctx.Cursor = Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, false))->Position;
	Tool.OnClick(Ctx);
	TestTrue(TEXT("second click clears"),
		M2HoldToolKind(*Actor->Network, M2HoldToolNodeFor(*Actor->Network, Tx, false)) == EHoldingPositionKind::None);

	return true;
}

#endif
