#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A taxiway east-west and a service road north-south, sharing the node at the origin. */
	void LayCrossing(URoadNetwork& Net, bool bDrawFarSideOfRoad)
	{
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		Net.DefaultProfile = Taxiway;

		const FRoadNodeId West   = Net.AddNode(FVector2D(-20000.0, 0.0));
		const FRoadNodeId Centre = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net.AddNode(FVector2D(20000.0, 0.0));
		const FRoadNodeId South  = Net.AddNode(FVector2D(0.0, -20000.0));

		Net.AddStraightSegment(West, Centre, Taxiway);
		Net.AddStraightSegment(Centre, East, Taxiway);
		Net.AddStraightSegment(South, Centre, Road);
		if (bDrawFarSideOfRoad)
		{
			const FRoadNodeId North = Net.AddNode(FVector2D(0.0, 20000.0));
			Net.AddStraightSegment(Centre, North, Road);
		}
	}

	/** A TURN PATH carries no DerivedFrom - that is how it is told apart from a segment's own
	 *  guideline (see FRoadGuidelineBuilder, where DerivedFrom is deliberately left unset). */
	bool IsTurnPath(const FGuidelineEdge& Edge)
	{
		return Edge.bAlive && Edge.bDerived && !Edge.DerivedFrom.IsSet();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadCrossesTaxiwayTest,
	"Airside.Build.RoadCrossesTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadCrossesTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LayCrossing(*Net, /*bDrawFarSideOfRoad=*/true);
	FRoadGuidelineBuilder::Build(*Net, FRoadNetworkSolver::SolveAll(*Net));

	// THE WHOLE CROSSING RULE, and it is already in the builder: Turn.AllowedTraffic is
	// FromMask & ToMask, so a turn between arms of different classes keeps only Emergency.
	// This test exists because that behaviour is now LOAD BEARING for the fuel slice - it is
	// the only thing keeping a truck off a taxiway - and nothing pinned it.
	int32 VehicleTurns = 0, AircraftTurns = 0, MixedTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!IsTurnPath(Edge)) { continue; }
		const bool bVehicle = Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle);
		const bool bAircraft = Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft);
		VehicleTurns += (bVehicle && !bAircraft) ? 1 : 0;
		AircraftTurns += (bAircraft && !bVehicle) ? 1 : 0;
		MixedTurns += (bVehicle && bAircraft) ? 1 : 0;
	}

	TestTrue(TEXT("vehicles get road-to-road turns through the crossing"), VehicleTurns > 0);
	TestTrue(TEXT("aircraft get taxiway-to-taxiway turns through it"), AircraftTurns > 0);

	// THE SAFETY PROPERTY. A single mixed turn is an aircraft admitted onto a service road,
	// or a truck onto a taxiway, and neither is recoverable by any later rule.
	TestEqual(TEXT("and NO turn admits both - no road-to-taxiway path for anybody"),
		MixedTurns, 0);

	// Emergency crosses between the two, alone, and that is deliberate rather than a leak:
	// the builder's own comment says a fire truck may cross between a service road and a
	// taxiway and nothing else may. Asserted so a later "tighten the mask" change has to
	// argue with it rather than silently delete it.
	int32 EmergencyOnlyTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!IsTurnPath(Edge)) { continue; }
		EmergencyOnlyTurns += (Edge.AllowedTraffic.Allows(ETraversalClass::Emergency)
			&& !Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft)
			&& !Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle)) ? 1 : 0;
	}
	TestTrue(TEXT("emergency alone may cross between the two"), EmergencyOnlyTurns > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEndsAgainstTaxiwayTest,
	"Airside.Build.RoadEndsAgainstTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEndsAgainstTaxiwayTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	LayCrossing(*Net, /*bDrawFarSideOfRoad=*/false);
	FRoadGuidelineBuilder::Build(*Net, FRoadNetworkSolver::SolveAll(*Net));

	// A road dead-ending against a taxiway's side: the only arm of its own class at that
	// node is itself, so there is nothing to turn INTO and the junction derives no vehicle
	// path through it.
	int32 VehicleTurns = 0, AircraftTurns = 0;
	for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
	{
		if (!IsTurnPath(Edge)) { continue; }
		VehicleTurns += Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle) ? 1 : 0;
		AircraftTurns += Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft) ? 1 : 0;
	}
	TestEqual(TEXT("no vehicle path through the junction"), VehicleTurns, 0);

	// NOT REFUSED, AND THE AIRCRAFT SIDE IS UNAFFECTED. The player may be about to draw the
	// far side, so the tool lets it stand and the census counts it; meanwhile the taxiway
	// must be exactly the taxiway it was.
	TestTrue(TEXT("aircraft still turn through it"), AircraftTurns > 0);
	return true;
}

#endif
