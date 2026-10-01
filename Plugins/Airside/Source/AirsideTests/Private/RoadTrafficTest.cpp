#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadTraffic.h"

#if WITH_DEV_AUTOMATION_TESTS

// ".RightOfWay", not the bare "Airside.Model.Traffic" this was: UE's automation tree drops
// a bare-named test the moment a DOTTED CHILD of that name exists (Airside.Model.Traffic.
// NodeYield, added in M2 task 5), so the two cannot both be registered. The class name is
// unchanged - only the tree path moved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadTrafficTest,
	"Airside.Model.Traffic.RightOfWay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadTrafficTest::RunTest(const FString& Parameters)
{
	// A default mask admits nobody. The default must be the CLOSED case: a guideline
	// created without an explicit mask should be unusable rather than universally usable,
	// so that forgetting to set access is a visible dead end and not a silent free-for-all.
	{
		const FTrafficMask Empty;
		TestFalse(TEXT("default mask admits no aircraft"), Empty.Allows(ETraversalClass::Aircraft));
		TestFalse(TEXT("default mask admits no vehicles"), Empty.Allows(ETraversalClass::GroundVehicle));
		TestFalse(TEXT("default mask admits no pedestrians"), Empty.Allows(ETraversalClass::Pedestrian));
		TestFalse(TEXT("default mask admits no emergency"), Empty.Allows(ETraversalClass::Emergency));
	}

	// Adding one class admits exactly that class - masks are a set, not a threshold.
	{
		FTrafficMask ServiceRoad;
		ServiceRoad.Add(ETraversalClass::GroundVehicle);
		ServiceRoad.Add(ETraversalClass::Emergency);

		TestTrue(TEXT("service road admits vehicles"), ServiceRoad.Allows(ETraversalClass::GroundVehicle));
		TestTrue(TEXT("service road admits emergency"), ServiceRoad.Allows(ETraversalClass::Emergency));
		TestFalse(TEXT("service road excludes aircraft"), ServiceRoad.Allows(ETraversalClass::Aircraft));
		TestFalse(TEXT("service road excludes pedestrians"), ServiceRoad.Allows(ETraversalClass::Pedestrian));
	}

	{
		const FTrafficMask All = FTrafficMask::All();
		TestTrue(TEXT("All admits aircraft"), All.Allows(ETraversalClass::Aircraft));
		TestTrue(TEXT("All admits vehicles"), All.Allows(ETraversalClass::GroundVehicle));
		TestTrue(TEXT("All admits pedestrians"), All.Allows(ETraversalClass::Pedestrian));
		TestTrue(TEXT("All admits emergency"), All.Allows(ETraversalClass::Emergency));

		const FTrafficMask OnlyAir = FTrafficMask::Only(ETraversalClass::Aircraft);
		TestTrue(TEXT("Only admits its class"), OnlyAir.Allows(ETraversalClass::Aircraft));
		TestFalse(TEXT("Only excludes the rest"), OnlyAir.Allows(ETraversalClass::Pedestrian));
	}

	// Spec 5.4: Emergency > Aircraft > Pedestrian > GroundVehicle.
	{
		TestTrue(TEXT("emergency outranks aircraft"),
			TraversalPriority(ETraversalClass::Emergency) > TraversalPriority(ETraversalClass::Aircraft));
		TestTrue(TEXT("aircraft outrank pedestrians"),
			TraversalPriority(ETraversalClass::Aircraft) > TraversalPriority(ETraversalClass::Pedestrian));
		TestTrue(TEXT("pedestrians outrank ground vehicles"),
			TraversalPriority(ETraversalClass::Pedestrian) > TraversalPriority(ETraversalClass::GroundVehicle));
	}

	// The three comparisons above are LITERAL rank checks on purpose: the loop below derives
	// nothing from them, so on its own it would pass with the whole order flipped. (A
	// ResolveRightOfWay wrapper used to be asserted beside them, "which class wins" against literal
	// expectations. It had no production caller - only this test - and was deleted 2026-10-01: the
	// rank is the rule, and the winner of a pair is whichever has the higher one.)

	// The order must be TOTAL. Asserted exhaustively over every ordered pair rather than by
	// spot-check: a partial order here would leave some crossing in the game with no defined
	// winner, and the failure would surface as two agents deadlocked rather than as anything
	// that looks like a rule bug. Distinct ranks make the comparison antisymmetric as well -
	// of any two different classes exactly one outranks the other.
	{
		const ETraversalClass Classes[] = {
			ETraversalClass::Aircraft, ETraversalClass::GroundVehicle,
			ETraversalClass::Pedestrian, ETraversalClass::Emergency };

		for (const ETraversalClass Left : Classes)
		{
			for (const ETraversalClass Right : Classes)
			{
				if (Left == Right)
				{
					continue;
				}

				TestNotEqual(TEXT("distinct classes never tie in priority"),
					TraversalPriority(Left), TraversalPriority(Right));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
