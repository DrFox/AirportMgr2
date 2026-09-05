#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Tool/RouteTool.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteToolDefaultAirframeTest,
	"Airside.Tool.RouteTool.DefaultAirframe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteToolDefaultAirframeTest::RunTest(const FString& Parameters)
{
	// Issue #30: AirframeFor used to be three helpers (GroundFor, ClimbFor, EngineFor), each
	// falling back to UAircraftType::PiperMeridian*() at its own call site - nothing asserted
	// that the fallback a route actually got was the SAME one UAirsideSettings resolves, so a
	// caller reading the resolver and one reading AirframeFor could silently drift apart. This
	// pins that they agree.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A plain taxiway node with no entity anywhere near it - most of the graph, and the case
	// AircraftAtNode returns null for.
	const FGuidelineNodeId Node = Net->AddGuidelineNode(FVector2D(0.0, 0.0));

	const FAirframe Resolved = AirframeFor(*Net, Node);
	const FAirframe Default = UAirsideSettings::ResolveDefaultAirframe();

	TestEqual(
		TEXT("a node with no design aircraft gets the resolver's turn rate, not a struct default "
			 "- MaxTurnRateDegPerSec 10.0 would mean AirframeFor fell through to a bare FAirframe()"),
		Resolved.Ground.MaxTurnRateDegPerSec, Default.Ground.MaxTurnRateDegPerSec);

	TestNotEqual(
		TEXT("and that turn rate is not the struct default either - the Piper's is 20, so this "
			 "would also pass if AirframeFor quietly returned FGroundPerformance()"),
		Resolved.Ground.MaxTurnRateDegPerSec, FGroundPerformance().MaxTurnRateDegPerSec);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
