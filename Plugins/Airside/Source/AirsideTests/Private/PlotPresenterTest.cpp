#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Build/DepotKit.h"
#include "Build/PlotLayoutStrategy.h"
#include "Content/AirsideSettings.h"
#include "Solve/PlotFit.h"
#include "Solve/PlotYard.h"

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
	 * A 20 m x 24 m plot, with room for the mix once the shed takes the back fence.
	 *
	 * DEEPER THAN ThreeBayPlotAt ON PURPOSE. The Tier 1 plot is 8 m deep and the shed is 8 m
	 * long, so square to the gate it spans the whole depth and leaves two 4 m strips; a
	 * rotated tank needs its DIAGONAL of clear space and does not fit in one. Under the bay
	 * grid three modules sat side by side there, which is why this only shows up now. A plot
	 * the player dragged depth into is what holds a scattered mix.
	 */
	TArray<FVector2D> DeepPlotAt(double X)
	{
		return { FVector2D(X, 0.0), FVector2D(X + 2000.0, 0.0),
		         FVector2D(X + 2000.0, 2400.0), FVector2D(X, 2400.0) };
	}

	/**
	 * A 30 m x 24 m plot - DeepPlotAt ten metres wider, for a test that needs ROOM LEFT OVER.
	 *
	 * DeepPlotAt held one of each with room to spare at the grey-box figures; at the real
	 * shed's (9.4 m deep, 5 m a bay plus 0.75 m caps, a 4 m apron) its 20 m reserves exactly
	 * the one-of-each it is given (2026-09-22), so a ghost test on it proves nothing.
	 */
	TArray<FVector2D> SpareRoomPlotAt(double X)
	{
		return { FVector2D(X, 0.0), FVector2D(X + 3000.0, 0.0),
		         FVector2D(X + 3000.0, 2400.0), FVector2D(X, 2400.0) };
	}

	/** PlaceDepot's mix and pose, on a plot with room behind the shed. */
	FEntityInstanceId PlaceDeepDepot(ARoadNetworkActor* Actor, UEntityDefinition* Depot, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X + 1000.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = DeepPlotAt(X);
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Actor->Network->PlaceEntity(Placement);
	}

	/** Every instance the plot presenter is holding, in the order it added them. */
	TArray<FTransform> PlotInstances(const AAirsideBuildingsActor* Buildings)
	{
		TArray<FTransform> Out;
		const UPlotPresenter* Plots = Buildings->GetPlotPresenter();
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
	if (!TestNotNull(TEXT("a plot presenter"), TestWorld.Buildings->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	Actor->RebuildMesh();
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	TestEqual(TEXT("an empty airport stands no modules up"), Plots->GetInstanceCount(), 0);
	TestEqual(TEXT("and no fence"), Plots->GetFencePostCount(), 0);

	PlaceDepot(Actor, Depot, 0.0);
	Actor->RebuildMesh();

	const int32 One = Plots->GetFencePostCount();

	// A FENCE STANDS even round a plot too small to seat a module - the 12 x 8 m Tier 1 plot
	// seats none under the band layout. The fence is the claim here; modules are
	// PlotPresenterLaysOutTheDepot's. The exact number moves with the spacing and is not the
	// claim either.
	TestTrue(TEXT("a fence stands up"), One > 0);

	// THE GATE IS A GAP. A fence with no gate is a depot no truck can leave, and it would
	// look completely correct from every angle - so the gate is counted, not eyeballed.
	TestTrue(TEXT("and the fence is left open at the gate"), Plots->GetGateGapCount() > 0);

	// A SECOND DEPOT MUST ADD, NOT REPLACE. A presenter that rebuilt from only the last
	// entity passes every single-depot assertion and loses every depot but one on screen.
	PlaceDepot(Actor, Depot, 4000.0);
	Actor->RebuildMesh();
	TestEqual(TEXT("a second depot adds its own, it does not replace the first"),
		Plots->GetFencePostCount(), One * 2);

	// AND A REBUILD IS IDEMPOTENT. RebuildMesh runs on every graph change, so a presenter
	// that appended instead of clearing would double the boxes every time the player drew a
	// road anywhere on the airport.
	Actor->RebuildMesh();
	TestEqual(TEXT("rebuilding again does not double the fence"),
		Plots->GetFencePostCount(), One * 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterLaysOutTheDepotTest,
	"Airside.Present.PlotPresenterLaysOutTheDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterLaysOutTheDepotTest::RunTest(const FString& Parameters)
{
	// COMPOSITION LEVEL, per CLAUDE.md. Every Airside.Solve.PlotYard test would still pass
	// if the presenter called the solver and then drew on a grid anyway, or never called it
	// at all - the "declared but never consumed" shape this project has shipped three times.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), TestWorld.Buildings->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDeepDepot(Actor, Depot, 0.0);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();

	// EVERY INSTANCE IS A MODULE since the fence moved to its own components (2026-09-22), so
	// the first depot's are simply the first Modules entries.
	const int32 Modules = Plots->GetModuleCount();
	if (!TestEqual(TEXT("all three modules of the mix stand"), Modules, 3))
	{
		return false;
	}

	const TArray<FTransform> Instances = PlotInstances(TestWorld.Buildings);
	if (!TestTrue(TEXT("the fence went up around them"), Plots->GetFencePostCount() > 0))
	{
		return false;
	}

	// NOT ONE HEADING REPEATED. A grid gives every module the same rotation, which is
	// exactly the complaint this feature answers - and comparing ALL instances instead
	// would pass on the fence alone, whose panels face four ways round a rectangle.
	// EVERY MODULE FACES THE SAME WAY, and that is now the claim rather than its opposite.
	//
	// THIS TEST ASSERTED THE REVERSE until 2026-09-20, and was right to: the yard was sampled
	// and "the modules do not all face the same way" was the whole point of sampling it. The
	// fuel depot lays out in bands now - see the layout-strategies design - so square to the
	// frontage IS the correct answer, and a jittered module here would mean the presenter had
	// quietly gone back to the scatter.
	//
	// THE SCATTER'S VARIETY IS STILL GUARDED, by Airside.Solve.PlotYardVariesWithSeed and
	// Airside.Build.ScatterStrategyIsTheScatter. It moved, it did not go.
	for (int32 Index = 1; Index < Modules; ++Index)
	{
		TestTrue(TEXT("every module is square to the frontage"),
			Instances[Index].GetRotation().Equals(Instances[0].GetRotation(), 0.001f));
	}

	// A REBUILD IS IDEMPOTENT, and here that is the determinism requirement seen from the
	// outside: RebuildMesh runs on every graph change, and a yard reseeded each time would
	// shift while the player laid a road on the far side of the airport.
	Actor->RebuildMesh();
	const TArray<FTransform> After = PlotInstances(TestWorld.Buildings);

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

	const TArray<FTransform> Both = PlotInstances(TestWorld.Buildings);
	if (!TestEqual(TEXT("the second depot stood its modules up too"), Both.Num(), Instances.Num() * 2))
	{
		return false;
	}

	// The first depot's modules are unchanged, so the second depot's are the ones after the
	// first depot's - compared RELATIVE to each depot's own pose, or two
	// identical yards 40 m apart would differ merely by being 40 m apart.
	//
	// TWO IDENTICAL PLOTS LAY OUT IDENTICALLY, which is also the reverse of what this asserted
	// until 2026-09-20. Every fuel depot built by the band layout shares its bones, and the
	// 2026-09-16 spec named that cost when it chose sampling instead. The evidence changed:
	// the sampled yard reached 65% coverage and 44 buildings wall to wall on a 45 m plot, and
	// a usable yard that repeats beats a varied one that does not.
	const int32 SecondStart = Instances.Num();
	for (int32 Index = 0; Index < Modules; ++Index)
	{
		const FVector FirstLocal = Both[Index].GetLocation() - FVector(1000.0, 0.0, 0.0);
		const FVector SecondLocal =
			Both[SecondStart + Index].GetLocation() - FVector(5000.0, 0.0, 0.0);

		TestTrue(TEXT("two depots on the same plot shape stand in the same places"),
			FirstLocal.Equals(SecondLocal, 1.0f));
		TestTrue(TEXT("and face the same way"),
			Both[Index].GetRotation().Equals(Both[SecondStart + Index].GetRotation(), 0.001f));
	}

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

	// THE CLAIM MOVED WITH THE PRESENTER on 2026-09-22: the buildings actor owns it now, so
	// that is the actor duplicated. DuplicateObject registers nothing, so the duplicate is
	// bound by hand - the thing under test is the re-pointing, not the binding.
	if (!TestNotNull(TEXT("a buildings actor"), TestWorld.Buildings)) { return false; }
	AAirsideBuildingsActor* Dup = DuplicateObject<AAirsideBuildingsActor>(
		TestWorld.Buildings, TestWorld.Buildings->GetOuter());
	if (!TestNotNull(TEXT("a duplicate"), Dup)) { return false; }

	if (!TestNotNull(TEXT("the duplicate has a plot presenter"), Dup->GetPlotPresenter()))
	{
		return false;
	}
	TestEqual(TEXT("and it is the duplicate's own, not the CDO's"),
		Dup->GetPlotPresenter()->GetOuter(), static_cast<UObject*>(Dup));

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDepot(Actor, Depot, 0.0);
	Dup->BindTo(Actor);

	// The real claim: the duplicate's boxes land somewhere its own presenter can count,
	// which they cannot if it is still filling the CDO's component.
	TestTrue(TEXT("the duplicate's own boxes stand up"),
		Dup->GetPlotPresenter()->GetFencePostCount() > 0);

	return true;
}

/**
 * A plot draws every reserved stand: the bought ones solid, the rest ghosted.
 *
 * THE GHOST IS NOT A MARKER. The 2026-09-16 spec removed a GRID of slot markers because a
 * uniform grid claimed a structure the scattered yard did not have. A ghost here is a solved
 * stand - its own footprint, its own sampled heading, from the code path that will place the
 * module when it is bought. It does not claim the yard has a structure; it shows the yard.
 *
 * COMPOSITION LEVEL, per CLAUDE.md and the two tests above it: every Airside.Solve test
 * would still pass if the presenter called Reserve and then drew the owned modules anyway,
 * which is the "declared but never consumed" shape this project has shipped three times.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterGhostsUnboughtSlotsTest,
	"Airside.Present.PlotPresenterGhostsUnboughtSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterGhostsUnboughtSlotsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), TestWorld.Buildings->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();

	// A PLOT WITH ROOM LEFT OVER, not ThreeBayPlotAt: 30 x 24 m reserves more than the starter
	// one-of-each, and a plot that reserved exactly three would pass this test while proving
	// nothing about ghosts - which DeepPlotAt became once the real shed arrived.
	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D(1500.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = SpareRoomPlotAt(0.0);
	Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
	Actor->Network->PlaceEntity(Placement);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();

	// WHAT WAS BOUGHT IS DRAWN SOLID. PlaceDeepDepot owns one of each, so three bays are lit
	// however the reservation grouped them.
	TestEqual(TEXT("the three owned modules are drawn solid"), Plots->GetModuleCount(), 3);

	// AND THE SPARE ROOM IS DRAWN. This is the claim the whole design rests on: the player
	// can see what the plot would hold before spending anything on it.
	TestTrue(TEXT("a 30 x 24 m plot has spare capacity to ghost"),
		Plots->GetGhostCount() > 0);

	// A RESERVATION CANNOT DROP. Unlike LayOut, Reserve chose the list, so a drop is a bug
	// rather than a refusal - and keeping the counter is how that stays assertable.
	TestEqual(TEXT("nothing was dropped"), Plots->GetDroppedCount(), 0);

	// SOLID PLUS GHOSTED IS THE WHOLE RESERVATION, so no bay is drawn twice and none is
	// silently skipped. Recomputed here rather than remembered: the presenter derives it the
	// same way, and a second stored copy is the drift this project names most often.
	// THROUGH THE SEAM, because the presenter goes through it. Calling PlotYard::Reserve here
	// would measure a band-laid depot against a scattered one's stand count.
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(UAirsideSettings::GetContent());

	// A NAMED LOCAL, because FPlotSite::Outline is a VIEW. Assigning SpareRoomPlotAt(0.0)
	// straight into it binds the view to a temporary that dies at the semicolon, and the solve
	// then reads freed memory - which here returned a plausible-looking 3 rather than crashing.
	const TArray<FVector2D> Outline = SpareRoomPlotAt(0.0);

	FPlotSite Site;
	Site.Outline = Outline;
	Site.FrontageA = FVector2D(0.0, 0.0);
	Site.FrontageB = FVector2D(3000.0, 0.0);
	Site.Gate = FVector2D(1500.0, 0.0);
	Site.Seed = DepotYardSeed(Site.Gate);

	const PlotYard::FReservation Reservation =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		Bays += Stand.RunLength;
	}
	TestEqual(TEXT("every reserved bay is drawn, once"),
		Plots->GetModuleCount() + Plots->GetGhostCount(), Bays);

	// AND THE ROOM READOUT IS THOSE GHOSTS, not a separate sampling pass. One evaluator.
	TestEqual(TEXT("room to grow is the count of unlit bays"),
		Plots->GetRoomForMore(), Plots->GetGhostCount());

	return true;
}

/**
 * An owned module that reserved no stand is counted as dropped.
 *
 * THE RUN-LENGTH REWRITE (61f92fc) DELETED THE OLD ++Dropped along with the old
 * one-module-per-stand loop and never replaced it, so GetDroppedCount() and the census log
 * said 0 no matter how many modules a player owned beyond what the plot could seat - while
 * GetDroppedCount's own comment calls a drop "a bug", i.e. an invariant with no accessor
 * that could ever go red (issue #193). Fifty pumps on a plot that can seat a handful is not
 * latent; PIE would have shown "50 pumps" bought and a fraction of them standing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterCountsDropsTest,
	"Airside.Present.PlotPresenterCountsDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterCountsDropsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), TestWorld.Buildings->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();

	// FAR MORE PUMPS THAN ANY PLOT THIS SIZE COULD SEAT - the point is not the exact number,
	// only that it exceeds the reservation's own ceiling for the kit.
	TArray<EDepotModule> Modules;
	for (int32 I = 0; I < 50; ++I)
	{
		Modules.Add(EDepotModule::Pump);
	}

	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D(1000.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = DeepPlotAt(0.0);
	Placement.Modules = Modules;
	Actor->Network->PlaceEntity(Placement);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();

	// THE SAME SOLVE THE PRESENTER RAN, through the seam it goes through - see
	// PlotPresenterGhostsUnboughtSlotsTest's own comment on why this recomputes rather than
	// remembers.
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(UAirsideSettings::GetContent());
	const TArray<FVector2D> Outline = DeepPlotAt(0.0);

	FPlotSite Site;
	Site.Outline = Outline;
	Site.FrontageA = FVector2D(0.0, 0.0);
	Site.FrontageB = FVector2D(2000.0, 0.0);
	Site.Gate = FVector2D(1000.0, 0.0);
	Site.Seed = DepotYardSeed(Site.Gate);

	const PlotYard::FReservation Reservation =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);
	const int32 PumpCeiling = Reservation.CeilingFor(static_cast<int32>(EDepotModule::Pump));

	if (!TestTrue(TEXT("the plot cannot seat all fifty pumps"), PumpCeiling < 50))
	{
		return false;
	}

	TestEqual(TEXT("every pump beyond the ceiling is dropped, not silently discarded"),
		Plots->GetDroppedCount(), 50 - PumpCeiling);

	return true;
}

/**
 * A built depot is the band layout's depot, and draws its objects rather than its aprons.
 *
 * MEASURED AGAINST THE STRATEGY DIRECTLY, which is what makes this red before the wiring:
 * the presenter still calls the scatter, and the two disagree about how many bays a plot
 * holds. Asserting only "no box is twelve metres long" would pass against the scatter too,
 * which knows nothing of aprons.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterDrawsTheObjectNotTheApronTest,
	"Airside.Present.PlotPresenterDrawsTheObjectNotTheApron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterDrawsTheObjectNotTheApronTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDeepDepot(Actor, Depot, /*X=*/0.0);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();

	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(UAirsideSettings::GetContent());
	const TArray<FVector2D> Outline = DeepPlotAt(0.0);

	FPlotSite Expected;
	Expected.Outline = Outline;
	Expected.FrontageA = FVector2D(0.0, 0.0);
	Expected.FrontageB = FVector2D(2000.0, 0.0);
	Expected.Gate = FVector2D(1000.0, 0.0);
	Expected.Seed = DepotYardSeed(Expected.Gate);

	const PlotYard::FReservation Bands =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Expected, Specs);

	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Bands.Stands)
	{
		Bays += Stand.RunLength;
	}
	if (!TestTrue(TEXT("the band layout holds something here"), Bays > 0)) { return false; }

	TestEqual(TEXT("the depot drawn is the band layout's depot"),
		Plots->GetModuleCount() + Plots->GetGhostCount(), Bays);

	// AND ITS OBJECTS ARE DRAWN, NOT ITS APRONS. The shed is 8 m long and its apron 4 m, so a
	// box 12 m long is the apron drawn as building. Scale is length / 100 - the engine cube
	// is 100 uu on a side.
	for (int32 I = 0; I < Plots->GetInstanceCount(); ++I)
	{
		FTransform At;
		if (!Plots->GetInstanceTransformForTest(I, At)) { continue; }
		TestFalse(TEXT("no box is drawn at footprint plus apron"),
			FMath::IsNearlyEqual(At.GetScale3D().X * 100.0, 1200.0, 1.0));
	}

	// AND NOTHING DROPPED. A reservation returns only what it placed, whatever strategy made
	// it, so this stays an invariant across the seam.
	TestEqual(TEXT("nothing was dropped"), Plots->GetDroppedCount(), 0);

	return true;
}

