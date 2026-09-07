#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/SelectTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FSelToolSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		virtual void Marker(const FVector2D&, EPreviewStyle Style) override { Markers.FindOrAdd(Style)++; }
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}
		int32 Count(EPreviewStyle S) const { const int32* N = Markers.Find(S); return N ? *N : 0; }
	};

	/** An actor with a network, one authored edge, one stand at (50000, 0) and one van
	 *  dispatched along the edge. Prefixed SelTool against the unity build. */
	struct FSelToolFixture
	{
		ARoadNetworkActor* Actor = nullptr;
		int32 StandIndex = INDEX_NONE;
		int32 AgentId = 0;
		FVector2D StandAt = FVector2D(50000.0, 0.0);
	};

	FSelToolFixture SelToolBuild(UWorld* World)
	{
		FSelToolFixture F;
		F.Actor = World->SpawnActor<ARoadNetworkActor>();
		if (F.Actor == nullptr) { return F; }
		F.Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
		URoadNetwork& Net = *F.Actor->Network;

		const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
		const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));

		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const FEntityInstanceId Placed = Net.PlaceEntity(Stand, Stand->Anchors, F.StandAt, 0.0);
		F.StandIndex = Placed.Index;

		FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::GroundVehicle;
		FAirframe Van = UAirsideSettings::ResolveDefaultAirframe();
		Van.Climb = FClimbPerformance();
		F.Actor->DispatchAgent(RouteSearch::Find(Net, Q), Van, ETraversalClass::GroundVehicle);
		F.AgentId = F.Actor->GetTraffic()->GetNewestAgentId();
		return F;
	}

	FToolContext SelToolContext(ARoadNetworkActor* Actor, FSelection& Selection, FVector2D Cursor, int32 HoverAgent)
	{
		FToolContext C;
		C.Target = Actor;
		C.SnapRadius = 400.0;
		FRoadSnapResult NoSnap; NoSnap.Position = Cursor;
		C.SetCursor(Cursor, NoSnap);
		C.HoverAgent = HoverAgent;
		C.Selection = &Selection;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSelectToolRegistryTest,
	"Airside.Tool.SelectTool.IsDefaultState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSelectToolRegistryTest::RunTest(const FString& Parameters)
{
	// CITIES-STYLE DEFAULT STATE (spec §2): the session opens in Select, and cancelling an
	// idle build tool comes back to it. Asserted on the registry and the session, not on
	// a controller, because both drivers read exactly these.
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	if (!TestTrue(TEXT("the registry has entries"), Registry.Num() > 0)) { return false; }
	TestTrue(TEXT("registry index 0 is Select"), Registry[0].Name.ToString() == TEXT("Select"));
	TestTrue(TEXT("Select took the route tool's key"), Registry[0].Key == EKeys::Four);

	FBuildSession Session;
	TestEqual(TEXT("a fresh session starts in Select"), Session.GetActiveToolIndex(), 0);

	// Cancel from an IDLE build tool returns to Select; cancel in Select stays.
	Session.SelectTool(1);
	TestEqual(TEXT("tool 1 is active"), Session.GetActiveToolIndex(), 1);
	FToolContext Empty;
	Session.CancelActiveGesture(Empty);
	TestEqual(TEXT("cancelling an idle build tool returns to Select"), Session.GetActiveToolIndex(), 0);
	Session.CancelActiveGesture(Empty);
	TestEqual(TEXT("cancelling in Select stays in Select"), Session.GetActiveToolIndex(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSelectToolPickTest,
	"Airside.Tool.SelectTool.Picks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSelectToolPickTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	FSelToolFixture F = SelToolBuild(World);
	if (!TestNotNull(TEXT("fixture actor"), F.Actor)) { return false; }
	if (!TestTrue(TEXT("a stand was placed"), F.StandIndex != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("an agent was dispatched"), F.AgentId > 0)) { return false; }

	FSelectTool Tool;
	FSelection Sel;

	// 1. A hovered aircraft wins, wherever the plane cursor is - even over the stand.
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, F.AgentId));
	TestTrue(TEXT("hover agent selects the aircraft"), Sel.Kind == ESelectionKind::Aircraft && Sel.Id == F.AgentId);

	// 2. No hover, cursor on the stand: the stand.
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	TestTrue(TEXT("a click on a stand selects it"), Sel.Kind == ESelectionKind::Stand && Sel.Id == F.StandIndex);

	// 3. Preview names the selection with the Selected style.
	{
		FSelToolSink Sink;
		Tool.BuildPreview(SelToolContext(F.Actor, Sel, FVector2D(90000.0, 90000.0), 0), Sink);
		TestEqual(TEXT("the selected stand gets one Selected marker"), Sink.Count(EPreviewStyle::Selected), 1);
		TestEqual(TEXT("nothing hovered, so no Hover marker"), Sink.Count(EPreviewStyle::Hover), 0);
	}
	{
		FSelToolSink Sink;
		Tool.BuildPreview(SelToolContext(F.Actor, Sel, FVector2D(90000.0, 90000.0), F.AgentId), Sink);
		TestEqual(TEXT("a hovered aircraft gets one Hover marker"), Sink.Count(EPreviewStyle::Hover), 1);
	}

	// 4. Empty click clears; cancel clears.
	Tool.OnClick(SelToolContext(F.Actor, Sel, FVector2D(90000.0, 90000.0), 0));
	TestFalse(TEXT("a click on nothing clears the selection"), Sel.IsSet());
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	Tool.OnCancel(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	TestFalse(TEXT("cancel clears the selection"), Sel.IsSet());

	// 5. Tick drops a selection whose agent is gone.
	Tool.OnClick(SelToolContext(F.Actor, Sel, F.StandAt, F.AgentId));
	F.Actor->GetTraffic()->RetireAgent(F.AgentId);
	Tool.Tick(SelToolContext(F.Actor, Sel, F.StandAt, 0));
	TestFalse(TEXT("a retired agent is no longer selected"), Sel.IsSet());

	// 6. Switching to a build tool clears the SESSION's selection.
	{
		FBuildSession Session;
		FBuildSessionTunables T;
		FToolContext C = Session.MakeContext(F.Actor, F.StandAt, T, false, false, 0);
		Session.GetActiveTool()->OnClick(C);
		TestTrue(TEXT("the session's selection is the stand"), Session.GetSelection().Kind == ESelectionKind::Stand);
		Session.SelectTool(1, C);
		TestFalse(TEXT("activating a build tool clears the selection"), Session.GetSelection().IsSet());
	}
	return true;
}

#endif
