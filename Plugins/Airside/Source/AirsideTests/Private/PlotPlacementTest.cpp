#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

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

#endif
