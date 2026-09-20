#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/EditTool.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Prefixed against the UNITY build - these test files share one translation unit. */
	struct FEditToolSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		TMap<EPreviewStyle, int32> Lines;
		TArray<FString> Labels;

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Lines.FindOrAdd(Style)++;
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle) override
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeSuppressesTheBuildToolTest,
	"Airside.Tool.EditModeSuppressesTheBuildTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeSuppressesTheBuildToolTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD, not a bare NewObject: this goes through the facade, and a half-built
	// actor is not evidence about it (#104).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	FBuildSession Session;
	TestTrue(TEXT("a session opens in Build, so nothing is editable until it is asked for"),
		Session.GetGestureMode() == EGestureMode::Build);

	// Taxiway (registry index 1), so the tool under test is one that DOES build on a click.
	Session.SelectTool(1);
	Session.SetGestureMode(EGestureMode::Edit);

	FBuildSessionTunables Tunables;
	const FToolContext Context =
		Session.MakeContext(Actor, FVector2D(1000.0, 1000.0), Tunables, false, false);

	IBuildTool* Active = Session.GetActiveTool();
	if (!TestNotNull(TEXT("a tool is active in Edit"), Active)) { return false; }
	Active->OnClick(Context);

	// THE WHOLE POINT OF THE MODE. A click that built here is the misclick this feature
	// exists to remove, only running in the opposite direction.
	const URoadNetwork* Network = Actor->GetNetwork();
	const int32 Nodes = Network != nullptr ? Network->GetNodes().Num() : 0;
	TestEqual(TEXT("a click in Edit builds nothing, because the build tool does not run"),
		Nodes, 0);

	// AND THE SAME CLICK IN BUILD DOES BUILD, so this is measuring the mode rather than a
	// tool that was broken all along - the control the rule needs to mean anything.
	Session.SetGestureMode(EGestureMode::Build);
	Session.GetActiveTool()->OnClick(
		Session.MakeContext(Actor, FVector2D(1000.0, 1000.0), Tunables, false, false));
	TestEqual(TEXT("the identical click in Build lays a node"),
		Actor->GetNetwork()->GetNodes().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditHandlesAreDeclaredForEveryRegistryEntryTest,
	"Airside.Tool.EditHandlesAreDeclaredForEveryRegistryEntry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditHandlesAreDeclaredForEveryRegistryEntryTest::RunTest(const FString& Parameters)
{
	// NAMES, NOT COUNTS. CLAUDE.md: where UE forces two lists to agree the consumer checks
	// IDENTITY and logs both on mismatch. A count would pass on a table where two entries
	// had swapped their handle kinds, which is precisely the drift this field exists to
	// make impossible.
	const TMap<FName, EEditHandleKind> Expected = {
		{ TEXT("Select"),          EEditHandleKind::None            },
		{ TEXT("Taxiway"),         EEditHandleKind::AirsideNode     },
		{ TEXT("Apron"),           EEditHandleKind::ApronCorner     },
		{ TEXT("Stand"),           EEditHandleKind::None            },
		{ TEXT("Guideline"),       EEditHandleKind::None            },
		{ TEXT("Runway"),          EEditHandleKind::RunwayThreshold },
		{ TEXT("HoldingPosition"), EEditHandleKind::None            },
		{ TEXT("Road"),            EEditHandleKind::ServiceRoadNode },
		{ TEXT("FuelDepot"),       EEditHandleKind::None            },
	};

	for (const FToolRegistration& Entry : ToolRegistry())
	{
		const EEditHandleKind* Want = Expected.Find(Entry.Id);
		if (Want == nullptr)
		{
			// A NEW TOOL MUST DECLARE WHAT EDIT MEANS FOR IT, even when the answer is None.
			// Failing rather than defaulting is the point: the default is silent, and a tool
			// that should have been editable would simply never light a handle.
			AddError(FString::Printf(
				TEXT("registry entry '%s' is not named in this test - a new tool must say "
					 "what Edit exposes for it, even if that is None"), *Entry.Id.ToString()));
			continue;
		}
		TestTrue(*FString::Printf(
			TEXT("'%s' declares the edit handles this test expects"), *Entry.Id.ToString()),
			Entry.EditHandles == *Want);
	}

	TestEqual(TEXT("and the table holds no entry beyond the ones named here"),
		ToolRegistry().Num(), Expected.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditHandlesReachTheContextTest,
	"Airside.Tool.EditHandlesReachTheContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditHandlesReachTheContextTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	FBuildSession Session;
	FBuildSessionTunables Tunables;

	// DECLARING IS HALF THE WORK - the other half is the value reaching the tool. A field
	// filled in the registry and never read is the dead-list bug this codebase has shipped
	// three times (ToolCommandList, GetModeCommands, ARoadBuildController::Tools).
	Session.SelectTool(1);                       // Taxiway
	Session.SetGestureMode(EGestureMode::Edit);
	TestTrue(TEXT("the lit tool's handle kind reaches the context"),
		Session.MakeContext(Actor, FVector2D::ZeroVector, Tunables, false, false).EditHandles
			== EEditHandleKind::AirsideNode);

	// THE LIT TOOL FILTERS, so switching it changes what is grabbable without leaving Edit.
	Session.SelectTool(2);                       // Apron
	TestTrue(TEXT("switching the lit tool switches the handle kind, with Edit still held"),
		Session.MakeContext(Actor, FVector2D::ZeroVector, Tunables, false, false).EditHandles
			== EEditHandleKind::ApronCorner);

	// AND NONE OUTSIDE EDIT, so nothing downstream can act on a handle kind while the build
	// tool is the one running.
	Session.SetGestureMode(EGestureMode::Build);
	TestTrue(TEXT("no handles are offered while the mode is Build"),
		Session.MakeContext(Actor, FVector2D::ZeroVector, Tunables, false, false).EditHandles
			== EEditHandleKind::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapChainExcludesTheDraggedNodeTest,
	"Airside.Tool.SnapChainExcludesTheDraggedNode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapChainExcludesTheDraggedNodeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(12000.0, 0.0));
	Actor->ConnectNodes(A, B);

	const URoadNetwork* Network = Actor->GetNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	FRoadSnapChain Chain;
	FRoadSnapSettings Settings;

	// WITHOUT the exclusion a cursor on A resolves to A. Correct for a click, and fatal for
	// a drag: the node being moved claims its own cursor and is pinned where it already is.
	{
		const FRoadSnapResult Hit = Chain.Resolve(*Network, FVector2D(0.0, 0.0), Settings);
		TestTrue(TEXT("a cursor on a node resolves to that node, which is what a click needs"),
			Hit.Kind == ERoadSnapKind::Node && Hit.Node.Index == A);
	}

	// WITH it, the same cursor falls through: A is excluded, its one arm is excluded with
	// it, and B is 120 m away.
	{
		FRoadSnapQuery Query;
		Query.Cursor = FVector2D(0.0, 0.0);
		Query.ExcludeNode = Network->NodeIdAt(A);

		const FRoadSnapResult Hit = Chain.Resolve(*Network, Query, Settings);
		TestTrue(TEXT("the excluded node does not claim its own cursor, which is what lets a "
					  "dragged node move at all"),
			Hit.Kind != ERoadSnapKind::Node);
	}

	// AND IT IS NOT A BLANKET REFUSAL. Another node is still found while one is excluded -
	// the merge-target case, and the reason ExcludeNode is a node rather than a bool.
	{
		FRoadSnapQuery Query;
		Query.Cursor = FVector2D(12000.0, 0.0);
		Query.ExcludeNode = Network->NodeIdAt(A);

		const FRoadSnapResult Hit = Chain.Resolve(*Network, Query, Settings);
		TestTrue(TEXT("a different node is still found while one is excluded"),
			Hit.Kind == ERoadSnapKind::Node && Hit.Node.Index == B);
	}

	// AN ARM OF THE EXCLUDED NODE IS NOT SPLITTABLE EITHER. Mid-arm, the segment rule would
	// otherwise offer to split the very road being dragged.
	{
		FRoadSnapQuery Query;
		Query.Cursor = FVector2D(6000.0, 0.0);
		Query.ExcludeNode = Network->NodeIdAt(A);

		const FRoadSnapResult Hit = Chain.Resolve(*Network, Query, Settings);
		TestTrue(TEXT("the dragged node's own arm offers no split"),
			Hit.Kind != ERoadSnapKind::Segment);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeDragSnapsExactlyToANodeTest,
	"Airside.Tool.EditModeDragSnapsExactlyToANode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDragSnapsExactlyToANodeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A road A-B to grab the end of, and a separate road C-D far enough away that only C
	// can claim the drop. C needs an arm of its own or it is not an AirsideNode handle.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(12000.0, 0.0));
	Actor->ConnectNodes(A, B);
	const int32 C = Actor->PlaceNode(FVector2D(0.0, 20000.0));
	const int32 D = Actor->PlaceNode(FVector2D(12000.0, 20000.0));
	Actor->ConnectNodes(C, D);

	const FVector2D CPosition = Actor->GetNetwork()->GetNodes()[C].Position;

	FBuildSession Session;
	Session.SelectTool(1);                       // Taxiway: lights AirsideNode handles
	Session.SetGestureMode(EGestureMode::Edit);
	FBuildSessionTunables Tunables;

	IBuildTool* Tool = Session.GetActiveTool();
	Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));

	// Just SHORT of C - inside its snap reach but not on it. A CLICK here would land on C
	// exactly; the whole point of this feature is that the drag now does the same.
	const FVector2D NearC = CPosition - FVector2D(0.0, 90.0);
	Tool->OnDrag(Session.MakeContext(Actor, NearC, Tunables, false, false));

	const FVector2D Landed = Actor->GetNetwork()->GetNodes()[A].Position;

	// BITWISE, not within a tolerance. FRoadSnapResult's contract is that a Node snap
	// carries the node's stored position copied verbatim - "not the cursor, and not a
	// recomputed value" - and a near miss here is a drag that merely LOOKS snapped, which
	// is the thing a merge cannot be built on.
	TestTrue(TEXT("the dragged node lands on exactly the coordinates the graph holds for the "
				  "node it snapped to, as a click would"),
		Landed.X == CPosition.X && Landed.Y == CPosition.Y);

	// AND IT REALLY TRAVELLED. Without the control, a drag that silently did nothing would
	// pass the assertion above the moment the node happened to start there.
	TestTrue(TEXT("and it is not simply where it started"), Landed.Y != 0.0);

	Tool->OnDragEnd(Session.MakeContext(Actor, NearC, Tunables, false, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadDrawToolNoLongerDragsNodesTest,
	"Airside.Tool.RoadDrawToolNoLongerDragsNodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadDrawToolNoLongerDragsNodesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(12000.0, 0.0));
	Actor->ConnectNodes(A, B);
	const FVector2D Was = Actor->GetNetwork()->GetNodes()[A].Position;

	FBuildSession Session;
	Session.SelectTool(1);                       // Taxiway, in BUILD mode
	FBuildSessionTunables Tunables;

	IBuildTool* Tool = Session.GetActiveTool();
	Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));
	Tool->OnDrag(Session.MakeContext(Actor, FVector2D(3000.0, 3000.0), Tunables, false, false));
	Tool->OnDragEnd(Session.MakeContext(Actor, FVector2D(3000.0, 3000.0), Tunables, false, false));

	// THE MISCLICK, PINNED. Any press that travelled over a node used to reshape the road,
	// with no way to decline it. Editing is deliberate now, so a drag under a build tool
	// must move nothing at all.
	const FVector2D Now = Actor->GetNetwork()->GetNodes()[A].Position;
	TestTrue(TEXT("a drag under the road tool moves nothing - editing needs the Edit mode"),
		Now.X == Was.X && Now.Y == Was.Y);

	// AND THE CONTROL: the identical drag in Edit does move it, so this is measuring the
	// mode rather than a drag that is broken everywhere.
	Session.SetGestureMode(EGestureMode::Edit);
	IBuildTool* Editing = Session.GetActiveTool();
	Editing->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));
	Editing->OnDrag(Session.MakeContext(Actor, FVector2D(3000.0, 3000.0), Tunables, false, false));
	Editing->OnDragEnd(Session.MakeContext(Actor, FVector2D(3000.0, 3000.0), Tunables, false, false));

	const FVector2D Moved = Actor->GetNetwork()->GetNodes()[A].Position;
	TestTrue(TEXT("the same drag in Edit does move it"),
		Moved.X != Was.X || Moved.Y != Was.Y);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditHandlesAreDrawnForTheLitToolTest,
	"Airside.Tool.EditHandlesAreDrawnForTheLitTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditHandlesAreDrawnForTheLitToolTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// One taxiway and one service road, so the filter has something to get wrong.
	const int32 T0 = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 T1 = Actor->PlaceNode(FVector2D(12000.0, 0.0));
	Actor->ConnectNodes(T0, T1, ERoadKind::Taxiway);
	const int32 R0 = Actor->PlaceNode(FVector2D(0.0, 30000.0));
	const int32 R1 = Actor->PlaceNode(FVector2D(12000.0, 30000.0));
	Actor->ConnectNodes(R0, R1, ERoadKind::ServiceRoad);

	FBuildSession Session;
	FBuildSessionTunables Tunables;
	Session.SetGestureMode(EGestureMode::Edit);

	// DRAWING IS THE HALF THAT MATTERS. A filter that computed the right set and drew
	// nothing would leave the mode looking identical to having no handles at all - the
	// failure IBuildTool::WantsFreeStartGuides records for guides.
	Session.SelectTool(1);                       // Taxiway
	{
		FEditToolSink Sink;
		Session.GetActiveTool()->BuildPreview(
			Session.MakeContext(Actor, FVector2D(50000.0, 50000.0), Tunables, false, false), Sink);
		TestEqual(TEXT("the taxiway's two nodes are drawn as handles, and the service road's "
					   "are not"), Sink.CountMarkers(EPreviewStyle::Handle), 2);
	}

	Session.SelectTool(7);                       // Road (service road) - registry index 7
	{
		FEditToolSink Sink;
		Session.GetActiveTool()->BuildPreview(
			Session.MakeContext(Actor, FVector2D(50000.0, 50000.0), Tunables, false, false), Sink);
		TestEqual(TEXT("switching the lit tool switches which nodes are grabbable"),
			Sink.CountMarkers(EPreviewStyle::Handle), 2);
	}

	// AND NOTHING AT ALL OUTSIDE EDIT, so the handles cannot litter the build modes.
	Session.SetGestureMode(EGestureMode::Build);
	{
		FEditToolSink Sink;
		Session.GetActiveTool()->BuildPreview(
			Session.MakeContext(Actor, FVector2D(50000.0, 50000.0), Tunables, false, false), Sink);
		TestEqual(TEXT("no handles are drawn while the mode is Build"),
			Sink.CountMarkers(EPreviewStyle::Handle), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeDragOffersGuidesTest,
	"Airside.Tool.EditModeDragOffersGuides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDragOffersGuidesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A road to drag the end of, and a second road off to one side whose nodes are something
	// to line up WITH.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(9000.0, 0.0));
	Actor->ConnectNodes(A, B);
	const int32 L0 = Actor->PlaceNode(FVector2D(3000.0, 15000.0));
	const int32 L1 = Actor->PlaceNode(FVector2D(9000.0, 15000.0));
	Actor->ConnectNodes(L0, L1);

	FBuildSession Session;
	Session.SelectTool(1);
	Session.SetGestureMode(EGestureMode::Edit);
	FBuildSessionTunables Tunables;

	IBuildTool* Tool = Session.GetActiveTool();
	Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));
	if (!TestTrue(TEXT("the drag started"), !Tool->IsIdle())) { return false; }

	// Drag A to very nearly level with the landmark row, and far enough from every node that
	// no snap claims it - a guide, not a snap, is what is under test.
	const FVector2D NearRow(0.0, 15000.0 - 60.0);
	const FToolContext Context = Session.MakeContext(Actor, NearRow, Tunables, false, false);

	TestTrue(TEXT("a guide resolves for a drag, which it never did while the drag described "
				  "no anchor at all"),
		Context.Guide.bActive);

	// MEASURES THE DRAWING, NOT THE DESCRIBING. IBuildTool::WantsFreeStartGuides records a
	// tool that described an anchor, had a guide computed for it and drew nothing - which
	// showed the player exactly what having no guide shows them.
	FEditToolSink Sink;
	Tool->BuildPreview(Context, Sink);
	TestTrue(TEXT("and the dashed line to whatever it lined up with is actually drawn"),
		Sink.CountLines(EPreviewStyle::Guide) > 0);

	Tool->OnDragEnd(Context);

	// AND THE GUIDE DIES WITH THE GESTURE. A winner left behind would be inherited by the
	// next drag and then held through the hysteresis rule itself.
	const FToolContext After = Session.MakeContext(Actor, NearRow, Tunables, false, false);
	TestFalse(TEXT("no guide is offered once nothing is in hand"), After.Guide.bActive);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeDropOnNodeMergesTest,
	"Airside.Tool.EditModeDropOnNodeMerges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDropOnNodeMergesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// Two roads that do not touch. Dragging B onto C is the gesture that joins them, and the
	// one the report asked for: there was no way to merge two close points at all.
	const int32 A = Actor->PlaceNode(FVector2D(-9000.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(0.0, 0.0));
	Actor->ConnectNodes(A, B);
	const int32 C = Actor->PlaceNode(FVector2D(600.0, 0.0));
	const int32 D = Actor->PlaceNode(FVector2D(9600.0, 0.0));
	Actor->ConnectNodes(C, D);

	const FVector2D CPosition = Actor->GetNetwork()->GetNodes()[C].Position;

	auto LiveNodes = [Actor]()
	{
		int32 Count = 0;
		for (const FRoadNode& Node : Actor->GetNetwork()->GetNodes())
		{
			Count += Node.bAlive ? 1 : 0;
		}
		return Count;
	};
	TestEqual(TEXT("four nodes to begin with"), LiveNodes(), 4);

	FBuildSession Session;
	Session.SelectTool(1);
	Session.SetGestureMode(EGestureMode::Edit);
	FBuildSessionTunables Tunables;

	IBuildTool* Tool = Session.GetActiveTool();
	Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, false, false));

	const FVector2D OnC = CPosition - FVector2D(40.0, 0.0);
	Tool->OnDrag(Session.MakeContext(Actor, OnC, Tunables, false, false));
	Tool->OnDragEnd(Session.MakeContext(Actor, OnC, Tunables, false, false));

	TestEqual(TEXT("dropping one node on another leaves one node where there were two"),
		LiveNodes(), 3);
	TestNull(TEXT("the node in hand is the one absorbed"), Actor->GetNetwork()->GetNode(
		Actor->GetNetwork()->NodeIdAt(B)));

	const FRoadNode* Survivor = Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(C));
	if (!TestNotNull(TEXT("the node aimed AT is the one that survives"), Survivor)) { return false; }
	TestEqual(TEXT("and it carries both roads, so the two runs are now one"),
		Survivor->Incident.Num(), 2);

	// ONE UNDO STEP FOR THE WHOLE DROP. The move and the merge are one action to the player,
	// so they must be one press - which is why MergeNodes runs INSIDE the drag's interactive
	// edit rather than opening an edit of its own.
	Actor->Undo();
	TestEqual(TEXT("a single undo puts both nodes back - the drag and its merge are one step"),
		LiveNodes(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeNamesItselfTest,
	"Airside.Tool.EditModeNamesItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeNamesItselfTest::RunTest(const FString& Parameters)
{
	// The overlay draws GetDisplayName so the active mode is never a thing you have to
	// remember. An edit tool reporting the build tool's name would make the mode invisible
	// at exactly the moment the player needs to know they are in it.
	FBuildSession Session;
	Session.SelectTool(1);
	const FString BuildName = Session.GetActiveTool()->GetDisplayName().ToString();

	Session.SetGestureMode(EGestureMode::Edit);
	const FString EditName = Session.GetActiveTool()->GetDisplayName().ToString();

	TestEqual(TEXT("the edit tool names itself Edit"), EditName, FString(TEXT("Edit")));
	TestNotEqual(TEXT("and that is not the lit build tool's name"), EditName, BuildName);
	return true;
}

#endif
