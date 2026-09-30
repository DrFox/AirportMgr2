#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadNetworkRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A 20 m x 24 m three-module depot, gate at the frontage midpoint, placed through the model.
	 *
	 * NOT THE 12 x 8 m TIER 1 PLOT: under the band layout that one seats none of the three
	 * (measured 2026-09-22 - "0 module bay(s) built, 3 dropped"), so a module count there
	 * would read 0 through a perfectly wired seam. This is PlotPresenterTest's DeepPlotAt,
	 * which Airside.Present.PlotPresenterLaysOutTheDepot asserts seats all three.
	 *
	 * NAMED FOR THIS FILE, not PlaceDepot: the tests module is a UNITY build, and
	 * PlotPresenterTest.cpp already has a PlaceDepot in its own anonymous namespace. Two of
	 * one name compile alone and collide together.
	 */
	void PlaceSeamTestDepot(ARoadNetworkActor* Road)
	{
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(1000.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0),
		                      FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Road->Network->PlaceEntity(Placement);
	}
}

/**
 * The buildings actor draws a depot the road network holds, through the delegate alone.
 *
 * THE SEAM TEST, per CLAUDE.md's refactor contract: OnTopologyRebuilt replaced a direct
 * Plots->RebuildFrom call (and OnNetworkChanged replaced OnTopologyRebuilt, #446), and a
 * delegate nobody bound passes every model test while the airport's depots silently vanish. Spawned by hand rather than by FAirsideTestWorld so the
 * test owns the order - road first, depot placed, THEN the buildings actor - which is also
 * what proves the catch-up rebuild on binding.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorDrawsThroughTheDelegateTest,
	"Airside.Present.BuildingsActorDrawsThroughTheDelegate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorDrawsThroughTheDelegateTest::RunTest(const FString& Parameters)
{
	// "Bare", NOT "TestWorld", throughout this file: Check-Architecture's assertion-reason rule
	// reads any Test*( call as an assertion, so TestWorld(false) fails the lint.
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }

	ARoadNetworkActor* Road = Bare.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a road network"), Road)) { return false; }
	Road->ClearNetwork();
	PlaceSeamTestDepot(Road);
	Road->RebuildMesh();

	AAirsideBuildingsActor* Buildings = Bare.World->SpawnActor<AAirsideBuildingsActor>();
	if (!TestNotNull(TEXT("a buildings actor"), Buildings)) { return false; }

	// FOUND WITHOUT BEING TOLD: the pointer was left empty, and there is one road network.
	TestEqual(TEXT("it bound to the only road network in the world"),
		Buildings->GetRoadNetwork(), Road);

	// CAUGHT UP ON BINDING. The depot was placed and rebuilt before this actor existed, so
	// the only way it can be standing is the rebuild BindTo runs itself.
	const UPlotPresenter* Plots = Buildings->GetPlotPresenter();
	if (!TestNotNull(TEXT("a plot presenter"), Plots)) { return false; }
	TestEqual(TEXT("the depot already standing is drawn on binding"), Plots->GetModuleCount(), 3);

	// AND FOLLOWS THE DELEGATE AFTERWARDS. Clearing the network and rebuilding must empty it;
	// an actor that drew once on binding and never listened again would keep the old depot.
	Road->ClearNetwork();
	Road->RebuildMesh();
	TestEqual(TEXT("a topology rebuild reaches it through the delegate"),
		Plots->GetModuleCount(), 0);

	return true;
}

/**
 * With no road network, the buildings actor draws nothing and does not crash.
 *
 * THE LEVEL THAT FORGOT ONE. A null dereference here would take the editor down on opening
 * any level that has a buildings actor and no road network; the Warning it logs instead is
 * what says why a depot is missing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorAloneDrawsNothingTest,
	"Airside.Present.BuildingsActorAloneDrawsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorAloneDrawsNothingTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }

	AAirsideBuildingsActor* Buildings = Bare.World->SpawnActor<AAirsideBuildingsActor>();
	if (!TestNotNull(TEXT("a buildings actor"), Buildings)) { return false; }

	TestNull(TEXT("no road network to bind to"), Buildings->GetRoadNetwork());
	TestEqual(TEXT("and nothing drawn"), Buildings->GetPlotPresenter()->GetInstanceCount(), 0);
	return true;
}

/**
 * FindOrCreate returns the existing actor rather than stacking a second.
 *
 * TWO BUILDINGS ACTORS ON ONE NETWORK DRAW EVERY DEPOT TWICE, exactly on top of each other,
 * which looks correct from every angle and doubles the instance cost. Three drivers call
 * FindOrCreate (the editor mode, the editor tool, the PIE controller), so re-entry is the
 * normal case rather than the edge.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorFindOrCreateIsIdempotentTest,
	"Airside.Present.BuildingsActorFindOrCreateIsIdempotent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorFindOrCreateIsIdempotentTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }
	ARoadNetworkActor* Road = Bare.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a road network"), Road)) { return false; }

	AAirsideBuildingsActor* First = AAirsideBuildingsActor::FindOrCreate(Bare.World, Road);
	AAirsideBuildingsActor* Second = AAirsideBuildingsActor::FindOrCreate(Bare.World, Road);
	if (!TestNotNull(TEXT("created"), First)) { return false; }
	TestEqual(TEXT("the second call finds the first"), Second, First);
	TestEqual(TEXT("bound to the road network it was given"), First->GetRoadNetwork(), Road);
	return true;
}

/**
 * #446: TWO NETWORK ACTORS GIVE ONE AIRPORT AND AN ERROR. Four lookups used to answer "which airport" with two rules
 * (the first found, the first spawned, never a guess), and with two actors the driver built into one while ops ran
 * the other. URoadNetworkRegistry holds the one answer: the second actor to register is refused, LOUDLY - an Error
 * naming both, which is what tells a designer which to delete - and the first keeps the slot. Not promoted when the
 * first leaves: which actor the game runs must never depend on teardown order.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegistrySecondAirportRefusedTest,
	"Airside.Present.Registry.SecondAirportIsRefusedLoudly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRegistrySecondAirportRefusedTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }
	ARoadNetworkActor* First = Bare.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a first road network"), First)) { return false; }
	TestEqual(TEXT("the first registers itself - no scan found it"), URoadNetworkRegistry::Find(Bare.World), First);

	AddExpectedMessagePlain(TEXT("is already this world's airport"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	ARoadNetworkActor* Second = Bare.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a second road network"), Second)) { return false; }
	TestEqual(TEXT("the second is refused: the first keeps the slot"), URoadNetworkRegistry::Find(Bare.World), First);
	TestEqual(TEXT("and ARoadNetworkActor::Find, every driver's lookup, gives the same answer"), ARoadNetworkActor::Find(Bare.World), First);

	First->Destroy();
	TestNull(TEXT("the first gone, the slot is empty - the refused second is not promoted by teardown order"),
		URoadNetworkRegistry::Find(Bare.World));
	return true;
}

/**
 * #446: THE BUILDINGS FOLLOW THE REGISTRY, so a buildings actor registered BEFORE its road network - actors register in
 * no promised order - binds the moment the road does. Its old scan ran once, at its own registration, and drew nothing
 * for the session if the road came second. THE SEAM TEST for URoadNetworkRegistry::OnAirportChanged's subscriber.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorBindsToALateAirportTest,
	"Airside.Present.BuildingsActorBindsToALateAirport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorBindsToALateAirportTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }
	AAirsideBuildingsActor* Buildings = Bare.World->SpawnActor<AAirsideBuildingsActor>();
	if (!TestNotNull(TEXT("a buildings actor"), Buildings)) { return false; }
	TestNull(TEXT("control: nothing to bind to yet"), Buildings->GetRoadNetwork());

	ARoadNetworkActor* Road = Bare.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a road network, second"), Road)) { return false; }
	TestEqual(TEXT("the buildings bound to it as it registered, with no call of their own"), Buildings->GetRoadNetwork(), Road);

	Road->Destroy();
	TestNull(TEXT("and let go when it left"), Buildings->GetRoadNetwork());
	return true;
}

#endif
