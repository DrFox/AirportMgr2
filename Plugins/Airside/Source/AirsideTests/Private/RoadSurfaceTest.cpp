#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/BuildCost.h"
#include "Build/RoadLaneMarkingBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Entities/EntityDefinition.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadSurfacePresenter.h"
#include "Profiles/RoadMaterialSet.h"
#include "Profiles/RoadProfile.h"
#include "StandFixture.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadDrawTool.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/CoreRedirects.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Rs prefix: these test files share one translation unit (unity build).

	/** Transient taxiway widths and nothing else - FNullEditTarget's inert rest (#189). */
	struct FRsWidthTarget : FNullEditTarget
	{
		TArray<URoadProfile*> Widths;

		/** What ResolveProfileFor answers when there are no widths - the level's own profile,
		 *  whose AllowedPavements the surface row then reads (the real actor always has one). */
		URoadProfile* LevelDefault = nullptr;

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
			return Widths.Num() == 0 ? LevelDefault
				: ResolveWidthProfile(Kind, WidthIndex == INDEX_NONE ? 0 : WidthIndex);
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

	/** Where Text sits as a serialised ANSI FString (int32 length incl. the null, then the
	 *  bytes), searching [From, To). The LAST match when bLast. INDEX_NONE when absent. */
	int32 RsFindSerialisedString(const TArray<uint8>& Bytes, const char* Text, int32 From, int32 To, bool bLast)
	{
		const int32 Len = FCStringAnsi::Strlen(Text) + 1;
		int32 Found = INDEX_NONE;
		for (int32 At = From; At + 4 + Len <= To; ++At)
		{
			int32 Prefix = 0;
			FMemory::Memcpy(&Prefix, &Bytes[At], 4);
			if (Prefix == Len && FMemory::Memcmp(&Bytes[At + 4], Text, Len) == 0)
			{
				Found = At;
				if (!bLast) { break; }
			}
		}
		return Found;
	}

	/** Replace the serialised FString at At (which spells OldLen bytes incl. the null) with Text. */
	void RsSpliceString(TArray<uint8>& Bytes, int32 At, int32 OldLen, const char* Text)
	{
		const int32 Len = FCStringAnsi::Strlen(Text) + 1;
		TArray<uint8> Fresh;
		Fresh.SetNumUninitialized(4 + Len);
		FMemory::Memcpy(Fresh.GetData(), &Len, 4);
		FMemory::Memcpy(Fresh.GetData() + 4, Text, Len);
		Bytes.RemoveAt(At, 4 + OldLen);
		Bytes.Insert(Fresh, At);
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

	// 1. THE ROW: after Width, grass then tarmac - SCALE ORDER, though every road asset lists
	// tarmac first (Pavement::Offered sorts, 2026-09-28) - with GRASS lit, the cheap start.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		const int32 Row = RsAxisIndex(Axes, TEXT("Surface"));
		// Row 2: Mode (strip stage 6), Width, then Surface.
		if (!TestEqual(TEXT("the surface row follows the width row"), Row, 2)) { return false; }
		if (!TestEqual(TEXT("two options"), Axes[Row].Options.Num(), 2)) { return false; }
		TestEqual(TEXT("grass first, as on the runway and stand rows"), Axes[Row].Options[0].Label.ToString(), FString(TEXT("grass")));
		TestEqual(TEXT("tarmac second"), Axes[Row].Options[1].Label.ToString(), FString(TEXT("tarmac")));
		TestEqual(TEXT("grass lit on a fresh tool"), Axes[Row].Current, 0);
		TestEqual(TEXT("and it is what a click would lay"), Tool.GetSurface(), EPavement::Grass);
	}

	// 2. A PICK SETS THE SURFACE, and leaves the width alone.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestTrue(TEXT("picking tarmac is accepted"), Tool.SelectVariant(Context, 2, 1));
		TestEqual(TEXT("the next click lays tarmac"), Tool.GetSurface(), EPavement::Tarmac);
		TestEqual(TEXT("the width was not touched - still the narrowest"), Tool.GetWidthIndex(), 0);
		TArray<FToolVariantAxis> Axes;
		Tool.GetVariantAxes(Context, Axes);
		TestEqual(TEXT("and tarmac is lit"), Axes[2].Current, 1);
	}

	// 3. SHIFT+KEY STEPS THE SURFACE; the plain key still steps the width.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		FToolContext Shift = Context;
		Shift.bInsertModifier = true;
		Tool.OnReselect(Shift);
		TestEqual(TEXT("Shift+key steps grass to tarmac"), Tool.GetSurface(), EPavement::Tarmac);
		TestEqual(TEXT("and not the width"), Tool.GetWidthIndex(), 0);
		Tool.OnReselect(Shift);
		TestEqual(TEXT("then wraps back to grass"), Tool.GetSurface(), EPavement::Grass);
		Tool.OnReselect(Context);
		TestEqual(TEXT("the plain key steps the width, narrowest to the next"), Tool.GetWidthIndex(), 1);
		TestEqual(TEXT("and not the surface"), Tool.GetSurface(), EPavement::Grass);
	}

	// 4. NO WIDTHS: surface is row 1 (after Mode), and a pick on it means surface - the Id, not the index.
	{
		FRsWidthTarget Empty;
		Empty.LevelDefault = TestProfiles::Taxiway();
		FToolContext EmptyContext;
		EmptyContext.Target = &Empty;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestTrue(TEXT("row 1 is surface when there are no widths"), Tool.SelectVariant(EmptyContext, 1, 1));
		TestEqual(TEXT("so the pick lays tarmac"), Tool.GetSurface(), EPavement::Tarmac);
		FToolContext Shift = EmptyContext;
		Shift.bInsertModifier = true;
		Tool.OnReselect(Shift);
		TestEqual(TEXT("and Shift+key still steps it with no widths to cycle"), Tool.GetSurface(), EPavement::Grass);
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
	// FROM TARMAC, so the Shift pick below changes something: grass is a fresh tool's default
	// since 2026-09-28, and a pick of what is already chosen would pass without reaching the road.
	Tool.RestoreSurface(EPavement::Tarmac);
	{
		FToolContext Pick = TestTool::ContextAt(*Actor, FVector2D::ZeroVector);
		Pick.bInsertModifier = true;
		Tool.OnReselect(Pick);
		if (!TestEqual(TEXT("Shift+key picked grass"), Tool.GetSurface(), EPavement::Grass)) { return false; }
	}
	Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(0.0, 0.0)));
	Tool.OnClick(TestTool::ContextAt(*Actor, FVector2D(40000.0, 0.0)));
	if (!TestNotNull(TEXT("the actor made a network"), Actor->Network.Get())) { return false; }
	const TArray<FRoadSegment>& Laid = Actor->Network->GetSegments();
	if (!TestEqual(TEXT("one segment was laid"), Laid.Num(), 1)) { return false; }
	TestEqual(TEXT("and it lies on grass"), Laid[0].Surface, EPavement::Grass);
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
	const FBuildQuote Tarmac = Actor->QuoteForConnect(From, FVector2D(10000.0, 20000.0), ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac);
	const FBuildQuote Grass = Actor->QuoteForConnect(From, FVector2D(10000.0, 20000.0), ERoadKind::Taxiway, INDEX_NONE, EPavement::Grass);
	TestEqual(TEXT("grass quotes at the grass factor of tarmac"), Grass.BaseAmount(), Tarmac.BaseAmount() * Pavement::RateFactor(EPavement::Grass), 1e-6);
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

	const double Tarmac = BuildCost::ForSegment(*Profile, 10000.0).BaseAmount();
	const double Grass = BuildCost::ForSegment(*Profile, 10000.0, EPavement::Grass).BaseAmount();
	TestEqual(TEXT("100 m of tarmac at 100 a metre"), Tarmac, 10000.0, 1e-6);
	TestEqual(TEXT("the same on grass costs the grass factor of it"), Grass, 10000.0 * Pavement::RateFactor(EPavement::Grass), 1e-6);
	TestTrue(TEXT("and the factor is a discount"), Pavement::RateFactor(EPavement::Grass) < 1.0);

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Seg = TestGraph::Lay(*Net, Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(10000.0, 0.0)), Profile);
	const double TarmacUpkeep = BuildCost::DailyUpkeep(*Net, 0.0);
	TestTrue(TEXT("the segment takes grass"), Net->SetSegmentSurface(Seg, EPavement::Grass));
	const double GrassUpkeep = BuildCost::DailyUpkeep(*Net, 0.0);
	TestEqual(TEXT("upkeep on tarmac"), TarmacUpkeep, 200.0, 1e-6);
	TestEqual(TEXT("upkeep on grass takes the same factor the build did"), GrassUpkeep, 200.0 * Pavement::RateFactor(EPavement::Grass), 1e-6);
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
		URoadMaterialSet::RunwaySlotName(EPavement::Grass),
		URoadMaterialSet::RunwaySlotName(EPavement::Tarmac),
		URoadMaterialSet::RunwaySlotName(EPavement::Concrete) });
	const int32 GrassSlot = Set->IndexOf(URoadMaterialSet::RunwaySlotName(EPavement::Grass));

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
	TestTrue(TEXT("the west arm takes grass"), Net->SetSegmentSurface(West, EPavement::Grass));
	{
		const FRsIds Ids = Build();
		TestTrue(TEXT("the grass arm has triangles"), Ids.West.Num() > 0);
		TestEqual(TEXT("every band of the grass arm, run-offs included, is one slot"), Ids.West.Num(), 1);
		TestTrue(TEXT("and it is the grass slot"), Ids.West.Contains(GrassSlot));
		TestFalse(TEXT("the tarmac arm is not grass"), Ids.East.Contains(GrassSlot));
		TestFalse(TEXT("PAVED WINS: the junction stays tarmac"), Ids.Junction.Contains(GrassSlot));
	}

	// 2. Every arm grass: the junction is grass too.
	Net->SetSegmentSurface(East, EPavement::Grass);
	Net->SetSegmentSurface(South, EPavement::Grass);
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

	TestTrue(TEXT("the road takes grass"), Net->SetSegmentSurface(Road, EPavement::Grass));
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
	TestTrue(TEXT("the west half takes grass"), Net->SetSegmentSurface(Grassy, EPavement::Grass));
	TestGraph::Derive(*Net);

	auto Find = [&](EPavement Needs, ETraversalClass Class)
	{
		const FGuidelineNodeId A = TestGraph::NodeFor(*Net, Grassy, true);
		const FGuidelineNodeId B = TestGraph::NodeFor(*Net, Paved, false);
		return RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, A, B, 0.0, Class).NeedsPavement(Needs));
	};

	if (!TestTrue(TEXT("a grass-capable aircraft taxis the grass"), Find(EPavement::Grass, ETraversalClass::Aircraft).IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("an aircraft that needs tarmac is refused it"), Find(EPavement::Tarmac, ETraversalClass::Aircraft).IsValid());
	{
		// NO NeedsPavement AT ALL - every query written before grass existed, a vehicle's included.
		const FRoutePlan Unset = RouteSearch::Find(*Net, FRouteQuery::For(ERouteErrand::GraphProbe,
			TestGraph::NodeFor(*Net, Grassy, true), TestGraph::NodeFor(*Net, Paved, false), 0.0, ETraversalClass::Aircraft));
		TestTrue(TEXT("a query that states no need gates nothing"), Unset.IsValid());
	}

	TestTrue(TEXT("the west half is repaved"), Net->SetSegmentSurface(Grassy, EPavement::Tarmac));
	TestGraph::Derive(*Net);
	TestTrue(TEXT("and the tarmac aircraft routes again"), Find(EPavement::Tarmac, ETraversalClass::Aircraft).IsValid());

	TestFalse(TEXT("a runway's own Surface field is never written"), Net->SetSegmentSurface(
		TestGraph::Lay(*Net, Net->AddNode(FVector2D(0.0, 50000.0)), Net->AddNode(FVector2D(150000.0, 50000.0)), TestProfiles::Runway()),
		EPavement::Grass));
	return true;
}

