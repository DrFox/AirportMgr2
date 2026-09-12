#include "CoreMinimal.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RoadProfileBands.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A 45 m runway W -> E along y = 0 and a taxiway leaving its middle due south.
	 * Returns the runway's first segment for the facts to be set through.
	 */
	FRoadSegmentId RunwayAndTaxiway(URoadNetwork& Net)
	{
		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		const FRoadNodeId W = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId X = Net.AddNode(FVector2D(50000.0, 0.0));
		const FRoadNodeId E = Net.AddNode(FVector2D(100000.0, 0.0));
		const FRoadNodeId S = Net.AddNode(FVector2D(50000.0, -30000.0));
		const FRoadSegmentId RW = Net.AddStraightSegment(W, X, Runway);
		Net.AddStraightSegment(X, E, Runway);
		Net.AddStraightSegment(X, S, Taxiway);
		return RW;
	}

	/** Every triangle's material id, sorted into the runway's strip or the taxiway proper. */
	void CountIds(const FRoadMeshBuffers& Buffers, TMap<int32, int32>& OutRunway, TMap<int32, int32>& OutTaxiway)
	{
		for (int32 Triangle = 0; Triangle * 3 + 2 < Buffers.Indices.Num(); ++Triangle)
		{
			FVector3d Centroid = FVector3d::ZeroVector;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Centroid += Buffers.Positions[Buffers.Indices[Triangle * 3 + Corner]];
			}
			Centroid /= 3.0;
			const int32 Id = Buffers.MaterialIDs.IsValidIndex(Triangle) ? Buffers.MaterialIDs[Triangle] : -1;
			if (FMath::Abs(Centroid.Y) <= 2250.0)
			{
				// Inside the strip's own width, which includes the junction the taxiway cuts.
				OutRunway.FindOrAdd(Id)++;
			}
			else if (Centroid.Y < -20000.0)
			{
				// The taxiway's own ribbon, well clear of the junction. A perpendicular exit
				// gets symmetric 60 m exit arcs and a flare fillet of the same radius
				// (ExitGeometry), so the junction's pavement - which is the RUNWAY's, the
				// dominant arm paving the junction - reaches well over 12 km-uu down the
				// taxiway. Everything nearer than this cut may legitimately be runway slot.
				OutTaxiway.FindOrAdd(Id)++;
			}
		}
	}
}

