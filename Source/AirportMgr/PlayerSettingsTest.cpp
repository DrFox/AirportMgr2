#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "BuildActions.h"
#include "PlayerSettings.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * GRID SNAP ON START TURNS A GRID ON, never a level's own grid off (plan 4b ruling): on, an airport
 * with no grid starts on the first step the bar's Grid button gives; off, whatever the level set
 * stays - the toggle is the player's preference for a fresh start, not an override of a design.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerSettingsStartGridTest, "AirportMgr.Settings.StartGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerSettingsStartGridTest::RunTest(const FString& Parameters)
{
	FSnapGuideSettings FirstStep;
	FirstStep.CycleGridStep();

	FSnapGuideSettings Guides;
	PlayerSettings::ApplyStartGrid(Guides, true);
	TestEqual(TEXT("on: no grid becomes the Grid button's first step"), Guides.GridStep, FirstStep.GridStep);

	FSnapGuideSettings Plain;
	PlayerSettings::ApplyStartGrid(Plain, false);
	TestEqual(TEXT("off: no grid stays no grid"), Plain.GridStep, EGridStep::Off);

	FSnapGuideSettings Designed;
	Designed.GridStep = EGridStep::TenMetres;
	PlayerSettings::ApplyStartGrid(Designed, false);
	TestEqual(TEXT("off: a level's own 10 m grid is left alone"), Designed.GridStep, EGridStep::TenMetres);
	PlayerSettings::ApplyStartGrid(Designed, true);
	TestEqual(TEXT("on: and so is it here - it is already on"), Designed.GridStep, EGridStep::TenMetres);
	return true;
}

/**
 * THE DRIVE SIDE IS ONE VALUE (spec section 3): Settings' "Traffic drives on" and the bar's "Drive
 * left" read and write the same thing - the road actor's, with its undo step - so neither can show
 * a side the airport is not driving on. Through the game sink, the way Settings reaches it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerSettingsDriveSideTest, "AirportMgr.Settings.DriveSideIsOneValue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerSettingsDriveSideTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("a road actor"), TestWorld.Actor)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	// LEFT, not the default: FPlayerSettings defaults to Right, so a sink that read nothing would
	// pass a Right start.
	// A NODE FIRST: the actor makes its network on the first edit, and a side is the network's.
	TestWorld.Actor->PlaceNode(FVector2D::ZeroVector);
	TestTrue(TEXT("the airport now drives on the left"), TestWorld.Actor->SetDriveSide(EDriveSide::Left));

	FGamePlayerSettingsSink Sink(*C);
	const FBuildAction* Bar = FindAction(FName(TEXT("game.driveside")));
	if (!TestNotNull(TEXT("the bar's drive-side action"), Bar)) { return false; }
	FBuildActionContext Ctx(*C);

	FPlayerSettings V = Sink.Read();
	TestEqual(TEXT("Settings reads the airport's side"), V.DriveSide, EDriveSide::Left);
	TestTrue(TEXT("which the bar's button lights for"), Bar->IsActive(Ctx));
	V.DriveSide = EDriveSide::Right;
	Sink.Apply(V);
	TestEqual(TEXT("Settings sets the airport's side"), TestWorld.Actor->GetDriveSide(), EDriveSide::Right);
	TestFalse(TEXT("and the bar's button goes out"), Bar->IsActive(Ctx));

	Bar->Execute(Ctx);
	TestEqual(TEXT("the bar's button flips it, and Settings reads that"), Sink.Read().DriveSide, EDriveSide::Left);
	return true;
}

#endif
