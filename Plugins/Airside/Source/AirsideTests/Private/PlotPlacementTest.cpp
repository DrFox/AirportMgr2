#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/RoadGeom.h"
#include "Build/DepotKit.h"
#include "YardAgreement.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A 12 m x 8 m plot - three bays, the Tier 1 depot as drawn. */
	TArray<FVector2D> ThreeBayPlot(double X)
	{
		return { FVector2D(X, 0.0), FVector2D(X + 1200.0, 0.0),
		         FVector2D(X + 1200.0, 1200.0), FVector2D(X, 1200.0) };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPlacementCarriesOutlineTest,
	"Airside.Entities.PlotPlacementCarriesOutline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPlacementCarriesOutlineTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D::ZeroVector;
	Placement.Heading = 0.0;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = ThreeBayPlot(0.0);
	Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };

	const FEntityInstanceId Placed = Net->PlaceEntity(Placement);
	if (!TestTrue(TEXT("the depot is placed"), Placed.IsSet())) { return false; }

	const FEntityInstance* Instance = Net->GetEntity(Placed);
	if (!TestNotNull(TEXT("and readable back"), Instance)) { return false; }

	TestEqual(TEXT("the drawn outline survives placement"), Instance->Outline.Num(), 4);
	TestEqual(TEXT("and so does the chosen mix"), Instance->Modules.Num(), 3);

	// The pose is still made, and still exactly one of them however many bays the plot
	// holds - BuildFuelDepot's ruling that two lead-ins from one small building into one
	// road is a duplicate painted line.
	TestTrue(TEXT("the depot still has its one pose node"), Instance->PoseNode.IsSet());

	// THE WHOLE POINT OF THE OVERLOAD: the pre-plot signature still compiles and still
	// means what it meant. Roughly thirty callers depend on that, almost all of them
	// tests, and a forwarder nobody exercises is a forwarder that rots.
	//
	// PoseRole::Fuel IS NOW EXPLICIT (Task 6): the signature's own default is
	// EServiceRole::Aircraft, so calling it with only four arguments - as this line did before
	// Task 6 - placed Depot's definition under a STAND'S pose role, and a stand placed the
	// legacy way now gets a Code C outline at placement (Ruling 6). Naming Fuel keeps this
	// case what its comment below always meant it to be: a depot, which never gets one.
	const FEntityInstanceId Old = Net->PlaceEntity(
		Depot, Depot->Anchors, FVector2D(5000.0, 0.0), 0.0, /*DesignWingspan=*/0.0,
		EServiceRole::Fuel);
	TestTrue(TEXT("the pre-plot signature still places"), Old.IsSet());

	const FEntityInstance* Legacy = Net->GetEntity(Old);
	if (!TestNotNull(TEXT("and is readable"), Legacy)) { return false; }
	TestEqual(TEXT("with no outline, which is what a plain plop means"),
		Legacy->Outline.Num(), 0);
	TestEqual(TEXT("and no modules either"), Legacy->Modules.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStarterTrucksAreStatedTest,
	"Airside.Entities.StarterTrucksAreStated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStarterTrucksAreStatedTest::RunTest(const FString& Parameters)
{
	// TRUCKS IS THE STARTER FLEET A PLACEMENT COMES WITH (facility-upgrades spec §6), never derived
	// from the sheds any more: sheds are BAYS the player fills by buying vehicles (R2, R3).
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	auto PlaceWith = [&](const TArray<EDepotModule>& Modules, int32 Trucks, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X, 0.0);
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = ThreeBayPlot(X);
		Placement.Modules = Modules;
		Placement.Trucks = Trucks;
		return Net->GetEntity(Net->PlaceEntity(Placement));
	};

	{
		const FEntityInstance* TwoSheds = PlaceWith({ EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank }, 0, 0.0);
		if (!TestNotNull(TEXT("two sheds placed"), TwoSheds)) { return false; }
		TestEqual(TEXT("two sheds stated with no starter trucks start with none - sheds are bays, not vehicles"), TwoSheds->Trucks, 0);
	}
	{
		const FEntityInstance* Stated = PlaceWith({ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, 2, 5000.0);
		if (!TestNotNull(TEXT("a plotted depot with a stated starter fleet places"), Stated)) { return false; }
		TestEqual(TEXT("keeps the count it was given, whatever its modules"), Stated->Trucks, 2);
	}
	{
		const FEntityInstance* Plain = Net->GetEntity(Net->PlaceEntity(
			Depot, Depot->Anchors, FVector2D(9000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 3));
		if (!TestNotNull(TEXT("a plotless depot places"), Plain)) { return false; }
		TestEqual(TEXT("and a plotless caller keeps its count too"), Plain->Trucks, 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayerDepotStartsWithNoTrucksTest,
	"Airside.Present.PlayerDepotStartsWithNoTrucks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerDepotStartsWithNoTrucksTest::RunTest(const FString& Parameters)
{
	// R3, AT THE GESTURE'S OWN DOOR: the depot the player draws has its kit and no vehicles, whatever
	// the definition's starter count says.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	TestEqual(TEXT("setup: the definition states a starter truck"), Actor->FuelDepotDefinition->Trucks, 1);

	const TArray<FVector2D> Plot = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0), FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestNotEqual(TEXT("the plot is placed"), Index, int32(INDEX_NONE))) { return false; }
	const FEntityInstance& Placed = Actor->Network->GetEntities()[Index];
	TestEqual(TEXT("with no starter trucks (R3)"), Placed.Trucks, 0);
	TestEqual(TEXT("and its whole start kit"), Placed.Modules.Num(), 3);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotOutlineIsAlwaysCounterClockwiseTest,
	"Airside.Entities.PlotOutlineIsAlwaysCounterClockwise",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotOutlineIsAlwaysCounterClockwiseTest::RunTest(const FString& Parameters)
{
	// A CLOCKWISE plot handed to the facade must be STORED counter-clockwise. The pad goes
	// through the same ear-clipper an apron does, which orients triangles from the winding,
	// and the surface is not two-sided - stored clockwise, the concrete faces DOWN and the
	// depot stands on visible grass. That shipped on 2026-09-15 and took a screenshot to
	// find, with nothing pinning it until now.
	//
	// DRIVEN THROUGH THE FACADE, not the model: URoadNetwork stores what it is given and the
	// correction is the facade's, so a model-level assertion would pass straight over the bug.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	// Clockwise: the rectangle the gesture commits, walked the other way round.
	// 20 m x 14 m SINCE #266: a plot must seat the mix it starts with, and under the real kits no
	// frontage under 20 m seats a shed (Airside.Content.SmallestAcceptedPlotSeatsTheStarterMix).
	const TArray<FVector2D> Clockwise = {
		FVector2D(0.0, 0.0), FVector2D(0.0, 1400.0),
		FVector2D(2000.0, 1400.0), FVector2D(2000.0, 0.0) };

	// The frontage in THAT winding order: the y = 0 edge runs from (2000,0) to (0,0).
	IRoadEditTarget* Target = Actor;
	Target->PlaceEntityInPlot(Clockwise, FVector2D(2000.0, 0.0), FVector2D(0.0, 0.0),
		{ EDepotModule::Shed }, EPlaceableEntity::FuelDepot);

	const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
	if (!TestTrue(TEXT("a depot was placed"), Entities.Num() > 0)) { return false; }

	double Twice = 0.0;
	const TArray<FVector2D>& Stored = Entities[0].Outline;
	for (int32 I = 0; I < Stored.Num(); ++I)
	{
		const FVector2D& P = Stored[I];
		const FVector2D& Q = Stored[(I + 1) % Stored.Num()];
		Twice += P.X * Q.Y - Q.X * P.Y;
	}
	TestTrue(TEXT("stored counter-clockwise however it was drawn"), Twice > 0.0);

	// AND THE MODULE IS STILL INSIDE THE PLOT. Reversing the outline without swapping the
	// frontage with it would leave PlotYard::InwardOf reading the interior side backwards and
	// lay every module across the road - a subtler failure than the invisible pad, and one
	// the winding assertion alone would not catch.
	TestTrue(TEXT("and the pose sits on the frontage, not across the road"),
		Entities[0].Position.Y > -1.0 && Entities[0].Position.Y < 1.0);

	return true;
}

/**
 * THE FACADE STORES THE FRONTAGE EDGE IT WAS GIVEN (#450).
 *
 * The frontage used to be forgotten at placement and recovered later by three different heuristics; PlaceEntityInPlot now finds the edge
 * of the outline it is about to STORE whose ends are the two points it was handed - after the winding reversal, which swaps them - and
 * writes its index on the entity. Three cases: the far edge of a counter-clockwise plot (so nothing can pass by reading edge 0), a
 * clockwise plot (whose stored outline is reversed, so the stored ends are the given ones swapped), and a frontage that is no edge of
 * its outline (refused: a nearest-edge guess would have built a yard facing the wrong side and said nothing).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPlacementStoresFrontageEdgeTest,
	"Airside.Entities.PlotPlacementStoresTheFrontageEdgeItWasGiven",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPlacementStoresFrontageEdgeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	const auto StoredFrontage = [Actor](int32 Index, FVector2D& OutA, FVector2D& OutB)
	{
		const FEntityInstance* Placed = Actor->Network->GetEntity(Actor->Network->EntityIdAt(Index));
		return Placed != nullptr && Placed->GetFrontage(OutA, OutB);
	};

	// COUNTER-CLOCKWISE, FRONTAGE ON THE FAR EDGE (corners 2 -> 3).
	const FVector2D Origin(20000.0, 20000.0);
	const TArray<FVector2D> Ccw = { Origin, Origin + FVector2D(4000.0, 0.0), Origin + FVector2D(4000.0, 3000.0), Origin + FVector2D(0.0, 3000.0) };
	const int32 CcwIndex = Actor->PlaceEntityInPlot(Ccw, Ccw[2], Ccw[3], TArray<EDepotModule>(), EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("a counter-clockwise plot is placed"), CcwIndex != INDEX_NONE)) { return false; }
	FVector2D A, B;
	if (!TestTrue(TEXT("it stores a frontage"), StoredFrontage(CcwIndex, A, B))) { return false; }
	TestEqual(TEXT("whose stored edge is the one it was given, the far edge, not corners 0 -> 1: start"), A, Ccw[2]);
	TestEqual(TEXT("and end"), B, Ccw[3]);
	TestEqual(TEXT("as the index of that edge"), Actor->Network->GetEntity(Actor->Network->EntityIdAt(CcwIndex))->FrontageEdge, 2);
	TestEqual(TEXT("with Position still the gate, the midpoint of that edge"),
		Actor->Network->GetEntity(Actor->Network->EntityIdAt(CcwIndex))->Position, (Ccw[2] + Ccw[3]) * 0.5);

	// CLOCKWISE: stored reversed, its frontage swapped with it - PlotYard::InwardOf reads the interior side off that direction.
	const FVector2D Away(40000.0, 20000.0);
	const TArray<FVector2D> Cw = { Away, Away + FVector2D(0.0, 3000.0), Away + FVector2D(4000.0, 3000.0), Away + FVector2D(4000.0, 0.0) };
	if (!TestTrue(TEXT("the premise: this outline is clockwise"), RoadGeom::PolygonArea(Cw) < 0.0)) { return false; }
	const int32 CwIndex = Actor->PlaceEntityInPlot(Cw, Cw[0], Cw[1], TArray<EDepotModule>(), EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("a clockwise plot is placed"), CwIndex != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("it stores a frontage too"), StoredFrontage(CwIndex, A, B))) { return false; }
	TestEqual(TEXT("whose start is the given END, the plot being stored reversed"), A, Cw[1]);
	TestEqual(TEXT("and whose end is the given START"), B, Cw[0]);

	// A FRONTAGE THAT IS NO EDGE: the diagonal. Refused, nothing placed.
	const int32 LiveBefore = Actor->Network->GetEntities().Num();
	const FVector2D Further(60000.0, 20000.0);
	const TArray<FVector2D> Rect = { Further, Further + FVector2D(4000.0, 0.0), Further + FVector2D(4000.0, 3000.0), Further + FVector2D(0.0, 3000.0) };
	TestEqual(TEXT("a frontage that is no edge of the outline is refused"),
		Actor->PlaceEntityInPlot(Rect, Rect[0], Rect[2], TArray<EDepotModule>(), EPlaceableEntity::FuelDepot), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("and places nothing"), Actor->Network->GetEntities().Num(), LiveBefore);
	return true;
}

/**
 * A PLOTTED DEPOT'S FRONTAGE EDGE SURVIVES UNDO AND REDO (#450, PR #491 review).
 *
 * Entities ride in the undo Memento whole, so FrontageEdge should travel with them - but the only undo test in this area used a plotless
 * depot, which has no frontage to lose. A depot placed on its FAR edge (so nothing passes by reading edge 0) is placed, undone, redone,
 * and then bulldozed and the bulldoze undone: each time the entity that comes back stores the same edge, reads back the same two ends, and
 * solves the same yard as before.
 *
 * WHAT THE ACTOR HALF DOES AND DOES NOT PIN. It pins the OUTCOME the player sees. The actor re-runs the load repair on a network undo
 * hands back, and EnsureDepotFrontages re-derives the edge for a depot with none stored - for a facade-placed depot, whose Position IS
 * its frontage's midpoint, the SAME edge - so a restore that dropped the field would be healed here and this half would stay green
 * (mutation-checked 2026-10-01: clearing FrontageEdge in a read pass of URoadNetwork::Serialize left it green for exactly that reason).
 * The second half is the one that measures the Memento's own copy: a DuplicateObject snapshot, no actor and so no repair, of a depot
 * whose Position would make a re-derivation answer edge 0 while it stores edge 2.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFrontageEdgeSurvivesUndoRedoTest,
	"Airside.Entities.PlotFrontageEdgeSurvivesUndoAndRedo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFrontageEdgeSurvivesUndoRedoTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<PlotYard::FKitSpec> Specs = Actor->ResolveDepotKits();

	const FVector2D Origin(20000.0, 20000.0);
	const TArray<FVector2D> Plot = { Origin, Origin + FVector2D(4000.0, 0.0), Origin + FVector2D(4000.0, 3000.0), Origin + FVector2D(0.0, 3000.0) };
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[2], Plot[3], TArray<EDepotModule>(), EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("the depot is placed on its far edge"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	const FEntityInstance* Placed = Actor->Network->GetEntity(Depot);
	if (!TestNotNull(TEXT("and reads back"), Placed)) { return false; }
	if (!TestEqual(TEXT("storing edge 2"), Placed->FrontageEdge, 2)) { return false; }
	const TOptional<PlotYard::FReservation> Yard = DepotKit::ReservationOf(*Placed, Specs);
	if (!TestTrue(TEXT("and solving a yard"), Yard.IsSet() && Yard->Stands.Num() > 0)) { return false; }

	// THE MEMENTO'S OWN COPY, with nothing to heal a loss: a depot whose POSITION is edge 0's midpoint (a re-derivation would say 0) but which
	// stores edge 2, duplicated the way the undo history snapshots a network.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Definition = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = Definition;
		Placement.Anchors = Definition->Anchors;
		Placement.Position = (Plot[0] + Plot[1]) * 0.5;
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = Plot;
		Placement.FrontageEdge = 2;
		const FEntityInstanceId Stored = Net->PlaceEntity(Placement);
		URoadNetwork* Snapshot = DuplicateObject<URoadNetwork>(Net, GetTransientPackage());
		if (!TestNotNull(TEXT("a DuplicateObject snapshot of a network holding a depot"), Snapshot)) { return false; }
		const FEntityInstance* Copied = Snapshot->GetEntity(Stored);
		if (!TestNotNull(TEXT("holds the depot"), Copied)) { return false; }
		TestEqual(TEXT("and the snapshot keeps the stored frontage edge - not edge 0, which its Position would re-derive"), Copied->FrontageEdge, 2);
	}

	// EACH RETURN OF THE DEPOT: the same entity, the same stored edge, the same ends, the same yard.
	const auto ExpectBack = [&](const TCHAR* When)
	{
		const FEntityInstance* Back = Actor->Network->GetEntity(Depot);
		if (!TestNotNull(*FString::Printf(TEXT("%s: the depot is back under the same id"), When), Back)) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: it stores the same frontage edge"), When), Back->FrontageEdge, 2);
		FVector2D A, B;
		if (!TestTrue(*FString::Printf(TEXT("%s: and the edge still names two ends"), When), Back->GetFrontage(A, B))) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: start"), When), A, Plot[2]);
		TestEqual(*FString::Printf(TEXT("%s: end"), When), B, Plot[3]);
		const TOptional<PlotYard::FReservation> Again = DepotKit::ReservationOf(*Back, Specs);
		if (!TestTrue(*FString::Printf(TEXT("%s: and it solves"), When), Again.IsSet())) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: the very yard it had before"), When), YardAgreement::Difference(*Yard, *Again), FString());
		return true;
	};

	if (!TestTrue(TEXT("the placement undoes"), Actor->Undo())) { return false; }
	TestNull(TEXT("and the depot is gone"), Actor->Network->GetEntity(Depot));
	if (!TestTrue(TEXT("and redoes"), Actor->Redo())) { return false; }
	if (!ExpectBack(TEXT("after undo then redo"))) { return false; }

	if (!TestTrue(TEXT("the depot is bulldozed"), Actor->DeleteEntity(Index))) { return false; }
	TestNull(TEXT("and gone"), Actor->Network->GetEntity(Depot));
	if (!TestTrue(TEXT("the bulldoze undoes"), Actor->Undo())) { return false; }
	return ExpectBack(TEXT("after a bulldoze is undone"));
}

/**
 * A DEPOT KEEPS ITS NUMBER THROUGH UNDO AND REDO (#490).
 *
 * Undo and redo restore the exact {Index, Generation} a slot had, and the Memento carries the whole entity and NextDepotNumber with it - so a
 * redone depot should come back as the same depot, with the same number, and the next one placed should not be handed it twice. The player's
 * sequence is the issue's own: depots 1 and 2, bulldoze 1, build another (3, in the recycled slot); then undo that placement, redo it, bulldoze it
 * and undo the bulldoze. Each time the depot that returns is the one under the same id, still depot 3, with the counter where it was.
 *
 * WHAT THE ACTOR HALF DOES AND DOES NOT PIN - as Airside.Entities.PlotFrontageEdgeSurvivesUndoAndRedo says of the frontage: the actor re-runs the
 * load repair on a network undo hands back, and EnsureStandNumbers numbers a depot at 0, so a restore that DROPPED the field is healed here (to
 * the counter's next value - which differs from 3 here, so it is not invisible, but it is not the Memento's own copy either). The second half is the
 * one that measures that: a DuplicateObject snapshot with no actor and no repair (Airside.Model.DepotNumbers holds the same at the model level).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotNumberSurvivesUndoRedoTest,
	"Airside.Entities.DepotNumberSurvivesUndoAndRedo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotNumberSurvivesUndoRedoTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

	// 40 x 30 m plots (4000 x 3000 uu), 200 m apart: none overlaps another, and each is the size the single-plot test above reserves a yard on.
	const auto PlaceDepotAt = [Actor](double X)
	{
		const FVector2D Origin(X, 20000.0);
		const TArray<FVector2D> Plot = { Origin, Origin + FVector2D(4000.0, 0.0), Origin + FVector2D(4000.0, 3000.0), Origin + FVector2D(0.0, 3000.0) };
		return Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1], TArray<EDepotModule>(), EPlaceableEntity::FuelDepot);
	};

	const int32 First = PlaceDepotAt(0.0);
	const int32 Second = PlaceDepotAt(20000.0);
	if (!TestTrue(TEXT("two depots are placed"), First != INDEX_NONE && Second != INDEX_NONE)) { return false; }
	TestEqual(TEXT("the first is depot 1"), Actor->Network->GetEntities()[First].DepotNumber, 1);
	TestEqual(TEXT("the second is depot 2"), Actor->Network->GetEntities()[Second].DepotNumber, 2);
	if (!TestTrue(TEXT("depot 1 is bulldozed"), Actor->DeleteEntity(First))) { return false; }
	const int32 ThirdIndex = PlaceDepotAt(40000.0);
	if (!TestTrue(TEXT("a third is placed"), ThirdIndex != INDEX_NONE)) { return false; }
	TestEqual(TEXT("it took the bulldozed depot's slot"), ThirdIndex, First);
	const FEntityInstanceId Third = Actor->Network->EntityIdAt(ThirdIndex);
	TestEqual(TEXT("and it is depot 3, not the retired 1"), Actor->Network->GetEntity(Third)->DepotNumber, 3);

	// EACH RETURN OF THE DEPOT: the same entity under the same id, still depot 3, the counter where it was.
	const auto ExpectBack = [&](const TCHAR* When)
	{
		const FEntityInstance* Back = Actor->Network->GetEntity(Third);
		if (!TestNotNull(*FString::Printf(TEXT("%s: the depot is back under the same id"), When), Back)) { return false; }
		TestEqual(*FString::Printf(TEXT("%s: it is still depot 3"), When), Back->DepotNumber, 3);
		TestEqual(*FString::Printf(TEXT("%s: and the next depot is 4 - the counter neither lost 3 nor spent it twice"), When), Actor->Network->GetNextDepotNumber(), 4);
		TestEqual(*FString::Printf(TEXT("%s: the survivor is still depot 2"), When), Actor->Network->GetEntities()[Second].DepotNumber, 2);
		return true;
	};

	if (!TestTrue(TEXT("the placement undoes"), Actor->Undo())) { return false; }
	TestNull(TEXT("and the depot is gone"), Actor->Network->GetEntity(Third));
	if (!TestTrue(TEXT("and redoes"), Actor->Redo())) { return false; }
	if (!ExpectBack(TEXT("after undo then redo"))) { return false; }

	if (!TestTrue(TEXT("the depot is bulldozed"), Actor->DeleteEntity(ThirdIndex))) { return false; }
	TestNull(TEXT("and gone"), Actor->Network->GetEntity(Third));
	if (!TestTrue(TEXT("the bulldoze undoes"), Actor->Undo())) { return false; }
	if (!ExpectBack(TEXT("after a bulldoze is undone"))) { return false; }

	// THE NEXT DEPOT after all of that is 4, never a number already worn.
	const int32 Fourth = PlaceDepotAt(60000.0);
	if (TestTrue(TEXT("a fourth is placed"), Fourth != INDEX_NONE))
	{
		TestEqual(TEXT("it is depot 4"), Actor->Network->GetEntities()[Fourth].DepotNumber, 4);
	}

	// THE MEMENTO'S OWN COPY, with nothing to heal a loss: a DuplicateObject snapshot of the live network, the way the undo history takes one.
	URoadNetwork* Snapshot = DuplicateObject<URoadNetwork>(Actor->Network, GetTransientPackage());
	if (!TestNotNull(TEXT("a snapshot"), Snapshot)) { return false; }
	const FEntityInstance* Copied = Snapshot->GetEntity(Third);
	if (TestNotNull(TEXT("the snapshot holds the third depot"), Copied))
	{
		TestEqual(TEXT("with its number"), Copied->DepotNumber, 3);
	}
	TestEqual(TEXT("and its counter"), Snapshot->GetNextDepotNumber(), 5);
	return true;
}

#endif