namespace
{
	/** An engine primitive by path - a stand-in for an authored piece, so no content is needed. */
	UStaticMesh* PartsTestMesh(const TCHAR* Path)
	{
		return LoadObject<UStaticMesh>(nullptr, Path);
	}

	/** PlaceDeepDepot's plot and pose, owning exactly Modules. */
	void PlacePartsDepot(ARoadNetworkActor* Actor, UEntityDefinition* Depot,
		const TArray<EDepotModule>& Modules)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(1000.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = DeepPlotAt(0.0);
		Placement.Modules = Modules;
		Actor->Network->PlaceEntity(Placement);
	}

	/** Where a point stands along the run - the envelope's own Across (right) axis. */
	double PartsAlongRun(const FTransform& Envelope, const FVector& Where)
	{
		const FVector Across = Envelope.GetRotation().GetRightVector();
		return FVector::DotProduct(Where - Envelope.GetLocation(), Across);
	}

	/** The shed as parts - cube cap, cylinder bay - over the grey-box specs with a 50 uu cap. */
	void PartsTestKit(UStaticMesh* Cap, UStaticMesh* Bay,
		TArray<PlotYard::FKitSpec>& OutSpecs, TArray<FDepotModuleLook>& OutLooks)
	{
		OutSpecs = DepotKitSpecs(nullptr);
		OutSpecs[static_cast<int32>(EDepotModule::Shed)].RunEndUu = 50.0;
		OutLooks.SetNum(OutSpecs.Num());
		FDepotModuleLook& Look = OutLooks[static_cast<int32>(EDepotModule::Shed)];
		Look.Assembly = EKitAssembly::Parts;
		Look.Cap = Cap;
		Look.Bay = Bay;
	}
}

