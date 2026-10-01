#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Algo/Reverse.h"
#include "Build/AnchorLink.h"
#include "Build/HoldingPositionMarkingBuilder.h"
#include "Build/StandMarkingBuilder.h"
#include "Build/StandTurnOffMarkingBuilder.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/TaxiwayStrip.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"
#include "Testing/AirsideTestWorld.h"
#include "Testing/AirsideTestGraph.h"
#include "Tool/RoadEditTarget.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Model/RoadGuideline.h"
#include "Solve/GuidelineGeom.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Present/RoadSurfacePresenter.h"
#include "Profiles/RoadMaterialSet.h"

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
		// THE FLOOR (#292): this file's own tests place stands with no content set configured
		// (every automation test still is that way), so the floor IS the resolved envelope
		// here - see FStandMarkingBuilder's own header for why the two must agree.
		const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, {}, Letter, Envelope);

		FEntityPlacement Placement;
		Placement.Definition = Definition;
		Placement.Anchors = Definition->Anchors;
		Placement.Position = Pose.Position;
		Placement.Heading = RoadGeom::Bearing(Pose.Facing);
		Placement.PoseRole = Definition->PoseRole;
		StandBox::BoxAt(Pose, Letter, Envelope, Placement.Outline);
		Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
		return Net.PlaceEntity(Placement);
	}

	/**
	 * A drawn stand on ANY rectangle: entrance edge from Origin along Bearing (radians), Width
	 * long, dragged Depth inward (PerpCCW of the entrance direction - PlaceDrawnStand's own
	 * sense). Pose from StandBox::PoseFor at the floor envelope, outline the rectangle itself
	 * rather than BoxAt's floor box, so a larger-than-floor stand keeps its drawn size.
	 */
	const FEntityInstance* PlaceDrawnRect(URoadNetwork& Net, UEntityDefinition* Definition, EIcaoCode Letter,
		const FVector2D& Origin, double Bearing, double Width, double Depth)
	{
		const FVector2D Along(FMath::Cos(Bearing), FMath::Sin(Bearing));
		const FVector2D Inward = RoadGeom::PerpCCW(Along);
		const FVector2D A = Origin;
		const FVector2D B = Origin + Along * Width;
		const StandBox::FStandPose Pose = StandBox::PoseFor(A, B, Inward, {}, Letter, IcaoCode::FloorEnvelopeForLetter(Letter));
		FEntityPlacement Placement;
		Placement.Definition = Definition;
		Placement.Anchors = Definition->Anchors;
		Placement.Position = Pose.Position;
		Placement.Heading = RoadGeom::Bearing(Pose.Facing);
		Placement.PoseRole = Definition->PoseRole;
		Placement.Outline = { A, B, B + Inward * Depth, A + Inward * Depth };
		Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
		return Net.GetEntity(Net.PlaceEntity(Placement));
	}

	/**
	 * The stands the paint-bounds tests measure, one per Case: the floor box of four letters, a B
	 * rotated to 135 degrees (no axis to hide behind), and a C drawn 12 m wider and 15 m deeper
	 * than its floor, so the hatch has to follow the DRAWN edge, not the letter's. Each case sits
	 * 40 m from the last, so no box reaches a neighbour.
	 */
	constexpr int32 PaintBoundsCases = 6;
	const FEntityInstance* PlacePaintBoundsCase(URoadNetwork& Net, int32 Case, FString& OutName)
	{
		const FVector2D Origin(Case * 40000.0, 0.0);
		if (Case < 4)
		{
			const EIcaoCode Floors[4] = { EIcaoCode::B, EIcaoCode::C, EIcaoCode::E, EIcaoCode::F };
			const EIcaoCode Letter = Floors[Case];
			OutName = FString::Printf(TEXT("Code %s floor"), IcaoCode::ToLetter(Letter));
			return PlaceDrawnRect(Net, UEntityDefinition::MakeStandTransient(Letter), Letter, Origin, 0.0,
				IcaoCode::StandWidthForLetter(Letter), IcaoCode::StandDepthForLetter(Letter));
		}
		if (Case == 4)
		{
			OutName = TEXT("Code B at 135 degrees");
			return PlaceDrawnRect(Net, UEntityDefinition::MakeStandTransient(EIcaoCode::B), EIcaoCode::B, Origin,
				FMath::DegreesToRadians(135.0), IcaoCode::StandWidthForLetter(EIcaoCode::B), IcaoCode::StandDepthForLetter(EIcaoCode::B));
		}
		OutName = TEXT("Code C larger than its floor");
		return PlaceDrawnRect(Net, UEntityDefinition::MakeStandTransient(EIcaoCode::C), EIcaoCode::C, Origin, 0.0,
			IcaoCode::StandWidthForLetter(EIcaoCode::C) + 1200.0, IcaoCode::StandDepthForLetter(EIcaoCode::C) + 1500.0);
	}

	/** Every EStandPaint on its own id (its index), so a test can tell the paints apart. */
	FStandPaintIds DistinctIds()
	{
		FStandPaintIds Ids;
		for (int32 Paint = 0; Paint < static_cast<int32>(EStandPaint::Count); ++Paint)
		{
			Ids.Ids[Paint] = Paint;
		}
		return Ids;
	}

	/** P inside (or within Tolerance of) the convex Outline, either winding. */
	bool InConvex(const TArray<FVector2D>& Outline, const FVector2D& P, double Tolerance)
	{
		double Twice = 0.0;
		for (int32 I = 0; I < Outline.Num(); ++I)
		{
			Twice += FVector2D::CrossProduct(Outline[I], Outline[(I + 1) % Outline.Num()]);
		}
		const double Sign = Twice >= 0.0 ? 1.0 : -1.0;
		for (int32 I = 0; I < Outline.Num(); ++I)
		{
			const FVector2D Edge = Outline[(I + 1) % Outline.Num()] - Outline[I];
			if (Sign * FVector2D::CrossProduct(Edge, P - Outline[I]) / Edge.Size() < -Tolerance) { return false; }
		}
		return true;
	}

	/**
	 * A real B stand through the facade and the real rebuild, so FStandLayoutBuild has laid its
	 * service layout - Airside.Build.StandPadSlots' own fixture shape. Null on failure.
	 */
	const FEntityInstance* PlaceRealBStand(FAutomationTestBase& Test, FAirsideTestWorld& World, FEntityInstanceId& OutId)
	{
		ARoadNetworkActor* Actor = World.Actor;
		if (!Test.TestNotNull(TEXT("actor constructed"), Actor)) { return nullptr; }
		Actor->ClearNetwork();
		IRoadEditTarget* Target = Actor;
		const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
		if (!Test.TestTrue(TEXT("a B stand is placed"), Target->PlaceStandInPlot(Pad, Pad[0], Pad[1], EPavement::Tarmac) != INDEX_NONE)) { return nullptr; }
		Actor->RebuildMesh();
		const URoadNetwork& Net = *Actor->Network;
		for (int32 Index = 0; Index < Net.GetEntities().Num(); ++Index)
		{
			if (Net.GetEntities()[Index].bAlive && Net.GetEntities()[Index].IsStand())
			{
				OutId = Net.EntityIdAt(Index);
				return &Net.GetEntities()[Index];
			}
		}
		Test.AddError(TEXT("no stand in the network after placement"));
		return nullptr;
	}

	/**
	 * The nodes of the stand's SERVICE POINTS - the anchors its definition lays a bay for - read
	 * from the layout's own data (UEntityDefinition::ServiceBays' AnchorId -> the instance's
	 * resolved anchor), never from where they happen to be.
	 */
	TSet<FGuidelineNodeId> ServicePointNodes(const FEntityInstance& Stand)
	{
		TSet<FGuidelineNodeId> Out;
		if (Stand.Definition == nullptr) { return Out; }
		for (const FServiceBay& Bay : Stand.Definition->ServiceBays)
		{
			for (const FResolvedAnchor& Anchor : Stand.ResolvedAnchors)
			{
				if (Anchor.Id == Bay.AnchorId) { Out.Add(Anchor.Node); }
			}
		}
		return Out;
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
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census);

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
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census);
		TestEqual(TEXT("Code C paints a, d, e, f - 4 segments"), Census.LetterSegments, 4);
	}
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		PlaceDrawnStand(*Net, Stand, EIcaoCode::E, 0.0);
		FRoadMeshBuffers Buffers;
		FStandMarkingCensus Census;
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census);
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
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census);

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
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor());
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
	const int32 Painted = FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor());
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
		FStandMarkingCensus Census;
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census);
		constexpr int32 FirstGlyphVertex = 8;
		// BOUNDED TO THE GLYPH'S OWN QUADS - four vertices per segment, read off the census -
		// since task 13 painted the boundary, restraint line and hatch AFTER the letter. Reading
		// to the end of the buffer would average a whole stand's symmetric paint into the mean.
		const int32 EndGlyphVertex = FirstGlyphVertex + Census.LetterSegments * 4;
		if (Census.LetterSegments == 0 || Buffers.Positions.Num() < EndGlyphVertex) { return false; }

		const FVector2D Facing(FMath::Cos(Instance->Heading), FMath::Sin(Instance->Heading));
		const FVector2D ReaderRight = RoadGeom::PerpCCW(Facing);
		// PlaceDrawnStand's own entrance edge: Y = 0 from X = 0 to the letter's floor width.
		const FVector2D EntranceMid(IcaoCode::StandWidthForLetter(Letter) * 0.5, 0.0);
		const FVector2D GlyphCentre = EntranceMid + Facing * FStandMarkingBuilder::GlyphInset;

		double Sum = 0.0;
		for (int32 Index = FirstGlyphVertex; Index < EndGlyphVertex; ++Index)
		{
			const FVector3d& V = Buffers.Positions[Index];
			Sum += FVector2D::DotProduct(FVector2D(V.X, V.Y) - GlyphCentre, ReaderRight);
		}
		OutMean = Sum / (EndGlyphVertex - FirstGlyphVertex);
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
	// BEHIND THE TAXIWAY'S CLEARANCE STRIP (2026-09-28), where the stand tool puts an entrance -
	// a bare 1000.0 sat over the pavement's own edge and is refused now.
	const FRoadSegmentId Taxi = Actor->Network->SegmentIdAt(0);
	const double EntranceY = Actor->Network->GetSegment(Taxi)->Profile->GetHalfWidthLeft()
		+ TaxiwayStrip::StripWidthOf(*Actor->Network, Taxi);
	const FVector2D A(0.0, EntranceY);
	const FVector2D B(Width, EntranceY);
	const TArray<FVector2D> Rect = { A, B, FVector2D(Width, EntranceY + Depth), FVector2D(0.0, EntranceY + Depth) };
	const int32 Placed = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
	if (!TestTrue(TEXT("the stand is placed"), Placed != INDEX_NONE)) { return false; }

	const int32 After = Actor->GetPresenter()->HoldingPaintTriangleCountForTest();
	TestTrue(TEXT("placing a stand paints it with no other edit in between - the same wiring "
		"issue #179 proved for the holding-position toggle"), After > Before);

	return true;
}