/**
 * THE GATE IS THE SHARED COMPARISON, not a grass special case: a taxiway whose pavement is
 * below the aircraft's need is refused whatever that pavement is. #356's gate tested
 * "IsGrassRoad". Since R12 the need is clamped to tarmac, the strongest a taxiway offers, so a
 * concrete-needing jet taxis on tarmac and is refused grass. Same W-M-E line as
 * RouteGrassGate - only the need and the west half's pavement differ, so the gate is the only
 * thing that can separate the answers.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsPavementRouteGateTest,
	"Airside.Model.RoutePavementGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsPavementRouteGateTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Taxiway = TestProfiles::Taxiway();
	const FRoadNodeId W = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId M = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadSegmentId West = TestGraph::Lay(*Net, W, M, Taxiway);
	const FRoadSegmentId Paved = TestGraph::Lay(*Net, M, E, Taxiway);
	TestGraph::Derive(*Net);
	if (!TestEqual(TEXT("the west half is tarmac"), Net->PavementOf(West), EPavement::Tarmac)) { return false; }

	auto Find = [&](EPavement Needs)
	{
		const FGuidelineNodeId A = TestGraph::NodeFor(*Net, West, true);
		const FGuidelineNodeId B = TestGraph::NodeFor(*Net, Paved, false);
		return RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, A, B, 0.0, ETraversalClass::Aircraft).NeedsPavement(Needs));
	};

	// R12: THE TAXIING NEED IS CLAMPED TO TARMAC, the strongest a taxiway can offer - real
	// heavies taxi on asphalt, and unclamped a concrete-needing type could reach no stand.
	TestTrue(TEXT("a concrete-needing jet is admitted to a tarmac taxiway (need clamped to tarmac)"),
		Find(EPavement::Concrete).IsValid());
	TestTrue(TEXT("and so is a tarmac-needing one"), Find(EPavement::Tarmac).IsValid());

	// AND STILL REFUSED GRASS: the clamp lowers the need to tarmac, never to nothing.
	if (!TestTrue(TEXT("the west half goes to grass"), Net->SetSegmentSurface(West, EPavement::Grass))) { return false; }
	TestGraph::Derive(*Net);
	TestFalse(TEXT("a concrete-needing jet is refused a grass taxiway"), Find(EPavement::Concrete).IsValid());
	TestFalse(TEXT("and so is a tarmac-needing one"), Find(EPavement::Tarmac).IsValid());
	if (!TestTrue(TEXT("the west half goes back to tarmac"), Net->SetSegmentSurface(West, EPavement::Tarmac))) { return false; }
	TestGraph::Derive(*Net);

	// THE PROFILE'S LIST GATES THE WRITE: a taxiway profile offers tarmac and grass, so a
	// concrete taxiway is refused by the network as well as never offered by the row.
	AddExpectedMessage(TEXT("SetSegmentSurface refused: concrete is not offered by profile"),
		EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("a taxiway profile does not offer concrete"), Net->SetSegmentSurface(West, EPavement::Concrete));
	TestEqual(TEXT("and the refused write left it tarmac"), Net->PavementOf(West), EPavement::Tarmac);

	// ONE ANSWER FOR RUNWAYS TOO: a strip's pavement is its facts', not its Surface field.
	const FRoadSegmentId Strip = TestGraph::Lay(*Net, Net->AddNode(FVector2D(0.0, 50000.0)),
		Net->AddNode(FVector2D(150000.0, 50000.0)), TestProfiles::Runway());
	FRunwayFacts Concrete;
	Concrete.Surface = EPavement::Concrete;
	if (!TestTrue(TEXT("the strip takes concrete facts"), Net->SetRunwayFacts(Strip, Concrete))) { return false; }
	TestEqual(TEXT("PavementOf reads a runway's facts"), Net->PavementOf(Strip), EPavement::Concrete);
	TestFalse(TEXT("and a concrete runway is never a grass road"), Net->IsGrassRoad(Strip));
	return true;
}

/**
 * A GRASS ROAD SAVED UNDER #356'S ENUM LOADS AS GRASS. ERoadSurface was Tarmac=0, Grass=1 and
 * EPavement is Grass=0, Tarmac=1, so a load by NUMBER would turn every saved grass road into
 * tarmac; this proves the load is by NAME. No level on disk held a grass road on 2026-09-27
 * (grep of Content for ERoadSurface: none), so the old bytes are made here: a segment is
 * tagged-serialised, and its Surface tag's enum name and value are respelt as #356 wrote
 * them. The Tarmac respelling is the control - it proves the load reads the patched value
 * rather than something left over.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsRoadSurfaceSavedByNameTest,
	"Airside.Model.RoadSurfaceSavedByName",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsRoadSurfaceSavedByNameTest::RunTest(const FString& Parameters)
{
	// THE REDIRECT IS REGISTERED - DefaultEngine.ini's entry, read by the running engine.
	const FCoreRedirectObjectName Redirected = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Enum,
		FCoreRedirectObjectName(TEXT("/Script/Airside.ERoadSurface")));
	TestEqual(TEXT("ERoadSurface redirects to EPavement"), Redirected.ToString(), FString(TEXT("/Script/Airside.EPavement")));

	// Bytes #356 would have written for a segment on OldValue ("ERoadSurface::Grass").
	auto OldBytes = [this](const char* OldValue, TArray<uint8>& Out) -> bool
	{
		FRoadSegment Saved;
		Saved.Surface = EPavement::Grass;
		Out.Reset();
		FMemoryWriter Writer(Out);
		FObjectAndNameAsStringProxyArchive WriteAr(Writer, false);
		FRoadSegment::StaticStruct()->SerializeItem(WriteAr, &Saved, nullptr);

		// THE TAG LAYOUT this relies on (PropertyTag.cpp, UE 5.8): Name, Type, int32 Size,
		// uint8 Flags, value. Checked, not assumed - a layout change fails here, loudly.
		const char* NewValue = "EPavement::Grass";
		const int32 NewLen = FCStringAnsi::Strlen(NewValue) + 1;
		const int32 Value = RsFindSerialisedString(Out, NewValue, 0, Out.Num(), false);
		if (!TestTrue(TEXT("the grass value is in the bytes"), Value >= 5)) { return false; }
		int32 Size = 0;
		FMemory::Memcpy(&Size, &Out[Value - 5], 4);
		if (!TestEqual(TEXT("the tag's Size sits five bytes before the value"), Size, 4 + NewLen)) { return false; }
		const int32 TypeAt = RsFindSerialisedString(Out, "EPavement", 0, Value - 5, true);
		if (!TestTrue(TEXT("the tag names its enum"), TypeAt != INDEX_NONE)) { return false; }

		// Value first, it lies later in the buffer, so the type's offset stays good.
		RsSpliceString(Out, Value, NewLen, OldValue);
		const int32 OldSize = 4 + FCStringAnsi::Strlen(OldValue) + 1;
		FMemory::Memcpy(&Out[Value - 5], &OldSize, 4);
		RsSpliceString(Out, TypeAt, FCStringAnsi::Strlen("EPavement") + 1, "ERoadSurface");
		return true;
	};

	auto Load = [](const TArray<uint8>& Bytes, EPavement Before)
	{
		FRoadSegment Loaded;
		Loaded.Surface = Before;
		FMemoryReader Reader(Bytes);
		FObjectAndNameAsStringProxyArchive ReadAr(Reader, false);
		FRoadSegment::StaticStruct()->SerializeItem(ReadAr, &Loaded, nullptr);
		return Loaded.Surface;
	};

	TArray<uint8> Grass;
	if (!OldBytes("ERoadSurface::Grass", Grass)) { return false; }
	TestTrue(TEXT("the respelt bytes carry the old enum's name"),
		RsFindSerialisedString(Grass, "ERoadSurface", 0, Grass.Num(), false) != INDEX_NONE);
	TestEqual(TEXT("an old grass road loads as grass, by name - not as index 1, which is now tarmac"),
		Load(Grass, EPavement::Tarmac), EPavement::Grass);

	TArray<uint8> Tarmac;
	if (!OldBytes("ERoadSurface::Tarmac", Tarmac)) { return false; }
	TestEqual(TEXT("CONTROL: the old tarmac spelling loads as tarmac, so the patched value is what is read"),
		Load(Tarmac, EPavement::Grass), EPavement::Tarmac);
	return true;
}

/**
 * A STAND'S PAD IS DRAWN WITH ITS PAVEMENT (shared-pavement Task 8, Step 7): a grass pad's
 * triangles carry the grass runway's slot - the one SurfaceSlotFor gives a grass road, so a
 * grass stand beside a grass taxiway is one field - and a tarmac pad's and a bare apron's carry
 * slot 0, the apron layer's own material, exactly as before stands had a pavement.
 *
 * TWO LEVELS. The builder half measures the rule on FRoadMeshBuilder::AddNetworkAprons, the
 * loop the presenter runs; the actor half measures the WIRING - that the apron component was
 * handed a material per slot and the grass pad's id resolves to the same material the road
 * layer's grass slot has, not the floor checker a UDynamicMeshComponent shows for a missing one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRsStandPadSlotsTest,
	"Airside.Build.StandPadSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRsStandPadSlotsTest::RunTest(const FString& Parameters)
{
	const FName GrassName = URoadMaterialSet::RunwaySlotName(EPavement::Grass);

	// --- 1. THE BUILDER: each pad takes its own pavement's slot -------------------------------
	{
		URoadMaterialSet* Set = URoadMaterialSet::MakeTransient({
			TEXT("Apron"),
			URoadMaterialSet::RunwaySlotName(EPavement::Grass),
			URoadMaterialSet::RunwaySlotName(EPavement::Tarmac),
			URoadMaterialSet::RunwaySlotName(EPavement::Concrete) });
		const int32 GrassSlot = Set->IndexOf(GrassName);
		if (!TestTrue(TEXT("the premise: grass is not slot 0"), GrassSlot > 0)) { return false; }

		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
		auto PlacePad = [&](double X, EPavement P)
		{
			const FEntityInstanceId Id = ServiceLinkFixture::PlaceStand(*Net, *Def, FVector2D(X, 0.0), 0.0);
			FRoadNetworkTestAccess Access(*Net);
			Access.SetEntityOutlineForTest(Id, { {X,0}, {X + 5000,0}, {X + 5000,3950}, {X,3950} });
			Access.SetEntityPavementForTest(Id, P);
		};
		PlacePad(0.0, EPavement::Grass);        // x in [0, 5000]
		PlacePad(20000.0, EPavement::Tarmac);   // x in [20000, 25000]
		FApronSurface Bare;
		Bare.Outline = { {40000,0}, {45000,0}, {45000,4000}, {40000,4000} };
		Net->AddApron(MoveTemp(Bare));          // x in [40000, 45000]

		FRoadMeshBuilder Builder(10.0, 512.0, Set);
		TestEqual(TEXT("two pads and an apron are built"), Builder.AddNetworkAprons(*Net), 3);

		TSet<int32> GrassPad, TarmacPad, BareApron;
		const FRoadMeshBuffers& Buffers = Builder.GetBuffers();
		for (int32 Triangle = 0; Triangle * 3 + 2 < Buffers.Indices.Num(); ++Triangle)
		{
			const double X = (Buffers.Positions[Buffers.Indices[Triangle * 3]].X
				+ Buffers.Positions[Buffers.Indices[Triangle * 3 + 1]].X
				+ Buffers.Positions[Buffers.Indices[Triangle * 3 + 2]].X) / 3.0;
			const int32 Id = Buffers.MaterialIDs.IsValidIndex(Triangle) ? Buffers.MaterialIDs[Triangle] : -1;
			(X < 10000.0 ? GrassPad : X < 30000.0 ? TarmacPad : BareApron).Add(Id);
		}
		TestTrue(TEXT("a grass pad's triangles all carry the grass slot"),
			GrassPad.Num() == 1 && GrassPad.Contains(GrassSlot));
		TestTrue(TEXT("a tarmac pad's carry slot 0, the apron material it always had"),
			TarmacPad.Num() == 1 && TarmacPad.Contains(0));
		TestTrue(TEXT("and so does a bare apron"), BareApron.Num() == 1 && BareApron.Contains(0));
	}

	// --- 2. THE ACTOR: the apron component resolves every slot it was handed ---------------
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	UMaterialInterface* ApronMat = UMaterial::GetDefaultMaterial(MD_PostProcess);
	Actor->ApronMaterial = ApronMat;
	Actor->ClearNetwork();

	IRoadEditTarget* Target = Actor;
	const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
	if (!TestTrue(TEXT("a grass B stand is placed"),
		Target->PlaceStandInPlot(Pad, Pad[0], Pad[1], EPavement::Grass) != INDEX_NONE)) { return false; }
	Actor->RebuildMesh();

	URoadSurfacePresenter* Presenter = Actor->GetPresenter();
	UDynamicMeshComponent* Apron = Presenter != nullptr ? Presenter->GetLayerComponentForTest(ESurfaceLayer::Apron) : nullptr;
	const URoadMaterialSet* Road = Presenter != nullptr ? Presenter->EffectiveMaterialSetForTest() : nullptr;
	if (!TestNotNull(TEXT("an apron component"), Apron) || !TestNotNull(TEXT("a road material set"), Road)) { return false; }

	const UE::Geometry::FDynamicMesh3& Mesh = Apron->GetDynamicMesh()->GetMeshRef();
	const UE::Geometry::FDynamicMeshMaterialAttribute* Ids =
		Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;
	if (!TestNotNull(TEXT("the apron mesh carries material ids"), Ids)) { return false; }
	TSet<int32> Seen;
	for (const int32 Tri : Mesh.TriangleIndicesItr()) { Seen.Add(Ids->GetValue(Tri)); }
	if (!TestEqual(TEXT("the grass pad is one slot"), Seen.Num(), 1)) { return false; }
	const int32 PadId = *Seen.CreateConstIterator();

	TArray<UMaterialInterface*> RoadMaterials;
	Road->ResolveMaterials(RoadMaterials);
	const int32 RoadGrass = Road->IndexOf(GrassName);
	if (!TestTrue(TEXT("the road layer has a grass slot"), RoadMaterials.IsValidIndex(RoadGrass))) { return false; }
	TestTrue(TEXT("the pad is not on slot 0"), PadId != 0);
	TestEqual(TEXT("the grass pad draws with the road layer's own grass material - one table, not two"),
		Apron->GetMaterial(PadId), RoadMaterials[RoadGrass]);
	TestEqual(TEXT("and slot 0 is still the apron material"), Apron->GetMaterial(0), ApronMat);
	return true;
}

#endif
