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
	const FEntityInstanceId Old = Net->PlaceEntity(
		Depot, Depot->Anchors, FVector2D(5000.0, 0.0), 0.0);
	TestTrue(TEXT("the pre-plot signature still places"), Old.IsSet());

	const FEntityInstance* Legacy = Net->GetEntity(Old);
	if (!TestNotNull(TEXT("and is readable"), Legacy)) { return false; }
	TestEqual(TEXT("with no outline, which is what a plain plop means"),
		Legacy->Outline.Num(), 0);
	TestEqual(TEXT("and no modules either"), Legacy->Modules.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrucksDerivedFromShedsTest,
	"Airside.Entities.TrucksDerivedFromSheds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrucksDerivedFromShedsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	auto PlaceWith = [&](const TArray<EDepotModule>& Modules, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X, 0.0);
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = ThreeBayPlot(X);
		Placement.Modules = Modules;
		return Net->GetEntity(Net->PlaceEntity(Placement));
	};

	// Two sheds is two trucks. The number UFuelService counts dispatches against, so this
	// is the one module whose effect is real today.
	{
		const FEntityInstance* Two = PlaceWith(
			{ EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank }, 0.0);
		if (!TestNotNull(TEXT("two sheds placed"), Two)) { return false; }
		TestEqual(TEXT("two sheds is two trucks"), Two->Trucks, 2);
	}

	// No shed is no trucks - allowed, because a part-built depot is a legitimate state and
	// the census warns rather than forbidding. It must not silently become one.
	{
		const FEntityInstance* None = PlaceWith(
			{ EDepotModule::Tank, EDepotModule::Pump }, 5000.0);
		if (!TestNotNull(TEXT("a shedless depot still places"), None)) { return false; }
		TestEqual(TEXT("no shed is no trucks, not a default of one"), None->Trucks, 0);
	}

	// A PLOTLESS caller still states its own count outright, because it has no modules to
	// derive one from. The two paths are exclusive by construction, which is the whole
	// reason FEntityPlacement::Trucks and Modules can coexist without disagreeing.
	{
		const FEntityInstance* Plain = Net->GetEntity(Net->PlaceEntity(
			Depot, Depot->Anchors, FVector2D(9000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 3));
		if (!TestNotNull(TEXT("a plotless depot places"), Plain)) { return false; }
		TestEqual(TEXT("and keeps the count it was given"), Plain->Trucks, 3);
	}

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
	const TArray<FVector2D> Clockwise = {
		FVector2D(0.0, 0.0), FVector2D(0.0, 1200.0),
		FVector2D(1200.0, 1200.0), FVector2D(1200.0, 0.0) };

	// The frontage in THAT winding order: the y = 0 edge runs from (1200,0) to (0,0).
	IRoadEditTarget* Target = Actor;
	Target->PlaceEntityInPlot(Clockwise, FVector2D(1200.0, 0.0), FVector2D(0.0, 0.0),
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
