#include "CoreMinimal.h"
#include "Build/StandMarkingBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS - the tests module is a UNITY build; see StandPlotPlacementTest.cpp's
// own comment on why a same-named anonymous helper in two files compiles alone and collides
// once both land in one translation unit.
namespace StandMarkingTest
{
	/**
	 * Places a drawn (plotted) stand of Letter directly through URoadNetwork::PlaceEntity,
	 * bypassing URoadEditFacade::PlaceStandInPlot's afford/refusal gate - this file measures
	 * FStandMarkingBuilder, a Build/ class with no notion of a purse or a taxiway, so the
	 * facade's gate is not this test's concern. The pose and outline are built the same way
	 * the facade builds them (StandBox::PoseFor/BoxAt, DesignWingspan =
	 * IcaoCode::DesignSpanForLetter(Letter)), so a stand placed here reads back exactly as
	 * one PlaceStandInPlot would have committed.
	 *
	 * Entrance edge on Y = 0 running +X from X = EntranceX, dragged Inward = +Y.
	 */
	FEntityInstanceId PlaceDrawnStand(URoadNetwork& Net, UEntityDefinition* Definition, EIcaoCode Letter, double EntranceX)
	{
		const double Width = IcaoCode::StandWidthForLetter(Letter);
		const FVector2D A(EntranceX, 0.0);
		const FVector2D B(EntranceX + Width, 0.0);
		const FVector2D Inward(0.0, 1.0);
		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, Letter);

		FEntityPlacement Placement;
		Placement.Definition = Definition;
		Placement.Anchors = Definition->Anchors;
		Placement.Position = Pose.Position;
		Placement.Heading = RoadGeom::Bearing(Pose.Facing);
		Placement.PoseRole = Definition->PoseRole;
		StandBox::BoxAt(Pose, Letter, Placement.Outline);
		Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
		return Net.PlaceEntity(Placement);
	}

	/** Every triangle in Buffers faces up, measured the way every winding test in this
	 *  project does (memory: CCW faces DOWN in Unreal - assert on the engine's own normal,
	 *  never a 2D signed area). */
	bool AllTrianglesFaceUp(const FRoadMeshBuffers& Buffers, FAutomationTestBase& Test)
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Buffers.Positions) { Mesh.AppendVertex(Position); }
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			Mesh.AppendTriangle(Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);
		}
		UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
		int32 Downward = 0;
		for (const int32 VertexId : Mesh.VertexIndicesItr())
		{
			if (Mesh.GetVertexNormal(VertexId).Z <= 0.0f) { ++Downward; }
		}
		const int32 Triangles = Buffers.Indices.Num() / 3;
		Test.TestEqual(TEXT("no vertex normal points down - the paint is visible from above"), Downward, 0);
		Test.TestEqual(TEXT("and FDynamicMesh3 refused none of the triangles"), Mesh.TriangleCount(), Triangles);
		return Downward == 0 && Mesh.TriangleCount() == Triangles;
	}
}

/**
 * Two drawn stands and one depot: only the stands paint, one lead-in and one stop bar each -
 * the depot's IsStand() is false regardless of whether it ever gets an outline, so it must
 * not be counted (RoadEntity.h's own warning against reading IsPlotted() to mean "is a
 * stand").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingPaintsOnePerStandTest,
	"Airside.Build.StandMarking.PaintsOnePerStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingPaintsOnePerStandTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();

	if (!TestTrue(TEXT("a Code C stand placed"), PlaceDrawnStand(*Net, Stand, EIcaoCode::C, 0.0).IsSet())) { return false; }
	if (!TestTrue(TEXT("a Code E stand placed clear of the first"), PlaceDrawnStand(*Net, Stand, EIcaoCode::E, 20000.0).IsSet())) { return false; }
	Net->PlaceEntity(Depot, Depot->Anchors, FVector2D(-9000.0, -9000.0), 0.0, /*DesignWingspan=*/0.0,
		Depot->PoseRole, Depot->Trucks);

	FRoadMeshBuffers Buffers;
	FStandMarkingCensus Census;
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers, &Census);

	TestEqual(TEXT("two stands painted, the depot excluded"), Painted, 2);
	TestEqual(TEXT("one lead-in per stand"), Census.LeadIns, 2);
	TestEqual(TEXT("one stop bar per stand"), Census.StopBars, 2);

	return true;
}

