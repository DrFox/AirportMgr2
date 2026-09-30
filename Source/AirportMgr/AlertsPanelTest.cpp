#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "UI/UiWindowHost.h"
#include "BuildBarWidget.h"
#include "Model/OpsEvents.h"
#include "Blueprint/UserWidget.h"
#include "BuildHudLayer.h"
#include "BuildActions.h"
#include "AlertsPanelWidget.h"
#include "Testing/AirsideTestWorld.h"
#include "Testing/AirsideTestGraph.h"
#include "Content/AirsideSettings.h"
#include "Misc/ScopeExit.h"
#include "Model/Flight.h"
#include "OpsRuntimeResolver.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Present/OpsRuntime.h"
#include "Model/RoadAgent.h"
#include "Present/AirsideTraffic.h"
#include "Tool/Selection.h"

#if WITH_DEV_AUTOMATION_TESTS

// OPS ALERTS STAGE 2 (spec 2026-09-29-ops-alerts §3): an alert's "Go" moves the camera to its subject and
// selects it, so the inspector opens on it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsGoPointTest, "AirportMgr.UI.Alerts.GoToAPoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsGoPointTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Point;
	Focus.Point = FVector2D(12000.0, -3400.0);
	TestTrue(TEXT("a point is always somewhere to go"), C->SelectAndFocus(Focus));
	TestEqual(TEXT("the camera's focus is now that point"), C->GetViewFocus(), Focus.Point);

	FAlertFocus Nowhere;
	TestFalse(TEXT("an alert with no place in the world moves nothing"), C->SelectAndFocus(Nowhere));
	TestEqual(TEXT("and the camera stays"), C->GetViewFocus(), Focus.Point);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsGoEntityTest, "AirportMgr.UI.Alerts.GoToAStandSelectsIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsGoEntityTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 40000.0));
	URoadNetwork& Net = *TestWorld.Actor->Network;
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(5000.0, 0.0), 0.0, 3600.0,
		StandDef->PoseRole, StandDef->Trucks);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);

	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Entity;
	Focus.Id = Stand.Index;
	Focus.Point = FVector2D(5000.0, 0.0);
	TestTrue(TEXT("a stand that exists is somewhere to go"), C->SelectAndFocus(Focus));
	TestEqual(TEXT("the camera goes there"), C->GetViewFocus(), Focus.Point);
	TestEqual(TEXT("and the stand is selected, so the inspector opens on it"), C->GetSelection().Kind, ESelectionKind::Stand);
	TestEqual(TEXT("that stand"), C->GetSelection().Id, Stand.Index);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsGoGoneTest, "AirportMgr.UI.Alerts.GoToSomethingGoneMovesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsGoGoneTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 40000.0));
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	const FVector2D Before = C->GetViewFocus();

	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Agent;
	Focus.Id = 424242;   // retired since the alert was raised
	Focus.Point = FVector2D(9000.0, 9000.0);
	TestFalse(TEXT("an aircraft that has gone is nowhere to go"), C->SelectAndFocus(Focus));
	TestEqual(TEXT("so the camera does not move to where it was"), C->GetViewFocus(), Before);
	TestFalse(TEXT("and nothing is selected"), C->GetSelection().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsGoAgentTest, "AirportMgr.UI.Alerts.GoToAnAgentSelectsIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsGoAgentTest::RunTest(const FString&)
{
	// THE AGENT CASE (ops batch 3 PR E - the third Go branch had no test): the camera goes to where the aircraft is
	// NOW, not where the alert saw it, and the aircraft is selected so its card opens.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = TestGraph::Node(Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(Net, 200000.0, 0.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(Net, A, B, Options);
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
		UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	for (int32 Frame = 0; Frame < 60; ++Frame) { Actor->Tick(1.0f / 30.0f); }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);

	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Agent;
	Focus.Id = Id;
	Focus.Point = FVector2D(-9000.0, -9000.0);   // where it was when the alert was raised - stale by now
	TestTrue(TEXT("an aircraft that exists is somewhere to go"), C->SelectAndFocus(Focus));
	const FRoadAgent* Agent = Actor->GetGroundTraffic()->FindAgent(Id);
	if (!TestNotNull(TEXT("the aircraft"), Agent)) { return false; }
	TestEqual(TEXT("the camera goes to where it is now"), C->GetViewFocus(), Agent->GroundPosition());
	TestEqual(TEXT("and the aircraft is selected, so the inspector opens on it"), C->GetSelection().Kind, ESelectionKind::Aircraft);
	TestEqual(TEXT("that aircraft"), C->GetSelection().Id, Id);
	return true;
}

