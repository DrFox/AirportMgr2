#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/BuildCost.h"
#include "Build/RoadLaneMarkingBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Rs prefix: these test files share one translation unit (unity build).

	/** Transient taxiway widths and nothing else - FNullEditTarget's inert rest (#189). */
	struct FRsWidthTarget : FNullEditTarget
	{
		TArray<URoadProfile*> Widths;

		virtual int32 GetWidthCount(ERoadKind Kind) const override
		{
			return Kind == ERoadKind::Taxiway ? Widths.Num() : 0;
		}
		virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override
		{
			return Kind == ERoadKind::Taxiway && Widths.Num() > 0
				? Widths[FMath::Clamp(Index, 0, Widths.Num() - 1)] : nullptr;
		}
		virtual URoadProfile* ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) override
		{
			return ResolveWidthProfile(Kind, WidthIndex == INDEX_NONE ? 0 : WidthIndex);
		}
	};

	int32 RsAxisIndex(const TArray<FToolVariantAxis>& Axes, const TCHAR* Id)
	{
		return Axes.IndexOfByPredicate([Id](const FToolVariantAxis& A) { return A.Id == FName(Id); });
	}

	/** Every triangle's material id, bucketed by where its centroid lies along y = 0. */
	struct FRsIds
	{
		TSet<int32> West;      // x < 30000: the west arm's ribbon, clear of the junction
		TSet<int32> East;      // x > 70000: the east arm's ribbon
		TSet<int32> Junction;  // within 1000 uu of the junction node at x = 50000
	};

	FRsIds RsCountIds(const FRoadMeshBuffers& Buffers)
	{
		FRsIds Out;
		for (int32 Triangle = 0; Triangle * 3 + 2 < Buffers.Indices.Num(); ++Triangle)
		{
			FVector3d Centroid = FVector3d::ZeroVector;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Centroid += Buffers.Positions[Buffers.Indices[Triangle * 3 + Corner]];
			}
			Centroid /= 3.0;
			const int32 Id = Buffers.MaterialIDs.IsValidIndex(Triangle) ? Buffers.MaterialIDs[Triangle] : -1;
			if (Centroid.X < 30000.0) { Out.West.Add(Id); }
			else if (Centroid.X > 70000.0) { Out.East.Add(Id); }
			else if (FVector2D::Distance(FVector2D(Centroid.X, Centroid.Y), FVector2D(50000.0, 0.0)) < 1000.0)
			{
				Out.Junction.Add(Id);
			}
		}
		return Out;
	}
}

