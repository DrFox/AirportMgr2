#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Model/OpsEvents.h"
#include "Blueprint/UserWidget.h"
#include "BuildHudLayer.h"
#include "BuildActions.h"
#include "AlertsPanelWidget.h"
#include "Testing/AirsideTestWorld.h"
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
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHudForTest())) { return false; }
	UAlertsPanelWidget* Panel = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	C->GetHudForTest()->AlertsPanel = Panel;
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

#endif
