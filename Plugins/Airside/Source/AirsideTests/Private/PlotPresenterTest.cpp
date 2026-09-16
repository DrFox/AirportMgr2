#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/PlotFit.h"

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
	FPlotShowsRoomToGrowTest,
	"Airside.Present.PlotShowsRoomToGrow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotShowsRoomToGrowTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), Actor->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	// The same placement PlaceDepot makes, with the plot's own size as the variable - the
	// outline comes from PlotFit::GridOutline so it is the rectangle the gesture commits,
	// not a hand-typed one that could disagree with it.
	const TArray<EDepotModule> AllThree =
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };

	auto PlaceMix = [&](int32 Width, int32 Depth, const TArray<EDepotModule>& Modules)
	{
		Actor->ClearNetwork();

		const FVector2D A(0.0, 0.0);
		const FVector2D B(Width * PlotFit::BayWidthUu, 0.0);

		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = (A + B) * 0.5;
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = PlotFit::GridOutline(A, B, Width, Depth);
		Placement.Modules = Modules;
		Actor->Network->PlaceEntity(Placement);
		Actor->RebuildMesh();
	};

	auto PlaceSized = [&](int32 Width, int32 Depth) { PlaceMix(Width, Depth, AllThree); };

	// THREE WIDE, TWO DEEP holds six slots and three modules, so three stand empty. This is
	// the whole payoff of the depth step before buying exists: a plot that visibly says
	// "three more fit here" rather than three sheds dumped in a corner.
	PlaceSized(3, 2);
	TestEqual(TEXT("a 3x2 plot with three modules has three slots to grow into"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 3);

	// ONE ROW DEEP has nowhere to grow, which is what the gesture warned about.
	PlaceSized(3, 1);
	TestEqual(TEXT("a one-row plot has no room to grow"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 0);

	// AND A NARROW PLOT DROPS MODULES RATHER THAN OVERFLOWING INTO ROW 2. Two bays across
	// take two modules; the third is not quietly moved to the back where no truck reaches.
	PlaceSized(2, 2);
	TestEqual(TEXT("two bays take two modules, not three"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 2);

	// AND THE MARKERS ARE REAL INSTANCES, not just a counter. GetEmptySlotCount could be
	// arithmetic that never reached the component - the "declared but never consumed" shape
	// CLAUDE.md names three times - and every assertion above would still pass.
	//
	// THE CLAIM IS AN INVARIANT, not a number: on ONE plot geometry, a slot is drawn whether
	// it holds a module or not, so taking modules away must leave the instance count exactly
	// where it was. Comparing two different plot SIZES would prove nothing, because their
	// fences differ too and the fence is most of the count.
	PlaceSized(3, 2);
	const int32 ThreeModules = Actor->GetPlotPresenter()->GetInstanceCount();
	PlaceMix(3, 2, { EDepotModule::Shed });
	TestEqual(TEXT("an emptier yard is the same six slots, drawn differently"),
		Actor->GetPlotPresenter()->GetInstanceCount(), ThreeModules);
	TestEqual(TEXT("and five of them are now room to grow"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 5);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPadIsPavementTest,
	"Airside.Present.PlotPadIsPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPadIsPavementTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("an apron component"), Actor->ApronComponent.Get())) { return false; }

	Actor->ClearNetwork();
	Actor->RebuildMesh();

	// Read the count BEFORE, so the claim is about what the plot ADDED rather than an
	// absolute number that moves with every other surface in the level.
	const int32 Before =
		Actor->ApronComponent->GetDynamicMesh()->GetMeshRef().TriangleCount();

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	PlaceDepot(Actor, Depot, 0.0);
	Actor->RebuildMesh();

	// ON THE APRON COMPONENT, not a component of its own. The pad is pavement and shares
	// the surface, the Z and the material of the aprons it abuts - two components would be
	// two surfaces the player can see the seam between.
	TestTrue(TEXT("the drawn plot became pavement, not bare grass"),
		Actor->ApronComponent->GetDynamicMesh()->GetMeshRef().TriangleCount() > Before);

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