/**
 * THE PAINT CARRIES ITS MEANING, NOT A COLOUR: every triangle the builder emits takes the
 * material id the caller mapped its EStandPaint to. Ids chosen here to be distinct and non-zero,
 * so a quad that ignored the table (id 0, AddQuad's default) cannot pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingPaintCarriesItsMeaningIdTest,
	"Airside.Build.StandMarking.PaintCarriesItsMeaningId",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingPaintCarriesItsMeaningIdTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	PlaceDrawnStand(*Net, UEntityDefinition::MakeStandTransient(), EIcaoCode::C, 0.0);

	FStandPaintIds Ids;
	for (int32 Paint = 0; Paint < static_cast<int32>(EStandPaint::Count); ++Paint)
	{
		Ids.Ids[Paint] = 10 + Paint;
	}
	FRoadMeshBuffers Buffers;
	FStandMarkingCensus Census;
	FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census, Ids);
	if (!TestTrue(TEXT("something painted"), Buffers.MaterialIDs.Num() > 0)) { return false; }

	TSet<int32> Seen(Buffers.MaterialIDs);
	for (const int32 Id : Seen)
	{
		TestTrue(FString::Printf(TEXT("id %d is one the table handed in"), Id), Id >= 10 && Id < 10 + static_cast<int32>(EStandPaint::Count));
	}
	TestTrue(TEXT("the lead-in, stop bar and letter are Guidance"), Seen.Contains(Ids[EStandPaint::Guidance]));
	return true;
}

/**
 * EVERY STAND PAINT RESOLVES ON THE BUILT COMPONENT (memory: an unresolved slot renders the
 * floor checker, silently). Modelled on Airside.Build.StandPadSlots: a real B stand through the
 * facade, the real rebuild, then the HoldingPaint component read back - each EStandPaint's slot
 * is declared, its id holds a material that is not the engine default, Guidance is slot 0 (the
 * road material the holding bars always had), and the two paint colours are MIDs of that
 * material with MarkingColor white and red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingPaintSlotsResolveTest,
	"Airside.Present.StandMarking.PaintSlotsResolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingPaintSlotsResolveTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	IRoadEditTarget* Target = Actor;
	const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	if (!TestTrue(TEXT("a B stand is placed"), Target->PlaceStandInPlot(Pad, Pad[0], Pad[1], EPavement::Tarmac) != INDEX_NONE)) { return false; }
	Actor->RebuildMesh();

	URoadSurfacePresenter* Presenter = Actor->GetPresenter();
	UDynamicMeshComponent* Paint = Presenter != nullptr ? Presenter->GetLayerComponentForTest(ESurfaceLayer::HoldingPaint) : nullptr;
	const URoadMaterialSet* Set = Presenter != nullptr ? Presenter->MarkingMaterialSetForTest() : nullptr;
	if (!TestNotNull(TEXT("a paint component"), Paint) || !TestNotNull(TEXT("a marking material set"), Set)) { return false; }
	TestEqual(TEXT("the component holds one material per declared slot - else the proxy draws everything as slot 0"),
		Paint->GetNumMaterials(), Set->Slots.Num());

	UMaterialInterface* Floor = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInterface* Road = Paint->GetMaterial(0);
	auto ColourAt = [&](int32 Id, FLinearColor& Out) -> bool
	{
		const UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Paint->GetMaterial(Id));
		return MID != nullptr && MID->Parent == Road
			&& MID->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("MarkingColor")), Out);
	};

	for (int32 Index = 0; Index < static_cast<int32>(EStandPaint::Count); ++Index)
	{
		const EStandPaint P = static_cast<EStandPaint>(Index);
		const FName SlotName = URoadSurfacePresenter::StandPaintSlot(P);
		const int32 Id = Set->IndexOf(SlotName);
		if (!TestTrue(FString::Printf(TEXT("paint %d's slot %s is declared"), Index, *SlotName.ToString()), Id != INDEX_NONE)) { continue; }
		UMaterialInterface* Mat = Paint->GetMaterial(Id);
		TestTrue(FString::Printf(TEXT("paint %d draws with a real material, not the floor checker"), Index), Mat != nullptr && Mat != Floor);

		FLinearColor Colour;
		switch (P)
		{
		case EStandPaint::Guidance:
			TestEqual(TEXT("Guidance is slot 0, the road material the holding bars always had"), Id, 0);
			break;
		case EStandPaint::Boundary:
			if (TestTrue(FString::Printf(TEXT("paint %d is a MarkingColor MID of the road material"), Index), ColourAt(Id, Colour)))
			{
				TestTrue(FString::Printf(TEXT("paint %d is white"), Index), Colour.Equals(FLinearColor::White));
			}
			break;
		case EStandPaint::SignBackground:
			// THE SIGN BOX (user 2026-09-29): near-black, so the yellow digits read against it.
			if (TestTrue(FString::Printf(TEXT("paint %d is a MarkingColor MID of the road material"), Index), ColourAt(Id, Colour)))
			{
				TestTrue(FString::Printf(TEXT("paint %d is near-black"), Index), Colour.GetLuminance() < 0.05f);
			}
			break;
		default:
			AddError(TEXT("an EStandPaint with no expectation here - add one"));
		}
	}

	// Every triangle's id is one the component can draw: an id >= NumMaterials is dropped by
	// the proxy with nothing logged.
	const UE::Geometry::FDynamicMesh3& Mesh = Paint->GetDynamicMesh()->GetMeshRef();
	const UE::Geometry::FDynamicMeshMaterialAttribute* MeshIds = Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;
	if (!TestNotNull(TEXT("the paint mesh carries material ids"), MeshIds)) { return false; }
	TSet<int32> Seen;
	for (const int32 Tri : Mesh.TriangleIndicesItr()) { Seen.Add(MeshIds->GetValue(Tri)); }
	for (const int32 Id : Seen)
	{
		TestTrue(FString::Printf(TEXT("mesh id %d has a material"), Id), Id >= 0 && Id < Paint->GetNumMaterials());
	}
	// And the stand's paint actually USES every slot - yellow guidance, white boundary - so a
	// builder that dropped every quad onto slot 0 cannot pass on the set alone.
	for (const EStandPaint P : { EStandPaint::Guidance, EStandPaint::Boundary })
	{
		TestTrue(FString::Printf(TEXT("the painted mesh carries paint %d's slot"), static_cast<int32>(P)),
			Seen.Contains(Set->IndexOf(URoadSurfacePresenter::StandPaintSlot(P))));
	}
	return true;
}

/**
 * EVERY PAINTED VERTEX LIES ON THE STAND (task 13): boundary and guidance all inside the drawn
 * outline, 1 uu of tolerance - paint that ran off the pad would lie on the
 * taxiway or a neighbour's ground. Three letters, so a figure right for one width is not enough.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandMarkingPaintStaysInsideOutlineTest,
	"Airside.Build.StandMarking.PaintStaysInsideOutline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandMarkingPaintStaysInsideOutlineTest::RunTest(const FString& Parameters)
{
	using namespace StandMarkingTest;
	// ONE STAND PER NETWORK, so the buffers measured are that stand's paint alone.
	TArray<FString> Names;
	Names.SetNum(PaintBoundsCases);
	for (int32 Case = 0; Case < PaintBoundsCases; ++Case)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FEntityInstance* Stand = PlacePaintBoundsCase(*Net, Case, Names[Case]);
		if (!TestNotNull(*FString::Printf(TEXT("%s resolves"), *Names[Case]), Stand)) { continue; }
		FRoadMeshBuffers Buffers;
		FStandMarkingCensus Census;
		FStandMarkingBuilder::Build(*Net, 10.0, Buffers, FLetterEnvelopeTable::Floor(), &Census, DistinctIds());
		TestTrue(FString::Printf(TEXT("%s painted a boundary edge per outline edge, a lead-in and a stop bar (%d, %d, %d)"), *Names[Case],
			Census.BoundaryEdges, Census.LeadIns, Census.StopBars),
			Census.BoundaryEdges == Stand->Outline.Num() && Census.LeadIns == 1 && Census.StopBars == 1);
		int32 Outside = 0;
		for (const FVector3d& V : Buffers.Positions)
		{
			Outside += InConvex(Stand->Outline, FVector2D(V.X, V.Y), 1.0) ? 0 : 1;
		}
		TestEqual(FString::Printf(TEXT("%s: no painted vertex off the stand"), *Names[Case]), Outside, 0);
	}
	return true;
}

/**
 * THE STOP LINE IS WHITE ON THE BUILT COMPONENT, not only in the builder's buffers: the seam is
 * URoadSurfacePresenter::RebuildMarkings resolving the white slot and handing its id to
 * FHoldingPositionMarkingBuilder (taxiway strip stage 4). A derived crossing and nothing else -
 * no stand, no runway - so every painted triangle on the holding-paint layer is a stop line.
 * Lives beside PaintSlotsResolve because it reads the same component the same way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossingStopLineIsWhiteTest,
	"Airside.Present.CrossingStopLineIsWhite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCrossingStopLineIsWhiteTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	FRoadCrossingFixture::Lay(*Actor->Network);
	Actor->RebuildMesh();

	URoadSurfacePresenter* Presenter = Actor->GetPresenter();
	UDynamicMeshComponent* Paint = Presenter != nullptr ? Presenter->GetLayerComponentForTest(ESurfaceLayer::HoldingPaint) : nullptr;
	const URoadMaterialSet* Set = Presenter != nullptr ? Presenter->MarkingMaterialSetForTest() : nullptr;
	if (!TestNotNull(TEXT("a paint component"), Paint) || !TestNotNull(TEXT("a marking material set"), Set)) { return false; }

	const int32 White = Set->IndexOf(URoadSurfacePresenter::StandPaintSlot(EStandPaint::Boundary));
	if (!TestTrue(TEXT("the layer declares its white slot"), White > 0)) { return false; }
	const UE::Geometry::FDynamicMesh3& Mesh = Paint->GetDynamicMesh()->GetMeshRef();
	const UE::Geometry::FDynamicMeshMaterialAttribute* MeshIds = Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;
	if (!TestNotNull(TEXT("the paint mesh carries material ids"), MeshIds)) { return false; }
	int32 Triangles = 0, OnWhite = 0;
	for (const int32 Tri : Mesh.TriangleIndicesItr())
	{
		++Triangles;
		OnWhite += MeshIds->GetValue(Tri) == White ? 1 : 0;
	}
	TestEqual(TEXT("two stop bars painted - one quad per road arm"), Triangles, 2 * 2);
	TestEqual(TEXT("every one of them on the white slot, not the yellow the aircraft holds use"), OnWhite, Triangles);
	return true;
}

// ---------------------------------------------------------------------------------------------
// THE TURN-OFF (taxiway strip stage 5): the yellow lead-in painted on the taxiway pavement where
// a stand's lead-in leaves the centreline, with an arrow and the stand's number - and a gap
// across the strip, which falls out of "paint only where paved" (BHX, user 2026-09-28).
// ---------------------------------------------------------------------------------------------

namespace StandTurnOffTest
{
	/** A C stand drawn through the facade behind the strip of a straight taxiway along Y = 0 -
	 *  where the stand tool itself puts one - and the real Topology rebuild placement runs. */
	bool PlaceBesideTaxiway(FAutomationTestBase& Test, ARoadNetworkActor* Actor, int32& OutIndex, double& OutHalfWidth, double& OutStrip)
	{
		Actor->ClearNetwork();
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
		const FRoadSegmentId Taxi = Actor->Network->SegmentIdAt(0);
		OutHalfWidth = Actor->Network->GetSegment(Taxi)->Profile->GetMaxHalfWidth();
		OutStrip = TaxiwayStrip::StripWidthOf(*Actor->Network, Taxi);
		const double EntranceY = Actor->Network->GetSegment(Taxi)->Profile->GetHalfWidthLeft() + OutStrip;
		const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
		const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
		const FVector2D A(-Width * 0.5, EntranceY);
		const FVector2D B(Width * 0.5, EntranceY);
		const TArray<FVector2D> Rect = { A, B, FVector2D(B.X, EntranceY + Depth), FVector2D(A.X, EntranceY + Depth) };
		OutIndex = Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac);
		return Test.TestTrue(TEXT("a C stand is placed behind the strip"), OutIndex != INDEX_NONE);
	}

	/** A bare network with a real taxiway SEGMENT and its guideline laid by hand, DerivedFrom
	 *  set as the builder would - so a lead-in can be cast at exact geometry, no balloon, no
	 *  junction. Returns the segment. */
	FRoadSegmentId LayRawTaxiway(URoadNetwork& Net, double Y, double WestX, double EastX, bool bReverseGuideline = false)
	{
		const FRoadNodeId A = Net.AddNode(FVector2D(WestX, Y));
		const FRoadNodeId B = Net.AddNode(FVector2D(EastX, Y));
		const FRoadSegmentId Seg = Net.AddStraightSegment(A, B, TestProfiles::Taxiway());
		FGuidelineEdge Edge;
		// bReverseGuideline: the guideline runs East -> West against its West -> East segment,
		// which changes which sweep FAnchorLink::Join lays first and nothing else.
		Edge.A = Net.AddGuidelineNode(FVector2D(bReverseGuideline ? EastX : WestX, Y));
		Edge.B = Net.AddGuidelineNode(FVector2D(bReverseGuideline ? WestX : EastX, Y));
		Edge.Control = FVector2D((WestX + EastX) * 0.5, Y);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 2300.0;
		Edge.DerivedFrom = Seg;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
		return Seg;
	}

	/** Least distance from P to the polyline Points. */
	double ToPolyline(const TArray<FVector2D>& Points, const FVector2D& P)
	{
		double Best = DBL_MAX;
		for (int32 I = 0; I + 1 < Points.Num(); ++I)
		{
			const double T = RoadGeom::ClosestPointOnSegment(Points[I], Points[I + 1], P);
			Best = FMath::Min(Best, FVector2D::Distance(P, Points[I] + (Points[I + 1] - Points[I]) * T));
		}
		return Best;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffPaintsOneNumberPerTurnOffTest,
	"Airside.Build.StandTurnOff.PaintsOneNumberPerTurnOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffPaintsOneNumberPerTurnOffTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	{
		FAirsideTestWorld TestWorld;
		if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
		int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
		if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }
		const int32 Number = TestWorld.Actor->Network->GetEntities()[Index].StandNumber;

		FRoadMeshBuffers Buffers;
		FStandTurnOffCensus Census;
		FStandTurnOffMarkingBuilder::Build(*TestWorld.Actor->Network, 0.0, Buffers, &Census);
		TestEqual(TEXT("one stand, one lead-in, one turn-off"), Census.TurnOffs, 1);
		TestTrue(TEXT("its number painted once, and it is the stand's own"),
			Census.NumbersPainted == TArray<int32>{ Number });
		TestEqual(TEXT("one arrow at the turn-off"), Census.Arrows, 1);
		TestEqual(TEXT("and the number is not an index - the first stand is 1"), Number, 1);
	}

	// A TAXI-THROUGH STAND has a way in from each side, so two turn-offs - and both carry the
	// SAME number, because it is one stand.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		LayRawTaxiway(*Net, 0.0, -10000.0, 10000.0);
		LayRawTaxiway(*Net, 14000.0, -10000.0, 10000.0);
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Stand->bTaxiThrough = true;
		const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 7000.0), UE_DOUBLE_HALF_PI);
		FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());
		const int32 Number = Net->GetEntity(Placed)->StandNumber;

		FRoadMeshBuffers Buffers;
		FStandTurnOffCensus Census;
		FStandTurnOffMarkingBuilder::Build(*Net, 0.0, Buffers, &Census);
		TestEqual(TEXT("a taxi-through stand turns off two taxiways"), Census.TurnOffs, 2);
		TestTrue(TEXT("and paints its one number at each"),
			Census.NumbersPainted == (TArray<int32>{ Number, Number }));
	}
	return true;
}

