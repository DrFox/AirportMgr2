#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/FenceLayout.h"
#include "Solve/PlotYard.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The 20 x 24 m depot, gate mid-frontage. */
	const TArray<FVector2D>& FenceTestOutline()
	{
		static const TArray<FVector2D> Outline = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0),
			FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
		return Outline;
	}

	void PlaceFenceTestDepot(ARoadNetworkActor* Road)
	{
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(1000.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = FenceTestOutline();
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Road->Network->PlaceEntity(Placement);
	}

	/** The layout the presenter is meant to have drawn - the same Solve, the same spec. */
	FenceLayout::FLayout FenceTestExpected()
	{
		FenceLayout::FSpec Spec;
		Spec.GateWidthUu = PlotYard::GateCorridorUu;
		return FenceLayout::Solve(FenceTestOutline(), FVector2D(1000.0, 0.0), Spec);
	}
}

/**
 * A placed depot's fence reaches the components: every post an instance of the right mesh,
 * every bay two triangles of fabric.
 *
 * COMPOSITION LEVEL, per CLAUDE.md: every Airside.Solve.FenceLayout test passes against a
 * presenter that never calls Solve, or calls it and draws into a component nobody renders.
 * This counts what the COMPONENTS hold, against the layout recomputed here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFenceReachesTheComponentsTest,
	"Airside.Present.PlotFenceReachesTheComponents",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFenceReachesTheComponentsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	if (!TestNotNull(TEXT("a buildings actor"), TestWorld.Buildings)) { return false; }
	ARoadNetworkActor* Road = TestWorld.Actor;
	Road->ClearNetwork();
	PlaceFenceTestDepot(Road);
	Road->RebuildMesh();

	const FenceLayout::FLayout Expected = FenceTestExpected();
	const int32 Heavy = Expected.CountOf(FenceLayout::EPostKind::Corner)
		+ Expected.CountOf(FenceLayout::EPostKind::Gate);

	TestEqual(TEXT("every line post is an instance of the line-post mesh"),
		TestWorld.Buildings->GetFencePostsForTest()->GetInstanceCount(),
		Expected.CountOf(FenceLayout::EPostKind::Line));
	TestEqual(TEXT("every corner and gate post is an instance of the heavy post"),
		TestWorld.Buildings->GetFenceHeavyPostsForTest()->GetInstanceCount(), Heavy);

	UDynamicMeshComponent* Fabric = TestWorld.Buildings->GetFenceFabricForTest();
	TestEqual(TEXT("every bay is two triangles of fabric"),
		Fabric->GetDynamicMesh()->GetMeshRef().TriangleCount(), 2 * Expected.Spans.Num());

	TestEqual(TEXT("the presenter's count agrees"),
		TestWorld.Buildings->GetPlotPresenter()->GetFencePostCount(),
		Expected.CountOf(FenceLayout::EPostKind::Line) + Heavy);
	TestEqual(TEXT("and the depot has its gate"),
		TestWorld.Buildings->GetPlotPresenter()->GetGateGapCount(), 1);

	// IDEMPOTENT: RebuildMesh runs on every graph change, and an appending fence would double
	// on every road the player drew anywhere on the airport.
	Road->RebuildMesh();
	TestEqual(TEXT("a rebuild does not double the posts"),
		TestWorld.Buildings->GetFencePostsForTest()->GetInstanceCount(),
		Expected.CountOf(FenceLayout::EPostKind::Line));
	TestEqual(TEXT("nor the fabric"),
		Fabric->GetDynamicMesh()->GetMeshRef().TriangleCount(), 2 * Expected.Spans.Num());
	return true;
}

/**
 * The fabric faces OUT and is textured by distance along the edge.
 *
 * ENGINE-COMPUTED NORMALS, per memory: Unreal's winding is left-handed and a hand-derived
 * cross product has agreed with itself while disagreeing with the rasteriser before. The
 * material is two-sided so either winding draws; facing out is what makes the lighting right.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFenceFabricFacesOutTest,
	"Airside.Present.PlotFenceFabricFacesOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFenceFabricFacesOutTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	if (!TestNotNull(TEXT("a buildings actor"), TestWorld.Buildings)) { return false; }
	TestWorld.Actor->ClearNetwork();
	PlaceFenceTestDepot(TestWorld.Actor);
	TestWorld.Actor->RebuildMesh();

	const UE::Geometry::FDynamicMesh3& Mesh =
		TestWorld.Buildings->GetFenceFabricForTest()->GetDynamicMesh()->GetMeshRef();
	if (!TestTrue(TEXT("there is fabric"), Mesh.TriangleCount() > 0)) { return false; }

	int32 South = 0;
	double MaxZ = 0.0;
	for (const int32 Tri : Mesh.TriangleIndicesItr())
	{
		FVector3d A, B, C;
		Mesh.GetTriVertices(Tri, A, B, C);
		MaxZ = FMath::Max(MaxZ, FMath::Max(A.Z, FMath::Max(B.Z, C.Z)));
		const FVector3d Centroid = (A + B + C) / 3.0;
		if (Centroid.Y < 0.0)
		{
			++South;
			TestTrue(TEXT("the frontage fabric faces south, out of the plot"),
				Mesh.GetTriNormal(Tri).Y < -0.99);
		}
	}
	TestTrue(TEXT("the frontage has fabric"), South > 0);
	TestEqual(TEXT("fabric is 2.4 m tall - the texture's V range"), MaxZ, 240.0, 1e-6);

	// U BY DISTANCE: the longest run of U on the 20 m frontage ends at 2000 / 240.
	const UE::Geometry::FDynamicMeshUVOverlay* UV = Mesh.Attributes()->GetUVLayer(0);
	float MaxU = 0.0f;
	for (const int32 Element : UV->ElementIndicesItr())
	{
		MaxU = FMath::Max(MaxU, UV->GetElement(Element).X);
	}
	TestEqual(TEXT("U is distance along the edge over the 2.4 m tile"),
		static_cast<double>(MaxU), 2400.0 / 240.0, 1e-4);
	return true;
}

#endif
