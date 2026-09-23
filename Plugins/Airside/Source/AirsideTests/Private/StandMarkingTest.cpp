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
	// verified by FStandMarkingPaintsOnePerStandTest's own count). EXACTLY TWO of them - its
	// far end - straddle the stop mark. Counted rather than read at fixed indices 2 and 3:
	// AddQuad swaps corners 1 and 3 whenever the handed-in order winds clockwise, so WHICH
	// slots hold the far end follows the sign of the across axis, not the geometry. The final
	// review's glyph-frame fix (I3) flipped that sign and moved the far end to slots 1 and 2
	// with the paint itself unchanged - a test pinned to slots measured the corner order.
	const double Tolerance = FStandMarkingBuilder::LeadInWidth * 0.5 + 0.01;
	int32 AtStopMark = 0;
	FString Distances;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FVector3d& V = Buffers.Positions[Index];
		const double Distance = FVector2D::Distance(FVector2D(V.X, V.Y), Instance->Position);
		Distances += FString::Printf(TEXT(" %.3f"), Distance);
		if (Distance <= Tolerance) { ++AtStopMark; }
	}
	TestEqual(FString::Printf(TEXT("two lead-in corners straddle the stop mark within %.3f (distances:%s)"),
		Tolerance, *Distances), AtStopMark, 2);

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
 * FINAL REVIEW I3: THE LETTER READS THE RIGHT WAY ROUND. The segment COUNT (GlyphSegments
 * above) cannot see a mirror - a reversed C has the same four strokes - so this measures
 * which SIDE the strokes land on, in the reader's own frame: Up = Facing (a pilot taxiing
 * in), Right = PerpCCW(Facing), the frame RunwayMarkingBuilder's FRunwayFrame derives for a
 * left-handed world. A "C" opens to the reader's right, so its mass sits LEFT of centre; a
 * "d"'s vertical stroke is on the right, so its mass sits RIGHT. Mirrored, both flip sign -
 * which is the whole defect: the painted "d" read as "b".
 *
 * Mean, not every vertex: C's top and bottom bars span the full width by design, so "every
 * vertex left of centre" is false for a correct C too. The mean still separates the two
 * cases by a whole stroke's width either way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingGlyphReadsUnmirroredTest,
	"Airside.Build.StandMarking.GlyphReadsUnmirrored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingGlyphReadsUnmirroredTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// Mean offset of the glyph's own vertices along the reader's right, from the glyph centre.
	// The builder paints lead-in (4 vertices) then stop bar (4) then the glyph, per stand - see
	// LeadInEndsAtStopMark above for the same reliance on emission order.
	auto MeanRightOf = [&](EIcaoCode Letter, double& OutMean) -> bool
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FEntityInstance* Instance = Net->GetEntity(PlaceDrawnStand(*Net, Stand, Letter, 0.0));
		if (Instance == nullptr) { return false; }
		FRoadMeshBuffers Buffers;
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers);
		constexpr int32 FirstGlyphVertex = 8;
		if (Buffers.Positions.Num() <= FirstGlyphVertex) { return false; }

		const FVector2D Facing(FMath::Cos(Instance->Heading), FMath::Sin(Instance->Heading));
		const FVector2D ReaderRight = RoadGeom::PerpCCW(Facing);
		// PlaceDrawnStand's own entrance edge: Y = 0 from X = 0 to the letter's floor width.
		const FVector2D EntranceMid(IcaoCode::StandWidthForLetter(Letter) * 0.5, 0.0);
		const FVector2D GlyphCentre = EntranceMid + Facing * FStandMarkingBuilder::GlyphInset;

		double Sum = 0.0;
		for (int32 Index = FirstGlyphVertex; Index < Buffers.Positions.Num(); ++Index)
		{
			const FVector3d& V = Buffers.Positions[Index];
			Sum += FVector2D::DotProduct(FVector2D(V.X, V.Y) - GlyphCentre, ReaderRight);
		}
		OutMean = Sum / (Buffers.Positions.Num() - FirstGlyphVertex);
		return true;
	};

	double MeanC = 0.0, MeanD = 0.0;
	if (!TestTrue(TEXT("a Code C glyph was painted to measure"), MeanRightOf(EIcaoCode::C, MeanC))) { return false; }
	if (!TestTrue(TEXT("a Code D glyph was painted to measure"), MeanRightOf(EIcaoCode::D, MeanD))) { return false; }

	TestTrue(FString::Printf(TEXT("C's strokes sit on the reader's LEFT - it opens to the right (mean %.1f uu)"), MeanC),
		MeanC < 0.0);
	TestTrue(FString::Printf(TEXT("d's upright sits on the reader's RIGHT - else it reads as b (mean %.1f uu)"), MeanD),
		MeanD > 0.0);
	return true;
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
