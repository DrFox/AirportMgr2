#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TAXIWAY NAMES AT THE COMPOSITION (spec "Seam tests"): through ARoadNetworkActor and its facade, so each test goes red
 * if its seam - the load repair, the facade's notify - stops calling the model.
 */

/**
 * REVIEW FOCUS 4: a network saved before names, with roads that lost their own profile to a save (Profile null -
 * URoadNetwork::DefaultProfile's own comment), is named on BOTH load paths: a level's and a save game's (Serialize alone).
 * Plan D6: only after RepairLoadedNetwork re-resolves the default can those roads be read as taxiways at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLoadTest, "Airside.Present.TaxiwayNames.LoadBackfillsBothPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLoadTest::RunTest(const FString&)
{
	for (const ELoadedFrom From : { ELoadedFrom::Level, ELoadedFrom::SaveGame })
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
		// THE NETWORK IS MADE BY THE FIRST EDIT (Airside.Present.RepairReResolvesAMissingDefault's own setup) - a fresh
		// actor has none, and the repair is a no-op without one.
		Actor->PlaceNode(FVector2D(0.0, -100000.0));
		URoadNetwork* Net = Actor->Network;
		if (!TestNotNull(TEXT("the first edit made a network"), Net)) { return false; }
		const FRoadSegmentId Lost = Net->AddStraightSegment(Net->AddNode({ 0.0, 0.0 }), Net->AddNode({ 50000.0, 0.0 }), nullptr);
		Net->DefaultProfile = nullptr;
		TestFalse(TEXT("control: with no default it reads as no taxiway, so naming before the repair would skip it"),
			TaxiwayStrip::HasStrip(*Net, Lost));
		Actor->RepairLoadedNetwork(From);
		TestEqual(FString::Printf(TEXT("load path %d: the profile-less road is taxiway A"), static_cast<int32>(From)),
			Net->TaxiwayDisplayName(Net->TaxiwayOf(Lost)), FString(TEXT("A")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
