#include "CoreMinimal.h"
#include "Build/DepotKit.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"
#include "YardAgreement.h"

#if WITH_DEV_AUTOMATION_TESTS

// AIRSIDE'S HALF OF A SHED PURCHASE (facility-upgrades spec §3): the mutator, the one solve that says
// how many slots a placed plot reserves, and the facade door that relights the yard and clears undo.

namespace
{
	/** 50 x 24 m: room for more than the start kit's one shed under the band layout (memory: the 12 x 8 m
	 *  plot seats nothing). Prefixed: unity build. */
	TArray<FVector2D> FacilityModuleWidePlot()
	{
		return { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
	}

	FEntityPlacement FacilityModulePlacement(UEntityDefinition* Depot, const TArray<FVector2D>& Plot)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = (Plot[0] + Plot[1]) * 0.5;
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = Plot;
		Placement.FrontageEdge = 0;   // the frontage is edge 0, Position its midpoint - see FEntityInstance::FrontageEdge
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Placement;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityModulesAddTest, "Airside.Model.EntityModules.AddAppendsToADepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FEntityModulesAddTest::RunTest(const FString&)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const FEntityInstanceId Depot = Net->PlaceEntity(FacilityModulePlacement(UEntityDefinition::MakeFuelDepotTransient(), Plot));
	TestTrue(TEXT("a shed is added to a live depot"), Net->AddEntityModule(Depot, EDepotModule::Shed));
	const FEntityInstance* After = Net->GetEntity(Depot);
	if (!TestNotNull(TEXT("the depot is still there"), After)) { return false; }
	TestEqual(TEXT("four modules now"), After->Modules.Num(), 4);
	TestEqual(TEXT("APPENDED - bay order is the fill order the presenter lights by"),
		static_cast<int32>(After->Modules.Last()), static_cast<int32>(EDepotModule::Shed));

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	TestFalse(TEXT("a stand has no bays to add to"), Net->AddEntityModule(Stand, EDepotModule::Shed));
	TestFalse(TEXT("an unset handle takes nothing"), Net->AddEntityModule(FEntityInstanceId(), EDepotModule::Shed));
	Net->RemoveEntity(Depot);
	TestFalse(TEXT("a removed depot takes nothing"), Net->AddEntityModule(Depot, EDepotModule::Shed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepotKitReservationOfTest, "Airside.Build.DepotKit.ReservationOfIsThePresentersSolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepotKitReservationOfTest::RunTest(const FString&)
{
	// ONE SOLVE: what the purchase rules call a free slot is exactly what the presenter draws ghosted.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const FEntityInstanceId Depot = Actor->Network->PlaceEntity(FacilityModulePlacement(UEntityDefinition::MakeFuelDepotTransient(), Plot));
	Actor->RebuildMesh();

	const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(*Actor->Network->GetEntity(Depot), Actor->ResolveDepotKits());
	if (!TestTrue(TEXT("a plotted depot has a reservation"), Reserved.IsSet())) { return false; }
	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Reserved->Stands) { Bays += Stand.RunLength; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	TestEqual(TEXT("its bays are every bay the presenter drew, lit or ghosted"), Bays, Plots->GetModuleCount() + Plots->GetGhostCount());

	UEntityDefinition* Plain = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Plotless = Actor->Network->PlaceEntity(Plain, Plain->Anchors, FVector2D(30000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);
	TestFalse(TEXT("a plotless depot reserves nothing - there is no ground to solve"),
		DepotKit::ReservationOf(*Actor->Network->GetEntity(Plotless), Actor->ResolveDepotKits()).IsSet());
	return true;
}

/**
 * A BUILT DEPOT'S YARD IS SOLVED FROM THE FRONTAGE IT STORES, NOT FROM A GUESS (#450).
 *
 * DepotKit::ReservationOf used to RECOVER the frontage - the edge whose midpoint was nearest Position - which is exact only while
 * Position stays the midpoint of the edge the gesture chose. The facade now stores the edge it was given
 * (FEntityInstance::FrontageEdge), so this places a depot whose POSITION is the midpoint of edge 0 but whose stored frontage is the
 * FAR edge 2: the old heuristic would solve edge 0 and face the yard at the road, the stored edge solves edge 2. Unset stores nothing
 * to solve from, and an index that names no edge is the same answer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepotKitReservationOfReadsStoredFrontageTest, "Airside.Build.DepotKit.ReservationOfReadsTheStoredFrontage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepotKitReservationOfReadsStoredFrontageTest::RunTest(const FString&)
{
	UEntityDefinition* Definition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();

	const auto PlaceWithFrontageEdge = [&](int32 FrontageEdge)
	{
		FEntityPlacement Placement = FacilityModulePlacement(Definition, Plot);   // Position: the midpoint of edge 0
		Placement.FrontageEdge = FrontageEdge;
		return Net->PlaceEntity(Placement);
	};

	// THE PREMISE: the two edges solve to different yards - else "it read edge 2" and "it read edge 0" are the same answer.
	const PlotYard::FReservation Edge0 = DepotKit::SolveYard(Plot, Plot[0], Plot[1], Definition, Specs);
	const PlotYard::FReservation Edge2 = DepotKit::SolveYard(Plot, Plot[2], Plot[3], Definition, Specs);
	if (!TestTrue(TEXT("the premise: both frontages reserve something"), Edge0.Stands.Num() > 0 && Edge2.Stands.Num() > 0)) { return false; }
	if (!TestNotEqual(TEXT("and the near edge and the far edge solve different yards"), YardAgreement::Difference(Edge0, Edge2), FString())) { return false; }

	const FEntityInstanceId FarFrontage = PlaceWithFrontageEdge(2);
	const TOptional<PlotYard::FReservation> Far = DepotKit::ReservationOf(*Net->GetEntity(FarFrontage), Specs);
	if (!TestTrue(TEXT("a depot storing its far edge solves"), Far.IsSet())) { return false; }
	TestEqual(TEXT("and solves THAT edge, not the one whose midpoint its Position happens to be"),
		YardAgreement::Difference(*Far, Edge2), FString());

	const FEntityInstanceId NearFrontage = PlaceWithFrontageEdge(0);
	const TOptional<PlotYard::FReservation> Near = DepotKit::ReservationOf(*Net->GetEntity(NearFrontage), Specs);
	if (!TestTrue(TEXT("a depot storing its near edge solves"), Near.IsSet())) { return false; }
	TestEqual(TEXT("and solves that one"), YardAgreement::Difference(*Near, Edge0), FString());

	const FEntityInstanceId NoFrontage = PlaceWithFrontageEdge(INDEX_NONE);
	TestFalse(TEXT("a depot with no stored frontage has nothing to solve from - the answer a plotless depot gives"),
		DepotKit::ReservationOf(*Net->GetEntity(NoFrontage), Specs).IsSet());
	const FEntityInstanceId OutOfRange = PlaceWithFrontageEdge(Plot.Num());
	TestFalse(TEXT("an index that names no edge of its outline is the same answer, not a read past the array"),
		DepotKit::ReservationOf(*Net->GetEntity(OutOfRange), Specs).IsSet());
	return true;
}

/**
 * A DEPOT SAVED BEFORE THE FRONTAGE WAS STORED KEEPS ITS YARD (#450).
 *
 * A level or a save written before FEntityInstance::FrontageEdge existed loads every plotted depot at INDEX_NONE, and ReservationOf
 * (which no longer searches) would solve nothing for it: no yard, no fence, no pump - on the owner's authored maps. EnsureDepotFrontages
 * is the one-shot migration, run by URoadNetwork::PostLoad for a level and by RepairLoadedNetwork for a save game (whose load is
 * Serialize alone). The legacy depot here has its POSITION on the far edge's midpoint, so "the edge whose midpoint is Position" is edge
 * 2, not the edge 0 a default would give; one that already stores an edge is left alone, and a stand and a plotless depot are untouched.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepotFrontageMigrationTest, "Airside.Model.DepotFrontageMigration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepotFrontageMigrationTest::RunTest(const FString&)
{
	UEntityDefinition* Definition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const FVector2D FarMid = (Plot[2] + Plot[3]) * 0.5;

	// A DEPOT AS A PRE-#450 LEVEL HOLDS IT: an outline and a Position, no stored frontage.
	const auto PlaceLegacy = [&](URoadNetwork& Net, int32 StoredEdge)
	{
		FEntityPlacement Placement = FacilityModulePlacement(Definition, Plot);
		Placement.Position = FarMid;
		Placement.FrontageEdge = StoredEdge;
		return Net.PlaceEntity(Placement);
	};

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FEntityInstanceId Legacy = PlaceLegacy(*Net, INDEX_NONE);
	const FEntityInstanceId AlreadyStored = PlaceLegacy(*Net, 1);   // Position on edge 2's midpoint, but it says edge 1: it is left alone
	const FEntityInstanceId Plotless = Net->PlaceEntity(Definition, Definition->Anchors, FVector2D(30000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);
	UEntityDefinition* StandDefinition = NewObject<UEntityDefinition>(GetTransientPackage());
	const FEntityInstanceId Stand = Net->PlaceEntity(StandDefinition, StandDefinition->Anchors, FVector2D(0.0, 30000.0), 0.0);
	if (!TestEqual(TEXT("the premise: a legacy plotted depot stores no frontage"), Net->GetEntity(Legacy)->FrontageEdge, static_cast<int32>(INDEX_NONE))) { return false; }
	TestFalse(TEXT("and so has nothing to solve from"), DepotKit::ReservationOf(*Net->GetEntity(Legacy), Specs).IsSet());

	// A STAND NOW STORES ITS OWN ENTRANCE FROM PLACEMENT (#450's leftover): this one got its Code C box and an edge at the door. What this test holds
	// is that the DEPOT migration does not touch it, so the edge is read before and after.
	const int32 StandEdgeBefore = Net->GetEntity(Stand)->FrontageEdge;
	TestEqual(TEXT("the migration gives exactly the one legacy plotted depot a frontage"), Net->EnsureDepotFrontages(), 1);
	TestEqual(TEXT("the edge whose midpoint is Position - the far edge, not edge 0"), Net->GetEntity(Legacy)->FrontageEdge, 2);
	TestEqual(TEXT("a depot already storing an edge keeps it"), Net->GetEntity(AlreadyStored)->FrontageEdge, 1);
	TestEqual(TEXT("a plotless depot stores none"), Net->GetEntity(Plotless)->FrontageEdge, static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("and a stand's entrance is left exactly as placement stored it"), Net->GetEntity(Stand)->FrontageEdge, StandEdgeBefore);
	TestEqual(TEXT("a second pass finds nothing to do"), Net->EnsureDepotFrontages(), 0);
	const TOptional<PlotYard::FReservation> Migrated = DepotKit::ReservationOf(*Net->GetEntity(Legacy), Specs);
	if (!TestTrue(TEXT("the migrated depot solves again"), Migrated.IsSet())) { return false; }
	TestEqual(TEXT("and solves the yard its own frontage makes"),
		YardAgreement::Difference(*Migrated, DepotKit::SolveYard(Plot, Plot[2], Plot[3], Definition, Specs)), FString());

	// THE LEVEL'S LOAD: PostLoad runs the migration (unwired, an authored map loads its depots with no yard).
	URoadNetwork* Loaded = NewObject<URoadNetwork>(GetTransientPackage());
	const FEntityInstanceId LoadedDepot = PlaceLegacy(*Loaded, INDEX_NONE);
	Loaded->PostLoad();
	TestEqual(TEXT("PostLoad stores the legacy depot's frontage"), Loaded->GetEntity(LoadedDepot)->FrontageEdge, 2);

	// A SAVE GAME'S LOAD, which is Serialize alone: the actor's repair runs the migration too.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->ClearNetwork();
	const FEntityInstanceId SavedDepot = PlaceLegacy(*Actor->Network, INDEX_NONE);
	Actor->RepairLoadedNetwork(ELoadedFrom::SaveGame);
	TestEqual(TEXT("RepairLoadedNetwork stores the legacy depot's frontage for a save game"), Actor->Network->GetEntity(SavedDepot)->FrontageEdge, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityModuleFacadeTest, "Airside.Present.Facility.ModulePurchaseRelightsAndClearsUndo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityModuleFacadeTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestNotEqual(TEXT("setup: the depot is placed"), Index, int32(INDEX_NONE))) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestTrue(TEXT("setup: placing it is an undo step"), Facade->CanUndo())) { return false; }
	const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(*Actor->Network->GetEntity(Depot), Actor->ResolveDepotKits());
	if (!TestTrue(TEXT("setup: the plot reserves a second shed"),
		Reserved.IsSet() && Reserved->CeilingFor(static_cast<int32>(EDepotModule::Shed)) >= 2)) { return false; }

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Built = Plots->GetModuleCount();
	const int32 Ghosts = Plots->GetGhostCount();
	TestTrue(TEXT("the facade takes the shed"), Facade->AddEntityModule(Depot, EDepotModule::Shed));
	TestEqual(TEXT("one more bay is lit - the Facts rebuild's announcement reached the buildings actor"), Plots->GetModuleCount(), Built + 1);
	TestEqual(TEXT("and one fewer is ghosted"), Plots->GetGhostCount(), Ghosts - 1);
	TestFalse(TEXT("R8: the purchase is a checkpoint - an undo would drop the shed and keep the money"), Facade->CanUndo());
	TestFalse(TEXT("an unset id is refused at the door"), Facade->AddEntityModule(FEntityInstanceId(), EDepotModule::Shed));
	return true;
}

/**
 * #446: A SHED PURCHASE RE-SOLVED THE WHOLE AIRPORT. AddEntityModule notified Topology, so buying one module re-derived
 * every guideline node and re-pointed every agent's route - for a write the derivation never reads. It notifies Facts
 * now: the buildings actor hears OnNetworkChanged(Facts) and lights the bay, the derived-graph pass does not run, and the
 * guideline handles the airport held before the purchase are the ones it holds after. The model's own GuidelineRevision
 * bump is what the job board hears instead (Airside.Model.EveryFactMovesTheGuidelineRevision).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityModuleNoRederiveTest, "Airside.Present.Facility.ModulePurchaseRelightsWithoutRederiving",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityModuleNoRederiveTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestNotEqual(TEXT("setup: the depot is placed"), Index, int32(INDEX_NONE))) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);

	// A ROAD FAR FROM THE PLOT, for a DERIVED guideline node: a Topology rebuild reallocates every derived node
	// (FRoadGuidelineBuilder::Build), so a handle to one outliving the purchase is the evidence nothing re-derived. The
	// depot's own pose node would not do - it is authored, and survives a Topology rebuild too.
	const int32 RoadA = Actor->PlaceNode(FVector2D(-60000.0, -60000.0));
	const int32 RoadB = Actor->PlaceNode(FVector2D(-60000.0, -40000.0));
	if (!TestTrue(TEXT("setup: a far road is laid"), Actor->ConnectNodes(RoadA, RoadB))) { return false; }
	FGuidelineNodeId Derived;
	const TArray<FGuidelineNode>& Nodes = Actor->Network->GetGuidelineNodes();
	for (int32 Node = 0; Node < Nodes.Num() && !Derived.IsSet(); ++Node)
	{
		if (Nodes[Node].bAlive && Nodes[Node].bDerived) { Derived = Actor->Network->GuidelineNodeIdAt(Node); }
	}
	if (!TestTrue(TEXT("setup: the road derived a guideline node"), Derived.IsSet())) { return false; }

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Built = Plots->GetModuleCount();
	const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
	const uint32 RevisionBefore = Actor->Network->GetGuidelineRevision();

	TestTrue(TEXT("the facade takes the shed"), Actor->GetEditFacade()->AddEntityModule(Depot, EDepotModule::Shed));
	TestEqual(TEXT("one more bay is lit - OnNetworkChanged(Facts) reached the buildings actor"), Plots->GetModuleCount(), Built + 1);
	TestEqual(TEXT("with no derived-graph pass: TopologyRebuildCountForTest did not move (#446)"),
		Actor->TopologyRebuildCountForTest(), TopologyBefore);
	TestNotNull(TEXT("so a derived handle held before the purchase is still live - the graph was not re-made"),
		Actor->Network->GetGuidelineNode(Derived));
	TestNotEqual(TEXT("and the caches that read modules were told by the model's own clock"),
		Actor->Network->GetGuidelineRevision(), RevisionBefore);
	return true;
}

/**
 * THE REPAIR'S DATA WRITE (#266): up to Count of one kind, the LAST owned first, and nothing else moves. The start kit is
 * the list's head, so a repair takes bought modules before the kit a depot was drawn with.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityModulesRemoveTest, "Airside.Model.EntityModules.RemoveTakesTheLastOfAKind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FEntityModulesRemoveTest::RunTest(const FString&)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const FEntityInstanceId Depot = Net->PlaceEntity(FacilityModulePlacement(UEntityDefinition::MakeFuelDepotTransient(), Plot));
	Net->AddEntityModule(Depot, EDepotModule::Shed);
	Net->AddEntityModule(Depot, EDepotModule::Tank);
	Net->AddEntityModule(Depot, EDepotModule::Shed);

	TestEqual(TEXT("two of the three sheds go"), Net->RemoveEntityModules(Depot, EDepotModule::Shed, 2), 2);
	const FEntityInstance* After = Net->GetEntity(Depot);
	if (!TestNotNull(TEXT("the depot is still there"), After)) { return false; }
	const TArray<EDepotModule> Expected = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump, EDepotModule::Tank };
	TestTrue(TEXT("the LAST two - the start kit's shed stays, and every other module keeps its place"), After->Modules == Expected);
	TestEqual(TEXT("asked for more than it owns, it removes what there is"), Net->RemoveEntityModules(Depot, EDepotModule::Pump, 5), 1);
	TestEqual(TEXT("a count below one removes nothing"), Net->RemoveEntityModules(Depot, EDepotModule::Tank, 0), 0);
	TestEqual(TEXT("so both tanks are still there"), Net->GetEntity(Depot)->Modules.Num(), 3);

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	TestEqual(TEXT("a stand has no modules to remove"), Net->RemoveEntityModules(Stand, EDepotModule::Shed, 1), 0);
	TestEqual(TEXT("an unset handle removes nothing"), Net->RemoveEntityModules(FEntityInstanceId(), EDepotModule::Shed, 1), 0);
	Net->RemoveEntity(Depot);
	TestEqual(TEXT("a removed depot removes nothing"), Net->RemoveEntityModules(Depot, EDepotModule::Tank, 1), 0);
	return true;
}

/**
 * THE REPAIR'S DOOR (#266): URoadEditFacade::RemoveUnseatedModules rebuilds the yard - the presenter's drop count goes to
 * zero - and leaves nothing to undo: a snapshot from before it would bring the unplaced sheds back (and the refund would
 * be paid again). Refused, changing nothing, while a drag is open. The over-owned depot is made the way only an old save
 * or a content change can make one now: modules written straight into the model past the plot's ceiling.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityUnseatedRemovalTest, "Airside.Present.Facility.UnseatedRemovalRelightsAndLeavesNoUndo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityUnseatedRemovalTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1], DepotKit::StarterModules(), EPlaceableEntity::FuelDepot);
	if (!TestNotEqual(TEXT("setup: the depot is placed"), Index, int32(INDEX_NONE))) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	URoadEditFacade* Facade = Actor->GetEditFacade();
	for (int32 I = 0; I < 20; ++I) { Actor->Network->AddEntityModule(Depot, EDepotModule::Shed); }
	Actor->RebuildMesh();
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Dropped = Plots->GetDroppedCount();
	if (!TestTrue(TEXT("setup: the plot drops some of the twenty-one sheds"), Dropped > 0)) { return false; }
	if (!TestTrue(TEXT("setup: placing it is an undo step"), Facade->CanUndo())) { return false; }

	Facade->BeginInteractiveEdit(TEXT("drag"));
	TestEqual(TEXT("with a drag open the door removes nothing"), Facade->RemoveUnseatedModules(Depot, EDepotModule::Shed, Dropped), 0);
	TestEqual(TEXT("and the depot still owns every shed"), Plots->GetDroppedCount(), Dropped);
	Facade->EndInteractiveEdit(false);
	TestTrue(TEXT("nor did it touch the history"), Facade->CanUndo());

	TestEqual(TEXT("the door removes exactly the unseated sheds"), Facade->RemoveUnseatedModules(Depot, EDepotModule::Shed, Dropped), Dropped);
	TestEqual(TEXT("the Facts rebuild's announcement reached the presenter: nothing is dropped now"), Plots->GetDroppedCount(), 0);
	TestFalse(TEXT("and there is nothing to undo - a repair is a checkpoint, not a step"), Facade->CanUndo());
	TestFalse(TEXT("nor to redo"), Facade->CanRedo());
	TestEqual(TEXT("a non-depot is refused at the door"), Facade->RemoveUnseatedModules(FEntityInstanceId(), EDepotModule::Shed, 1), 0);
	return true;
}

#endif
