#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Algo/Reverse.h"
#include "Build/RoadMeshBuilder.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/PolygonInset.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPolygonInsetTest,
	"Airside.Solve.PolygonInset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPolygonInsetTest::RunTest(const FString& Parameters)
{
	// A 15 x 8 m plot - narrower than any depot the tool draws since the 20 m floor (owner ruling
	// 2026-09-30), and a plain rectangle is all the inset needs - inset by the 1 m hazard band.
	const TArray<FVector2D> Ccw = { FVector2D(0.0, 0.0), FVector2D(1500.0, 0.0),
	                                FVector2D(1500.0, 800.0), FVector2D(0.0, 800.0) };
	TArray<FVector2D> Inner;
	if (!TestTrue(TEXT("a 15 x 8 m plot takes a 1 m band"), PolygonInset::Inset(Ccw, 100.0, Inner)))
	{
		return false;
	}
	const TArray<FVector2D> Expected = { FVector2D(100.0, 100.0), FVector2D(1400.0, 100.0),
	                                     FVector2D(1400.0, 700.0), FVector2D(100.0, 700.0) };
	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("corner %d moves straight in by the band (%s)"), Index, *Inner[Index].ToString()),
			Inner[Index].Equals(Expected[Index], 1e-9));
	}

	// EITHER WINDING, one answer: inward is read off the area's sign, so a clockwise outline
	// must not inset OUTWARD - that would paint the band on the grass round the plot.
	TArray<FVector2D> Cw = Ccw;
	Algo::Reverse(Cw);
	TArray<FVector2D> CwInner;
	if (TestTrue(TEXT("a clockwise outline insets too"), PolygonInset::Inset(Cw, 100.0, CwInner)))
	{
		TestEqual(TEXT("to the same area"), FMath::Abs(PolygonInset::SignedArea(CwInner)), 1300.0 * 600.0, 1e-6);
		TestTrue(TEXT("and keeps its winding, so a ring pairs edge i with edge i"),
			PolygonInset::SignedArea(CwInner) < 0.0);
	}

	// REFUSED, NOT WRONG. A plot narrower than two bands would come back turned inside out.
	const TArray<FVector2D> Narrow = { FVector2D(0.0, 0.0), FVector2D(150.0, 0.0),
	                                   FVector2D(150.0, 800.0), FVector2D(0.0, 800.0) };
	TArray<FVector2D> NarrowInner;
	TestFalse(TEXT("a 1.5 m wide pad cannot take two 1 m bands"), PolygonInset::Inset(Narrow, 100.0, NarrowInner));
	TestEqual(TEXT("and a refusal leaves nothing behind to be drawn by accident"), NarrowInner.Num(), 0);
	TestFalse(TEXT("a zero distance is refused"), PolygonInset::Inset(Ccw, 0.0, NarrowInner));
	return true;
}

namespace
{
	/** A 15 x 8 m depot straight through the model. Named for this file: the tests module is a
	 *  unity build and PlotPresenterTest.cpp has its own PlaceDepot. */
	void PlacePaintTestDepot(ARoadNetworkActor* Actor, UEntityDefinition* Depot)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(750.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(1500.0, 0.0),
		                      FVector2D(1500.0, 800.0), FVector2D(0.0, 800.0) };
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Actor->Network->PlaceEntity(Placement);
	}

	/** Apron-layer area per paint tag, read off the RENDERED mesh - what the material sees. */
	TMap<int32, double> AreaByPaint(UDynamicMeshComponent& Component, int32& OutOddTags)
	{
		TMap<int32, double> Area;
		OutOddTags = 0;
		const UE::Geometry::FDynamicMesh3& Mesh = Component.GetDynamicMesh()->GetMeshRef();
		const UE::Geometry::FDynamicMeshUVOverlay* UV1 = Mesh.Attributes()->GetUVLayer(1);
		for (const int32 Triangle : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Elements = UV1->GetTriangle(Triangle);
			const float Tags[3] = { UV1->GetElement(Elements.A).X, UV1->GetElement(Elements.B).X,
			                        UV1->GetElement(Elements.C).X };
			const int32 Tag = FMath::RoundToInt32(Tags[0]);
			if (Tags[0] != static_cast<float>(Tag) || Tags[1] != Tags[0] || Tags[2] != Tags[0])
			{
				++OutOddTags;
			}
			const UE::Geometry::FIndex3i V = Mesh.GetTriangle(Triangle);
			const FVector2D A(Mesh.GetVertex(V.A));
			const FVector2D B(Mesh.GetVertex(V.B));
			const FVector2D C(Mesh.GetVertex(V.C));
			Area.FindOrAdd(Tag) += FMath::Abs(FVector2D::CrossProduct(B - A, C - A)) * 0.5;
		}
		return Area;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotPadIsPaintedTest,
	"Airside.Present.DepotPadIsPainted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotPadIsPaintedTest::RunTest(const FString& Parameters)
{
	// THE REPORTED DEFECT (2026-09-27): a depot's pad was the same concrete as every apron and
	// read, zoomed out, as bare ground. Option 1 paints it a pale slab inside a red hazard
	// band. Measured on the APRON COMPONENT the actor renders, so this goes red if
	// RebuildAprons stops tagging, not only if the builder does.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an apron component"), Actor != nullptr ? Actor->ApronComponent.Get() : nullptr)) { return false; }
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestTrue(TEXT("depot and stand definitions"), Depot != nullptr && Stand != nullptr)) { return false; }

	Actor->ClearNetwork();
	PlacePaintTestDepot(Actor, Depot);
	Actor->RebuildMesh();

	int32 OddTags = 0;
	TMap<int32, double> Area = AreaByPaint(*Actor->ApronComponent, OddTags);
	const double Slab = Area.FindRef(static_cast<int32>(FRoadMeshBuilder::EApronPaint::FuelSlab));
	const double Band = Area.FindRef(static_cast<int32>(FRoadMeshBuilder::EApronPaint::HazardBand));
	const double Width = FRoadMeshBuilder::HazardBandUu;

	TestEqual(TEXT("every triangle carries one whole tag - no vertex welded across two paints"), OddTags, 0);
	TestEqual(TEXT("no depot triangle is left plain concrete"),
		Area.FindRef(static_cast<int32>(FRoadMeshBuilder::EApronPaint::Concrete)), 0.0, 1e-6);
	TestEqual(TEXT("the slab is the plot inset by the band"), Slab, (1500.0 - 2.0 * Width) * (800.0 - 2.0 * Width), 1.0);
	TestEqual(TEXT("slab and band tile the plot exactly - no gap to the grass, no overlap to z-fight"),
		Slab + Band, 1500.0 * 800.0, 1.0);

	// A STAND BESIDE IT STAYS CONCRETE. A plotted stand's pad goes through the same loop, and
	// IsDepot - not IsPlotted - is what picks the paint (Check-Architecture rule 17's point).
	Actor->Network->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 20000.0), 0.0);
	Actor->RebuildMesh();
	Area = AreaByPaint(*Actor->ApronComponent, OddTags);
	TestTrue(TEXT("the stand's pad is concrete"),
		Area.FindRef(static_cast<int32>(FRoadMeshBuilder::EApronPaint::Concrete)) > 0.0);
	TestEqual(TEXT("and it added no slab"), Area.FindRef(static_cast<int32>(FRoadMeshBuilder::EApronPaint::FuelSlab)), Slab, 1.0);
	TestEqual(TEXT("and no band"), Area.FindRef(static_cast<int32>(FRoadMeshBuilder::EApronPaint::HazardBand)), Band, 1.0);
	return true;
}

#endif