/**
 * THE SURFACE FOLLOWS THE FACTS. A runway's bands, and the junction a runway dominates,
 * take the slot its surface names; the taxiway beside it keeps its profile's. Change the
 * facts and rebuild, and the ids move with them - nothing about the profile changed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwaySurfaceSlotsTest,
	"Airside.Build.RunwaySurfaceSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwaySurfaceSlotsTest::RunTest(const FString& Parameters)
{
	URoadMaterialSet* Set = URoadMaterialSet::MakeTransient({
		TEXT("Asphalt"), TEXT("Concrete"), TEXT("Kerb"),
		URoadMaterialSet::RunwaySlotName(ERunwaySurface::Grass),
		URoadMaterialSet::RunwaySlotName(ERunwaySurface::Tarmac),
		URoadMaterialSet::RunwaySlotName(ERunwaySurface::Concrete) });
	const int32 Tarmac = Set->IndexOf(TEXT("RunwayTarmac"));
	const int32 Grass = Set->IndexOf(TEXT("RunwayGrass"));
	TestEqual(TEXT("reinforced shares concrete's slot"), URoadMaterialSet::RunwaySlotName(ERunwaySurface::Reinforced), FName(TEXT("RunwayConcrete")));

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId RW = RunwayAndTaxiway(*Net);

	auto BuildAndCount = [&](TMap<int32, int32>& Runway, TMap<int32, int32>& Taxiway)
	{
		FRoadMeshBuilder Builder(10.0, 512.0, Set);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		Builder.Build(*Net, Solved, 1);
		CountIds(Builder.GetBuffers(), Runway, Taxiway);
	};

	// 1. Tarmac by default (an existing level's runways).
	{
		TMap<int32, int32> Runway, Taxiway;
		BuildAndCount(Runway, Taxiway);
		TestTrue(TEXT("the strip has triangles"), Runway.Num() > 0);
		TestTrue(TEXT("the taxiway has triangles"), Taxiway.Num() > 0);
		TestEqual(TEXT("every triangle on the strip - ribbons and the junction - is RunwayTarmac"), Runway.Num(), 1);
		TestTrue(TEXT("and that one id is the tarmac slot"), Runway.Contains(Tarmac));
		TestFalse(TEXT("the taxiway never takes a runway slot"), Taxiway.Contains(Tarmac) || Taxiway.Contains(Grass));
		for (const TPair<int32, int32>& Pair : Taxiway)
		{
			TestTrue(TEXT("the taxiway's ids are its profile's own bands"), Pair.Key == 0 || Pair.Key == 1);
		}
	}

	// 2. The facts change, the profile does not, and the pavement follows the facts.
	{
		FRunwayFacts Facts;
		Facts.Surface = ERunwaySurface::Grass;
		TestTrue(TEXT("the strip becomes grass"), Net->SetRunwayFacts(RW, Facts));
		TMap<int32, int32> Runway, Taxiway;
		BuildAndCount(Runway, Taxiway);
		TestEqual(TEXT("every triangle on the strip is now RunwayGrass"), Runway.Num(), 1);
		TestTrue(TEXT("and that one id is the grass slot"), Runway.Contains(Grass));
	}

	// 3. A set that does not declare the runway slots: the override falls back to 0 and is
	//    counted once, so a project without runway materials still draws runways as roads.
	{
		URoadMaterialSet* Plain = URoadMaterialSet::MakeTransient({ TEXT("Asphalt"), TEXT("Concrete") });
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(
			Net->ProfileFor(*Net->GetSegment(RW)), Plain, URoadMaterialSet::RunwaySlotName(ERunwaySurface::Grass));
		for (const int32 Slot : Bands.SlotIndices)
		{
			TestEqual(TEXT("an undeclared runway slot falls back to 0"), Slot, 0);
		}
		TestEqual(TEXT("and is reported once, not once per band"), Bands.UnresolvedSlots, 1);
	}
	return true;
}

/**
 * THE COMPOSITION. Spawn the actor, place a runway through it, and the runway paint
 * component holds triangles while the mesh was skinned with a set carrying the runway
 * slots - the seam between presenter, builder and component measured where it is
 * consumed, not where it is declared.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayMarkingsDrawnTest,
	"Airside.Present.RunwayMarkingsDrawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayMarkingsDrawnTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to register components in"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	TestEqual(TEXT("no runway, no runway paint"), Actor->GetPresenter()->RunwayMarkingTriangleCountForTest(), 0);

	// The network is created on demand by the first node placed through the actor, and
	// PlaceRunway refuses a null network rather than creating one (see the facade). The
	// node this leaves behind is isolated and contributes no surface - the same device
	// Airside.Present.MeshIsFreshAfterLoad uses.
	Actor->PlaceNode(FVector2D(0.0, 60000.0));

	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	if (!TestTrue(TEXT("a runway is placed through the actor"),
		Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(150000.0, 0.0), Runway))) { return false; }

	TestTrue(TEXT("the runway paint component now holds triangles"), Actor->GetPresenter()->RunwayMarkingTriangleCountForTest() > 0);
	const URoadMaterialSet* Effective = Actor->GetPresenter()->EffectiveMaterialSetForTest();
	if (!TestNotNull(TEXT("the mesh was skinned with an effective set"), Effective)) { return false; }
	TestNotEqual(TEXT("which declares the tarmac slot"), Effective->IndexOf(URoadMaterialSet::RunwaySlotName(ERunwaySurface::Tarmac)), (int32)INDEX_NONE);
	TestNotEqual(TEXT("and the grass slot"), Effective->IndexOf(URoadMaterialSet::RunwaySlotName(ERunwaySurface::Grass)), (int32)INDEX_NONE);
	TestNotEqual(TEXT("and the concrete slot"), Effective->IndexOf(URoadMaterialSet::RunwaySlotName(ERunwaySurface::Concrete)), (int32)INDEX_NONE);
	TestTrue(TEXT("with the surface slot first, so a band's id 0 means what it always did"), Effective->Slots.Num() >= 4);
	return true;
}

#endif