/**
 * A shed run is assembled from parts: cap, the bays bought, mirrored cap, then ghost bays.
 *
 * DRIVEN THROUGH THE PRESENTER with a stand-in look rather than through content, so what is
 * asserted is the assembly and not whatever the shipped kit happens to be today.
 * AirportMgr.Content.DepotDrawsItsShippedMeshes is the other half: the actor handing the
 * real kit in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterAssemblesAShedFromPartsTest,
	"Airside.Present.PlotPresenterAssemblesAShedFromParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterAssemblesAShedFromPartsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	UStaticMesh* Cap = PartsTestMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Bay = PartsTestMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!TestTrue(TEXT("presenter, depot and both stand-in meshes"),
		Plots != nullptr && Depot != nullptr && Cap != nullptr && Bay != nullptr)) { return false; }

	Actor->ClearNetwork();
	PlacePartsDepot(Actor, Depot, { EDepotModule::Shed });

	TArray<PlotYard::FKitSpec> Specs;
	TArray<FDepotModuleLook> Looks;
	PartsTestKit(Cap, Bay, Specs, Looks);
	const PlotYard::FKitSpec& Shed = Specs[static_cast<int32>(EDepotModule::Shed)];

	Plots->RebuildFrom(*Actor->Network, Specs, FFenceKit(), Looks);

	TestEqual(TEXT("one shed bay bought"), Plots->GetModuleCount(), 1);
	TestEqual(TEXT("a built bay is one bay mesh"), Plots->GetMeshInstanceCountForTest(Bay, false), 1);
	TestEqual(TEXT("closed by a cap at each end - a partly-bought run is a finished building"),
		Plots->GetMeshInstanceCountForTest(Cap, false), 2);
	TestEqual(TEXT("and its ghosted bays carry no caps of their own"),
		Plots->GetMeshInstanceCountForTest(Cap, true), 0);
	const int32 GhostBays = Plots->GetMeshInstanceCountForTest(Bay, true);
	if (!TestTrue(*FString::Printf(
		TEXT("the run reserved more than the bay bought, got %d ghost bay(s)"), GhostBays),
		GhostBays >= 1)) { return false; }

	FTransform Envelope;
	FTransform NearCap;
	FTransform FarCap;
	FTransform FirstGhost;
	if (!TestTrue(TEXT("the envelope and every piece can be read back"),
		Plots->GetInstanceTransformForTest(0, Envelope)
		&& Plots->GetMeshInstanceTransformForTest(Cap, false, 0, NearCap)
		&& Plots->GetMeshInstanceTransformForTest(Cap, false, 1, FarCap)
		&& Plots->GetMeshInstanceTransformForTest(Bay, true, 0, FirstGhost))) { return false; }

	// THE FAR CAP IS MIRRORED, THE NEAR ONE IS NOT - one mesh authored for the near end, as
	// the shed's SPEC.md authors it. A far cap drawn unmirrored shows its open side outward.
	TestTrue(TEXT("the near cap is not mirrored"), NearCap.GetDeterminant() > 0.0);
	TestTrue(TEXT("the far cap is"), FarCap.GetDeterminant() < 0.0);

	// ALONG THE RUN, from the built envelope's near edge: cube and cylinder are 100 uu and
	// centred, so each centre sits 50 uu past where the piece starts. Cap (50 reserved), one
	// 400 uu bay, far cap, then the ghosts carry on from the far cap's end.
	const double LitWidth = Shed.RunWidthUu(1);
	const double Start = -LitWidth * 0.5;
	TestNearlyEqual(TEXT("the near cap starts the run"),
		PartsAlongRun(Envelope, NearCap.GetLocation()), Start + 50.0, 0.01);
	TestNearlyEqual(TEXT("the far cap starts after one cap and one bay"),
		PartsAlongRun(Envelope, FarCap.GetLocation()),
		Start + Shed.RunEndUu + Shed.Footprint.WidthUu + 50.0, 0.01);
	TestNearlyEqual(TEXT("and the first ghost bay starts where the built building ends"),
		PartsAlongRun(Envelope, FirstGhost.GetLocation()), Start + LitWidth + 50.0, 0.01);

	// ALL IN ONE ROW: the depth offset centres every piece on the same line through the stand.
	const FVector Forward = Envelope.GetRotation().GetForwardVector();
	const double Row = FVector::DotProduct(NearCap.GetLocation() - Envelope.GetLocation(), Forward);
	TestNearlyEqual(TEXT("the far cap stands in the near cap's row"),
		FVector::DotProduct(FarCap.GetLocation() - Envelope.GetLocation(), Forward), Row, 0.01);
	TestNearlyEqual(TEXT("and so does the ghost bay"),
		FVector::DotProduct(FirstGhost.GetLocation() - Envelope.GetLocation(), Forward), Row, 0.01);

	// IDEMPOTENT, like every other derived geometry: a rebuild that appended would double the
	// shed every time a road was drawn anywhere on the airport.
	Plots->RebuildFrom(*Actor->Network, Specs, FFenceKit(), Looks);
	TestEqual(TEXT("rebuilding again does not double the caps"),
		Plots->GetMeshInstanceCountForTest(Cap, false), 2);
	return true;
}

/**
 * With no bay bought the ghost is the whole building, both caps included, and nothing is solid.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterGhostsAWholeUnboughtShedTest,
	"Airside.Present.PlotPresenterGhostsAWholeUnboughtShed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterGhostsAWholeUnboughtShedTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	UStaticMesh* Cap = PartsTestMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Bay = PartsTestMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!TestTrue(TEXT("presenter, depot and both stand-in meshes"),
		Plots != nullptr && Depot != nullptr && Cap != nullptr && Bay != nullptr)) { return false; }

	Actor->ClearNetwork();
	PlacePartsDepot(Actor, Depot, {});

	TArray<PlotYard::FKitSpec> Specs;
	TArray<FDepotModuleLook> Looks;
	PartsTestKit(Cap, Bay, Specs, Looks);
	Plots->RebuildFrom(*Actor->Network, Specs, FFenceKit(), Looks);

	if (!TestTrue(TEXT("the plot reserved at least one shed run"),
		Plots->GetMeshInstanceCountForTest(Bay, true) >= 1)) { return false; }
	TestEqual(TEXT("nothing is built"), Plots->GetMeshInstanceCountForTest(Bay, false)
		+ Plots->GetMeshInstanceCountForTest(Cap, false), 0);
	const int32 GhostCaps = Plots->GetMeshInstanceCountForTest(Cap, true);
	TestTrue(*FString::Printf(TEXT("every ghosted run has both its caps, got %d cap(s)"), GhostCaps),
		GhostCaps >= 2 && GhostCaps % 2 == 0);
	return true;
}

/**
 * A baked mesh stands centred on the envelope it replaces, on the ground, facing the stand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterCentresABakedMeshTest,
	"Airside.Present.PlotPresenterCentresABakedMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterCentresABakedMeshTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	UStaticMesh* Tank = PartsTestMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (!TestTrue(TEXT("presenter, depot and the stand-in mesh"),
		Plots != nullptr && Depot != nullptr && Tank != nullptr)) { return false; }

	Actor->ClearNetwork();
	PlacePartsDepot(Actor, Depot, { EDepotModule::Tank });

	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);
	TArray<FDepotModuleLook> Looks;
	Looks.SetNum(Specs.Num());
	Looks[static_cast<int32>(EDepotModule::Tank)].Baked = { Tank };

	Plots->RebuildFrom(*Actor->Network, Specs, FFenceKit(), Looks);

	FTransform Envelope;
	FTransform Drawn;
	if (!TestTrue(TEXT("one tank built and drawn from its mesh"),
		Plots->GetModuleCount() == 1
		&& Plots->GetInstanceTransformForTest(0, Envelope)
		&& Plots->GetMeshInstanceTransformForTest(Tank, false, 0, Drawn))) { return false; }

	TestNearlyEqual(TEXT("centred on its envelope in X"),
		Drawn.GetLocation().X, Envelope.GetLocation().X, 0.01);
	TestNearlyEqual(TEXT("and in Y"), Drawn.GetLocation().Y, Envelope.GetLocation().Y, 0.01);
	TestNearlyEqual(TEXT("standing on the ground by its own bounds, not sunk to its waist"),
		Drawn.GetLocation().Z, -Tank->GetBoundingBox().Min.Z, 0.01);
	TestNearlyEqual(TEXT("facing the way the stand faces"),
		Drawn.Rotator().Yaw, Envelope.Rotator().Yaw, 0.01);
	return true;
}

#endif
