#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Counts what a tool asked to have drawn, so previews can be asserted without a HUD. */
	struct FCountingPreviewSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		TMap<EPreviewStyle, int32> Lines;
		TArray<FString> Labels;

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			Lines.FindOrAdd(Style)++;
		}
		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			Labels.Add(Text);
		}

		int32 CountMarkers(EPreviewStyle Style) const
		{
			const int32* Found = Markers.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
		int32 CountLines(EPreviewStyle Style) const
		{
			const int32* Found = Lines.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
	};

	FToolContext BaseContext(ARoadNetworkActor* Actor)
	{
		FToolContext Context;
		Context.Target = Actor;
		Context.Limits.MinSegmentLength = 250.0;
		Context.Limits.MinTurnDegrees = 25.0;
		return Context;
	}

	/** Open ground, as the snap chain's Free fallback would report it. */
	FToolContext AtGround(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		FToolContext Context = BaseContext(Actor);
		Context.Snap.Kind = ERoadSnapKind::Free;
		Context.Snap.Position = Where;
		Context.Cursor = Where;
		return Context;
	}

	/** On an existing node, as the node rule would report it. */
	FToolContext AtNode(ARoadNetworkActor* Actor, int32 NodeIndex)
	{
		FToolContext Context = BaseContext(Actor);
		const FRoadNode& Node = Actor->Network->GetNodes()[NodeIndex];

		Context.Snap.Kind = ERoadSnapKind::Node;
		Context.Snap.Node.Index = NodeIndex;
		Context.Snap.Node.Generation = Node.Generation;
		Context.Snap.Position = Node.Position;
		Context.Cursor = Node.Position;
		return Context;
	}

	/** On a segment, as the segment rule would report it. */
	FToolContext AtSegment(ARoadNetworkActor* Actor, int32 SegmentIndex, const FVector2D& Where)
	{
		FToolContext Context = BaseContext(Actor);
		Context.Snap.Kind = ERoadSnapKind::Segment;
		Context.Snap.Segment.Index = SegmentIndex;
		Context.Snap.Segment.Generation = Actor->Network->GetSegments()[SegmentIndex].Generation;
		Context.Snap.Position = Where;
		Context.Cursor = Where;
		return Context;
	}

	int32 LiveNodes(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FRoadNode& Node : Actor->Network->GetNodes())
		{
			if (Node.bAlive) { ++Alive; }
		}
		return Alive;
	}

	int32 LiveSegments(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FRoadSegment& Segment : Actor->Network->GetSegments())
		{
			if (Segment.bAlive) { ++Alive; }
		}
		return Alive;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadDrawToolTest,
	"RoadNet.Tool.RoadDraw",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadDrawToolTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// The whole interaction layer was untestable while it lived on a PlayerController in
	// the game module, which has no test target - every click behaviour was checked by eye.
	// A tool that takes a context and drives a facade is a sequence of ordinary calls.

	// --- Drawing a road, click by click ----------------------------------------------
	{
		FRoadDrawTool Tool;
		TestTrue(TEXT("a fresh tool has nothing part-drawn"), Tool.IsIdle());

		Tool.OnClick(AtGround(Actor, FVector2D(0.0, 0.0)));
		TestFalse(TEXT("the first click starts a chain"), Tool.IsIdle());
		TestEqual(TEXT("and places one node"), LiveNodes(Actor), 1);
		TestEqual(TEXT("but no road yet"), LiveSegments(Actor), 0);

		const int32 Started = Tool.GetPendingNode();
		Tool.OnClick(AtGround(Actor, FVector2D(4000.0, 0.0)));
		TestEqual(TEXT("the second click adds the far node"), LiveNodes(Actor), 2);
		TestEqual(TEXT("and connects them"), LiveSegments(Actor), 1);
		TestFalse(TEXT("the chain continues"), Tool.IsIdle());
		TestNotEqual(TEXT("chaining from the node just reached"), Tool.GetPendingNode(), Started);
	}

	// --- The reported bug, now an assertion ------------------------------------------
	//
	// Click once to start a road, then right-click to leave the tool: the node that click
	// dropped has no road on it, and used to be left behind on the map for ever.
	{
		Actor->ClearNetwork();
		FRoadDrawTool Tool;

		Tool.OnClick(AtGround(Actor, FVector2D(1000.0, 1000.0)));
		TestEqual(TEXT("a node is dropped"), LiveNodes(Actor), 1);

		Tool.OnCancel(AtGround(Actor, FVector2D(1000.0, 1000.0)));
		TestTrue(TEXT("cancelling ends the chain"), Tool.IsIdle());
		TestEqual(TEXT("and takes the bare node it dropped with it"), LiveNodes(Actor), 0);
	}

	// A node that picked up a road is part of the network now, whoever made it.
	{
		Actor->ClearNetwork();
		FRoadDrawTool Tool;

		Tool.OnClick(AtGround(Actor, FVector2D(0.0, 0.0)));
		Tool.OnClick(AtGround(Actor, FVector2D(4000.0, 0.0)));
		Tool.OnCancel(AtGround(Actor, FVector2D(4000.0, 0.0)));

		TestTrue(TEXT("cancelling a drawn road ends the chain"), Tool.IsIdle());
		TestEqual(TEXT("and keeps both its nodes"), LiveNodes(Actor), 2);
		TestEqual(TEXT("and the road"), LiveSegments(Actor), 1);
	}

	// Starting a chain ON an existing node and then cancelling must not delete that node -
	// the chain did not create it. This is what the created flag is for.
	{
		Actor->ClearNetwork();
		const int32 Existing = Actor->PlaceNode(FVector2D(2000.0, 2000.0));
		FRoadDrawTool Tool;

		Tool.OnClick(AtNode(Actor, Existing));
		TestFalse(TEXT("clicking an existing node starts a chain from it"), Tool.IsIdle());

		Tool.OnCancel(AtNode(Actor, Existing));
		TestTrue(TEXT("cancelling ends it"), Tool.IsIdle());
		TestTrue(TEXT("and leaves a node the chain did not create alone"),
			Actor->Network->GetNodes()[Existing].bAlive);
	}

	// --- Modifiers --------------------------------------------------------------------
	{
		Actor->ClearNetwork();
		const int32 West = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 East = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(West, East);

		FRoadDrawTool Tool;

		// Shift on a road inserts a node WITHOUT starting a chain, which is the whole
		// difference between it and a plain click.
		FToolContext Insert = AtSegment(Actor, 0, FVector2D(3000.0, 0.0));
		Insert.bInsertModifier = true;
		Tool.OnClick(Insert);

		TestEqual(TEXT("shift-click inserts a node"), LiveNodes(Actor), 3);
		TestEqual(TEXT("splitting the road in two"), LiveSegments(Actor), 2);
		TestTrue(TEXT("and starts nothing"), Tool.IsIdle());

		// Ctrl removes whatever the snap resolved.
		FToolContext Remove = AtNode(Actor, West);
		Remove.bRemoveModifier = true;
		Tool.OnClick(Remove);
		TestFalse(TEXT("ctrl-click removes the node"), Actor->Network->GetNodes()[West].bAlive);
	}

	// A plain click on a road splits it AND chains on, which is what makes drawing a road
	// into an existing one one gesture rather than two.
	{
		Actor->ClearNetwork();
		const int32 West = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 East = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(West, East);

		FRoadDrawTool Tool;
		Tool.OnClick(AtSegment(Actor, 0, FVector2D(3000.0, 0.0)));

		TestEqual(TEXT("a plain click on a road also splits it"), LiveNodes(Actor), 3);
		TestFalse(TEXT("but it chains on from the new node"), Tool.IsIdle());
	}

	// --- Leaving the tool -------------------------------------------------------------
	{
		Actor->ClearNetwork();
		FRoadDrawTool Tool;

		Tool.OnClick(AtGround(Actor, FVector2D(500.0, 500.0)));
		Tool.OnDeactivate(AtGround(Actor, FVector2D(500.0, 500.0)));

		TestTrue(TEXT("switching tools abandons a part-drawn chain"), Tool.IsIdle());
		TestEqual(TEXT("and does not leave its node behind"), LiveNodes(Actor), 0);
	}

	// --- Preview ----------------------------------------------------------------------
	//
	// BuildPreview is const, per design spec 7.2, so a tool physically cannot mutate the
	// network while describing what it would do. Asserted rather than assumed.
	{
		Actor->ClearNetwork();
		FRoadDrawTool Tool;
		Tool.OnClick(AtGround(Actor, FVector2D(0.0, 0.0)));

		const int32 NodesBefore = LiveNodes(Actor);

		FCountingPreviewSink Sink;
		Tool.BuildPreview(AtGround(Actor, FVector2D(4000.0, 0.0)), Sink);

		TestTrue(TEXT("a chaining tool marks where the road would run from"),
			Sink.CountMarkers(EPreviewStyle::Pending) > 0);
		TestEqual(TEXT("drawing a preview creates nothing"), LiveNodes(Actor), NodesBefore);

		// Aiming a deletion shows what would go, and the road that would replace it.
		Actor->ClearNetwork();
		const int32 Hub = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 Spoke = Actor->PlaceNode(FVector2D(5000.0, 0.0));
		Actor->ConnectNodes(Hub, Spoke);

		FToolContext Aim = AtNode(Actor, Hub);
		Aim.bRemoveModifier = true;

		FCountingPreviewSink Doomed;
		FRoadDrawTool Fresh;
		Fresh.BuildPreview(Aim, Doomed);

		TestTrue(TEXT("aiming a deletion marks the doomed node"),
			Doomed.CountMarkers(EPreviewStyle::Doomed) > 0);
		TestTrue(TEXT("and the road that goes with it"),
			Doomed.CountLines(EPreviewStyle::Doomed) > 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
