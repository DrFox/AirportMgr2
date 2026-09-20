#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/EditTool.h"
#include "Tool/RoadBuildTool.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadNaming.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoveRunwayThresholdRefusesUnderMinimumLengthTest,
	"Airside.Model.MoveRunwayThresholdRefusesUnderMinimumLength",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMoveRunwayThresholdRefusesUnderMinimumLengthTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A NODE FIRST, purely to bring the network into being: the facade creates URoadNetwork
	// lazily inside PlaceNode and PlaceRunway does NOT - see LayRunway in the fixtures, which
	// records the crash that taught this.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

	// Our own bar rather than the fixture's LayRunway, which drops MinimumRunwayLength to
	// 100 uu and would leave this test with nothing to refuse.
	const double Minimum = 10000.0;
	Actor->MinimumRunwayLength = Minimum;

	URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Profile->bContinuousThroughJunctions = true;

	// Comfortably over the minimum, so there is room to drag the threshold IN.
	Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(Minimum * 1.5, 0.0), Profile);

	const URoadNetwork* Network = Actor->GetNetwork();
	if (!TestTrue(TEXT("the runway was laid"), Network != nullptr && Network->GetNodes().Num() >= 2))
	{
		return false;
	}

	// The threshold at the far end - the one a drag would pull inwards.
	int32 Far = INDEX_NONE;
	for (int32 Index = 0; Index < Network->GetNodes().Num(); ++Index)
	{
		if (Network->GetNodes()[Index].bAlive && Network->GetNodes()[Index].Position.X > Minimum)
		{
			Far = Index;
			break;
		}
	}
	if (!TestTrue(TEXT("a far threshold exists"), Far != INDEX_NONE)) { return false; }

	// A SHORTENING THAT STAYS LEGAL IS ACCEPTED - the control, without which the refusal
	// below would pass on a MoveNode that had simply stopped working on runways.
	TestTrue(TEXT("a threshold may be pulled in while the strip stays long enough"),
		Actor->MoveNode(Far, FVector2D(Minimum * 1.2, 0.0)));

	// AND ONE THAT WOULD CUT IT UNDER THE MINIMUM IS REFUSED. MinSegmentLength is the
	// solver's floor and says nothing about whether a STRIP is still a runway; without this
	// clause the aircraft admitted yesterday is refused today with nothing to say when it
	// changed.
	TestFalse(TEXT("but not past the minimum runway length"),
		Actor->MoveNode(Far, FVector2D(Minimum * 0.5, 0.0)));

	TestTrue(TEXT("and the refused move left the threshold exactly where it was"),
		Actor->GetNetwork()->GetNodes()[Far].Position.Equals(FVector2D(Minimum * 1.2, 0.0), 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDraggingAThresholdRedesignatesTheRunwayTest,
	"Airside.Model.DraggingAThresholdRedesignatesTheRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDraggingAThresholdRedesignatesTheRunwayTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// THE DESIGNATOR IS DERIVED, NOT STORED - RunwayDesignator::ToPairText takes a direction
	// and keeps nothing, and RoadNaming's header was written for this exact case: "the same
	// strip must not become '27/09' because a node was dragged". So this needed no code at
	// all, and this test is what stops that being an assumption.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	Actor->MinimumRunwayLength = 10000.0;

	URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Profile->bContinuousThroughJunctions = true;

	const double Length = Actor->MinimumRunwayLength * 1.5;
	Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(Length, 0.0), Profile);

	const URoadNetwork* Network = Actor->GetNetwork();
	if (!TestTrue(TEXT("the runway was laid"), Network != nullptr && Network->GetSegments().Num() > 0))
	{
		return false;
	}

	const FRoadSegmentId Strip = Network->SegmentIdAt(0);
	// NAMED AS A RUNWAY, whatever the bearing works out to. Which pair of numbers +X earns
	// is RunwayDesignator's business and the compass convention's - see
	// Airside.Tool.WorldAxesAreNamedByTheCompass - and asserting a particular pair here
	// would be this test holding a second opinion about north.
	const FString Before = RoadNaming::Describe(*Network, Strip);
	TestTrue(*FString::Printf(TEXT("the strip is named as a runway, not as a road: '%s'"),
		*Before), Before.Contains(TEXT("runway")));

	// Swing the far threshold well north - about 37 degrees, four designator steps.
	int32 Far = INDEX_NONE;
	for (int32 Index = 0; Index < Network->GetNodes().Num(); ++Index)
	{
		if (Network->GetNodes()[Index].bAlive && Network->GetNodes()[Index].Position.X > Length * 0.5)
		{
			Far = Index;
			break;
		}
	}
	if (!TestTrue(TEXT("a far threshold exists"), Far != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("the threshold swings"),
			Actor->MoveNode(Far, FVector2D(Length * 0.8, Length * 0.6))))
	{
		return false;
	}

	const FString After = RoadNaming::Describe(*Actor->GetNetwork(), Strip);
	TestNotEqual(TEXT("dragging a threshold re-derives the runway's designator, with no "
					  "stored value to go stale"), After, Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMoveApronCornerRefusesSelfIntersectionTest,
	"Airside.Model.MoveApronCornerRefusesSelfIntersection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMoveApronCornerRefusesSelfIntersectionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A square, counter-clockwise.
	const TArray<FVector2D> Square = {
		FVector2D(0.0, 0.0), FVector2D(10000.0, 0.0),
		FVector2D(10000.0, 10000.0), FVector2D(0.0, 10000.0) };
	const int32 Apron = Actor->AddApron(Square);
	if (!TestTrue(TEXT("the apron was laid"), Apron != INDEX_NONE)) { return false; }

	// A MOVE THAT KEEPS THE OUTLINE SIMPLE IS ACCEPTED - the control, without which the
	// refusal below would pass on a mutator that had simply stopped working.
	TestTrue(TEXT("a corner may be moved while the outline stays simple"),
		Actor->MoveApronCorner(Apron, 2, FVector2D(14000.0, 12000.0)));

	const FApronSurface* Live = Actor->GetNetwork()->GetApron(Actor->GetNetwork()->ApronIdAt(Apron));
	if (!TestNotNull(TEXT("the apron lives"), Live)) { return false; }
	TestTrue(TEXT("and the corner really moved"),
		Live->Outline[2].Equals(FVector2D(14000.0, 12000.0), 1e-6));

	// DRAGGED ACROSS THE POLYGON, making a bow-tie. A self-intersecting outline has no
	// inside, and the surface builder has no answer for one.
	//
	// (-5000, 5000) AND NOT (-5000, -5000), which was the first case tried and is simple: a
	// non-convex quad still has an inside. This one puts the corner past the far EDGE, so
	// the arm reaching it crosses the outline's left side at (0, 3333) - worked through on
	// paper rather than guessed, because a refusal test whose case is legal passes by
	// accident the day the rule is deleted.
	TestFalse(TEXT("but not across its own outline"),
		Actor->MoveApronCorner(Apron, 2, FVector2D(-5000.0, 5000.0)));

	Live = Actor->GetNetwork()->GetApron(Actor->GetNetwork()->ApronIdAt(Apron));
	if (!TestNotNull(TEXT("the apron still lives"), Live)) { return false; }
	TestTrue(TEXT("and the refused move left the corner exactly where it was - a refusal "
				  "inside an edit scope would have left a changed graph with no undo"),
		Live->Outline[2].Equals(FVector2D(14000.0, 12000.0), 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeDragsAnApronCornerTest,
	"Airside.Tool.EditModeDragsAnApronCorner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDragsAnApronCornerTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const TArray<FVector2D> Square = {
		FVector2D(0.0, 0.0), FVector2D(10000.0, 0.0),
		FVector2D(10000.0, 10000.0), FVector2D(0.0, 10000.0) };
	const int32 Apron = Actor->AddApron(Square);
	if (!TestTrue(TEXT("the apron was laid"), Apron != INDEX_NONE)) { return false; }

	// A road too, so a filter that confused the two kinds would be caught: the Apron tool
	// must offer FOUR handles, not four corners plus two nodes.
	const int32 N0 = Actor->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 N1 = Actor->PlaceNode(FVector2D(-20000.0, 9000.0));
	Actor->ConnectNodes(N0, N1);

	FBuildSession Session;
	Session.SelectTool(2);                       // Apron
	Session.SetGestureMode(EGestureMode::Edit);
	FBuildSessionTunables Tunables;

	{
		FEditToolSink Sink;
		Session.GetActiveTool()->BuildPreview(
			Session.MakeContext(Actor, FVector2D(50000.0, 50000.0), Tunables, false, false), Sink);
		TestEqual(TEXT("the Apron tool offers the outline's four corners and none of the "
					   "road's nodes"), Sink.CountMarkers(EPreviewStyle::Handle), 4);
	}

	// AN APRON CORNER IS NOT IN THE ROAD GRAPH, so the snap chain would never mention it -
	// the pick is by distance. Grabbing one and dragging it must still move it.
	IBuildTool* Tool = Session.GetActiveTool();
	Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(10000.0, 10000.0), Tunables, false, false));
	if (!TestFalse(TEXT("a corner really was grabbed"), Tool->IsIdle())) { return false; }

	Tool->OnDrag(Session.MakeContext(Actor, FVector2D(13000.0, 11000.0), Tunables, false, false));
	Tool->OnDragEnd(Session.MakeContext(Actor, FVector2D(13000.0, 11000.0), Tunables, false, false));

	const FApronSurface* Live = Actor->GetNetwork()->GetApron(Actor->GetNetwork()->ApronIdAt(Apron));
	if (!TestNotNull(TEXT("the apron lives"), Live)) { return false; }
	TestTrue(TEXT("the dragged corner moved"),
		!Live->Outline[2].Equals(FVector2D(10000.0, 10000.0), 1.0));

	// AND THE ROAD DID NOT. A handle kind squeezed into a node index is how a wrong index
	// reaches MoveNode and silently moves something else.
	TestTrue(TEXT("and the road's nodes are untouched"),
		Actor->GetNetwork()->GetNodes()[N0].Position.Equals(FVector2D(-20000.0, 0.0), 1e-6)
		&& Actor->GetNetwork()->GetNodes()[N1].Position.Equals(FVector2D(-20000.0, 9000.0), 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeCtrlClickRemovesANodeTest,
	"Airside.Tool.EditModeCtrlClickRemovesANode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeCtrlClickRemovesANodeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// A-B-C in a line. Removing B must HEAL - A and C rejoin - rather than leaving two
	// stubs, which is what "as now" meant when the gesture was approved.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(9000.0, 0.0));
	const int32 C = Actor->PlaceNode(FVector2D(18000.0, 0.0));
	Actor->ConnectNodes(A, B);
	Actor->ConnectNodes(B, C);

	FBuildSession Session;
	Session.SelectTool(1);                       // Taxiway
	Session.SetGestureMode(EGestureMode::Edit);
	FBuildSessionTunables Tunables;

	// A PLAIN CLICK DOES NOTHING - the control. Without it, a removal that fired on every
	// click would pass the assertion below and destroy the mode's whole premise.
	Session.GetActiveTool()->OnClick(
		Session.MakeContext(Actor, FVector2D(9000.0, 0.0), Tunables, /*bRemove*/ false, false));
	TestNotNull(TEXT("a plain click in Edit removes nothing"),
		Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(B)));

	// CTRL+CLICK TAKES IT. The held key still means Remove inside Edit, though Remove is a
	// sticky mode Edit excludes - the two are different gestures, and MakeContext ORs them.
	Session.GetActiveTool()->OnClick(
		Session.MakeContext(Actor, FVector2D(9000.0, 0.0), Tunables, /*bRemove*/ true, false));

	TestNull(TEXT("ctrl+click removes the node under the cursor"),
		Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(B)));

	// AND IT HEALED. A deletion that merely subtracted would leave A and C stranded, which
	// is the behaviour PlanNodeDeletion exists to avoid.
	const FRoadNode* Left = Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(A));
	if (!TestNotNull(TEXT("the near end survives"), Left)) { return false; }
	TestEqual(TEXT("and is rejoined to the far end rather than left a stub"),
		Left->Incident.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeRemovalRespectsTheHandleFilterTest,
	"Airside.Tool.EditModeRemovalRespectsTheHandleFilter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeRemovalRespectsTheHandleFilterTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 S0 = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 S1 = Actor->PlaceNode(FVector2D(9000.0, 0.0));
	Actor->ConnectNodes(S0, S1, ERoadKind::ServiceRoad);

	FBuildSession Session;
	FBuildSessionTunables Tunables;
	Session.SetGestureMode(EGestureMode::Edit);

	// TAXIWAY LIT, SERVICE-ROAD NODE UNDER THE CURSOR. The snap chain will name it happily -
	// it knows nothing of the lit tool - so without the filter the removal would take a node
	// this mode never drew a handle on.
	Session.SelectTool(1);                       // Taxiway
	Session.GetActiveTool()->OnClick(
		Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, /*bRemove*/ true, false));
	TestNotNull(TEXT("a node the lit tool exposes no handle for is not removable"),
		Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(S0)));

	// AND THE CONTROL: with Road lit it IS a handle, and the same click takes it. Without
	// this, a removal broken everywhere would pass the assertion above.
	Session.SelectTool(7);                       // Road
	Session.GetActiveTool()->OnClick(
		Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables, /*bRemove*/ true, false));
	TestNull(TEXT("but the tool that does expose it can remove it"),
		Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(S0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEditModeDrawsTheRemovalItWouldMakeTest,
	"Airside.Tool.EditModeDrawsTheRemovalItWouldMake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEditModeDrawsTheRemovalItWouldMakeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(9000.0, 0.0));
	const int32 C = Actor->PlaceNode(FVector2D(18000.0, 0.0));
	Actor->ConnectNodes(A, B);
	Actor->ConnectNodes(B, C);

	FBuildSession Session;
	Session.SelectTool(1);
	Session.SetGestureMode(EGestureMode::Edit);
	FBuildSessionTunables Tunables;

	// DRAWING IS HALF THE WORK, the lesson WantsFreeStartGuides records. A removal the
	// player cannot see coming is the misclick this whole mode exists to remove, in a new
	// place.
	FEditToolSink Sink;
	Session.GetActiveTool()->BuildPreview(
		Session.MakeContext(Actor, FVector2D(9000.0, 0.0), Tunables, /*bRemove*/ true, false), Sink);

	TestTrue(TEXT("what the ctrl+click would take is drawn as doomed"),
		Sink.CountMarkers(EPreviewStyle::Doomed) > 0);
	TestTrue(TEXT("and so are the roads that go with it"),
		Sink.CountLines(EPreviewStyle::Doomed) > 0);
	TestTrue(TEXT("and the rejoin it would make, because deleting is not purely subtractive"),
		Sink.CountLines(EPreviewStyle::Heal) > 0);

	// THE HANDLES ARE NOT DRAWN WHILE CTRL IS HELD. Offering "you may grab these" beside
	// "these are about to go" is two futures at once.
	TestEqual(TEXT("the grab handles stand down while a removal is being aimed"),
		Sink.CountMarkers(EPreviewStyle::Handle), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNodeVisibilityIsDeclaredForEveryRegistryEntryTest,
	"Airside.Tool.RoadNodeVisibilityIsDeclaredForEveryRegistryEntry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNodeVisibilityIsDeclaredForEveryRegistryEntryTest::RunTest(const FString& Parameters)
{
	// NAMES, NOT COUNTS, like the EditHandles check beside it: a count passes on a table
	// where two entries have swapped answers.
	//
	// THE FALSE ROWS ARE THE INTERESTING ONES. Holding-point and guideline LOOK like they
	// need road nodes - both talk about clicking nodes - but they pick GUIDELINE nodes,
	// which GuidelineOverlay draws under its own G toggle. The runway tool reads no snap at
	// all. Getting any of those wrong puts the scaffolding back on screen for a tool that
	// never wanted it.
	const TMap<FName, bool> Expected = {
		{ TEXT("Select"),          false },
		{ TEXT("Taxiway"),         true  },   // snaps to nodes to chain and close junctions
		{ TEXT("Apron"),           false },
		{ TEXT("Stand"),           false },
		{ TEXT("Guideline"),       false },   // guideline nodes, not road nodes
		{ TEXT("Runway"),          false },   // reads no snap
		{ TEXT("HoldingPosition"), false },   // guideline nodes, not road nodes
		{ TEXT("Road"),            true  },   // the same FRoadDrawTool as Taxiway
		{ TEXT("FuelDepot"),       false },
	};

	for (const FToolRegistration& Entry : ToolRegistry())
	{
		const bool* Want = Expected.Find(Entry.Id);
		if (Want == nullptr)
		{
			AddError(FString::Printf(
				TEXT("registry entry '%s' is not named in this test - a new tool must say "
					 "whether the node rings belong on screen under it"), *Entry.Id.ToString()));
			continue;
		}
		TestEqual(*FString::Printf(TEXT("'%s' declares the node visibility this test expects"),
			*Entry.Id.ToString()), Entry.bShowsRoadNodes, *Want);
	}
	TestEqual(TEXT("and the table holds no entry beyond the ones named here"),
		ToolRegistry().Num(), Expected.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNodesStandDownOutsideTheRoadToolsTest,
	"Airside.Tool.RoadNodesStandDownOutsideTheRoadTools",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNodesStandDownOutsideTheRoadToolsTest::RunTest(const FString& Parameters)
{
	// THE REPORTED DEFECT: "when a road is not on edit or create mode we should not see the
	// white circle junction nodes". The rings are scaffolding, and an airport being looked
	// at rather than built should read as an airport.
	FBuildSession Session;

	Session.SelectTool(0);                       // Select
	TestFalse(TEXT("looking at the airport draws no node rings"),
		Session.WantsRoadNodesDrawn());

	Session.SelectTool(1);                       // Taxiway
	TestTrue(TEXT("but drawing a taxiway does - the ring is the junction you are aiming at"),
		Session.WantsRoadNodesDrawn());

	Session.SelectTool(3);                       // Stand
	TestFalse(TEXT("placing a stand does not"), Session.WantsRoadNodesDrawn());

	Session.SelectTool(7);                       // Road
	TestTrue(TEXT("drawing a service road does"), Session.WantsRoadNodesDrawn());

	// EDIT WANTS THEM WHATEVER IS LIT, because the mode is ABOUT the points and the tool
	// only says which of them are grabbable.
	Session.SelectTool(0);                       // Select - which on its own draws none
	Session.SetGestureMode(EGestureMode::Edit);
	TestTrue(TEXT("Edit draws them whatever tool is lit"), Session.WantsRoadNodesDrawn());

	Session.SetGestureMode(EGestureMode::Build);
	TestFalse(TEXT("and leaving Edit puts them away again"), Session.WantsRoadNodesDrawn());
	return true;
}

namespace
{
	/** A straight runway with one taxiway exit partway along it, which is the shape the
	 *  bend was reported on. Returns the exit node's slot index, or INDEX_NONE. */
	int32 LayRunwayWithAnExit(ARoadNetworkActor* Actor, double Length, double ExitAt)
	{
		// A node first, purely to bring the network into being - see LayRunway in the
		// fixtures for the crash that taught this.
		Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
		Actor->MinimumRunwayLength = 10000.0;

		URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Profile->bContinuousThroughJunctions = true;
		if (!Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(Length, 0.0), Profile))
		{
			return INDEX_NONE;
		}

		// Split the strip where the exit joins, then run a taxiway off it - the same two
		// steps a click with the taxiway tool performs.
		const URoadNetwork* Network = Actor->GetNetwork();
		int32 Strip = INDEX_NONE;
		for (int32 Index = 0; Index < Network->GetSegments().Num(); ++Index)
		{
			if (Network->GetSegments()[Index].bAlive
				&& Network->IsRunwaySegment(Network->SegmentIdAt(Index)))
			{
				Strip = Index;
				break;
			}
		}
		if (Strip == INDEX_NONE)
		{
			return INDEX_NONE;
		}

		const int32 Exit = Actor->SplitSegment(Strip, FVector2D(ExitAt, 0.0));
		if (Exit == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		const int32 Away = Actor->PlaceNode(FVector2D(ExitAt, 9000.0));
		Actor->ConnectNodes(Exit, Away, ERoadKind::Taxiway);
		return Exit;
	}

	/** The largest perpendicular departure of any runway node from the line through the
	 *  strip's two ends. Zero on a straight runway. */
	double RunwayBend(const URoadNetwork& Network)
	{
		TArray<FVector2D> OnStrip;
		for (int32 Index = 0; Index < Network.GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Network.GetSegments()[Index].bAlive || !Network.IsRunwaySegment(Id))
			{
				continue;
			}
			const FRoadSegment& Segment = Network.GetSegments()[Index];
			for (const FRoadNodeId End : { Segment.A, Segment.B })
			{
				if (const FRoadNode* Node = Network.GetNode(End))
				{
					OnStrip.AddUnique(Node->Position);
				}
			}
		}
		if (OnStrip.Num() < 3)
		{
			return 0.0;
		}

		// The two ends are the extremes along the strip's own direction.
		FVector2D Lo = OnStrip[0];
		FVector2D Hi = OnStrip[0];
		for (const FVector2D& P : OnStrip)
		{
			if (P.X < Lo.X || (P.X == Lo.X && P.Y < Lo.Y)) { Lo = P; }
			if (P.X > Hi.X || (P.X == Hi.X && P.Y > Hi.Y)) { Hi = P; }
		}
		const FVector2D Line = Hi - Lo;
		if (Line.IsNearlyZero())
		{
			return 0.0;
		}
		const FVector2D Normal = FVector2D(-Line.Y, Line.X).GetSafeNormal();

		double Worst = 0.0;
		for (const FVector2D& P : OnStrip)
		{
			Worst = FMath::Max(Worst, FMath::Abs(FVector2D::DotProduct(P - Lo, Normal)));
		}
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayExitSlidesAlongButNeverOffTest,
	"Airside.Model.RunwayExitSlidesAlongButNeverOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayExitSlidesAlongButNeverOffTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 Exit = LayRunwayWithAnExit(Actor, 40000.0, 20000.0);
	if (!TestTrue(TEXT("a runway with an exit was laid"), Exit != INDEX_NONE)) { return false; }

	TestTrue(TEXT("the runway starts straight"),
		RunwayBend(*Actor->GetNetwork()) < 1.0);

	// DRAGGED SIDEWAYS AND ALONG AT ONCE. The along part must take, the sideways part must
	// not - which is what makes this a slide rather than a refusal or a free move.
	TestTrue(TEXT("the exit moves"), Actor->MoveNode(Exit, FVector2D(26000.0, 7000.0)));

	const FRoadNode* Moved = Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(Exit));
	if (!TestNotNull(TEXT("the exit survives"), Moved)) { return false; }

	TestTrue(TEXT("it slid ALONG the strip, taking the component the runway permits"),
		FMath::Abs(Moved->Position.X - 26000.0) < 1.0);
	TestTrue(TEXT("and not across it, so the strip is still straight"),
		FMath::Abs(Moved->Position.Y) < 1.0);

	// MEASURED, not inferred from the node's own coordinates: the bend is a property of the
	// whole chain, and this is the thing the markings assume. A runway that passed the two
	// assertions above and still bent somewhere else would be the same defect in a new place.
	TestTrue(TEXT("the chain has no perpendicular departure at all - which is what "
				  "FRunwayMarkingBuilder's single straight frame assumes"),
		RunwayBend(*Actor->GetNetwork()) < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayThresholdWithExitsWillNotMoveTest,
	"Airside.Model.RunwayThresholdWithExitsWillNotMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayThresholdWithExitsWillNotMoveTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	const int32 Exit = LayRunwayWithAnExit(Actor, 40000.0, 20000.0);
	if (!TestTrue(TEXT("a runway with an exit was laid"), Exit != INDEX_NONE)) { return false; }

	// The far threshold: the end node, which has exactly one runway arm.
	const URoadNetwork* Network = Actor->GetNetwork();
	int32 Far = INDEX_NONE;
	for (int32 Index = 0; Index < Network->GetNodes().Num(); ++Index)
	{
		const FRoadNode& Node = Network->GetNodes()[Index];
		if (Node.bAlive && Node.Position.Equals(FVector2D(40000.0, 0.0), 1.0))
		{
			Far = Index;
			break;
		}
	}
	if (!TestTrue(TEXT("the far threshold exists"), Far != INDEX_NONE)) { return false; }

	// SWINGING IT WOULD LEAVE THE EXIT BEHIND and bend the strip, so the sideways component
	// is dropped and the along-strip one taken - the same slide an interior exit gets, for
	// the same reason.
	TestTrue(TEXT("a threshold with exits behind it still moves"),
		Actor->MoveNode(Far, FVector2D(38000.0, 12000.0)));

	const FRoadNode* Moved = Actor->GetNetwork()->GetNode(Actor->GetNetwork()->NodeIdAt(Far));
	if (!TestNotNull(TEXT("the threshold survives"), Moved)) { return false; }
	TestTrue(TEXT("it shortened along its own line"),
		FMath::Abs(Moved->Position.X - 38000.0) < 1.0);
	TestTrue(TEXT("and did not swing off it, so the exit is not left behind"),
		FMath::Abs(Moved->Position.Y) < 1.0);
	TestTrue(TEXT("the runway is still straight"), RunwayBend(*Actor->GetNetwork()) < 1.0);

	// EXTENDING ALONG THE LINE IS UNTOUCHED - the control. Without it this test would pass
	// on a threshold that had been pinned in place altogether.
	TestTrue(TEXT("and it still extends along its own line"),
		Actor->MoveNode(Far, FVector2D(46000.0, 0.0)));
	TestTrue(TEXT("still straight after that"), RunwayBend(*Actor->GetNetwork()) < 1.0);
	TestTrue(TEXT("and the extension really took"),
		FMath::Abs(Actor->GetNetwork()->GetNodes()[Far].Position.X - 46000.0) < 1.0);
	return true;
}

#endif