namespace
{
	FOpsAlert AlertsPanelTestAlert(EAlertKind Kind, int32 Id, EAlertFocusKind Focus = EAlertFocusKind::None, FVector2D Point = FVector2D::ZeroVector)
	{
		FOpsAlert Alert;
		Alert.Key.Kind = Kind;
		Alert.Key.Id = Id;
		Alert.Text = FText::FromString(FString::Printf(TEXT("alert %d"), Id));
		Alert.Focus.Kind = Focus;
		Alert.Focus.Point = Point;
		return Alert;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsWindowFollowsTest, "AirportMgr.UI.Alerts.WindowFollowsRaiseAndClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsWindowFollowsTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	UAlertsPanelWidget* Panel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an alerts window"), Panel)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Panel->BindTo(*Events);

	const FOpsAlert A = AlertsPanelTestAlert(EAlertKind::FlightStranded, 1);
	Events->OnAlertRaised.Broadcast(A);
	Events->OnAlertRaised.Broadcast(AlertsPanelTestAlert(EAlertKind::JobUnserviceable, 2));
	Events->OnAlertRaised.Broadcast(A);   // a re-raise after a load - the same key
	TestEqual(TEXT("one row per standing problem - a re-raise updates, it does not stack"), Panel->AlertCount(), 2);
	Events->OnAlertCleared.Broadcast(A.Key);
	TestEqual(TEXT("a cleared problem leaves the list"), Panel->AlertCount(), 1);
	Events->OnAlertsReset.Broadcast();
	TestEqual(TEXT("a load or re-attach empties it - the re-raises that follow refill it"), Panel->AlertCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsBadgeTest, "AirportMgr.UI.Alerts.BarBadgeCounts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsBadgeTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHud())) { return false; }
	UAlertsPanelWidget* Panel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	C->GetHud()->AlertsPanel = Panel;
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Panel->BindTo(*Events);

	const FBuildAction* Action = BuildActions().FindByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("game.alerts")); });
	if (!TestNotNull(TEXT("the bar has an Alerts button"), Action) || !TestTrue(TEXT("with a live label"), static_cast<bool>(Action->DynamicLabel)))
	{
		return false;
	}
	FBuildActionContext Ctx(*C);
	TestFalse(TEXT("unlit with nothing wrong"), Action->IsActive(Ctx));
	Events->OnAlertRaised.Broadcast(AlertsPanelTestAlert(EAlertKind::FlightStranded, 1));
	Events->OnAlertRaised.Broadcast(AlertsPanelTestAlert(EAlertKind::Overdrawn, 0));
	TestTrue(TEXT("the label counts the standing problems"), Action->DynamicLabel(Ctx).ToString().Contains(TEXT("2")));
	TestTrue(TEXT("and the button is lit while any exist"), Action->IsActive(Ctx));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsRowGoTest, "AirportMgr.UI.Alerts.RowGoMovesTheCamera",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsRowGoTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	UAlertsPanelWidget* Panel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Panel->BindTo(*Events);
	Events->OnAlertRaised.Broadcast(AlertsPanelTestAlert(EAlertKind::HeldStandLost, 7, EAlertFocusKind::Point, FVector2D(-2500.0, 800.0)));
	TestTrue(TEXT("Go on a row with a place goes there"), Panel->Go(0, *C));
	TestEqual(TEXT("the camera's focus is the alert's"), C->GetViewFocus(), FVector2D(-2500.0, 800.0));
	TestFalse(TEXT("a row that is not there does nothing"), Panel->Go(5, *C));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsCancelFlightTest, "AirportMgr.UI.Alerts.CancelFlightCancelsOnlyAFlightCannotLandRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsCancelFlightTest::RunTest(const FString&)
{
	// THE CANCEL BESIDE A "FLIGHT CANNOT LAND" ROW (#442), through the panel's own method and a real attached runtime: the flight is
	// cancelled and its stand freed; any other kind of alert offers no cancel; a row that has cleared since is refused.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 90000.0));
	URoadNetwork* Net = TestWorld.Actor->Network;
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FTestAirport Field = FTestAirport::Build(Airframe, FTestAirportOptions(), Net);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	UGroundTraffic* Model = TestWorld.Actor->GetTraffic()->GetModel();
	if (!TestNotNull(TEXT("a traffic model"), Model)) { return false; }
	UFlightBoard* Board = Runtime->GetFlightBoard();
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Id = Board->TakeNextId();
	Flight->Airframe = Airframe;
	Flight->Callsign = TEXT("CU 204");
	Flight->OfferWindowSeconds = 60.0;
	Flight->OfferSecondsLeft = 60.0;
	Flight->LeadTimeSeconds = 1000.0;
	Flight->RunwayPreference = Field.Threshold;
	Board->AddOffer(*Runtime->GetClock(), Flight);
	if (!TestTrue(TEXT("a flight accepted, holding a stand"), Board->Accept(*Model, *Net, *Runtime->GetClock(), *Flight))) { return false; }
	const FEntityInstance* Stand = Net->GetEntity(Flight->Stand);
	if (!TestNotNull(TEXT("its stand"), Stand)) { return false; }
	const FGuidelineNodeId StandNode = Stand->PoseNode;

	UAlertsPanelWidget* Panel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an alerts window"), Panel)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Panel->BindTo(*Events);
	const FOpsAlert Cannot = AlertsPanelTestAlert(EAlertKind::FlightCannotLand, Flight->Id);
	const FOpsAlert Lost = AlertsPanelTestAlert(EAlertKind::HeldStandLost, Flight->Id);
	Events->OnAlertRaised.Broadcast(Cannot);
	Events->OnAlertRaised.Broadcast(Lost);

	TestFalse(TEXT("a held-stand alert for the same flight offers no cancel"), Panel->CancelFlightOf(Lost.Key, *Runtime));
	TestEqual(TEXT("and cancelled nothing"), Flight->GetPhase(), EFlightPhase::Accepted);
	TestTrue(TEXT("CONTROL: the stand is still held"), Model->IsStandHeld(StandNode, 0));

	TestTrue(TEXT("the flight-cannot-land row's Cancel is taken"), Panel->CancelFlightOf(Cannot.Key, *Runtime));
	TestEqual(TEXT("the flight is cancelled"), Flight->GetPhase(), EFlightPhase::Cancelled);
	TestFalse(TEXT("and its stand released"), Model->IsStandHeld(StandNode, 0));
	TestFalse(TEXT("a second click on the same row finds nothing still to cancel"), Panel->CancelFlightOf(Cannot.Key, *Runtime));

	Events->OnAlertCleared.Broadcast(Cannot.Key);
	TestFalse(TEXT("and once the alert has cleared the row is gone"), Panel->CancelFlightOf(Cannot.Key, *Runtime));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsCancelClickTest, "AirportMgr.UI.Alerts.CancelButtonCancelsItsFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsCancelClickTest::RunTest(const FString&)
{
	// THE WIRING, AT THE BUTTON (#442 review): CancelFlightOf is tested above by calling it, which passes a row whose button was built
	// and never bound. This paints a FlightCannotLand row, checks its Cancel flight is bound to its own entry, and CLICKS it - the real
	// OnClicked - and the flight is cancelled through the panel's own runtime (the resolver's, stood in for the headless world).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 90000.0));
	URoadNetwork* Net = TestWorld.Actor->Network;
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FTestAirport Field = FTestAirport::Build(Airframe, FTestAirportOptions(), Net);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	UGroundTraffic* Model = TestWorld.Actor->GetTraffic()->GetModel();
	if (!TestNotNull(TEXT("a traffic model"), Model)) { return false; }
	UFlightBoard* Board = Runtime->GetFlightBoard();
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Id = Board->TakeNextId();
	Flight->Airframe = Airframe;
	Flight->Callsign = TEXT("CU 204");
	Flight->OfferWindowSeconds = 60.0;
	Flight->OfferSecondsLeft = 60.0;
	Flight->LeadTimeSeconds = 1000.0;
	Flight->RunwayPreference = Field.Threshold;
	Board->AddOffer(*Runtime->GetClock(), Flight);
	if (!TestTrue(TEXT("a flight accepted, holding a stand"), Board->Accept(*Model, *Net, *Runtime->GetClock(), *Flight))) { return false; }
	const FEntityInstance* Stand = Net->GetEntity(Flight->Stand);
	if (!TestNotNull(TEXT("its stand"), Stand)) { return false; }
	const FGuidelineNodeId StandNode = Stand->PoseNode;

	UAlertsPanelWidget* Panel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an alerts window"), Panel)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Panel->BindTo(*Events);
	const FOpsAlert Cannot = AlertsPanelTestAlert(EAlertKind::FlightCannotLand, Flight->Id, EAlertFocusKind::Point, Field.Threshold);
	const FOpsAlert Lost = AlertsPanelTestAlert(EAlertKind::HeldStandLost, Flight->Id, EAlertFocusKind::Point, Field.Threshold);
	Events->OnAlertRaised.Broadcast(Cannot);
	Events->OnAlertRaised.Broadcast(Lost);
	Panel->PaintRowsForTest();

	TestTrue(TEXT("the flight-cannot-land row's Cancel flight is bound to its own entry"), Panel->IsCancelBoundForTest(Cannot.Key));
	TestFalse(TEXT("a held-stand row has no such button"), Panel->IsCancelBoundForTest(Lost.Key));
	TestFalse(TEXT("and there is none to click on it"), Panel->ClickCancelForTest(Lost.Key));

	// NO RUNTIME FOR THE WORLD: the click is refused, logged, and cancels nothing (a world with no game instance and no override).
	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, nullptr);
	TestTrue(TEXT("the click reaches the button"), Panel->ClickCancelForTest(Cannot.Key));
	TestEqual(TEXT("with no runtime to ask, nothing is cancelled"), Flight->GetPhase(), EFlightPhase::Accepted);

	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);
	ON_SCOPE_EXIT { OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, nullptr); };
	TestTrue(TEXT("the click reaches the button"), Panel->ClickCancelForTest(Cannot.Key));
	TestEqual(TEXT("and the flight is cancelled"), Flight->GetPhase(), EFlightPhase::Cancelled);
	TestFalse(TEXT("its stand released"), Model->IsStandHeld(StandNode, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsGoLeavesBuildToolTest, "AirportMgr.UI.Alerts.GoLeavesABuildTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsGoLeavesBuildToolTest::RunTest(const FString&)
{
	// A BUILD TOOL IS MODAL OVER THE AIRPORT (FBuildSession's own rule): selecting a stand while the road
	// tool stays live would put the next world click's road under an open stand card (stage 2 review).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 40000.0));
	URoadNetwork& Net = *TestWorld.Actor->Network;
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(5000.0, 0.0), 0.0, 3600.0,
		StandDef->PoseRole, StandDef->Trucks);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	C->SelectTool(1);
	if (!TestEqual(TEXT("a build tool is live"), C->GetSession().GetActiveToolIndex(), 1)) { return false; }

	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Entity;
	Focus.Id = Stand.Index;
	Focus.Point = FVector2D(5000.0, 0.0);
	TestTrue(TEXT("Go"), C->SelectAndFocus(Focus));
	TestEqual(TEXT("Go puts the select tool back first"), C->GetSession().GetActiveToolIndex(), 0);
	TestEqual(TEXT("and then selects the stand"), C->GetSelection().Kind, ESelectionKind::Stand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlertsIsAWindowTest, "AirportMgr.UI.Alerts.HudHostsItAsAWindow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAlertsIsAWindowTest::RunTest(const FString&)
{
	// CreateAll AND WireWindows ARE TWO LISTS that must agree (BuildHudLayer.cpp): a panel created but not
	// wired is never shown. Driven through the HUD's own WireWindows, not a hand-added window.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHud())) { return false; }
	UBuildHudLayer* Hud = C->GetHud();
	Hud->BuildBar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	Hud->AlertsPanel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	Hud->WireWindows();
	TestNotNull(TEXT("the alerts panel is a window the host can show"), Hud->WindowHost->WindowForTest(TEXT("alerts")));
	return true;
}

#endif
