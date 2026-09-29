#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
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

#endif
