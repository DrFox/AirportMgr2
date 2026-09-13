#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildHUD.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildHUDLooksTest,
	"AirportMgr.HUD.LooksCoverEveryStyle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildHUDLooksTest::RunTest(const FString& Parameters)
{
	// THE ONE-LIST CHECK for Looks (#104). A style Marker/Line/CrossMark/Label draws with no
	// matching entry used to fall through StyleColour's checkNoEntry() crash; now it is
	// LookFor's, and this is what catches a style left out of the constructor's seeding list
	// before a player does, at the first frame that draws it.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildHUD* Hud = World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	// EPreviewStyle is a plain 0-based enum ending at ServiceAnchor - iterated the same way
	// FBuildActionsRegistryTest walks EActionSection, rather than by reflection.
	for (uint8 S = 0; S <= static_cast<uint8>(EPreviewStyle::ServiceAnchor); ++S)
	{
		const EPreviewStyle Style = static_cast<EPreviewStyle>(S);
		const FPreviewLook& Look = Hud->LookForTest(Style);
		TestTrue(*FString::Printf(TEXT("style %d has a positive radius scale"), S), Look.RadiusScale > 0.0f);
		TestTrue(*FString::Printf(TEXT("style %d has a positive thickness scale"), S), Look.ThicknessScale > 0.0f);
	}
	return true;
}

#endif
