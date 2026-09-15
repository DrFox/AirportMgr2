#include "CoreMinimal.h"
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
		         FVector2D(X + 1200.0, 800.0), FVector2D(X, 800.0) };
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

#endif
