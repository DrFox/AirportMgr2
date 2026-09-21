#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Solve/RoadGeom.h"
#include "Tool/BuildGesture.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TWO OF THE FIVE DRIVER-DIVERGENCE BEHAVIOURS FROM ISSUE #191/#92-#93: DragThresholdPixels
 * and MaxPlaceDistanceFactor were each typed as a literal on ARoadBuildController AND, before
 * this fix, a second literal on URoadBuildEditorTool's side (a file-scope constexpr for the
 * former, TNumericLimits<double>::Max() - i.e. no cap at all - for the latter). Both drivers
 * now default from the same shared constant (FBuildGesture::DefaultThresholdPixels,
 * RoadGeom::DefaultMaxPlaceDistanceFactor); this pins that ARoadBuildController's own
 * UPROPERTY defaults actually READ that constant rather than a fresh literal that happens to
 * equal it today. A hand-typed "4.0" or "6.0" reintroduced here would still compile and would
 * still pass a value-only comparison against itself - this compares against the SHARED name,
 * so a future edit to one side without the other goes red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildControllerDriverParityConstantsTest,
	"AirportMgr.Actions.DriverParityConstantsMatchTheSharedDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildControllerDriverParityConstantsTest::RunTest(const FString& Parameters)
{
	const ARoadBuildController* Defaults = GetMutableDefault<ARoadBuildController>();
	if (!TestNotNull(TEXT("the controller's class default object"), Defaults))
	{
		return false;
	}

	TestEqual(TEXT("PIE's drag threshold starts at the one FBuildGesture (and the editor tool) uses"),
		Defaults->DragThresholdPixels, FBuildGesture::DefaultThresholdPixels);
	TestEqual(TEXT("PIE's horizon-cap factor starts at the one RayToPlaneZ's callers share"),
		Defaults->MaxPlaceDistanceFactor, RoadGeom::DefaultMaxPlaceDistanceFactor);

	return true;
}

#endif
