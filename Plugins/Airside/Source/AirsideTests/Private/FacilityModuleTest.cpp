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
	TestEqual(TEXT("one more bay is lit - the Topology rebuild reached the buildings actor"), Plots->GetModuleCount(), Built + 1);
	TestEqual(TEXT("and one fewer is ghosted"), Plots->GetGhostCount(), Ghosts - 1);
	TestFalse(TEXT("R8: the purchase is a checkpoint - an undo would drop the shed and keep the money"), Facade->CanUndo());
	TestFalse(TEXT("a stand-shaped id is refused at the door"), Facade->AddEntityModule(FEntityInstanceId(), EDepotModule::Shed));
	return true;
}

#endif