/**
 * THE LETTER ON THE GROUND IS THE LETTER ADMISSION USES: Code C paints a,d,e,f (4 segments),
 * Code E paints a,d,e,f,g (5) - the same seven-segment shapes a calculator draws for those
 * hex digits, read off FStandMarkingBuilder::GlyphSegments by the enum's own index.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingGlyphSegmentsTest,
	"Airside.Build.StandMarking.GlyphSegments",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingGlyphSegmentsTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		PlaceDrawnStand(*Net, Stand, EIcaoCode::C, 0.0);
		FRoadMeshBuffers Buffers;
		FStandMarkingCensus Census;
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers, &Census);
		TestEqual(TEXT("Code C paints a, d, e, f - 4 segments"), Census.LetterSegments, 4);
	}
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		PlaceDrawnStand(*Net, Stand, EIcaoCode::E, 0.0);
		FRoadMeshBuffers Buffers;
		FStandMarkingCensus Census;
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers, &Census);
		TestEqual(TEXT("Code E paints a, d, e, f, g - 5 segments"), Census.LetterSegments, 5);
	}
	return true;
}

/**
 * A raw-model fixture stand - one placed with no drawn plot, whose Outline is therefore the
 * migration's own Code C box (URoadNetwork::GiveStandOutlineIfMissing) and whose
 * DesignWingspan is still 0, unknown - paints no letter, but still paints its lead-in and
 * stop bar. See FStandMarkingBuilder.h and the spec's revised Paint section for why this
 * case exists at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingUnknownWingspanTest,
	"Airside.Build.StandMarking.UnknownWingspanPaintsNoLetter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingUnknownWingspanTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	// The plain overload: Outline defaults empty and DesignWingspan defaults 0.0, exactly the
	// "nobody ever measured or drew this one" case - and PlaceEntity backfills a Code C box
	// for it immediately (GiveStandOutlineIfMissing), so IsPlotted() is true even though
	// nothing was drawn.
	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D::ZeroVector, 0.0);
	const FEntityInstance* Instance = Net->GetEntity(Placed);
	if (!TestNotNull(TEXT("the stand resolves"), Instance)) { return false; }
	if (!TestTrue(TEXT("plotted (backfilled) but wingspan unknown"),
		Instance->IsStand() && Instance->IsPlotted() && Instance->DesignWingspan <= 0.0)) { return false; }

	FRoadMeshBuffers Buffers;
	FStandMarkingCensus Census;
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers, &Census);

	TestEqual(TEXT("still painted"), Painted, 1);
	TestEqual(TEXT("lead-in still painted"), Census.LeadIns, 1);
	TestEqual(TEXT("stop bar still painted"), Census.StopBars, 1);
	TestEqual(TEXT("no letter - the wingspan that would choose one was never captured"), Census.LetterSegments, 0);

	return true;
}

/**
 * THE LEAD-IN ENDS AT THE STOP MARK: its far end straddles Entity.Position, within the
 * lead-in's own half-width - the pilot's nose, stopped on the mark, sits exactly where the
 * paint says to stop.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingLeadInEndsAtStopMarkTest,
	"Airside.Build.StandMarking.LeadInEndsAtStopMark",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingLeadInEndsAtStopMarkTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceDrawnStand(*Net, Stand, EIcaoCode::C, 0.0);
	const FEntityInstance* Instance = Net->GetEntity(Placed);
	if (!TestNotNull(TEXT("the stand resolves"), Instance)) { return false; }

	FRoadMeshBuffers Buffers;
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers);
	if (!TestEqual(TEXT("one stand painted"), Painted, 1)) { return false; }

	// The lead-in is the FIRST quad MarkingQuads::AddQuad emits (4 consecutive vertices,
	// verified by FStandMarkingPaintsOnePerStandTest's own count) - its last two vertices are
	// indices 2 and 3.
	const FVector3d& V2 = Buffers.Positions[2];
	const FVector3d& V3 = Buffers.Positions[3];
	const double Tolerance = FStandMarkingBuilder::LeadInWidth * 0.5 + 0.01;
	const double Distance2 = FVector2D::Distance(FVector2D(V2.X, V2.Y), Instance->Position);
	const double Distance3 = FVector2D::Distance(FVector2D(V3.X, V3.Y), Instance->Position);

	TestTrue(FString::Printf(TEXT("vertex 2 straddles the stop mark (%.3f <= %.3f)"), Distance2, Tolerance),
		Distance2 <= Tolerance);
	TestTrue(FString::Printf(TEXT("vertex 3 straddles the stop mark (%.3f <= %.3f)"), Distance3, Tolerance),
		Distance3 <= Tolerance);

	return true;
}

/** Every marking quad this builder emits faces up, measured the way every winding test here
 *  is (engine-computed normal, never a 2D signed area). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingQuadsFaceUpTest,
	"Airside.Build.StandMarking.QuadsFaceUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingQuadsFaceUpTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	PlaceDrawnStand(*Net, Stand, EIcaoCode::C, 0.0);
	PlaceDrawnStand(*Net, Stand, EIcaoCode::E, 20000.0);

	FRoadMeshBuffers Buffers;
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers);
	if (!TestEqual(TEXT("two stands painted"), Painted, 2)) { return false; }
	if (!TestTrue(TEXT("some geometry to measure"), Buffers.Indices.Num() > 0)) { return false; }

	return AllTrianglesFaceUp(Buffers, *this);
}

/**
 * THE COMPOSITION QUESTION (controller note): does placing a stand rebuild markings with no
 * other edit in between, the same as FHoldingPositionMeshFollowsToggleTest proves for
 * SetIntermediateHoldingPosition (issue #179)? PlaceStandInPlot's own commit already runs
 * through URoadEditFacade::CommitPurchase -> CommitAndNotify -> NotifyChanged(Topology) ->
 * ARoadNetworkActor::RebuildMeshForChange(Topology) -> URoadSurfacePresenter::Rebuild, which
 * calls RebuildMarkings unconditionally - so this is a proof the WIRING (this task's own
 * hook into RebuildMarkings) actually sits on that path, not a claim that the path itself
 * needed building.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingPaintsAfterPlacementTest,
	"Airside.Present.StandMarking.PaintsAfterPlacement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingPaintsAfterPlacementTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);

	const int32 Before = Actor->GetPresenter()->HoldingPaintTriangleCountForTest();

	const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	const FVector2D A(0.0, 1000.0);
	const FVector2D B(Width, 1000.0);
	const TArray<FVector2D> Rect = { A, B, FVector2D(Width, 1000.0 + Depth), FVector2D(0.0, 1000.0 + Depth) };
	const int32 Placed = Target->PlaceStandInPlot(Rect, A, B);
	if (!TestTrue(TEXT("the stand is placed"), Placed != INDEX_NONE)) { return false; }

	const int32 After = Actor->GetPresenter()->HoldingPaintTriangleCountForTest();
	TestTrue(TEXT("placing a stand paints it with no other edit in between - the same wiring "
		"issue #179 proved for the holding-position toggle"), After > Before);

	return true;
}

#endif
