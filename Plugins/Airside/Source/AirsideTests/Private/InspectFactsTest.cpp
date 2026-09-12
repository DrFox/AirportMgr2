#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectFactsTest,
	"Airside.Model.InspectFacts",
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
	FRouteQuery Q; Q.Start = A; Q.Goal = Pose; Q.Class = ETraversalClass::Aircraft;
	FAirframe Piper = UAirsideSettings::ResolveDefaultAirframe();
	Piper.TypeCode = TEXT("PA46");
	const int32 Id = Traffic->DispatchAgent(Net, RouteSearch::Find(*Net, Q), Piper, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	FAgentFacts Facts;
	TestFalse(TEXT("an unknown id yields no facts"), InspectFacts::DescribeAgent(*Traffic, Net, Id + 99, Facts));
	if (!TestTrue(TEXT("the agent yields facts"), InspectFacts::DescribeAgent(*Traffic, Net, Id, Facts))) { return false; }

	TestEqual(TEXT("id"), Facts.Id, Id);
	TestEqual(TEXT("type name is the airframe's code"), Facts.TypeName, FString(TEXT("PA46")));
	TestEqual(TEXT("phase"), Facts.Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("destination names the stand by index"),
		Facts.Destination, FString::Printf(TEXT("Stand %d"), StandId.Index));
	TestEqual(TEXT("status while moving freely"), Facts.Status, FString(TEXT("Taxiing")));
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
		Scripted.WaitingOn = 7;
		TestEqual(TEXT("holding for another"), InspectFacts::StatusOf(Scripted), FString(TEXT("Holding for aircraft 7")));
		Scripted.bDepartureArmed = true;
		TestEqual(TEXT("armed departure outranks holding"), InspectFacts::StatusOf(Scripted), FString(TEXT("Departure armed")));
		Scripted.bDepartureArmed = false; Scripted.WaitingOn = 0;
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

#endif