/**
 * THE ROAD TOOL OFFERS A SURFACE ROW AND TAKES A PICK - world-free. Fails if the row is missing,
 * if a pick lands on the width instead (rows chosen by index rather than Id), or if Shift+key
 * steps the width rather than the surface.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsToolSurfaceRowTest,
	"Airside.Tool.Variants.RoadSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsToolSurfaceRowTest::RunTest(const FString& Parameters)
{
	FRsWidthTarget Target;
	Target.Widths = { URoadProfile::MakeTransient(1050.0, 1500.0), URoadProfile::MakeTransient(2300.0, 1500.0) };
	FToolContext Context;
	Context.Target = &Target;

	// 1. THE ROW: after Width, tarmac then grass, tarmac lit - what every road before it was.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		const int32 Row = RsAxisIndex(Axes, TEXT("Surface"));
		if (!TestEqual(TEXT("the surface row follows the width row"), Row, 1)) { return false; }
		if (!TestEqual(TEXT("two options"), Axes[Row].Options.Num(), 2)) { return false; }
		TestEqual(TEXT("tarmac first"), Axes[Row].Options[0].Label.ToString(), FString(TEXT("tarmac")));
		TestEqual(TEXT("grass second"), Axes[Row].Options[1].Label.ToString(), FString(TEXT("grass")));
		TestEqual(TEXT("tarmac lit on a fresh tool"), Axes[Row].Current, 0);
		TestEqual(TEXT("and it is what a click would lay"), Tool.GetSurface(), ERoadSurface::Tarmac);
	}

	// 2. A PICK SETS THE SURFACE, and leaves the width alone.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestTrue(TEXT("picking grass is accepted"), Tool.SelectVariant(Context, 1, 1));
		TestEqual(TEXT("the next click lays grass"), Tool.GetSurface(), ERoadSurface::Grass);
		TestEqual(TEXT("the width was not touched"), Tool.GetWidthIndex(), INDEX_NONE);
		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		TestEqual(TEXT("and grass is lit"), Axes[1].Current, 1);
	}

	// 3. SHIFT+KEY STEPS THE SURFACE; the plain key still steps the width.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		FToolContext Shift = Context;
		Shift.bInsertModifier = true;
		Tool.OnReselect(Shift);
		TestEqual(TEXT("Shift+key steps tarmac to grass"), Tool.GetSurface(), ERoadSurface::Grass);
		TestEqual(TEXT("and not the width"), Tool.GetWidthIndex(), INDEX_NONE);
		Tool.OnReselect(Shift);
		TestEqual(TEXT("then wraps back to tarmac"), Tool.GetSurface(), ERoadSurface::Tarmac);
		Tool.OnReselect(Context);
		TestNotEqual(TEXT("the plain key steps the width"), Tool.GetWidthIndex(), (int32)INDEX_NONE);
		TestEqual(TEXT("and not the surface"), Tool.GetSurface(), ERoadSurface::Tarmac);
	}

	// 4. NO WIDTHS: surface is row 0, and a pick on row 0 means surface - the Id, not the index.
	{
		FRsWidthTarget Empty;
		FToolContext EmptyContext;
		EmptyContext.Target = &Empty;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestTrue(TEXT("row 0 is surface when there are no widths"), Tool.SelectVariant(EmptyContext, 0, 1));
		TestEqual(TEXT("so the pick lays grass"), Tool.GetSurface(), ERoadSurface::Grass);
		FToolContext Shift = EmptyContext;
		Shift.bInsertModifier = true;
		Tool.OnReselect(Shift);
		TestEqual(TEXT("and Shift+key still steps it with no widths to cycle"), Tool.GetSurface(), ERoadSurface::Tarmac);
	}
	return true;
}

/**
 * THE COMPOSITION: grass picked on the tool reaches the segment the click lays, through the
 * actor, the facade and the network - and survives the two edits that recreate segments, a
 * split and a node-deletion heal. Fails if any seam drops the surface on the floor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsGrassRoadLaidTest,
	"Airside.Present.GrassRoadLaid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsGrassRoadLaidTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }

	// 1. THE TOOL'S PICK REACHES THE ROAD.
	FRoadDrawTool Tool(ERoadKind::Taxiway);
	{
		FToolContext Pick = TestTool::ContextAt(*Actor, FVector2D::ZeroVector);
		Pick.bInsertModifier = true;
		Tool.OnReselect(Pick);
		if (!TestEqual(TEXT("Shift+key picked grass"), Tool.GetSurface(), ERoadSurface::Grass)) { return false; }
	}
	Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 0.0)));
	Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(40000.0, 0.0)));
	if (!TestNotNull(TEXT("the actor made a network"), Actor->Network.Get())) { return false; }
	const TArray<FRoadSegment>& Laid = Actor->Network->GetSegments();
	if (!TestEqual(TEXT("one segment was laid"), Laid.Num(), 1)) { return false; }
	TestEqual(TEXT("and it lies on grass"), Laid[0].Surface, ERoadSurface::Grass);
	TestTrue(TEXT("which the network calls a grass road"), Actor->Network->IsGrassRoad(Actor->Network->SegmentIdAt(0)));

	// 2. A SPLIT KEEPS IT on both halves.
	const int32 Middle = Actor->SplitSegment(0, FVector2D(20000.0, 0.0));
	if (!TestNotEqual(TEXT("the split made a node"), Middle, (int32)INDEX_NONE)) { return false; }
	int32 Alive = 0;
	for (int32 Index = 0; Index < Actor->Network->GetSegments().Num(); ++Index)
	{
		if (Actor->Network->GetSegments()[Index].bAlive)
		{
			++Alive;
			TestTrue(TEXT("each half is still grass"), Actor->Network->IsGrassRoad(Actor->Network->SegmentIdAt(Index)));
		}
	}
	TestEqual(TEXT("two halves"), Alive, 2);

	// 3. DELETING THE MIDDLE HEALS IT AS GRASS, not as the level's default tarmac.
	TestTrue(TEXT("the middle node deletes"), Actor->DeleteNode(Middle));
	Alive = 0;
	for (int32 Index = 0; Index < Actor->Network->GetSegments().Num(); ++Index)
	{
		if (Actor->Network->GetSegments()[Index].bAlive)
		{
			++Alive;
			TestTrue(TEXT("the healed road is grass"), Actor->Network->IsGrassRoad(Actor->Network->SegmentIdAt(Index)));
		}
	}
	TestEqual(TEXT("one healed road"), Alive, 1);

	// 4. THE QUOTE FOLLOWS THE SURFACE the click will lay - a preview that priced tarmac would
	//    promise one figure and charge another. Only a ratio when the profile has a price at all.
	const int32 From = Actor->PlaceNode(FVector2D(0.0, 20000.0));
	const FBuildQuote Tarmac = Actor->QuoteForConnect(From, FVector2D(10000.0, 20000.0), ERoadKind::Taxiway, INDEX_NONE, ERoadSurface::Tarmac);
	const FBuildQuote Grass = Actor->QuoteForConnect(From, FVector2D(10000.0, 20000.0), ERoadKind::Taxiway, INDEX_NONE, ERoadSurface::Grass);
	TestEqual(TEXT("grass quotes at the grass factor of tarmac"), Grass.BaseAmount, Tarmac.BaseAmount * BuildCost::GrassRateFactor, 1e-6);
	return true;
}

/**
 * GRASS IS PRICED BELOW TARMAC, for the build and the upkeep alike - the two must agree or a
 * grass road's upkeep would bill a surface its construction did not charge for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsGrassCostTest,
	"Airside.Build.GrassRoadCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsGrassCostTest::RunTest(const FString& Parameters)
{
	URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);
	Profile->CostPerMetre = 100.0;
	Profile->UpkeepPerMetrePerDay = 2.0;

	const double Tarmac = BuildCost::ForSegment(*Profile, 10000.0).BaseAmount;
	const double Grass = BuildCost::ForSegment(*Profile, 10000.0, ERoadSurface::Grass).BaseAmount;
	TestEqual(TEXT("100 m of tarmac at 100 a metre"), Tarmac, 10000.0, 1e-6);
	TestEqual(TEXT("the same on grass costs the grass factor of it"), Grass, 10000.0 * BuildCost::GrassRateFactor, 1e-6);
	TestTrue(TEXT("and the factor is a discount"), BuildCost::GrassRateFactor < 1.0);

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Seg = TestGraph::Lay(*Net, Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(10000.0, 0.0)), Profile);
	const double TarmacUpkeep = BuildCost::DailyUpkeep(*Net, 0.0);
	TestTrue(TEXT("the segment takes grass"), Net->SetSegmentSurface(Seg, ERoadSurface::Grass));
	const double GrassUpkeep = BuildCost::DailyUpkeep(*Net, 0.0);
	TestEqual(TEXT("upkeep on tarmac"), TarmacUpkeep, 200.0, 1e-6);
	TestEqual(TEXT("upkeep on grass takes the same factor the build did"), GrassUpkeep, 200.0 * BuildCost::GrassRateFactor, 1e-6);
	return true;
}

/**
 * A GRASS ROAD IS PAINTED WITH THE GRASS SLOT, kerbs and run-offs included, and a junction
 * where grass meets tarmac stays TARMAC - paved wins. The grass arm has the lowest segment id,
 * so the old widest-then-lowest-id rule alone would have paved the junction with grass: the
 * junction bucket is what discriminates the rule.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsGrassRoadSlotsTest,
	"Airside.Build.GrassRoadSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsGrassRoadSlotsTest::RunTest(const FString& Parameters)
{
	URoadMaterialSet* Set = URoadMaterialSet::MakeTransient({
		TEXT("Asphalt"), TEXT("Concrete"), TEXT("Kerb"),
		URoadMaterialSet::RunwaySlotName(ERunwaySurface::Grass),
		URoadMaterialSet::RunwaySlotName(ERunwaySurface::Tarmac),
		URoadMaterialSet::RunwaySlotName(ERunwaySurface::Concrete) });
	const int32 GrassSlot = Set->IndexOf(URoadMaterialSet::RunwaySlotName(ERunwaySurface::Grass));

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	const FRoadNodeId W = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId X = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadNodeId S = Net->AddNode(FVector2D(50000.0, -30000.0));
	const FRoadSegmentId West = Net->AddStraightSegment(W, X, Taxiway);
	const FRoadSegmentId East = Net->AddStraightSegment(X, E, Taxiway);
	const FRoadSegmentId South = Net->AddStraightSegment(X, S, Taxiway);
	if (!TestTrue(TEXT("the west arm has the lowest id"), West.Index < East.Index && West.Index < South.Index)) { return false; }

	auto Build = [&]()
	{
		FRoadMeshBuilder Builder(10.0, 512.0, Set);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		Builder.Build(*Net, Solved, 1);
		return RsCountIds(Builder.GetBuffers());
	};

	// 0. All tarmac: nothing is grass - the path every road took before surfaces existed.
	{
		const FRsIds Ids = Build();
		TestTrue(TEXT("the junction has triangles"), Ids.Junction.Num() > 0);
		TestFalse(TEXT("a tarmac network has no grass anywhere"),
			Ids.West.Contains(GrassSlot) || Ids.East.Contains(GrassSlot) || Ids.Junction.Contains(GrassSlot));
	}

	// 1. The west arm on grass: its whole width is the grass slot, the rest is untouched.
	TestTrue(TEXT("the west arm takes grass"), Net->SetSegmentSurface(West, ERoadSurface::Grass));
	{
		const FRsIds Ids = Build();
		TestTrue(TEXT("the grass arm has triangles"), Ids.West.Num() > 0);
		TestEqual(TEXT("every band of the grass arm, run-offs included, is one slot"), Ids.West.Num(), 1);
		TestTrue(TEXT("and it is the grass slot"), Ids.West.Contains(GrassSlot));
		TestFalse(TEXT("the tarmac arm is not grass"), Ids.East.Contains(GrassSlot));
		TestFalse(TEXT("PAVED WINS: the junction stays tarmac"), Ids.Junction.Contains(GrassSlot));
	}

	// 2. Every arm grass: the junction is grass too.
	Net->SetSegmentSurface(East, ERoadSurface::Grass);
	Net->SetSegmentSurface(South, ERoadSurface::Grass);
	{
		const FRsIds Ids = Build();
		TestTrue(TEXT("an all-grass junction is grass"), Ids.Junction.Num() == 1 && Ids.Junction.Contains(GrassSlot));
	}
	return true;
}

/**
 * NO LANE PAINT ON A GRASS ROAD. The same two-way road paints dashes on tarmac and none on
 * grass - the tarmac half is what proves the builder would have painted it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsGrassRoadUnpaintedTest,
	"Airside.Build.GrassRoadUnpainted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsGrassRoadUnpaintedTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Road = TestGraph::Lay(*Net, Net->AddNode(FVector2D(0.0, 0.0)),
		Net->AddNode(FVector2D(20000.0, 0.0)), URoadProfile::MakeServiceRoadTransient());
	FRoadNetworkSolver::SolveAll(*Net);

	FRoadMeshBuffers OnTarmac;
	const int32 TarmacDashes = FRoadLaneMarkingBuilder::Build(*Net, 1.0, OnTarmac);
	if (!TestTrue(TEXT("a tarmac two-way road is painted"), TarmacDashes > 0)) { return false; }

	TestTrue(TEXT("the road takes grass"), Net->SetSegmentSurface(Road, ERoadSurface::Grass));
	FRoadMeshBuffers OnGrass;
	int32 Segments = -1;
	TestEqual(TEXT("the same road on grass is not"), FRoadLaneMarkingBuilder::Build(*Net, 1.0, OnGrass, &Segments), 0);
	TestEqual(TEXT("and no segment is counted as painted"), Segments, 0);
	return true;
}

/**
 * AN AIRCRAFT THAT NEEDS PAVEMENT IS NOT ROUTED OVER A GRASS TAXIWAY - and one that does not,
 * or a vehicle, still is. A single line W-M-E with only W-M on grass: the gate is the only
 * thing that can separate the answers. Repaving W-M opens it again, so the refusal is the
 * surface and not something else about the graph.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsGrassRouteGateTest,
	"Airside.Model.RouteGrassGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsGrassRouteGateTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	const FRoadNodeId W = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId M = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadSegmentId Grassy = TestGraph::Lay(*Net, W, M, Taxiway);
	const FRoadSegmentId Paved = TestGraph::Lay(*Net, M, E, Taxiway);
	TestTrue(TEXT("the west half takes grass"), Net->SetSegmentSurface(Grassy, ERoadSurface::Grass));
	TestGraph::Derive(*Net);

	auto Find = [&](ERunwaySurface Needs, ETraversalClass Class)
	{
		const FGuidelineNodeId A = TestGraph::NodeFor(*Net, Grassy, true);
		const FGuidelineNodeId B = TestGraph::NodeFor(*Net, Paved, false);
		return RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, A, B, 0.0, Class).NeedsSurface(Needs));
	};

	if (!TestTrue(TEXT("a grass-capable aircraft taxis the grass"), Find(ERunwaySurface::Grass, ETraversalClass::Aircraft).IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("an aircraft that needs tarmac is refused it"), Find(ERunwaySurface::Tarmac, ETraversalClass::Aircraft).IsValid());
	{
		// NO NeedsSurface AT ALL - every query written before grass existed, a vehicle's included.
		const FRoutePlan Unset = RouteSearch::Find(*Net, FRouteQuery::For(ERouteErrand::GraphProbe,
			TestGraph::NodeFor(*Net, Grassy, true), TestGraph::NodeFor(*Net, Paved, false), 0.0, ETraversalClass::Aircraft));
		TestTrue(TEXT("a query that states no need gates nothing"), Unset.IsValid());
	}

	TestTrue(TEXT("the west half is repaved"), Net->SetSegmentSurface(Grassy, ERoadSurface::Tarmac));
	TestGraph::Derive(*Net);
	TestTrue(TEXT("and the tarmac aircraft routes again"), Find(ERunwaySurface::Tarmac, ETraversalClass::Aircraft).IsValid());

	TestFalse(TEXT("a runway's own Surface field is never written"), Net->SetSegmentSurface(
		TestGraph::Lay(*Net, Net->AddNode(FVector2D(0.0, 50000.0)), Net->AddNode(FVector2D(150000.0, 50000.0)), TestProfiles::Runway()),
		ERoadSurface::Grass));
	return true;
}

#endif