/**
 * THE GAP ACROSS THE STRIP (Review Focus 4): every triangle the turn-off paints lies on the
 * taxiway's pavement - never in the strip between the pavement edge and the stand, which is
 * grass or shoulder and carries no paint (spec "Paint (user, from BHX)"). Paired with a count,
 * so an empty buffer cannot pass. It used to also require the lead-in to reach the pavement
 * edge; that paint was the sweeps', which are no longer painted (user 2026-09-29) - here the
 * triangles are the arrow and the sign.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffLeadInOnlyOnPavementTest,
	"Airside.Build.StandTurnOff.LeadInOnlyOnPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffLeadInOnlyOnPavementTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }
	if (!TestTrue(TEXT("the taxiway has a strip, or this test measures nothing"), Strip > 0.0)) { return false; }

	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	FStandTurnOffMarkingBuilder::Build(*TestWorld.Actor->Network, 0.0, Buffers, &Census);
	TestTrue(TEXT("triangles were emitted"), Buffers.Indices.Num() >= 3);

	int32 OffPavement = 0;
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const FVector3d Centroid = (Buffers.Positions[Buffers.Indices[Slot]] + Buffers.Positions[Buffers.Indices[Slot + 1]]
			+ Buffers.Positions[Buffers.Indices[Slot + 2]]) / 3.0;
		if (FMath::Abs(Centroid.Y) > HalfWidth + 1.0) { ++OffPavement; }
	}
	TestEqual(TEXT("no turn-off paint lies off the taxiway pavement - the strip is a gap"), OffPavement, 0);
	return true;
}

/**
 * NO PAINT ON THE SWEEPS (user 2026-09-29: "these are not needed"): the two curved links Join
 * lays from the lead end onto the taxiway stay in the GRAPH - the aircraft still turns along
 * them - but carry no yellow. Only the lead-in itself is painted, and only where paved.
 * The fixture's lead end sits behind the pavement edge (Join lays it a fillet radius back from
 * the corner), so no lead-in quad lies on the pavement at all: with the sweeps painted this
 * count was the sweeps' quads, never zero.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffPaintsNoSweepTest,
	"Airside.Build.StandTurnOff.PaintsNoSweep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffPaintsNoSweepTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }
	const URoadNetwork& Net = *TestWorld.Actor->Network;

	// THE FIXTURE HAS SWEEPS, and its lead end is off the pavement - or a zero below measures
	// nothing. The lead-in is the pose's one link; the sweeps are the other links at its far end.
	const FGuidelineNodeId PoseId = Net.GetEntities()[Index].PoseNode;
	const FGuidelineNode* Pose = Net.GetGuidelineNode(PoseId);
	if (!TestTrue(TEXT("the stand has a lead-in"), Pose != nullptr && Pose->Incident.Num() >= 1)) { return false; }
	const FGuidelineEdge* Lead = Net.GetGuidelineEdge(Pose->Incident[0]);
	const FGuidelineNode* End = Net.GetGuidelineNode(Lead->A == PoseId ? Lead->B : Lead->A);
	int32 Sweeps = 0;
	for (const FGuidelineEdgeId Id : End->Incident)
	{
		const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
		Sweeps += Id != Pose->Incident[0] && !Edge->DerivedFrom.IsSet() ? 1 : 0;
	}
	if (!TestEqual(TEXT("Join laid two sweeps"), Sweeps, 2)) { return false; }
	if (!TestTrue(*FString::Printf(TEXT("the lead end is behind the pavement edge (%.0f vs %.0f)"), FMath::Abs(End->Position.Y), HalfWidth),
		FMath::Abs(End->Position.Y) > HalfWidth)) { return false; }

	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	FStandTurnOffMarkingBuilder::Build(Net, 0.0, Buffers, &Census);
	TestEqual(TEXT("the turn-off is still signed"), Census.Arrows, 1);
	TestEqual(TEXT("no lead-in quad on the pavement - the sweeps are not painted"), Census.LeadInCentres.Num(), 0);
	return true;
}

/**
 * NO ROOM TO SWEEP (Review Focus 3): a lead-in that meets the taxiway too near its end joins it
 * hard - one split, no sweeps (FAnchorLink::Join's first branch). Still one turn-off and one
 * number, placed at the lead end, which IS the taxiway node.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffNoRoomBranchTest,
	"Airside.Build.StandTurnOff.NoRoomBranch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffNoRoomBranchTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LayRawTaxiway(*Net, 0.0, -10000.0, 0.0);
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	// 15 uu short of the taxiway's end: past the weld tolerance, so the join splits, but with
	// no room either side for a sweep.
	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(-15.0, 7000.0), UE_DOUBLE_HALF_PI);
	FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

	// THE FIXTURE REALLY IS THE NO-ROOM SHAPE: the pose's lead-in ends on a node that carries
	// the taxiway's own guideline, and no other link leaves it.
	const FGuidelineNodeId PoseId = Net->GetEntity(Placed)->PoseNode;
	const FGuidelineNode* Pose = Net->GetGuidelineNode(PoseId);
	if (!TestTrue(TEXT("the stand has one lead-in"), Pose != nullptr && Pose->Incident.Num() == 1)) { return false; }
	const FGuidelineEdgeId LeadId = Pose->Incident[0];
	const FGuidelineEdge* Lead = Net->GetGuidelineEdge(LeadId);
	const FGuidelineNode* End = Net->GetGuidelineNode(Lead->A == PoseId ? Lead->B : Lead->A);
	bool bOnTaxiway = false, bSwept = false;
	for (const FGuidelineEdgeId Id : End->Incident)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		bOnTaxiway |= Edge->DerivedFrom.IsSet();
		bSwept |= !Edge->DerivedFrom.IsSet() && Id != LeadId;
	}
	if (!TestTrue(TEXT("the fixture is the no-room branch: the lead end is ON the taxiway, no sweep leaves it"), bOnTaxiway && !bSwept)) { return false; }

	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	FStandTurnOffMarkingBuilder::Build(*Net, 0.0, Buffers, &Census);
	TestEqual(TEXT("still one turn-off"), Census.TurnOffs, 1);
	TestTrue(TEXT("still one number, the stand's"),
		Census.NumbersPainted == TArray<int32>{ Net->GetEntity(Placed)->StandNumber });
	TestTrue(TEXT("the lead-in is painted across the pavement from the lead end"), Census.LeadInCentres.Num() > 0);
	const double HalfWidth = TestProfiles::Taxiway()->GetMaxHalfWidth();
	int32 OffPavement = 0;
	for (const FVector2D& Centre : Census.LeadInCentres)
	{
		OffPavement += FMath::Abs(Centre.Y) > HalfWidth + 1.0 ? 1 : 0;
	}
	TestEqual(TEXT("and only on the pavement"), OffPavement, 0);
	// THE GRAPH SAMPLES ONCE: the paint lies on the lead edge as URoadNetwork::SampleGuideline
	// samples it - what the router costs and the follower walks - not a second curve. (Moved
	// here from FollowsTheDerivedEdges, whose fixture no longer paints a lead-in quad.)
	TArray<FVector2D> LeadPoints;
	if (!TestTrue(TEXT("the lead-in samples"), Net->SampleGuideline(LeadId, LeadPoints))) { return false; }
	double Worst = 0.0;
	for (const FVector2D& Centre : Census.LeadInCentres)
	{
		Worst = FMath::Max(Worst, ToPolyline(LeadPoints, Centre));
	}
	TestTrue(*FString::Printf(TEXT("every lead-in paint centre is within 1 uu of the sampled lead edge (worst %.3f)"), Worst), Worst <= 1.0);
	return true;
}

/** NO TAXIWAY (Review Focus 5): a stand whose lead-in found nothing - no turn-off paint, no
 *  crash, and the stand is still numbered. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffNoTaxiwayTest,
	"Airside.Build.StandTurnOff.NoTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffNoTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(0.0, 7000.0), UE_DOUBLE_HALF_PI);
	FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	TestEqual(TEXT("nothing to paint"), FStandTurnOffMarkingBuilder::Build(*Net, 0.0, Buffers, &Census), 0);
	TestEqual(TEXT("no turn-off"), Census.TurnOffs, 0);
	TestEqual(TEXT("no number painted"), Census.NumbersPainted.Num(), 0);
	TestEqual(TEXT("no triangles"), Buffers.Indices.Num(), 0);
	TestEqual(TEXT("the stand is still numbered"), Net->GetEntity(Placed)->StandNumber, 1);
	return true;
}

/**
 * THE SEAM: the presenter's marking rebuild runs the turn-off builder. Placing a stand paints the
 * layer with exactly what the three marking builders make of the network - so a presenter that
 * skipped this one comes up short by the turn-off's triangles.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffPresenterPaintsItTest,
	"Airside.Present.StandTurnOff.PaintsAfterPlacement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffPresenterPaintsItTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }
	const URoadNetwork& Net = *TestWorld.Actor->Network;

	FRoadMeshBuffers Holding, Stands, TurnOffs;
	FHoldingPositionMarkingBuilder::Build(Net, 0.0, Holding);
	FStandMarkingBuilder::Build(Net, 0.0, Stands, UAirsideSettings::ResolveLetterEnvelopeTable());
	FStandTurnOffMarkingBuilder::Build(Net, 0.0, TurnOffs);
	if (!TestTrue(TEXT("the turn-off paints something"), TurnOffs.Indices.Num() > 0)) { return false; }
	TestEqual(TEXT("the paint layer holds the holding, stand AND turn-off paint"),
		TestWorld.Actor->GetPresenter()->HoldingPaintTriangleCountForTest(),
		(Holding.Indices.Num() + Stands.Indices.Num() + TurnOffs.Indices.Num()) / 3);
	return true;
}

/**
 * THE SIGN SITS ON THE LEAD-IN'S OWN AXIS (user 2026-09-29, samples/standsigns.png): a short
 * STRAIGHT arrow on the line the lead-in leaves the taxiway centreline along, just clear of the
 * centreline paint, pointing INTO the stand - not a chevron half-way round a sweep. The stand
 * beside the taxiway at y = 0 faces +Y, so its axis is x = 0 and "into the stand" is +Y.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffSignOnTheLeadInAxisTest,
	"Airside.Build.StandTurnOff.SignOnTheLeadInAxis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffSignOnTheLeadInAxisTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }

	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	FStandTurnOffMarkingBuilder::Build(*TestWorld.Actor->Network, 0.0, Buffers, &Census);
	if (!TestEqual(TEXT("one arrow"), Census.ArrowTails.Num(), 1) || !TestEqual(TEXT("tip beside tail"), Census.ArrowTips.Num(), 1)) { return false; }
	const FVector2D Tail = Census.ArrowTails[0];
	const FVector2D Tip = Census.ArrowTips[0];
	TestTrue(*FString::Printf(TEXT("the arrow lies on the lead-in axis x = 0 (tail %.1f, tip %.1f)"), Tail.X, Tip.X),
		FMath::Abs(Tail.X) <= 1.0 && FMath::Abs(Tip.X) <= 1.0);
	TestTrue(TEXT("it points INTO the stand (+Y)"), Tip.Y > Tail.Y);
	TestTrue(*FString::Printf(TEXT("it starts clear of the centreline paint (tail y %.0f)"), Tail.Y),
		Tail.Y >= FStandTurnOffMarkingBuilder::ArrowStartFromCentreline - 1.0);
	TestTrue(*FString::Printf(TEXT("and ends on the taxiway pavement (tip y %.0f, half-width %.0f)"), Tip.Y, HalfWidth),
		Tip.Y <= HalfWidth);
	TestTrue(TEXT("SHORT and STRAIGHT: tip to tail is the arrow's own length"),
		FMath::IsNearlyEqual(FVector2D::Distance(Tail, Tip), FStandTurnOffMarkingBuilder::ArrowLength, 1.0));
	return true;
}

/**
 * THE NUMBER READS FROM BOTH SIDES (user 2026-09-29): two faces, one each side of the arrow,
 * the text running ALONG the axis, the glyph tops toward the arrow - so they read in opposite
 * directions, one for a pilot on each side, as samples/standsigns.png shows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffNumberReadsFromBothSidesTest,
	"Airside.Build.StandTurnOff.NumberReadsFromBothSides",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffNumberReadsFromBothSidesTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }

	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	FStandTurnOffMarkingBuilder::Build(*TestWorld.Actor->Network, 0.0, Buffers, &Census);
	if (!TestEqual(TEXT("two faces per turn-off"), Census.SignFaces.Num(), 2)) { return false; }
	const FStandSignFace& L = Census.SignFaces[0];
	const FStandSignFace& R = Census.SignFaces[1];
	TestTrue(TEXT("one face each side of the axis"), L.Origin.X * R.Origin.X < 0.0);
	for (const FStandSignFace* F : { &L, &R })
	{
		TestTrue(TEXT("the text runs along the axis"), FMath::IsNearlyEqual(FMath::Abs(F->Right.Y), 1.0, 1e-6));
		TestTrue(TEXT("the glyph tops face the arrow"), FVector2D::DotProduct(F->Up, FVector2D(-F->Origin.X, 0.0)) > 0.0);
		TestTrue(TEXT("and the face is on the taxiway pavement"), F->Origin.Y > 0.0 && F->Origin.Y < HalfWidth);
	}
	TestTrue(TEXT("the two read in OPPOSITE directions"), FVector2D::DotProduct(L.Right, R.Right) < -0.999);
	return true;
}

/**
 * EACH FACE SITS ON A BLACK BOX (user 2026-09-29): the digits are painted in the guidance
 * yellow over a box in EStandPaint::SignBackground, which contains them and lies BELOW them
 * (a lower Z, so the digits win the depth test rather than z-fighting the box).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffNumberOnABlackBoxTest,
	"Airside.Build.StandTurnOff.NumberOnABlackBox",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffNumberOnABlackBoxTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, TestWorld.Actor, Index, HalfWidth, Strip)) { return false; }

	FStandPaintIds Ids;
	for (int32 Paint = 0; Paint < static_cast<int32>(EStandPaint::Count); ++Paint) { Ids.Ids[Paint] = 10 + Paint; }
	const int32 Black = Ids[EStandPaint::SignBackground];
	const int32 Yellow = Ids[EStandPaint::Guidance];
	constexpr double Z = 10.0;
	FRoadMeshBuffers Buffers;
	FStandTurnOffCensus Census;
	FStandTurnOffMarkingBuilder::Build(*TestWorld.Actor->Network, Z, Buffers, &Census, Ids);
	if (!TestEqual(TEXT("two faces"), Census.SignFaces.Num(), 2)) { return false; }

	int32 BlackTris = 0;
	double BlackZ = -DBL_MAX, YellowZ = DBL_MAX;
	for (int32 T = 0; T < Buffers.MaterialIDs.Num(); ++T)
	{
		const double TriZ = Buffers.Positions[Buffers.Indices[T * 3]].Z;
		if (Buffers.MaterialIDs[T] == Black) { ++BlackTris; BlackZ = FMath::Max(BlackZ, TriZ); }
		if (Buffers.MaterialIDs[T] == Yellow) { YellowZ = FMath::Min(YellowZ, TriZ); }
	}
	TestEqual(TEXT("one black box (two triangles) per face"), BlackTris, 4);
	TestTrue(*FString::Printf(TEXT("the boxes lie below the yellow (%.2f < %.2f)"), BlackZ, YellowZ), BlackZ < YellowZ);
	TestTrue(TEXT("and still above the pavement the paint lies on"), BlackZ > Z - 0.5);

	// Each box CONTAINS its face's digits: the middle of the text, half a cell up from its foot.
	for (const FStandSignFace& F : Census.SignFaces)
	{
		// A hair off the text's exact middle: that is the box's centre, which lies on the diagonal
		// its two triangles share - a boundary PointInPolygon does not promise either answer on.
		const FVector2D Middle = F.Origin + F.Up * (FStandTurnOffMarkingBuilder::NumberHeight * 0.5)
			+ F.Right * 7.0 + F.Up * 3.0;
		bool bInside = false;
		for (int32 T = 0; T < Buffers.MaterialIDs.Num() && !bInside; ++T)
		{
			if (Buffers.MaterialIDs[T] != Black) { continue; }
			const FVector3d& P0 = Buffers.Positions[Buffers.Indices[T * 3]];
			const FVector3d& P1 = Buffers.Positions[Buffers.Indices[T * 3 + 1]];
			const FVector3d& P2 = Buffers.Positions[Buffers.Indices[T * 3 + 2]];
			const TArray<FVector2D> Tri{ FVector2D(P0.X, P0.Y), FVector2D(P1.X, P1.Y), FVector2D(P2.X, P2.Y) };
			bInside = RoadGeom::PointInPolygon(Tri, Middle);
		}
		TestTrue(TEXT("its black box sits under the text"), bInside);
	}
	return true;
}

/**
 * A NEIGHBOUR DOES NOT MOVE A NUMBER (final review 2026-09-29): stand 2 placed beside stand 1 on
 * the same taxiway re-splits the edge stand 1 joined, on every rebuild; stand 1's painted number
 * stays where it was, to 1 uu.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandTurnOffNeighbourDoesNotMoveTheNumberTest,
	"Airside.Present.StandTurnOff.NeighbourDoesNotMoveTheNumber",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandTurnOffNeighbourDoesNotMoveTheNumberTest::RunTest(const FString& Parameters)
{
	using namespace StandTurnOffTest;
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	int32 Index = INDEX_NONE; double HalfWidth = 0.0, Strip = 0.0;
	if (!PlaceBesideTaxiway(*this, Actor, Index, HalfWidth, Strip)) { return false; }

	const auto NumberOf = [this, Actor](int32 Number, FVector2D& Out)
	{
		FRoadMeshBuffers Buffers;
		FStandTurnOffCensus Census;
		FStandTurnOffMarkingBuilder::Build(*Actor->Network, 0.0, Buffers, &Census);
		const int32 At = Census.NumbersPainted.Find(Number);
		if (!TestTrue(*FString::Printf(TEXT("stand %d's number is painted"), Number), At != INDEX_NONE)) { return false; }
		Out = Census.NumberOrigins[At];
		return true;
	};
	FVector2D Before;
	if (!NumberOf(1, Before)) { return false; }

	// STAND 2, BESIDE IT on the same taxiway: entrance on the same line, 20 m east of stand 1's
	// east edge - close enough that its sweeps split the edge stand 1's sweeps split.
	const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	double EntranceY = DBL_MAX;
	for (const FVector2D& P : Actor->Network->GetEntities()[Index].Outline) { EntranceY = FMath::Min(EntranceY, P.Y); }
	const FVector2D A(Width * 0.5 + 2000.0, EntranceY);
	const FVector2D B(A.X + Width, EntranceY);
	IRoadEditTarget* Target = Actor;
	const TArray<FVector2D> Rect = { A, B, FVector2D(B.X, EntranceY + Depth), FVector2D(A.X, EntranceY + Depth) };
	if (!TestTrue(TEXT("stand 2 is placed beside stand 1"), Target->PlaceStandInPlot(Rect, A, B, EPavement::Tarmac) != INDEX_NONE)) { return false; }
	Actor->RebuildMesh();

	FVector2D After;
	if (!NumberOf(1, After)) { return false; }
	FVector2D Second;
	TestTrue(TEXT("stand 2 is signed too - the neighbour really joined the taxiway"), NumberOf(2, Second));
	TestTrue(*FString::Printf(TEXT("stand 1's number has not moved (%.1f uu)"), FVector2D::Distance(Before, After)),
		FVector2D::Distance(Before, After) <= 1.0);
	return true;
}

// ----------------------------------------------------------------------------------------------------------------------------------------------
// A STAND'S ENTRANCE IS STORED, AND BOTH READERS READ IT (#450's leftover)
//
// FEntityInstance::FrontageEdge holds a plotted depot's frontage since #491; two readers still RECOVERED a stand's by heuristic on every call -
// UStandDefinitionCache::PoseFromOutline (the rearmost MIDPOINT, on every load) and FStandMarkingBuilder::FrameFor (the rearmost CORNER, on
// every rebuild). They now read the stored edge, and the stand paint is VISIBLE, so the contract is that NOTHING THAT IS DRAWN MOVED: the pin
// below measures, for every stand fixture, the stored-edge answer against a verbatim copy of each retired heuristic, compared as values.
// ----------------------------------------------------------------------------------------------------------------------------------------------

namespace StandFrontageTest
{
	/** THE RETIRED POSE HEURISTIC, VERBATIM (UStandDefinitionCache::PoseFromOutline before #450's leftover): the edge whose midpoint lies furthest
	 *  behind the stop mark along the stand's heading. A COPY, on purpose: the pin compares the stored edge against what the readers USED TO
	 *  compute, so it must not call the production function that replaced it. */
	int32 RetiredPoseEntrance(const FEntityInstance& Stand)
	{
		const TArray<FVector2D>& Outline = Stand.Outline;
		const FVector2D Facing(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
		int32 Entrance = 0;
		double Behind = TNumericLimits<double>::Max();
		for (int32 Corner = 0; Corner < Outline.Num(); ++Corner)
		{
			const FVector2D Mid = 0.5 * (Outline[Corner] + Outline[(Corner + 1) % Outline.Num()]);
			const double Along = FVector2D::DotProduct(Mid - Stand.Position, Facing);
			if (Along < Behind)
			{
				Behind = Along;
				Entrance = Corner;
			}
		}
		return Entrance;
	}

	/** THE RETIRED PAINT HEURISTIC, VERBATIM (FStandMarkingBuilder::FrameFor before): the outline's rearmost CORNER along Facing, as the
	 *  signed distance behind the stop mark (negative = behind). */
	double RetiredPaintBehind(const FEntityInstance& Stand)
	{
		const FVector2D Facing(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
		double Behind = DBL_MAX;
		for (const FVector2D& Corner : Stand.Outline)
		{
			Behind = FMath::Min(Behind, FVector2D::DotProduct(Corner - Stand.Position, Facing));
		}
		return Behind;
	}

	/** A SUB-MILLIMETRE, in uu (1 uu = 1 cm): far below anything drawn, far above the ulp noise of a cos/sin pair. The pose inputs below are
	 *  compared EXACTLY (they are the same two vertices); only the paint distance - a dot product, and a min of two equal-in-theory corners
	 *  against the midpoint of the same edge - may differ in the last bits. */
	constexpr double PaintToleranceUu = 1.0e-4;

	/** One stand, measured both ways. Returns whether it agreed; every disagreement is an error naming the fixture. */
	bool ExpectStoredAgreesWithRetiredHeuristics(FAutomationTestBase& Test, const URoadNetwork& Net, FEntityInstanceId Id, const FString& Name)
	{
		const FEntityInstance* Stand = Net.GetEntity(Id);
		if (!Test.TestNotNull(*(Name + TEXT(": the stand is alive")), Stand)) { return false; }
		FVector2D StoredA, StoredB;
		if (!Test.TestTrue(*(Name + TEXT(": it stores an entrance edge")), Stand->GetFrontage(StoredA, StoredB))) { return false; }

		// THE POSE'S INPUTS: the entrance's two ends. PoseFor is a pure function of them and the outline, so equal ends = the same pose.
		const int32 Retired = RetiredPoseEntrance(*Stand);
		const FVector2D RetiredA = Stand->Outline[Retired];
		const FVector2D RetiredB = Stand->Outline[(Retired + 1) % Stand->Outline.Num()];
		bool bAgreed = Test.TestEqual(*(Name + TEXT(": the stored edge is the rearmost-midpoint edge the pose reader used to find")), Stand->FrontageEdge, Retired);
		bAgreed &= Test.TestTrue(*(Name + TEXT(": and its two ends are the same two points")), StoredA == RetiredA && StoredB == RetiredB);

		// THE PAINT'S INPUTS: how far behind the stop mark the entrance sits, and the point on the pose's line it centres on.
		FStandPaintFrame Frame;
		if (!Test.TestTrue(*(Name + TEXT(": it has a paint frame")), FStandMarkingBuilder::FrameFor(*Stand, FLetterEnvelopeTable::Floor(), Frame))) { return false; }
		const double RetiredBehind = RetiredPaintBehind(*Stand);
		const FVector2D RetiredMid = Stand->Position + Frame.Facing * RetiredBehind;
		bAgreed &= Test.TestTrue(*FString::Printf(TEXT("%s: the paint's setback is the rearmost corner's (%.6f vs %.6f)"), *Name, Frame.Setback, -RetiredBehind),
			FMath::Abs(Frame.Setback - (-RetiredBehind)) <= PaintToleranceUu);
		bAgreed &= Test.TestTrue(*FString::Printf(TEXT("%s: and the lead-in centres on the same point ((%.6f, %.6f) vs (%.6f, %.6f))"), *Name,
				Frame.EntranceMid.X, Frame.EntranceMid.Y, RetiredMid.X, RetiredMid.Y),
			Frame.EntranceMid.Equals(RetiredMid, PaintToleranceUu));
		return bAgreed;
	}

	/** A drawn stand of Letter placed through URoadNetwork::PlaceEntity at Heading, the way a fixture builds one (StandMarkingTest::PlaceDrawnStand,
	 *  rotated): StandBox::BoxAt off a pose, no frontage stated. ExtraDepth > 0 drags the far edge out, a stand drawn deeper than its floor. */
	FEntityInstanceId PlaceBoxStand(URoadNetwork& Net, UEntityDefinition* Definition, EIcaoCode Letter, const FVector2D& Position, double Heading, double ExtraDepth)
	{
		const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Letter);
		StandBox::FStandPose Pose;
		Pose.Position = Position;
		Pose.Facing = FVector2D(FMath::Cos(Heading), FMath::Sin(Heading));
		FEntityPlacement Placement;
		Placement.Definition = Definition;
		Placement.Anchors = Definition->Anchors;
		Placement.Position = Pose.Position;
		Placement.Heading = RoadGeom::Bearing(Pose.Facing);
		Placement.PoseRole = Definition->PoseRole;
		StandBox::BoxAt(Pose, Letter, Envelope, Placement.Outline);
		if (ExtraDepth > 0.0)
		{
			Placement.Outline[2] += Pose.Facing * ExtraDepth;
			Placement.Outline[3] += Pose.Facing * ExtraDepth;
		}
		Placement.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
		return Net.PlaceEntity(Placement);
	}

	/** Alive, a stand, and drawn - one line, both names (Check-Architecture's is-plotted-not-depot rule: IsPlotted() alone does not say "depot"). */
	bool IsDrawnStand(const URoadNetwork& Net, FEntityInstanceId Id)
	{
		const FEntityInstance* Entity = Net.GetEntity(Id);
		return Entity != nullptr && (Entity->IsStand() && Entity->IsPlotted());
	}

	/** A stand as a level saved before stands had outlines holds it: placed, then its outline and its entrance cleared. EnsureStandOutlines gives it a Code C
	 *  box on load, and the entrance is an edge OF that box - so the frontage migration has to run AFTER the outline one, in PostLoad and in
	 *  RepairLoadedNetwork both. Run the other way round it finds no outline, stores nothing, and the box arrives with no entrance. */
	FEntityInstanceId PlaceOutlinelessLegacyStand(URoadNetwork& Net, UEntityDefinition* Definition, const FVector2D& Position, double Heading)
	{
		const FEntityInstanceId Id = PlaceBoxStand(Net, Definition, EIcaoCode::C, Position, Heading, 0.0);
		FRoadNetworkTestAccess Access(Net);
		Access.SetEntityOutlineForTest(Id, TArray<FVector2D>());
		Access.SetEntityFrontageForTest(Id, INDEX_NONE);
		return Id;
	}

	/** PlaceBoxStand with the outline listed the OTHER way round (the facade reverses a clockwise outline, which puts the drawn far edge at 0 -> 1),
	 *  so the entrance is NOT edge 0 and a migration that stored 0 by default cannot pass. The frontage is cleared: a level as saved before it was stored. */
	FEntityInstanceId PlaceLegacyReversedStand(URoadNetwork& Net, UEntityDefinition* Definition, const FVector2D& Position, double Heading)
	{
		const FEntityInstanceId Id = PlaceBoxStand(Net, Definition, EIcaoCode::C, Position, Heading, 0.0);
		const FEntityInstance* Placed = Net.GetEntity(Id);
		if (Placed == nullptr) { return FEntityInstanceId(); }
		TArray<FVector2D> Reversed = Placed->Outline;
		Algo::Reverse(Reversed);
		FRoadNetworkTestAccess Access(Net);
		Access.SetEntityOutlineForTest(Id, Reversed);
		Access.SetEntityFrontageForTest(Id, INDEX_NONE);
		return Id;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandFrontageStoredEdgeTest,
	"Airside.Model.StandFrontage.StoredEdgeIsTodaysHeuristicAnswer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandFrontageStoredEdgeTest::RunTest(const FString& Parameters)
{
	using namespace StandFrontageTest;

	// EVERY WAY THE GAME MAKES A STAND, so a path that stores the wrong edge (or none) has a fixture to fail on:
	//   - the FACADE, through PlaceStandInPlot, every letter, drawn four ways (see below: both windings, the list started at another corner, square and
	//     rotated) - the commit path, whose STORED edge is the one it was GIVEN and so the only one that could differ from the heuristic;
	//   - PlaceEntity with a BoxAt outline and no frontage, at headings all round, and a stand dragged deeper than its floor;
	//   - the point-placed stand (no outline: PlaceEntity gives it its Code C box);
	//   - and every one of them again after a LEGACY LOAD (the edge cleared, EnsureStandFrontages run).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Target = Actor;

	// FOUR WAYS TO DRAW EACH LETTER, chosen so the edge the facade STORES is not always the same index (a pin whose fixtures all store edge 0 cannot tell a
	// facade that stores the given edge from one that stores 0):
	//   - drawn to the entrance's LEFT (the rectangle is wound counter-clockwise, so the facade keeps the list as it is: the entrance is edge 0);
	//   - drawn to its RIGHT (clockwise: the facade REVERSES it, which puts the drawn far edge at 0 -> 1 and the entrance at 2 - the case the old
	//     searches were written for);
	//   - and some of those with the list STARTED TWO CORNERS ON, so the entrance is the list's THIRD edge. (Two, not one: the facade measures a
	//     stand's width off the list's first edge and its depth off the second - StandBox::WidthOf/DepthOf - so a list turned by one corner swaps them
	//     and reads as another letter, or none. The far edge first is the one other start the gesture's own reversal can produce.)
	// Two are axis-aligned and two rotated.
	TArray<TPair<FString, FEntityInstanceId>> Facade;
	int32 Slot = 0;
	int32 NonZeroEdges = 0;
	for (const EIcaoCode Letter : { EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		for (int32 Variant = 0; Variant < 4; ++Variant)
		{
			const bool bRotated = Variant >= 2;
			const bool bToTheRight = (Variant % 2) == 1;
			const int32 Shift = (Variant == 1 || Variant == 2) ? 2 : 0;   // list started two corners on: the entrance is the list's THIRD edge
			const double Width = IcaoCode::StandWidthForLetter(Letter);
			const double Depth = IcaoCode::StandDepthForLetter(Letter);
			const double Angle = bRotated ? FMath::DegreesToRadians(37.0 + 11.0 * static_cast<double>(Letter)) : 0.0;
			const FVector2D Along(FMath::Cos(Angle), FMath::Sin(Angle));
			const FVector2D Dir = bToTheRight ? -RoadGeom::PerpCCW(Along) : RoadGeom::PerpCCW(Along);
			// 40 km a slot: the widest floor is under 190 m, so no two can overlap whatever the table's figures do later.
			const FVector2D Origin(40000.0 * static_cast<double>(Slot++), 0.0);
			const TArray<FVector2D> Drawn = { Origin, Origin + Along * Width, Origin + Along * Width + Dir * Depth, Origin + Dir * Depth };
			TArray<FVector2D> Rect;
			for (int32 Corner = 0; Corner < 4; ++Corner) { Rect.Add(Drawn[(Corner + Shift) % 4]); }
			const int32 EntranceAt = (4 - Shift) % 4;   // where the drawn entrance Drawn[0] -> Drawn[1] sits in the list
			const FString Name = FString::Printf(TEXT("facade Code %s drawn to the %s, %s, list started %d corner(s) on"), IcaoCode::ToLetter(Letter),
				bToTheRight ? TEXT("right") : TEXT("left"), bRotated ? TEXT("rotated") : TEXT("square"), Shift);
			const int32 Index = Target->PlaceStandInPlot(Rect, Rect[EntranceAt], Rect[(EntranceAt + 1) % 4], EPavement::Tarmac);
			if (TestTrue(*(Name + TEXT(": placed")), Index != INDEX_NONE))
			{
				const FEntityInstanceId Placed = Actor->Network->EntityIdAt(Index);
				NonZeroEdges += Actor->Network->GetEntity(Placed)->FrontageEdge > 0 ? 1 : 0;
				Facade.Add(TPair<FString, FEntityInstanceId>(Name, Placed));
			}
		}
	}
	TestTrue(TEXT("the facade fixtures include stands whose stored entrance is NOT edge 0 - the pin would otherwise not tell a stored edge from a default"), NonZeroEdges >= 5);

	URoadNetwork* Model = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Definition = UEntityDefinition::MakeStandTransient();
	TArray<TPair<FString, FEntityInstanceId>> Boxes;
	for (int32 Step = 0; Step < 12; ++Step)
	{
		const double Heading = FMath::DegreesToRadians(Step * 30.0 + 7.0);
		const FVector2D Where(30000.0 * static_cast<double>(Step), 60000.0);
		const double ExtraDepth = (Step % 3 == 2) ? 2500.0 : 0.0;
		const EIcaoCode Letter = (Step % 2 == 0) ? EIcaoCode::C : EIcaoCode::E;
		const FString Name = FString::Printf(TEXT("box Code %s heading %d%s"), IcaoCode::ToLetter(Letter), Step * 30 + 7, ExtraDepth > 0.0 ? TEXT(" deeper than its floor") : TEXT(""));
		const FEntityInstanceId Id = PlaceBoxStand(*Model, Definition, Letter, Where, Heading, ExtraDepth);
		if (TestTrue(*(Name + TEXT(": placed")), Id.IsSet())) { Boxes.Add(TPair<FString, FEntityInstanceId>(Name, Id)); }
	}
	for (int32 Step = 0; Step < 6; ++Step)
	{
		const double Heading = FMath::DegreesToRadians(Step * 60.0);
		const FString Name = FString::Printf(TEXT("point-placed heading %d"), Step * 60);
		const FEntityInstanceId Id = Model->PlaceEntity(Definition, Definition->Anchors, FVector2D(30000.0 * static_cast<double>(Step), 120000.0), Heading);
		if (TestTrue(*(Name + TEXT(": placed")), Id.IsSet())) { Boxes.Add(TPair<FString, FEntityInstanceId>(Name, Id)); }
	}
	if (!TestTrue(TEXT("setup: twenty facade stands and eighteen model ones"), Facade.Num() == 20 && Boxes.Num() == 18)) { return false; }

	int32 Disagreed = 0;
	for (const TPair<FString, FEntityInstanceId>& Fixture : Facade)
	{
		Disagreed += ExpectStoredAgreesWithRetiredHeuristics(*this, *Actor->Network, Fixture.Value, Fixture.Key) ? 0 : 1;
	}
	for (const TPair<FString, FEntityInstanceId>& Fixture : Boxes)
	{
		Disagreed += ExpectStoredAgreesWithRetiredHeuristics(*this, *Model, Fixture.Value, Fixture.Key) ? 0 : 1;
	}

	// A LEVEL SAVED BEFORE THE EDGE WAS STORED: clear it on every fixture, run the migration, ask the same question again. The migration's
	// answer must be the retired heuristic's too, or an authored map's stands would move the day it first loaded.
	FRoadNetworkTestAccess FacadeAccess(*Actor->Network);
	FRoadNetworkTestAccess ModelAccess(*Model);
	for (const TPair<FString, FEntityInstanceId>& Fixture : Facade) { FacadeAccess.SetEntityFrontageForTest(Fixture.Value, INDEX_NONE); }
	for (const TPair<FString, FEntityInstanceId>& Fixture : Boxes) { ModelAccess.SetEntityFrontageForTest(Fixture.Value, INDEX_NONE); }
	TestEqual(TEXT("the migration stores an edge on every drawn stand of the facade's network"), Actor->Network->EnsureStandFrontages(), Facade.Num());
	TestEqual(TEXT("and of the model network"), Model->EnsureStandFrontages(), Boxes.Num());
	for (const TPair<FString, FEntityInstanceId>& Fixture : Facade)
	{
		Disagreed += ExpectStoredAgreesWithRetiredHeuristics(*this, *Actor->Network, Fixture.Value, Fixture.Key + TEXT(" after the legacy migration")) ? 0 : 1;
	}
	for (const TPair<FString, FEntityInstanceId>& Fixture : Boxes)
	{
		Disagreed += ExpectStoredAgreesWithRetiredHeuristics(*this, *Model, Fixture.Value, Fixture.Key + TEXT(" after the legacy migration")) ? 0 : 1;
	}
	TestEqual(TEXT("no stand fixture disagreed with the heuristic it retired - if one does, what is DRAWN would move: stop and report it"), Disagreed, 0);
	return true;
}

/**
 * BOTH READERS READ THE STORED EDGE, NOT THEIR OWN SEARCH.
 *
 * The pin above passes for a reader that still searches (the search and the stored edge agree on every fixture - that is its point), so
 * this is the half that measures the READ: the stored edge is overwritten with the FAR edge, which no search would ever name. A reader that
 * searched answers as before; one that reads the edge follows it. (Mutation-checked: putting either search back turns this red.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandFrontageReadersTest,
	"Airside.Model.StandFrontage.ReadersReadTheStoredEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandFrontageReadersTest::RunTest(const FString& Parameters)
{
	using namespace StandFrontageTest;

	// THE PAINT: a stand whose stored entrance is moved to its far edge paints from THAT edge.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FEntityInstanceId Id = PlaceBoxStand(*Net, UEntityDefinition::MakeStandTransient(), EIcaoCode::C, FVector2D(0.0, 0.0), 0.0, 0.0);
		if (!TestTrue(TEXT("a stand is placed"), Id.IsSet())) { return false; }
		FStandPaintFrame Entrance;
		if (!TestTrue(TEXT("it has a paint frame"), FStandMarkingBuilder::FrameFor(*Net->GetEntity(Id), FLetterEnvelopeTable::Floor(), Entrance))) { return false; }
		TestEqual(TEXT("its entrance is BoxAt's edge 0"), Net->GetEntity(Id)->FrontageEdge, 0);

		TestTrue(TEXT("the far edge is stored"), FRoadNetworkTestAccess(*Net).SetEntityFrontageForTest(Id, 2));
		const FEntityInstance* Stand = Net->GetEntity(Id);
		FStandPaintFrame Far;
		if (!TestTrue(TEXT("and it still has a paint frame"), FStandMarkingBuilder::FrameFor(*Stand, FLetterEnvelopeTable::Floor(), Far))) { return false; }
		const FVector2D Facing(FMath::Cos(Stand->Heading), FMath::Sin(Stand->Heading));
		const double FarBehind = FMath::Min(FVector2D::DotProduct(Stand->Outline[2] - Stand->Position, Facing),
			FVector2D::DotProduct(Stand->Outline[3] - Stand->Position, Facing));
		TestTrue(*FString::Printf(TEXT("the paint follows the STORED edge: setback %.1f is the far edge's (%.1f), not the entrance's (%.1f)"),
				Far.Setback, -FarBehind, Entrance.Setback),
			FMath::Abs(Far.Setback - (-FarBehind)) <= PaintToleranceUu && FMath::Abs(Far.Setback - Entrance.Setback) > 1000.0);

		// NO STORED EDGE: the floor figure, the same fallback a stand nobody drew gets - the paint is not lost and nothing searches.
		TestTrue(TEXT("no edge is stored"), FRoadNetworkTestAccess(*Net).SetEntityFrontageForTest(Id, INDEX_NONE));
		FStandPaintFrame None;
		if (TestTrue(TEXT("a plotted stand with no stored entrance still has a paint frame"), FStandMarkingBuilder::FrameFor(*Net->GetEntity(Id), FLetterEnvelopeTable::Floor(), None)))
		{
			const double Floor = StandBox::EntranceSetback(EIcaoCode::C, IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C));
			TestTrue(*FString::Printf(TEXT("painted at the floor setback (%.1f), as a stand nobody drew is: %.1f"), Floor, None.Setback), FMath::Abs(None.Setback - Floor) <= PaintToleranceUu);
		}
	}

	// THE POSE: the load's re-derivation (UStandDefinitionCache::RebindStandDefinitions, here through the actor's own repair) re-poses a stand off
	// its STORED entrance. Left as placed it is a no-op - a stand placed today re-derives its own pose; pointed at its far edge it moves.
	{
		FAirsideTestWorld TestWorld;
		if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->ClearNetwork();
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
		IRoadEditTarget* Target = Actor;
		const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
		const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
		const TArray<FVector2D> Rect = { FVector2D(0.0, 5000.0), FVector2D(Width, 5000.0), FVector2D(Width, 5000.0 + Depth), FVector2D(0.0, 5000.0 + Depth) };
		const int32 Index = Target->PlaceStandInPlot(Rect, Rect[0], Rect[1], EPavement::Tarmac);
		if (!TestTrue(TEXT("a Code C stand is placed"), Index != INDEX_NONE)) { return false; }
		const FEntityInstanceId Id = Actor->Network->EntityIdAt(Index);
		const FVector2D Placed = Actor->Network->GetEntity(Id)->Position;
		const int32 Entrance = Actor->Network->GetEntity(Id)->FrontageEdge;
		if (!TestTrue(TEXT("it stores an entrance"), Entrance != INDEX_NONE)) { return false; }

		Actor->RebindStandDefinitions();
		TestTrue(TEXT("control: re-deriving the pose off the stored entrance leaves a stand placed today where it is"),
			Actor->Network->GetEntity(Id)->Position.Equals(Placed, 1.0));

		FRoadNetworkTestAccess(*Actor->Network).SetEntityFrontageForTest(Id, (Entrance + 2) % 4);
		Actor->RebindStandDefinitions();
		const FVector2D Moved = Actor->Network->GetEntity(Id)->Position;
		TestTrue(*FString::Printf(TEXT("the re-pose follows the STORED edge: the stop mark moved from (%.0f, %.0f) to (%.0f, %.0f)"), Placed.X, Placed.Y, Moved.X, Moved.Y),
			!Moved.Equals(Placed, 100.0));
	}
	return true;
}

/**
 * A STAND SAVED BEFORE THE ENTRANCE WAS STORED KEEPS ITS POSE AND ITS PAINT (the migration).
 *
 * A level or save written before FEntityInstance::FrontageEdge held a stand's entrance loads every drawn stand at INDEX_NONE. EnsureStandFrontages
 * stores the rule the readers used to run, once: only on a stand that has an outline and no edge, idempotently, on both load paths (PostLoad for a
 * level, RepairLoadedNetwork for a save game, whose load is Serialize alone). A stand already storing an edge is left alone - the player's stored
 * fact outranks a search - and so is every depot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandFrontageMigrationTest,
	"Airside.Model.StandFrontage.MigrationStoresTheEntranceOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandFrontageMigrationTest::RunTest(const FString& Parameters)
{
	using namespace StandFrontageTest;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	FRoadNetworkTestAccess Access(*Net);

	const FEntityInstanceId Legacy = PlaceLegacyReversedStand(*Net, StandDef, FVector2D(0.0, 0.0), FMath::DegreesToRadians(30.0));
	const FEntityInstanceId AlreadyStored = PlaceBoxStand(*Net, StandDef, EIcaoCode::C, FVector2D(20000.0, 0.0), 0.0, 0.0);
	const FEntityInstanceId Outlineless = PlaceBoxStand(*Net, StandDef, EIcaoCode::C, FVector2D(40000.0, 0.0), 0.0, 0.0);
	TArray<FVector2D> DepotPlot = { FVector2D(0.0, 30000.0), FVector2D(5000.0, 30000.0), FVector2D(5000.0, 32400.0), FVector2D(0.0, 32400.0) };
	FEntityPlacement DepotPlacement;
	DepotPlacement.Definition = DepotDef;
	DepotPlacement.Anchors = DepotDef->Anchors;
	DepotPlacement.Position = (DepotPlot[0] + DepotPlot[1]) * 0.5;
	DepotPlacement.PoseRole = EServiceRole::Fuel;
	DepotPlacement.Outline = DepotPlot;
	DepotPlacement.FrontageEdge = 2;
	const FEntityInstanceId Depot = Net->PlaceEntity(DepotPlacement);
	if (!TestTrue(TEXT("setup: three stands and a depot"), Legacy.IsSet() && AlreadyStored.IsSet() && Outlineless.IsSet() && Depot.IsSet())) { return false; }

	// THE RETIRED SEARCH'S ANSWER for the reversed box - not edge 0, which is the far edge once the list is turned round.
	const int32 Expected = RetiredPoseEntrance(*Net->GetEntity(Legacy));
	if (!TestTrue(TEXT("the premise: the legacy stand's entrance is not edge 0, so storing the default cannot pass"), Expected != 0)) { return false; }
	// A LEVEL AS PRE-#450 HOLDS IT: an outline, no stored entrance. The "already stored" stand names the FAR edge - not what a search would say.
	Access.SetEntityFrontageForTest(AlreadyStored, 2);
	Access.SetEntityOutlineForTest(Outlineless, {});
	if (!TestEqual(TEXT("the premise: a legacy stand stores no entrance"), Net->GetEntity(Legacy)->FrontageEdge, static_cast<int32>(INDEX_NONE))) { return false; }
	TestEqual(TEXT("and a stand whose outline is gone stores none either (the frontage follows the outline)"), Net->GetEntity(Outlineless)->FrontageEdge, static_cast<int32>(INDEX_NONE));

	TestEqual(TEXT("EnsureDepotFrontages leaves every stand alone"), Net->EnsureDepotFrontages(), 0);
	TestEqual(TEXT("the migration gives exactly the one legacy drawn stand an entrance"), Net->EnsureStandFrontages(), 1);
	TestEqual(TEXT("the rearmost-midpoint edge the readers used to find"), Net->GetEntity(Legacy)->FrontageEdge, Expected);
	TestEqual(TEXT("a stand already storing an edge keeps it - the stored fact outranks a search"), Net->GetEntity(AlreadyStored)->FrontageEdge, 2);
	TestEqual(TEXT("a stand with no outline has none to store"), Net->GetEntity(Outlineless)->FrontageEdge, static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("a depot's own frontage is untouched"), Net->GetEntity(Depot)->FrontageEdge, 2);
	TestEqual(TEXT("a second pass finds nothing to do"), Net->EnsureStandFrontages(), 0);

	// THE LEVEL'S LOAD: PostLoad runs it. (Unwired, an authored map's stands load with no entrance: no pose repair, floor-figure paint.)
	URoadNetwork* Loaded = NewObject<URoadNetwork>(GetTransientPackage());
	const FEntityInstanceId LoadedStand = PlaceLegacyReversedStand(*Loaded, StandDef, FVector2D(0.0, 0.0), FMath::DegreesToRadians(30.0));
	const FEntityInstanceId LoadedBare = PlaceOutlinelessLegacyStand(*Loaded, StandDef, FVector2D(20000.0, 0.0), FMath::DegreesToRadians(30.0));
	if (!TestFalse(TEXT("the premise: the outline-less legacy stand has no outline before the load"), IsDrawnStand(*Loaded, LoadedBare))) { return false; }
	Loaded->PostLoad();
	TestEqual(TEXT("PostLoad stores the legacy stand's entrance"), Loaded->GetEntity(LoadedStand)->FrontageEdge, Expected);
	// THE ORDER, held by a stand that has no outline until the load gives it one: EnsureStandOutlines first (the Code C box), THEN the entrance - edge 0, BoxAt's own.
	// EnsureStandFrontages ahead of it sees no outline, stores nothing, and the box arrives with INDEX_NONE (the migration test's other order, mutation-checked).
	TestTrue(TEXT("PostLoad gave the outline-less stand its box"), IsDrawnStand(*Loaded, LoadedBare));
	TestEqual(TEXT("and THEN its entrance: edge 0 of the box (frontages after outlines in PostLoad)"), Loaded->GetEntity(LoadedBare)->FrontageEdge, 0);

	// A SAVE GAME'S LOAD, which is Serialize alone: the actor's repair runs the migration too.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->ClearNetwork();
	const FEntityInstanceId SavedStand = PlaceLegacyReversedStand(*Actor->Network, StandDef, FVector2D(0.0, 0.0), FMath::DegreesToRadians(30.0));
	const FEntityInstanceId SavedBare = PlaceOutlinelessLegacyStand(*Actor->Network, StandDef, FVector2D(20000.0, 0.0), FMath::DegreesToRadians(30.0));
	if (!TestFalse(TEXT("the premise: the outline-less legacy stand has no outline before the repair"), IsDrawnStand(*Actor->Network, SavedBare))) { return false; }
	Actor->RepairLoadedNetwork(ELoadedFrom::SaveGame);
	TestEqual(TEXT("RepairLoadedNetwork stores the legacy stand's entrance for a save game"), Actor->Network->GetEntity(SavedStand)->FrontageEdge, Expected);
	// THE SAME ORDER ON THE SAVE-GAME PATH, which is its own list (RepairLoadedNetwork, not PostLoad): the box first, then its entrance.
	TestTrue(TEXT("the repair gave the outline-less stand its box"), IsDrawnStand(*Actor->Network, SavedBare));
	TestEqual(TEXT("and THEN its entrance: edge 0 of the box (frontages after outlines in the repair)"), Actor->Network->GetEntity(SavedBare)->FrontageEdge, 0);
	return true;
}

/**
 * A STAND'S ENTRANCE MUST BE AN EDGE OF ITS OUTLINE, OR THE PLACEMENT IS REFUSED - the depot's own rule (PlaceEntityInPlot refuses a frontage that is
 * not an edge), so a stand's plot and a depot's agree. Asked with a pair that is not an edge of the rectangle (a corner and a point off the outline, and
 * two corners that are diagonal), nothing is placed and nothing is charged; asked with the real entrance it places as before. Without the refusal the
 * placement would have gone through and PlaceEntity would have stored a searched edge - a guess about which side the stand faces, that the stored
 * entrance exists to remove.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandFrontageRefusesNonEdgeTest,
	"Airside.Model.StandFrontage.RefusesAnEntranceThatIsNotAnEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandFrontageRefusesNonEdgeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Target = Actor;

	const double Width = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Depth = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	const TArray<FVector2D> Rect = { FVector2D(0.0, 5000.0), FVector2D(Width, 5000.0), FVector2D(Width, 5000.0 + Depth), FVector2D(0.0, 5000.0 + Depth) };
	const auto LiveStands = [Actor]
	{
		int32 Count = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities()) { Count += (Entity.bAlive && Entity.IsStand()) ? 1 : 0; }
		return Count;
	};
	const int32 Before = LiveStands();

	TestEqual(TEXT("a corner and a point off the outline: refused"),
		Target->PlaceStandInPlot(Rect, Rect[0], FVector2D(Width * 0.5, 5000.0), EPavement::Tarmac), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("two corners that are diagonal, not an edge: refused"),
		Target->PlaceStandInPlot(Rect, Rect[0], Rect[2], EPavement::Tarmac), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("the right edge named the wrong way round for the outline's winding: refused (given, not searched for)"),
		Target->PlaceStandInPlot(Rect, Rect[1], Rect[0], EPavement::Tarmac), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("nothing was placed by any of them"), LiveStands(), Before);

	const int32 Placed = Target->PlaceStandInPlot(Rect, Rect[0], Rect[1], EPavement::Tarmac);
	if (TestTrue(TEXT("the real entrance places"), Placed != INDEX_NONE))
	{
		TestEqual(TEXT("and stores that edge"), Actor->Network->GetEntities()[Placed].FrontageEdge, 0);
	}
	return true;
}

#endif
