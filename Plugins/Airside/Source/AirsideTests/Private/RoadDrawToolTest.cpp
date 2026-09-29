#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Present/RoadEditHistory.h"
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
		/** Where and how each of Labels was asked for, index for index - a readout's place and
		 *  colour are half of what it says. */
		TArray<FVector2D> LabelAt;
		TArray<EPreviewStyle> LabelStyle;

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
			LabelAt.Add(At);
			LabelStyle.Add(Style);
		}

		int32 FindLabel(const FString& Text) const
		{
			return Labels.IndexOfByKey(Text);
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

	/** Open ground, as the snap chain's Free fallback would report it. TestTool::ContextAt
	 *  (#104) plus this tool's own placement limits (see BaseContext above). */
	FToolContext AtGround(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		FToolContext Context = TestTool::ContextAt(*Actor, Where);
		Context.Limits.MinSegmentLength = 250.0;
		Context.Limits.MinTurnDegrees = 25.0;
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
	"Airside.Tool.RoadDraw",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadDrawToolTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD, not a bare NewObject: this tool drives the facade through the actor and a
	// half-built actor is not evidence about what it does (#104).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
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

	// BuildSession::SelectTool deactivates the outgoing tool with a default FToolContext
	// when the session holds no IRoadEditTarget of its own (see its doc comment) - which
	// means Context.Target is null here, not merely unusual. FRoadDrawTool::OnCancel's
	// early-out on a null Target used to leave State chaining in that case (issue #193),
	// so the part-drawn state survived to the tool's next activation with nothing able to
	// cancel it from outside a click.
	{
		Actor->ClearNetwork();
		FRoadDrawTool Tool;

		Tool.OnClick(AtGround(Actor, FVector2D(500.0, 500.0)));
		TestFalse(TEXT("setup: the click started a chain"), Tool.IsIdle());

		Tool.OnDeactivate(FToolContext());

		TestTrue(TEXT("deactivating with no target still abandons the chain"), Tool.IsIdle());
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

// NAMED DISTINCTLY, not Airside.Tool.RoadDraw.Length: a dotted child drops its bare-named
// parent from the automation tree, and only the run count would notice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadDrawLengthReadoutTest,
	"Airside.Tool.RoadDrawLengthReadout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadDrawLengthReadoutTest::RunTest(const FString& Parameters)
{
	// Reported 2026-09-27: the runway preview says how long the strip is, taxiways and roads
	// said nothing, so the player measured a taxiway by eye against a runway's number.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// Both kinds, because both run FRoadChainingState and the report named both.
	for (const ERoadKind Kind : { ERoadKind::Taxiway, ERoadKind::ServiceRoad })
	{
		const TCHAR* Name = Kind == ERoadKind::ServiceRoad ? TEXT("road") : TEXT("taxiway");

		{
			Actor->ClearNetwork();
			FRoadDrawTool Tool(Kind);
			Tool.OnClick(AtGround(Actor, FVector2D(0.0, 0.0)));

			FCountingPreviewSink Sink;
			Tool.BuildPreview(AtGround(Actor, FVector2D(8500.0, 0.0)), Sink);

			const int32 At = Sink.FindLabel(TEXT("85 m"));
			if (TestTrue(FString::Printf(TEXT("%s: the ghost says its length in metres (%s)"), Name,
					*FString::Join(Sink.Labels, TEXT(" | "))), At != INDEX_NONE))
			{
				// Mid-segment, so it never sits on the price or refusal at the cursor end.
				TestTrue(FString::Printf(TEXT("%s: at the segment's midpoint"), Name),
					Sink.LabelAt[At].Equals(FVector2D(4250.0, 0.0), 1.0));
				TestEqual(FString::Printf(TEXT("%s: in the ghost's own style"), Name),
					Sink.LabelStyle[At], EPreviewStyle::Pending);
			}
		}

		// A refused segment STILL says its length - "how long is it" matters most when the
		// answer is "too short", which is exactly what the runway's refusal says too.
		{
			Actor->ClearNetwork();
			FRoadDrawTool Tool(Kind);
			Tool.OnClick(AtGround(Actor, FVector2D(0.0, 0.0)));

			FCountingPreviewSink Sink;
			Tool.BuildPreview(AtGround(Actor, FVector2D(200.0, 0.0)), Sink);

			const int32 At = Sink.FindLabel(TEXT("2 m"));
			if (TestTrue(FString::Printf(TEXT("%s: a too-short ghost still says its length (%s)"), Name,
					*FString::Join(Sink.Labels, TEXT(" | "))), At != INDEX_NONE))
			{
				TestEqual(FString::Printf(TEXT("%s: coloured as refused, like the ghost"), Name),
					Sink.LabelStyle[At], EPreviewStyle::Refused);
			}
		}

		// The length of what the click BUILDS: with a snap guide active the click commits the
		// guide's point (ResolveToNode -> RoadGuidedSnap), not the raw cursor.
		{
			Actor->ClearNetwork();
			FRoadDrawTool Tool(Kind);
			Tool.OnClick(AtGround(Actor, FVector2D(0.0, 0.0)));

			FToolContext Guided = AtGround(Actor, FVector2D(4000.0, 300.0));
			Guided.Guide.bActive = true;
			Guided.Guide.Point = FVector2D(5000.0, 0.0);

			FCountingPreviewSink Sink;
			Tool.BuildPreview(Guided, Sink);

			TestTrue(FString::Printf(TEXT("%s: the length follows the guide, not the cursor (%s)"), Name,
				*FString::Join(Sink.Labels, TEXT(" | "))), Sink.FindLabel(TEXT("50 m")) != INDEX_NONE);
		}

		// Nothing to measure yet: an idle tool has no segment, so no length.
		{
			Actor->ClearNetwork();
			FRoadDrawTool Tool(Kind);
			FCountingPreviewSink Sink;
			Tool.BuildPreview(AtGround(Actor, FVector2D(4000.0, 0.0)), Sink);
			TestFalse(FString::Printf(TEXT("%s: an idle tool shows no length"), Name),
				Sink.Labels.ContainsByPredicate([](const FString& L) { return L.EndsWith(TEXT(" m")); }));
		}
	}

	return true;
}

/**
 * Roads and taxiways refuse inside a taxiway's clearance strip (strip stage 3), and the
 * tool's readout, its click and the facade's ConnectNodes give ONE answer - they all ask
 * URoadEditFacade::WhySegmentRefused, so none can approve what another refuses.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadDrawRefusedInsideStripTest,
	"Airside.Tool.RoadRefusedInsideStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadDrawRefusedInsideStripTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;

	// A TAXIWAY LAID THROUGH THE FACADE at the level's default width along y 0. Its keep-out
	// is read back rather than typed, so a change of default width moves the fixture's
	// expectation rather than silently clearing the road below.
	const auto LayTaxiway = [Actor]()
	{
		Actor->ClearNetwork();
		const int32 W = Actor->PlaceNode(FVector2D(-20000.0, 0.0));
		const int32 E = Actor->PlaceNode(FVector2D(20000.0, 0.0));
		Actor->ConnectNodes(W, E, ERoadKind::Taxiway, INDEX_NONE);
	};
	LayTaxiway();
	{
		const FRoadSegmentId Taxi = Actor->Network->SegmentIdAt(0);
		const URoadProfile* Profile = Actor->Network->ProfileFor(Actor->Network->GetSegments()[0]);
		const double Reach = TaxiwayStrip::StripWidthOf(*Actor->Network, Taxi)
			+ (Profile != nullptr ? Profile->GetMaxHalfWidth() : 0.0);
		if (!TestTrue(FString::Printf(TEXT("the default taxiway's keep-out reaches past 20 m (%.0f uu), or this proves nothing"), Reach),
			Reach > 2500.0))
		{
			return false;
		}
	}

	// 1. A ROAD ALONGSIDE, 20 m off: the readout says so, the click lays nothing, and the
	//    facade refuses the same pair of nodes directly.
	{
		FRoadDrawTool Tool(ERoadKind::ServiceRoad);
		Tool.OnClick(AtGround(Actor, FVector2D(-5000.0, 2000.0)));
		const int32 Start = Tool.GetPendingNode();
		if (!TestTrue(TEXT("the first click starts a chain"), Start != INDEX_NONE)) { return false; }

		FCountingPreviewSink Sink;
		Tool.BuildPreview(AtGround(Actor, FVector2D(5000.0, 2000.0)), Sink);
		const int32 Why = Sink.Labels.IndexOfByPredicate([](const FString& L) { return L.Contains(TEXT("clearance strip")); });
		if (TestTrue(FString::Printf(TEXT("the readout names the clearance strip (labels: %s)"), *FString::Join(Sink.Labels, TEXT(" | "))),
			Why != INDEX_NONE))
		{
			TestEqual(TEXT("in the Refused style"), Sink.LabelStyle[Why], EPreviewStyle::Refused);
		}

		const int32 Before = LiveSegments(Actor);
		Tool.OnClick(AtGround(Actor, FVector2D(5000.0, 2000.0)));
		TestEqual(TEXT("the click lays nothing"), LiveSegments(Actor), Before);

		const int32 Far = Actor->PlaceNode(FVector2D(5000.0, 2000.0));
		FLogLineSpy Spy(TEXT("LogRoadMesh"));
		GLog->AddOutputDevice(&Spy);
		const bool bConnected = Actor->ConnectNodes(Start, Far, ERoadKind::ServiceRoad, 0);
		GLog->RemoveOutputDevice(&Spy);
		TestFalse(TEXT("ConnectNodes refuses the same road"), bConnected);
		TestTrue(FString::Printf(TEXT("and logs why (%s)"), *FString::Join(Spy.CapturedLines, TEXT(" | "))),
			Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("ConnectNodes refused: inside")); }));
		TestEqual(TEXT("still nothing laid"), LiveSegments(Actor), Before);
	}

	// 2. THE SAME ROAD ENDING ON THE TAXIWAY AT A RIGHT ANGLE, by a Segment snap mid-taxiway
	//    (Review Focus 1): the preview judges the unsplit segment, the click the split one,
	//    and both must say yes.
	{
		LayTaxiway();
		FRoadDrawTool Tool(ERoadKind::ServiceRoad);
		Tool.OnClick(AtGround(Actor, FVector2D(8000.0, 10000.0)));

		const FToolContext OnTaxiway = AtSegment(Actor, 0, FVector2D(8000.0, 0.0));
		FCountingPreviewSink Sink;
		Tool.BuildPreview(OnTaxiway, Sink);
		TestFalse(FString::Printf(TEXT("the preview does not refuse a square join (labels: %s)"), *FString::Join(Sink.Labels, TEXT(" | "))),
			Sink.LabelStyle.Contains(EPreviewStyle::Refused));

		const int32 Before = LiveSegments(Actor);
		Tool.OnClick(OnTaxiway);
		TestEqual(TEXT("the click splits the taxiway and lays the road - two more live segments"),
			LiveSegments(Actor), Before + 2);
	}

	// 3. THE TAXIWAY TOOL refuses a parallel taxiway 20 m away - its pavement is in the
	//    first one's strip.
	{
		LayTaxiway();
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		Tool.OnClick(AtGround(Actor, FVector2D(-5000.0, 2000.0)));
		const int32 Before = LiveSegments(Actor);
		Tool.OnClick(AtGround(Actor, FVector2D(5000.0, 2000.0)));
		TestEqual(TEXT("a parallel taxiway 20 m off is refused"), LiveSegments(Actor), Before);
	}
	return true;
}

/**
 * A NODE DRAGGED INTO A STRIP IS REFUSED (strip stage 3, Review Focus 3): MoveNode judges every
 * segment the node drags, at the position it would land, before anything changes - so a
 * refusal leaves the node where it was and pushes no undo step.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoveNodeRefusedIntoStripTest,
	"Airside.Present.MoveNodeRefusedIntoStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMoveNodeRefusedIntoStripTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();

	// A 24 m taxiway along y 0 (40 m keep-out), and a service road running north from 60 m off it.
	Actor->ConnectNodes(Actor->PlaceNode(FVector2D(-20000.0, 0.0)), Actor->PlaceNode(FVector2D(20000.0, 0.0)),
		ERoadKind::Taxiway, INDEX_NONE);
	const int32 Far = Actor->PlaceNode(FVector2D(0.0, 10000.0));
	const int32 End = Actor->PlaceNode(FVector2D(0.0, 6000.0));
	if (!TestTrue(TEXT("the road is laid clear of the strip"), Actor->ConnectNodes(Far, End, ERoadKind::ServiceRoad, 0))) { return false; }

	const int32 UndoBefore = Actor->History->UndoDepth();
	FLogLineSpy Spy(TEXT("LogRoadMesh"));
	GLog->AddOutputDevice(&Spy);
	const bool bMoved = Actor->MoveNode(End, FVector2D(0.0, 2000.0));
	GLog->RemoveOutputDevice(&Spy);
	TestFalse(TEXT("dragging the road's end 20 m off the taxiway is refused"), bMoved);
	TestEqual(TEXT("the node is where it was"), Actor->Network->GetNodes()[End].Position, FVector2D(0.0, 6000.0));
	TestEqual(TEXT("and no undo step was pushed"), Actor->History->UndoDepth(), UndoBefore);
	TestTrue(FString::Printf(TEXT("the log says why (%s)"), *FString::Join(Spy.CapturedLines, TEXT(" | "))),
		Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("MoveNode refused: inside")); }));

	// THE CONTROL: a move that stays clear still moves, so the refusal above is the strip's.
	TestTrue(TEXT("a move to 50 m off is allowed"), Actor->MoveNode(End, FVector2D(0.0, 5000.0)));
	TestEqual(TEXT("and lands"), Actor->Network->GetNodes()[End].Position, FVector2D(0.0, 5000.0));
	return true;
}

/**
 * A HEAL THAT WOULD RUN THROUGH A STRIP IS SKIPPED, NOT THE DELETE (Review Focus 4). Deleting
 * the apex of a road bent around a taxiway's dead end would rejoin its two sides straight
 * across the end's keep-out: the node still goes, the heal does not, and the log says so -
 * silence would leave a gap the player did not ask for with nothing to explain it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHealRefusedAcrossStripTest,
	"Airside.Present.HealRefusedAcrossStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHealRefusedAcrossStripTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();

	// A taxiway ending at the origin, from the south.
	Actor->ConnectNodes(Actor->PlaceNode(FVector2D(0.0, -20000.0)), Actor->PlaceNode(FVector2D(0.0, 0.0)),
		ERoadKind::Taxiway, INDEX_NONE);
	// A road W - A - Apex - B - E bent up and over it; every piece 60 m+ from the dead end.
	const int32 W = Actor->PlaceNode(FVector2D(-16000.0, 3000.0));
	const int32 A = Actor->PlaceNode(FVector2D(-6000.0, 3000.0));
	const int32 Apex = Actor->PlaceNode(FVector2D(0.0, 9000.0));
	const int32 B = Actor->PlaceNode(FVector2D(6000.0, 3000.0));
	const int32 E = Actor->PlaceNode(FVector2D(16000.0, 3000.0));
	bool bLaid = Actor->ConnectNodes(W, A, ERoadKind::ServiceRoad, 0);
	bLaid &= Actor->ConnectNodes(A, Apex, ERoadKind::ServiceRoad, 0);
	bLaid &= Actor->ConnectNodes(Apex, B, ERoadKind::ServiceRoad, 0);
	bLaid &= Actor->ConnectNodes(B, E, ERoadKind::ServiceRoad, 0);
	if (!TestTrue(TEXT("the bent road is laid clear of the strip"), bLaid)) { return false; }

	const FRoadDeletionPlan Plan = Actor->PlanNodeDeletion(Apex);
	if (!TestTrue(TEXT("the apex plans a heal - or this proves nothing"), Plan.bValid && Plan.Rejoin.Num() > 0)) { return false; }

	FLogLineSpy Spy(TEXT("LogRoadMesh"));
	GLog->AddOutputDevice(&Spy);
	const bool bDeleted = Actor->DeleteNode(Apex);
	GLog->RemoveOutputDevice(&Spy);
	TestTrue(TEXT("the delete itself still happens"), bDeleted);
	TestNull(TEXT("the apex is gone"), Actor->Network->GetNode(Actor->Network->NodeIdAt(Apex)));
	TestEqual(TEXT("both stubs remain, and no heal across the strip - the taxiway, W-A and B-E"), LiveSegments(Actor), 3);
	TestTrue(FString::Printf(TEXT("the log names the skipped heal (%s)"), *FString::Join(Spy.CapturedLines, TEXT(" | "))),
		Spy.CapturedLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("Heal skipped: inside")); }));
	return true;
}

/**
 * A MOVE JUDGES THE NODE'S OWN ARMS AGAINST EACH OTHER (final review 2). Every arm is replaced
 * by its moved self, so each was ignored when judging the others - and a T-junction dragged
 * along its taxiway left the road running 28 degrees off the taxiway, along its strip.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoveNodeJudgesItsOwnArmsTest,
	"Airside.Present.MoveNodeJudgesItsOwnArms",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMoveNodeJudgesItsOwnArmsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();

	const int32 W = Actor->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 N = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 E = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	const int32 S = Actor->PlaceNode(FVector2D(0.0, 10000.0));
	bool bLaid = Actor->ConnectNodes(W, N, ERoadKind::Taxiway, INDEX_NONE);
	bLaid &= Actor->ConnectNodes(N, E, ERoadKind::Taxiway, INDEX_NONE);
	bLaid &= Actor->ConnectNodes(N, S, ERoadKind::ServiceRoad, 0);
	if (!TestTrue(TEXT("a square T-junction is laid"), bLaid)) { return false; }

	TestFalse(TEXT("dragging the junction to (190 m, -5 m) - the road now 28 degrees off W-N - is refused"),
		Actor->MoveNode(N, FVector2D(19000.0, -500.0)));
	TestEqual(TEXT("the junction is where it was"), Actor->Network->GetNodes()[N].Position, FVector2D(0.0, 0.0));
	TestTrue(TEXT("control: a short slide along the taxiway keeps the road square enough, and moves"),
		Actor->MoveNode(N, FVector2D(1000.0, 0.0)));
	return true;
}

/**
 * A MERGE REFUSES WHAT THE STRIP REFUSES (final review 3). The edit tool merges on drop whenever
 * the drag snapped to a node, whatever MoveNode answered - so the merge itself asks the judge,
 * of every arm it moved, in its own Verify: one evaluator, and the edit reverts whole.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMergeRefusedIntoStripTest,
	"Airside.Present.MergeRefusedIntoStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMergeRefusedIntoStripTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();

	// A taxiway ending at T, and a road whose end R sits beside T so that, folded into T, it
	// leaves T 20 degrees off the taxiway's arm - along its strip.
	const int32 W = Actor->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 T = Actor->PlaceNode(FVector2D(0.0, 0.0));
	Actor->ConnectNodes(W, T, ERoadKind::Taxiway, INDEX_NONE);
	const FVector2D Along(FMath::Cos(FMath::DegreesToRadians(160.0)), FMath::Sin(FMath::DegreesToRadians(160.0)));
	const int32 Far = Actor->PlaceNode(Along * 10000.0);
	const int32 R = Actor->PlaceNode(FVector2D(300.0, 300.0));
	// A LAYOUT THAT PREDATES THE STRIP (stage 3, 2026-09-29): the road already sits in the strip;
	// what is judged here is the merge - see TestTool::ConnectUnjudged.
	if (!TestTrue(TEXT("the road is laid"), TestTool::ConnectUnjudged(*Actor, Far, R, ERoadKind::ServiceRoad))) { return false; }

	const int32 SegmentsBefore = LiveSegments(Actor);
	TestFalse(TEXT("folding the road's end into the taxiway at 20 degrees is refused"), Actor->MergeNodes(T, R));
	TestNotNull(TEXT("the road's end survives - the edit reverted whole"), Actor->Network->GetNode(Actor->Network->NodeIdAt(R)));
	TestEqual(TEXT("and nothing was re-pointed"), LiveSegments(Actor), SegmentsBefore);
	return true;
}

/**
 * A SEGMENT SNAP ON A CURVED TAXIWAY: PREVIEW AND COMMIT AGREE (final review 4). The split
 * straightens the curve into two chords, so the preview judges those chords; a refusal is made
 * BEFORE the click splits anything, and leaves the taxiway whole.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCurvedSnapAgreesTest,
	"Airside.Tool.RoadCurvedSnapAgrees",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCurvedSnapAgreesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();

	// A quarter-arc taxiway, laid straight into the model - ConnectNodes lays straight only.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 10000.0));
	FRoadNodeId NA, NB;
	Actor->MakeLiveNodeId(A, NA);
	Actor->MakeLiveNodeId(B, NB);
	Actor->Network->AddSegment(NA, NB, FVector2D(10000.0, 0.0), Actor->ResolveProfileFor(ERoadKind::Taxiway, INDEX_NONE));
	Actor->RebuildMesh();

	// 80 degrees to the curve's tangent at its midpoint, 53 to the chord the split makes.
	const FVector2D OnArc(7500.0, 2500.0);
	const FVector2D Start = OnArc + FVector2D(FMath::Cos(FMath::DegreesToRadians(125.0)), FMath::Sin(FMath::DegreesToRadians(125.0))) * 6000.0;
	FRoadDrawTool Tool(ERoadKind::ServiceRoad);
	Tool.OnClick(AtGround(Actor, Start));

	const FToolContext Snap = AtSegment(Actor, 0, OnArc);
	FCountingPreviewSink Sink;
	Tool.BuildPreview(Snap, Sink);
	TestTrue(FString::Printf(TEXT("the preview refuses what the commit would (labels: %s)"), *FString::Join(Sink.Labels, TEXT(" | "))),
		Sink.LabelStyle.Contains(EPreviewStyle::Refused));

	Tool.OnClick(Snap);
	TestEqual(TEXT("the refused click leaves the taxiway whole - one segment, unsplit"), LiveSegments(Actor), 1);
	return true;
}

/**
 * A SKIPPED HEAL LEAVES NO BARE NODES (final review 5). The deletion plan sweeps only what it
 * expects the heal to leave bare; skip the heal and the two ends it would have joined had no
 * other road, so they are swept too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHealSkippedSweepsBareEndsTest,
	"Airside.Present.HealSkippedSweepsBareEnds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHealSkippedSweepsBareEndsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();

	Actor->ConnectNodes(Actor->PlaceNode(FVector2D(0.0, -20000.0)), Actor->PlaceNode(FVector2D(0.0, 0.0)),
		ERoadKind::Taxiway, INDEX_NONE);
	const int32 A = Actor->PlaceNode(FVector2D(-6000.0, 3000.0));
	const int32 Apex = Actor->PlaceNode(FVector2D(0.0, 9000.0));
	const int32 B = Actor->PlaceNode(FVector2D(6000.0, 3000.0));
	bool bLaid = Actor->ConnectNodes(A, Apex, ERoadKind::ServiceRoad, 0);
	bLaid &= Actor->ConnectNodes(Apex, B, ERoadKind::ServiceRoad, 0);
	if (!TestTrue(TEXT("the bent road is laid"), bLaid)) { return false; }

	TestTrue(TEXT("the apex deletes"), Actor->DeleteNode(Apex));
	TestEqual(TEXT("only the taxiway is left"), LiveSegments(Actor), 1);
	TestEqual(TEXT("and only its two nodes - A and B were not left bare"), LiveNodes(Actor), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
