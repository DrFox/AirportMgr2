#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelService.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPumpDwellTest,
	"AirportOps.Ops.PumpDwell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPumpDwellTest::RunTest(const FString& Parameters)
{
	UFuelService* Service = NewObject<UFuelService>();
	if (!TestNotNull(TEXT("a fuel service"), Service)) { return false; }

	const double Base = Service->DwellSeconds;

	FEntityInstance Depot;

	// ONE PUMP IS THE BASE FIGURE, so the Tier 1 depot on the concept sheet behaves exactly
	// as every depot did before modules existed. A first pump that halved the dwell would
	// be a silent buff to the only depot the player has.
	Depot.Modules = { EDepotModule::Shed, EDepotModule::Pump };
	TestTrue(TEXT("one pump gives the base dwell"),
		FMath::IsNearlyEqual(Service->DwellSecondsFor(Depot), Base));

	Depot.Modules = { EDepotModule::Shed, EDepotModule::Pump, EDepotModule::Pump };
	TestTrue(TEXT("two pumps halve it"),
		FMath::IsNearlyEqual(Service->DwellSecondsFor(Depot), Base * 0.5));

	// THE FLOOR. A yard full of pumps must not make refuelling instant - the dwell is the
	// only pressure the fuel loop applies.
	Depot.Modules.Init(EDepotModule::Pump, 100);
	TestTrue(TEXT("a pump farm is floored, not instant"),
		Service->DwellSecondsFor(Depot) >= Service->MinDwellSeconds);

	// A PLOTLESS DEPOT keeps the service-wide figure. Every depot in every save written
	// before plots existed is one of these, and answering "no pump" for them would break
	// the fuel loop for all of them at once.
	Depot.Modules.Reset();
	TestTrue(TEXT("a depot with no modules is not a depot with no pump"),
		UFuelService::HasWorkingPump(Depot));
	TestTrue(TEXT("and it dwells for the base time"),
		FMath::IsNearlyEqual(Service->DwellSecondsFor(Depot), Base));

	// A MODULAR depot with no pump cannot fuel, and says so through HasWorkingPump rather
	// than through a dwell of zero - ChooseDepot skips it before a truck is ever sent, so
	// the player sees a depot that never dispatches rather than a truck that drives out and
	// then does nothing.
	Depot.Modules = { EDepotModule::Shed, EDepotModule::Tank };
	TestFalse(TEXT("a modular depot with no pump cannot fuel"),
		UFuelService::HasWorkingPump(Depot));

	return true;
}

#endif
