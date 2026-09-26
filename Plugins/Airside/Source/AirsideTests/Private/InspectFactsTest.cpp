#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "StandFixture.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FGuidelineEdgeId InspJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	/**
	 * A straight GroundVehicle guideline standing in for a drawn service road - the shape
	 * ServiceLinkTest.cpp's own ServiceLinkFixture::Lay builds for the same fixtures. NOT
	 * shared from there: StandFixture.h declares only PlaceStand/FarEdgeX/FarRoadX, and Lay
	 * has no declaration to include - a five-line local copy here costs less than adding one
	 * for a single further caller.
	 */
	void LayServiceRoad(URoadNetwork& Net, const FVector2D& From, const FVector2D& To)
	{
		const FGuidelineNodeId A = Net.AddGuidelineNode(From);
		const FGuidelineNodeId B = Net.AddGuidelineNode(To);
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectFactsTest,
	// A LEAF NAME, not the bare "Airside.Model.InspectFacts" this used to be: UE 5.8's
	// automation tree drops a bare-named test's own RunTest, silently, the moment a dotted
	// sibling (StandServiceableWithFarRoad et al, below) registers - it becomes a parent
	// GROUP node instead of a leaf. See unreal-automation-test-tree-drops-bare-parent.
	"Airside.Model.InspectFacts.AgentAndStandFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectFactsTest::RunTest(const FString& Parameters)
{
	// THE PANEL'S CONTRACT. The widget never reads FRoadAgent; it reads these. So each fact
	// the panel shows is pinned here, world-free, against a scripted agent - and when M3's
	// UFlight fills the same struct the panel does not change.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	InspJoin(*Net, A, B);

	// A stand whose pose node is the route's goal, so a taxi to it is a taxi "to Stand N".
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId StandId = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 5000.0), 0.0, 1800.0);
	if (!TestTrue(TEXT("stand placed"), StandId.IsSet())) { return false; }
	const FGuidelineNodeId Pose = Net->GetEntity(StandId)->PoseNode;
	if (!TestTrue(TEXT("the stand has a pose node"), Pose.IsSet())) { return false; }
	InspJoin(*Net, B, Pose);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	FAirframe Piper = UAirsideSettings::ResolveDefaultAirframe();
	Piper.TypeCode = TEXT("PA46");
	const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, A, Pose, ETraversalClass::Aircraft), Piper, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	FAgentFacts Facts;
	TestFalse(TEXT("an unknown id yields no facts"), InspectFacts::DescribeAgent(*Traffic, Net, Id + 99, Facts));
	if (!TestTrue(TEXT("the agent yields facts"), InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts))) { return false; }

	TestEqual(TEXT("id"), Facts.Id, Id);
	TestEqual(TEXT("type name is the airframe's code"), Facts.TypeName, FString(TEXT("PA46")));
	TestEqual(TEXT("phase"), Facts.Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("destination names the stand by index"),
		Facts.Destination, FString::Printf(TEXT("Stand %d"), StandId.Index));
	// Status while plain-taxiing is not asserted here: StatusOf's Taxiing case is a bare
	// mirror of the Phase already checked above (#104) - the real content of Status, its
	// PRECEDENCE over Phase, is what the scripted block below actually tests.
	TestFalse(TEXT("cannot depart while taxiing"), Facts.bCanDepart);
	TestTrue(TEXT("engine running while taxiing"), Facts.bEngineRunning);

	// Stand facts: occupied by the INBOUND agent already.
	FStandFacts SF;
	TestFalse(TEXT("a dead index yields no stand facts"), InspectFacts::DescribeStand(Traffic, *Net, 99, SF));
	if (!TestTrue(TEXT("the stand yields facts"), InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF))) { return false; }
	TestEqual(TEXT("occupant is the inbound agent"), SF.OccupantAgent, Id);
	TestFalse(TEXT("inbound: reserved, not parked"), SF.bOccupantParked);
	TestEqual(TEXT("1800 uu (18 m) span is ICAO code B"), SF.SizeClass, FString(TEXT("B")));
	TestTrue(TEXT("a pose node with an edge is reachable"), SF.bReachable);
	TestEqual(TEXT("anchor count is the definition's"), SF.AnchorCount, Stand->Anchors.Num());

	// Tick to Parked: the status and bCanDepart flip, heading/speed are reported.
	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("parked"), InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts) && Facts.Phase == EAgentPhase::Parked)) { return false; }
	TestTrue(TEXT("a parked aircraft can depart"), Facts.bCanDepart);
	TestTrue(TEXT("status says shutting down while the countdown runs"), Facts.Status.StartsWith(TEXT("Shutting down")));
	TestTrue(TEXT("speed is zero when parked"), FMath::IsNearlyZero(Facts.GroundSpeed, 1.0));
	TestTrue(TEXT("heading is a compass figure"), Facts.HeadingDegrees >= 0.0 && Facts.HeadingDegrees < 360.0);

	InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF);
	TestEqual(TEXT("the parked agent still occupies the stand"), SF.OccupantAgent, Id);
	TestTrue(TEXT("and is reported parked"), SF.bOccupantParked);

	// Status precedence: WaitingOn beats everything but an armed departure.
	{
		// Scripted rather than staged with a second aircraft: the precedence is the thing
		// under test, and the arbitration that sets WaitingOn has its own tests.
		FRoadAgent Scripted = *Traffic->FindAgent(Id);
		Scripted.Phase = EAgentPhase::Taxiing;
		// Through Refuse, not by hand: WaitingOn is private outside FClaimPass/RoadAgent.cpp
		// (issue #174). Step, resource and stop distance are unread by StatusOf, so their
		// values here are arbitrary - only the blocker id (7) is under test.
		Scripted.Refuse(0, FTrafficResource(), TNumericLimits<double>::Max(), 7);
		TestEqual(TEXT("holding for another"), InspectFacts::StatusOf(Scripted), FString(TEXT("Holding for aircraft 7")));
		Scripted.bDepartureArmed = true;
		TestEqual(TEXT("armed departure outranks holding"), InspectFacts::StatusOf(Scripted), FString(TEXT("Departure armed")));
		Scripted.bDepartureArmed = false; Scripted.ClearArbitration();
		// The seed's value does not matter to StatusOf - only IsCrossing() does - but
		// BeginCrossing needs one to keep CrossingRunway/CrossingPhase together (issue #82).
		FRoadSegmentId ScriptedRunway;
		ScriptedRunway.Index = 0;
		Scripted.BeginCrossing(ScriptedRunway, ECrossingPhase::OnStrip);
		TestEqual(TEXT("crossing"), InspectFacts::StatusOf(Scripted), FString(TEXT("Crossing runway")));
	}

	// Empty stand: retire the agent.
	Traffic->RetireAgent(Id);
	InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF);
	TestEqual(TEXT("no occupant once the agent is gone"), SF.OccupantAgent, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandServiceableWithFarRoadTest,
	"Airside.Model.InspectFacts.StandServiceableWithFarRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandServiceableWithFarRoadTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE SERVICEABLE CASE: a road at the far edge (Task 4's own FarRoadX, the shipping
	// Code C stand) joins every declared bay entry, so bServiceable - and the inspector's
	// new "Service road: joined" line - reads true.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const double RoadX = FarRoadX();
	LayServiceRoad(*Net, FVector2D(RoadX, -20000.0), FVector2D(RoadX, 20000.0));

	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
	FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

	FStandFacts SF;
	if (!TestTrue(TEXT("the stand yields facts"),
		InspectFacts::DescribeStand(nullptr, *Net, Placed.Index, SF))) { return false; }
	TestTrue(TEXT("every declared bay entry joined the far-edge road"), SF.bServiceable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandUnserviceableWithoutRoadTest,
	"Airside.Model.InspectFacts.StandUnserviceableWithoutRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandUnserviceableWithoutRoadTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE UNSERVICEABLE CASE: no road at all. A stand is still PLACED (spec 2026-09-26 -
	// "still placed, and reported unserviceable until one is drawn"), so this asserts the
	// FACT reads false rather than the placement being refused.
	//
	// NO FAnchorLink::Build CALL, deliberately: a service bay's lane is laid at placement
	// (BuildStandTemplate), before any linking pass runs, so IsServiceNodeConnected already
	// has an owned-only ring to walk with nothing to escape to - the same "no road" state
	// AnchorLink::Build would leave it in, without that pass's "joins nothing" warnings.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	FStandFacts SF;
	if (!TestTrue(TEXT("the stand yields facts"),
		InspectFacts::DescribeStand(nullptr, *Net, Placed.Index, SF))) { return false; }
	TestFalse(TEXT("no bay entry joins anything with no road drawn"), SF.bServiceable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPartiallyJoinedIsUnserviceableTest,
	"Airside.Model.InspectFacts.StandPartiallyJoinedIsUnserviceable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPartiallyJoinedIsUnserviceableTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE PARTIAL CASE (ruling, task 5): some declared entries joined, others not, must
	// still read bServiceable false - "every", not "any". Code F's own two-sided fixture
	// (FPartialJoinWarnsTest, ServiceLinkTest.cpp) is reused rather than invented: its
	// starboard/port contacts sit far enough apart (>ServiceLinkRadius) that a short
	// starboard-only road cannot reach port by accident - see that test's own measured-not-
	// assumed comment for why Code C cannot isolate a side.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(EIcaoCode::F);

	double StarboardY = 0.0;
	bool bHasStarboard = false;
	bool bHasPort = false;
	for (const FServiceBay& Bay : Stand->ServiceBays)
	{
		if (Bay.EntryLocal.Y >= 0.0) { StarboardY = Bay.EntryLocal.Y; bHasStarboard = true; }
		else { bHasPort = true; }
	}
	if (!TestTrue(TEXT("the template has a service on each side"), bHasStarboard && bHasPort))
	{
		return false;
	}

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const double FarSideX = FarEdgeX(*Stand) + 400.0;

	// SHORT AND CENTRED ON THE STARBOARD CONTACT ONLY - same 1000 uu half-span
	// FPartialJoinWarnsTest measured, so it stays out of the port contact's reach.
	constexpr double HalfSpan = 1000.0;
	LayServiceRoad(*Net,
		FVector2D(FarSideX, StarboardY - HalfSpan), FVector2D(FarSideX, StarboardY + HalfSpan));

	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	// THE PARTIAL LINE FIRES (Joined > 0, Refused > 0) - expected rather than merely
	// tolerated, as ServiceLinkTest.cpp's own fixture does, so a silent change to the
	// half-plane that stopped exercising the partial branch would show here as a missing
	// expected error rather than a quietly-passing test.
	AddExpectedError(TEXT("service entrances joined a road"), EAutomationExpectedErrorFlags::Contains, 1);
	FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());

	FStandFacts SF;
	if (!TestTrue(TEXT("the stand yields facts"),
		InspectFacts::DescribeStand(nullptr, *Net, Placed.Index, SF))) { return false; }
	TestFalse(TEXT("a partial join is still unserviceable - every entry, not any"), SF.bServiceable);
	return true;
}

#endif
