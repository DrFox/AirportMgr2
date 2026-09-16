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

	/**
	 * A 12 m x 24 m plot - three bays across, three rows deep.
	 *
	 * DEEPER THAN ThreeBayPlotAt ON PURPOSE. The Tier 1 plot is 8 m deep and the shed is 8 m
	 * long, so square to the gate it spans the whole depth and leaves two 4 m strips; a
	 * rotated tank needs its DIAGONAL of clear space and does not fit in one. Under the bay
	 * grid three modules sat side by side there, which is why this only shows up now. A plot
	 * the player dragged depth into is what holds a scattered mix.
	 */
	TArray<FVector2D> DeepPlotAt(double X)
	{
		return { FVector2D(X, 0.0), FVector2D(X + 1200.0, 0.0),
		         FVector2D(X + 1200.0, 2400.0), FVector2D(X, 2400.0) };
	}

	/** PlaceDepot's mix and pose, on a plot with room behind the shed. */
	FEntityInstanceId PlaceDeepDepot(ARoadNetworkActor* Actor, UEntityDefinition* Depot, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X + 600.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = DeepPlotAt(X);
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Actor->Network->PlaceEntity(Placement);
	}

	/** Every instance the plot presenter is holding, in the order it added them. */
	TArray<FTransform> PlotInstances(const ARoadNetworkActor* Actor)
	{
		TArray<FTransform> Out;
		const UPlotPresenter* Plots = Actor->GetPlotPresenter();
		if (Plots == nullptr)
		{
			return Out;
		}
		for (int32 Index = 0; Index < Plots->GetInstanceCount(); ++Index)
		{
			FTransform Transform;
			if (Plots->GetInstanceTransformForTest(Index, Transform))
			{
				Out.Add(Transform);
			}
		}
		return Out;
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
	FPlotPresenterScattersModulesTest,
	"Airside.Present.PlotPresenterScattersModules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterScattersModulesTest::RunTest(const FString& Parameters)
{
	// COMPOSITION LEVEL, per CLAUDE.md. Every Airside.Solve.PlotYard test would still pass
	// if the presenter called the solver and then drew on a grid anyway, or never called it
	// at all - the "declared but never consumed" shape this project has shipped three times.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), Actor->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDeepDepot(Actor, Depot, 0.0);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = Actor->GetPlotPresenter();

	// THE MODULES ARE THE FIRST INSTANCES a plot adds, before its fence. That ordering is
	// why this can name them at all - and why it uses ONE depot: with two, the second
	// depot's modules sit after the first depot's fence and the slice would be wrong.
	const int32 Modules = Plots->GetModuleCount();
	if (!TestEqual(TEXT("all three modules of the mix stand"), Modules, 3))
	{
		return false;
	}

	const TArray<FTransform> Instances = PlotInstances(Actor);
	if (!TestTrue(TEXT("the fence went up around them"), Instances.Num() > Modules))
	{
		return false;
	}

	// NOT ONE HEADING REPEATED. A grid gives every module the same rotation, which is
	// exactly the complaint this feature answers - and comparing ALL instances instead
	// would pass on the fence alone, whose panels face four ways round a rectangle.
	bool bHeadingsDiffer = false;
	for (int32 Index = 1; Index < Modules; ++Index)
	{
		if (!Instances[Index].GetRotation().Equals(Instances[0].GetRotation(), 0.001f))
		{
			bHeadingsDiffer = true;
			break;
		}
	}
	TestTrue(TEXT("the modules do not all face the same way"), bHeadingsDiffer);

	// A REBUILD IS IDEMPOTENT, and here that is the determinism requirement seen from the
	// outside: RebuildMesh runs on every graph change, and a yard reseeded each time would
	// shift while the player laid a road on the far side of the airport.
	Actor->RebuildMesh();
	const TArray<FTransform> After = PlotInstances(Actor);

	if (!TestEqual(TEXT("a rebuild puts the same number of things up"),
		Instances.Num(), After.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Instances.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("instance %d did not move on rebuild"), Index),
			Instances[Index].GetLocation().Equals(After[Index].GetLocation(), 0.0f));
	}

	// AND TWO DEPOTS DO NOT LOOK ALIKE. Seeded off position, so the second depot on the
	// airport must lay out differently from the first - the complaint that started this.
	PlaceDeepDepot(Actor, Depot, 4000.0);
	Actor->RebuildMesh();

	const TArray<FTransform> Both = PlotInstances(Actor);
	if (!TestTrue(TEXT("the second depot stood up too"), Both.Num() > Instances.Num()))
	{
		return false;
	}

	// The first depot's modules are unchanged, so the second depot's are the ones added
	// after the first plot's fence - compared RELATIVE to each depot's own pose, or two
	// identical yards 40 m apart would differ merely by being 40 m apart.
	const int32 SecondStart = Instances.Num();
	bool bYardsDiffer = false;
	for (int32 Index = 0; Index < Modules; ++Index)
	{
		const FVector FirstLocal = Both[Index].GetLocation() - FVector(600.0, 0.0, 0.0);
		const FVector SecondLocal =
			Both[SecondStart + Index].GetLocation() - FVector(4600.0, 0.0, 0.0);
		if (!FirstLocal.Equals(SecondLocal, 1.0f)
			|| !Both[Index].GetRotation().Equals(Both[SecondStart + Index].GetRotation(), 0.001f))
		{
			bYardsDiffer = true;
			break;
		}
	}
	TestTrue(TEXT("two depots with the same mix lay out differently"), bYardsDiffer);

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
