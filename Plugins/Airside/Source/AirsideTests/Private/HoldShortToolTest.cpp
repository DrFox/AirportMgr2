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
#include "Tool/HoldShortTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Counts what a hold-short gesture emits. NOT a HUD: the styles are meanings, so a
	 * test is exactly as valid a sink as a canvas - see IToolPreviewSink.
	 *
	 * The M2Hold prefix is deliberate. AirsideTests is a UNITY build, so a bare FHoldSink
	 * here would collide with any other file's own.
	 */
	struct FM2HoldSink : IToolPreviewSink
	{
		int32 HoldBars = 0;
		int32 Refused = 0;
		FString LastLabel;

		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::HoldShort) { ++HoldBars; }
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldShortToolTest,
	"Airside.Tool.HoldShort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldShortToolTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD AND A REAL ACTOR, not the model alone: the claim under test reaches
	// through the facade to the undo stack, and a bar placed on a URoadNetwork by hand
	// would prove nothing about whether the player can take it back.
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

	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FRoadNodeId T = Net.AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net.AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net.AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net.AddStraightSegment(T, E, Runway);
	Net.AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net.AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net.AddStraightSegment(E, X, Taxiway);
	Actor->RebuildMesh();

	const FGuidelineNodeId Bar = M2HoldToolNodeFor(Net, Tx, true);
	const FGuidelineNodeId Far = M2HoldToolNodeFor(Net, Tx, false);
	if (!TestTrue(TEXT("both taxiway end nodes exist"), Bar.IsSet() && Far.IsSet())) { return false; }

	// The registry knows the tool, under key 8, and the session builds it.
	TestTrue(TEXT("registered under Eight"), ToolRegistry().Last().Key == EKeys::Eight);
	FBuildSession Session;
	TestEqual(TEXT("the session holds every registered tool"), Session.NumTools(), ToolRegistry().Num());

	FHoldShortTool Tool;
	FToolContext Ctx;
	Ctx.Target = Actor;
	Ctx.SnapRadius = 400.0;

	Ctx.Cursor = Net.GetGuidelineNode(Bar)->Position;
	Tool.OnClick(Ctx);

	// A CHAIN MEMBER, not the one segment named above - the same reason
	// Airside.Build.HoldShortSurvivesRebuild states: E joins two continuous runway segments,
	// so which of them RunwayNearGuidelineNode's one-hop walk meets first is decided by the
	// junction's arm order. Both ARE the runway, and a bar can protect nothing else.
	TestTrue(TEXT("click sets the bar for the runway it joins"),
		Net.RunwayChain(R1).Contains(Net.GetGuidelineNode(Bar)->HoldShortFor));
	FM2HoldSink Sink;
	GuidelineOverlay::Draw(Net, Sink);
	TestEqual(TEXT("the overlay draws one hold bar"), Sink.HoldBars, 1);

	// Undo and redo hand back FRESH network objects, so the node is re-found through
	// Actor->Network each time rather than through a handle captured before the snapshot.
	TestTrue(TEXT("undoable"), Actor->Undo());
	TestFalse(TEXT("undo clears it"),
		Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->HoldShortFor.IsSet());
	TestTrue(TEXT("redo"), Actor->Redo());
	TestTrue(TEXT("redo restores it"),
		Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->HoldShortFor.IsSet());

	Ctx.Cursor = Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->Position;
	Tool.OnClick(Ctx);
	TestFalse(TEXT("second click clears"),
		Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->HoldShortFor.IsSet());

	Ctx.Cursor = Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, false))->Position;
	Tool.OnClick(Ctx);
	TestFalse(TEXT("a node with no runway near is refused"),
		Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, false))->HoldShortFor.IsSet());
	TestFalse(TEXT("with a reason"), Tool.LastRefusal.IsEmpty());
	FM2HoldSink Sink2;
	Tool.BuildPreview(Ctx, Sink2);
	TestEqual(TEXT("the preview shows the refusal"), Sink2.Refused, 1);
	return true;
}

#endif
