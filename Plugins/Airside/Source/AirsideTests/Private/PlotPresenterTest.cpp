#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A 12 m x 8 m plot at X - three bays, the Tier 1 depot as drawn. */
	TArray<FVector2D> ThreeBayPlotAt(double X)
	{
		return { FVector2D(X, 0.0), FVector2D(X + 1200.0, 0.0),
		         FVector2D(X + 1200.0, 800.0), FVector2D(X, 800.0) };
	}

	/**
	 * Place a three-bay depot straight through the model, bypassing the tool.
	 *
	 * The GATE is at the frontage midpoint, exactly as URoadEditFacade::PlaceEntityInPlot
	 * puts it - the presenter recovers the frontage edge from that, so a test that put the
	 * pose anywhere else would be testing a depot the facade never builds.
	 */
	FEntityInstanceId PlaceDepot(ARoadNetworkActor* Actor, UEntityDefinition* Depot, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X + 600.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = ThreeBayPlotAt(X);
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Actor->Network->PlaceEntity(Placement);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterDressesEachBayTest,
	"Airside.Present.PlotPresenterDressesEachBay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterDressesEachBayTest::RunTest(const FString& Parameters)
{
	// COMPOSITION LEVEL, per CLAUDE.md: spawn the actor and drive it, rather than poking the
	// model struct. A presenter that is never wired to the actor passes every model test and
	// draws nothing at all, which is exactly the failure this test exists to catch.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), Actor->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	Actor->RebuildMesh();
	TestEqual(TEXT("an empty airport stands nothing up"),
		Actor->GetPlotPresenter()->GetInstanceCount(), 0);

	PlaceDepot(Actor, Depot, 0.0);
	Actor->RebuildMesh();

	const int32 One = Actor->GetPlotPresenter()->GetInstanceCount();

	// Three modules plus a fence, so the count is well over three - the exact number moves
	// with the panel length and is not the claim. What IS the claim is that the modules and
	// the fence both went up.
	TestTrue(TEXT("three modules and a fence stand up"), One > 3);

	// THE GATE IS A GAP. A fence with no gate is a depot no truck can leave, and it would
	// look completely correct from every angle - so the skipped bay is counted, not eyeballed.
	TestTrue(TEXT("and the fence is left open at the gate"),
		Actor->GetPlotPresenter()->GetGateGapCount() > 0);

	// A SECOND DEPOT MUST ADD, NOT REPLACE. A presenter that rebuilt from only the last
	// entity passes every single-depot assertion and loses every depot but one on screen.
	PlaceDepot(Actor, Depot, 4000.0);
	Actor->RebuildMesh();
	TestEqual(TEXT("a second depot adds its own, it does not replace the first"),
		Actor->GetPlotPresenter()->GetInstanceCount(), One * 2);

	// AND A REBUILD IS IDEMPOTENT. RebuildMesh runs on every graph change, so a presenter
	// that appended instead of clearing would double the boxes every time the player drew a
	// road anywhere on the airport.
	Actor->RebuildMesh();
	TestEqual(TEXT("rebuilding again does not double the boxes"),
		Actor->GetPlotPresenter()->GetInstanceCount(), One * 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterSurvivesDuplicationTest,
	"Airside.Present.PlotPresenterSurvivesDuplication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterSurvivesDuplicationTest::RunTest(const FString& Parameters)
{
	// PIE DUPLICATES THE LEVEL, and a Transient non-instanced pointer comes back naming the
	// CDO's subobject rather than this actor's - which is why PostInitProperties re-points
	// all of them by name. The component travels the same way and the presenter must be
	// re-pointed AT IT, or a duplicate's boxes are added to a component no level renders:
	// the depot would simply be invisible in PIE and correct in the editor.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	ARoadNetworkActor* Dup = DuplicateObject<ARoadNetworkActor>(Actor, Actor->GetOuter());
	if (!TestNotNull(TEXT("a duplicate"), Dup)) { return false; }

	if (!TestNotNull(TEXT("the duplicate has a plot presenter"), Dup->GetPlotPresenter()))
	{
		return false;
	}
	TestEqual(TEXT("and it is the duplicate's own, not the CDO's"),
		Dup->GetPlotPresenter()->GetOuter(), static_cast<UObject*>(Dup));

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Dup->ClearNetwork();
	PlaceDepot(Dup, Depot, 0.0);
	Dup->RebuildMesh();

	// The real claim: the duplicate's boxes land somewhere its own presenter can count,
	// which they cannot if it is still filling the CDO's component.
	TestTrue(TEXT("the duplicate's own boxes stand up"),
		Dup->GetPlotPresenter()->GetInstanceCount() > 3);

	return true;
}

#endif
