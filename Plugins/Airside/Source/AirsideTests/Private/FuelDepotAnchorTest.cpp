#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A straight EAST-WEST guideline at Y, admitting exactly one class (plus Emergency,
	 *  as every derived guideline does). */
	void LayEastWest(URoadNetwork& Net, double Y, ETraversalClass Class,
		FGuidelineNodeId& OutWest, FGuidelineNodeId& OutEast)
	{
		OutWest = Net.AddGuidelineNode(FVector2D(-10000.0, Y));
		OutEast = Net.AddGuidelineNode(FVector2D(10000.0, Y));

		FGuidelineEdge Edge;
		Edge.A = OutWest;
		Edge.B = OutEast;
		Edge.Control = FVector2D(0.0, Y);
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	/** A straight NORTH-SOUTH guideline at X. */
	void LayNorthSouth(URoadNetwork& Net, double X, ETraversalClass Class,
		FGuidelineNodeId& OutSouth, FGuidelineNodeId& OutNorth)
	{
		OutSouth = Net.AddGuidelineNode(FVector2D(X, -10000.0));
		OutNorth = Net.AddGuidelineNode(FVector2D(X, 10000.0));

		FGuidelineEdge Edge;
		Edge.A = OutSouth;
		Edge.B = OutNorth;
		Edge.Control = FVector2D(X, 0.0);
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 2300.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	int32 IncidentCount(const URoadNetwork& Net, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Net.GetGuidelineNode(Node);
		return Found != nullptr ? Found->Incident.Num() : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotJoinsRoadTest,
	"Airside.Entities.DepotJoinsRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotJoinsRoadTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	TestEqual(TEXT("its pose is a vehicle's, not an aircraft's"),
		static_cast<int32>(Depot->PoseRole), static_cast<int32>(EServiceRole::Fuel));
	TestEqual(TEXT("it has one truck"), Depot->Trucks, 1);
	TestEqual(TEXT("and NO anchors - the pose IS its road connection"), Depot->Anchors.Num(), 0);

	// ON A ROAD. The pose lead-in leaves along heading + 180 (see FAnchorLink), so a depot
	// at +90 degrees casts straight down -Y at a line below it - the same arithmetic a stand
	// uses, which is why a depot needs no placement rule of its own.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		LayEastWest(*Net, 0.0, ETraversalClass::GroundVehicle, West, East);

		const FEntityInstanceId Placed = Net->PlaceEntity(Depot, Depot->Anchors,
			FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5, /*DesignWingspan=*/0.0, Depot->PoseRole);
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the depot resolves"), Instance)) { return false; }

		TestEqual(TEXT("the instance captured the pose role"),
			static_cast<int32>(Instance->PoseRole), static_cast<int32>(EServiceRole::Fuel));

		const FGuidelineNodeId Pose = Instance->PoseNode;
		TestEqual(TEXT("it starts an island"), IncidentCount(*Net, Pose), 0);

		TestTrue(TEXT("the pose lead-in joins the road"), FAnchorLink::Build(*Net) >= 1);
		TestTrue(TEXT("and the pose node now has line on it"), IncidentCount(*Net, Pose) > 0);

		// THE POINT: a truck can be routed off the depot. Before PoseRole the lead-in was
		// cast as an AIRCRAFT unconditionally, found no aircraft guideline, and logged
		// "joins nothing" on every rebuild for ever.
		FRouteQuery Query;
		Query.Start = Pose;
		Query.Goal = East;
		Query.Class = ETraversalClass::GroundVehicle;
		TestTrue(TEXT("a vehicle routes off the depot"), RouteSearch::Find(*Net, Query).IsValid());
	}

	// OFF ANY ROAD - a depot facing a TAXIWAY. Its class is refused by the edge's own mask,
	// so it stays unjoined and is counted, which is the census warning the player reads.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		LayEastWest(*Net, 0.0, ETraversalClass::Aircraft, West, East);

		const FEntityInstanceId Placed = Net->PlaceEntity(Depot, Depot->Anchors,
			FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5, 0.0, Depot->PoseRole);
		TestEqual(TEXT("a depot facing a taxiway joins nothing"), FAnchorLink::Build(*Net), 0);
		TestEqual(TEXT("and its pose node is still an island"),
			IncidentCount(*Net, Net->GetEntity(Placed)->PoseNode), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandFuelAnchorJoinsRoadTest,
	"Airside.Entities.StandFuelAnchorJoinsRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandFuelAnchorJoinsRoadTest::RunTest(const FString& Parameters)
{
	// THE FIXTURE'S GEOMETRY, spelled out because it IS the test.
	//
	// The stand faces +X (heading 0). Two rays leave it, and they are cast differently:
	//   - the POSE ray leaves along heading + 180 (FAnchorLink: +X faces the terminal, so
	//     the lead-in runs back out to the movement area) - toward -X, at a TAXIWAY laid
	//     as a north-south line to the west;
	//   - an ANCHOR ray leaves along its own world heading, with no 180. HydrantPit is at
	//     local (-1200, +700) with LocalHeading -90 degrees (see BuildCodeCStand), so it
	//     casts straight down -Y, at a ROAD laid as an east-west line to the south.
	//
	// The two rays cannot poach each other's line: the pose's link is aircraft-class and
	// the hydrant's is vehicle-class, and FAnchorLink filters candidate edges by the link's
	// class before it measures anything.
	//
	// This is the fixture the spec calls "the one that logs the stand's fuel anchor unjoined
	// today": until a service road existed there was nothing of its class to reach.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FGuidelineNodeId TaxiSouth, TaxiNorth, RoadWest, RoadEast;
	LayNorthSouth(*Net, -10000.0, ETraversalClass::Aircraft, TaxiSouth, TaxiNorth);
	LayEastWest(*Net, -6000.0, ETraversalClass::GroundVehicle, RoadWest, RoadEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	TestEqual(TEXT("a stand's pose is still an aircraft's"),
		static_cast<int32>(Stand->PoseRole), static_cast<int32>(EServiceRole::Aircraft));

	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Stand->Anchors,
		FVector2D(0.0, 0.0), /*Heading=*/0.0, /*DesignWingspan=*/3600.0, Stand->PoseRole);
	const FEntityInstance* Instance = Net->GetEntity(Placed);
	if (!TestNotNull(TEXT("the stand resolves"), Instance)) { return false; }

	const FGuidelineNodeId Pose = Instance->PoseNode;
	FAnchorLink::Build(*Net);

	// The aircraft half still works - a stand nothing can taxi to would make the fuel half
	// meaningless, and it is the half every existing test relies on.
	TestTrue(TEXT("the stand's own pose still joins the taxiway"), IncidentCount(*Net, Pose) > 0);

	const TArray<FName> FuelIds = Net->GetAnchorIdsForRole(Placed, EServiceRole::Fuel);
	if (!TestEqual(TEXT("the stand has one fuel anchor"), FuelIds.Num(), 1)) { return false; }

	const FResolvedAnchor* Fuel = Net->FindResolvedAnchor(Placed, FuelIds[0]);
	if (!TestNotNull(TEXT("it resolved to a node"), Fuel)) { return false; }
	TestTrue(TEXT("and now joins the road"), IncidentCount(*Net, Fuel->Node) > 0);

	// A TRUCK CAN REACH IT. This node is the OUT plan's goal, so a stand whose fuel anchor
	// is an island is a stand no fuel service can ever serve - and "the search found
	// nothing" would otherwise be indistinguishable from a broken search.
	FRouteQuery Query;
	Query.Start = RoadWest;
	Query.Goal = Fuel->Node;
	Query.Class = ETraversalClass::GroundVehicle;
	TestTrue(TEXT("a truck routes from the road to the hydrant"),
		RouteSearch::Find(*Net, Query).IsValid());

	// AND AN AIRCRAFT CANNOT. The lead-in carries FTrafficMask::Only(GroundVehicle) plus
	// Emergency, so the hydrant is not somewhere an aeroplane can be sent by mistake.
	Query.Start = TaxiSouth;
	Query.Class = ETraversalClass::Aircraft;
	TestFalse(TEXT("an aircraft cannot be routed to the hydrant"),
		RouteSearch::Find(*Net, Query).IsValid());
	return true;
}

#endif
