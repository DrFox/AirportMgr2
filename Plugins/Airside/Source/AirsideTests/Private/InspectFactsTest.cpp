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
#include "Model/TaxiwayRestriction.h"
#include "Model/TrafficOccupancy.h"
#include "AirsideTestFixtures.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "Profiles/RoadProfile.h"
#include "Solve/IcaoCode.h"
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
	// PLACE AND DELETE ONE FIRST, so the stand below recycles slot 0 but is issued number 2 -
	// the index and the number differ, and a panel that named stands by index would show
	// "Stand 0" where the player's ground paint says 2 (taxiway strip stage 5).
	const FEntityInstanceId Retired = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(-50000.0, 5000.0), 0.0, 1800.0);
	if (!TestTrue(TEXT("a stand placed and removed first"), Retired.IsSet() && Net->RemoveEntity(Retired))) { return false; }
	const FEntityInstanceId StandId = Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(20000.0, 5000.0), 0.0, 1800.0);
	if (!TestTrue(TEXT("stand placed"), StandId.IsSet())) { return false; }
	const int32 StandNumber = Net->GetEntity(StandId)->StandNumber;
	if (!TestTrue(TEXT("the stand's number differs from its index, or this test measures nothing"),
			StandNumber == 2 && StandId.Index != StandNumber)) { return false; }
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
	TestEqual(TEXT("destination names the stand by its NUMBER, not its index"),
		Facts.Destination, FString::Printf(TEXT("Stand %d"), StandNumber));
	// Status while plain-taxiing is not asserted here: StatusOf's Taxiing case is a bare
	// mirror of the Phase already checked above (#104) - the real content of Status, its
	// PRECEDENCE over Phase, is what the scripted block below actually tests.
	TestFalse(TEXT("cannot depart while taxiing"), Facts.bCanDepart);
	TestTrue(TEXT("engine running while taxiing"), Facts.bEngineRunning);

	// Stand facts: occupied by the INBOUND agent already.
	FStandFacts SF;
	TestFalse(TEXT("a dead index yields no stand facts"), InspectFacts::DescribeStand(Traffic, *Net, 99, SF));
	if (!TestTrue(TEXT("the stand yields facts"), InspectFacts::DescribeStand(Traffic, *Net, StandId.Index, SF))) { return false; }
	TestEqual(TEXT("the stand facts carry its number"), SF.Number, StandNumber);
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
		// THE HOLD LINE (inspector hold info, 2026-09-30), the blocker named by id: StatusOf has no
		// traffic to ask what agent 7 is, nor a flight board for its registration. A default
		// FTrafficResource is a Node - a crossing.
		TestEqual(TEXT("holding for another, in the hold line's words"), InspectFacts::StatusOf(Scripted),
			FString(TEXT("Waiting at crossing for aircraft 7 · 0:00")));
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

	// THE SERVICEABLE CASE: a road at the far edge (StandFixture.h's FarRoadX, the shipping
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

	// THE PARTIAL CASE (ruling, 2026-09-26): some declared entries joined, others not, must
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectFactsPushbackTest, "Airside.Model.InspectFacts.PushbackDemand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectFactsPushbackTest::RunTest(const FString& Parameters)
{
	// WHAT GETS IT OFF THE STAND, in the card's Demands block (2026-09-28).
	TestEqual(TEXT("self"), InspectFacts::PushbackText(EPushbackNeed::SelfManoeuvre), FString(TEXT("reverses itself")));
	TestEqual(TEXT("hand"), InspectFacts::PushbackText(EPushbackNeed::HandTug), FString(TEXT("needs a hand tug")));
	TestEqual(TEXT("vehicle"), InspectFacts::PushbackText(EPushbackNeed::VehicleTug), FString(TEXT("needs a tug")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectFactsTaxiwayTest,
	"Airside.Model.InspectFacts.Taxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectFactsTaxiwayTest::RunTest(const FString& Parameters)
{
	// STRIP STAGE 6: the taxiway card - its letter, width and strip, and, restricted, the letter
	// it operates at and what restricts it (spec: "max span 65 m - restricted by building at ...").
	constexpr double Pavement = 2600.0;   // Code F
	const double ReachE = 0.5 * Pavement + IcaoCode::TaxiwayStripFor(EIcaoCode::E, Pavement);
	const double ReachF = 0.5 * Pavement + IcaoCode::TaxiwayStripFor(EIcaoCode::F, Pavement);
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	const double RoadY = 0.5 * (ReachE + ReachF) + Road->GetMaxHalfWidth();

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadSegmentId Taxi = Net->AddStraightSegment(Net->AddNode({ -20000.0, 0.0 }), Net->AddNode({ 20000.0, 0.0 }),
		URoadProfile::MakeTransient(Pavement, 1733.0));
	const FRoadSegmentId RoadSeg = Net->AddStraightSegment(Net->AddNode({ -3000.0, RoadY }), Net->AddNode({ 3000.0, RoadY }), Road);

	{
		FTaxiwayCardFacts Card;
		if (TestTrue(TEXT("a taxiway is described"), InspectFacts::DescribeTaxiway(*Net, Taxi.Index, Card)))
		{
			TestEqual(TEXT("by its pavement's letter"), Card.Letter, FString(TEXT("F")));
			TestEqual(TEXT("and width"), Card.Width, Pavement, 0.5);
			TestFalse(TEXT("unrestricted before the pass has run"), Card.RestrictedTo.IsSet());
			TestEqual(TEXT("its strip is F's"), Card.Strip, IcaoCode::TaxiwayStripFor(EIcaoCode::F, Pavement), 0.5);
		}
	}
	TaxiwayRestriction::Apply(*Net);
	{
		FTaxiwayCardFacts Card;
		if (TestTrue(TEXT("described again"), InspectFacts::DescribeTaxiway(*Net, Taxi.Index, Card)))
		{
			TestTrue(TEXT("restricted to E"), Card.RestrictedTo.IsSet() && Card.RestrictedTo.GetValue() == TEXT("E"));
			TestEqual(TEXT("by the road"), Card.RestrictedBy, FString(TEXT("a service road")));
			TestEqual(TEXT("operating E's strip"), Card.Strip, IcaoCode::TaxiwayStripFor(EIcaoCode::E, Pavement), 0.5);
		}
	}
	{
		FTaxiwayCardFacts Card;
		TestFalse(TEXT("a service road is no taxiway card"), InspectFacts::DescribeTaxiway(*Net, RoadSeg.Index, Card));
	}

	// A STAND THE STRIP COVERS SAYS WHY IT IS CLOSED, in the words the placement refusal uses.
	UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
	const FEntityInstanceId Stand = ServiceLinkFixture::PlaceStand(*Net, *Def, FVector2D(0.0, -8000.0), 0.0);
	// 5 m INSIDE E's reach: the taxiway operates at E after the pass, and its E strip is the one
	// that closes stands (StripWidthOf's ruling) - flush with E's edge would be open.
	const double Near = -(ReachE - 500.0);
	FRoadNetworkTestAccess(*Net).SetEntityOutlineForTest(Stand,
		{ { -2000.0, Near - 4000.0 }, { 2000.0, Near - 4000.0 }, { 2000.0, Near }, { -2000.0, Near } });
	{
		FStandFacts S;
		if (TestTrue(TEXT("the stand is described"), InspectFacts::DescribeStand(nullptr, *Net, Stand.Index, S)))
		{
			TestTrue(FString::Printf(TEXT("and says it is closed by the strip: '%s'"), *S.ClosedBecause),
				S.ClosedBecause.Contains(TEXT("clearance strip")));
		}
	}
	FRoadNetworkTestAccess(*Net).SetEntityOutlineForTest(Stand,
		{ { -2000.0, -ReachF - 5000.0 }, { 2000.0, -ReachF - 5000.0 }, { 2000.0, -ReachF - 1000.0 }, { -2000.0, -ReachF - 1000.0 } });
	{
		FStandFacts S;
		InspectFacts::DescribeStand(nullptr, *Net, Stand.Index, S);
		TestTrue(TEXT("a stand clear of every strip is not closed"), S.ClosedBecause.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectFactsHoldLineTest, "Airside.Model.InspectFacts.HoldLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectFactsHoldLineTest::RunTest(const FString& Parameters)
{
	// ONE WORDING PER KIND: what it is held at, for whom, how long (inspector hold info). The
	// inspector and StatusOf both compose through this, so a sentence pinned here is the one the
	// player reads.
	FAgentHold Hold;
	TestEqual(TEXT("no hold, no line - a card must not say 'waiting' for a mover"),
		InspectFacts::HoldLine(Hold, TEXT("G-HDVK")), FString());

	Hold.WaitingOn = 9;
	Hold.StalledSeconds = 80.4;
	Hold.At = EHoldAt::Runway;
	Hold.RunwayPair = TEXT("09/27");
	TestEqual(TEXT("a runway refusal names the strip by its pair"), InspectFacts::HoldLine(Hold, TEXT("G-HDVK")),
		FString(TEXT("Holding short of runway 09/27 for G-HDVK · 1:20")));
	Hold.RunwayPair.Reset();
	TestEqual(TEXT("with no network to name it, still a runway"), InspectFacts::HoldLine(Hold, TEXT("G-HDVK")),
		FString(TEXT("Holding short of runway for G-HDVK · 1:20")));

	Hold.At = EHoldAt::Behind;
	Hold.StalledSeconds = 12.9;
	TestEqual(TEXT("an edge is a queue - the clock truncates, never rounds up to a second not yet waited"),
		InspectFacts::HoldLine(Hold, TEXT("G-HDVK")), FString(TEXT("Waiting behind G-HDVK · 0:12")));

	Hold.At = EHoldAt::Crossing;
	Hold.StalledSeconds = 5.0;
	TestEqual(TEXT("a node is a crossing"), InspectFacts::HoldLine(Hold, TEXT("Bowser #7")),
		FString(TEXT("Waiting at crossing for Bowser #7 · 0:05")));

	TestEqual(TEXT("an hour of waiting stays m:ss, not a wrapped 0:00"), InspectFacts::MinutesSeconds(3725.0), FString(TEXT("62:05")));
	TestEqual(TEXT("a negative clock reads zero"), InspectFacts::MinutesSeconds(-3.0), FString(TEXT("0:00")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectFactsDeadlockTest, "Airside.Model.InspectFacts.HoldAndDeadlockPartners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectFactsDeadlockTest::RunTest(const FString& Parameters)
{
	// THE CARD'S RAW FACTS: whom it waits for, at what, how long - and, for a ring, who else is in
	// it, from the SAME UGroundTraffic::CurrentDeadlocks the Deadlock alert reads. Scripted through
	// FGroundTrafficTestAccess: the arbitration that jams two aircraft has its own tests, and a real
	// jam would be replanned by the resolver before this could look at it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	const FGuidelineEdgeId Lane = InspJoin(*Net, A, B);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FRoutePlan Plan = TestGraph::Probe(*Net, A, B, ETraversalClass::Aircraft);
	const int32 One = Traffic->DispatchAgent(Net, Plan, Airframe, ETraversalClass::Aircraft, 1.0);
	const int32 Two = Traffic->DispatchAgent(Net, Plan, Airframe, ETraversalClass::Aircraft, 1.0);
	const int32 Queued = Traffic->DispatchAgent(Net, Plan, Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("three aircraft"), One > 0 && Two > 0 && Queued > 0)) { return false; }

	FAgentFacts Facts;
	InspectFacts::DescribeAgent(*Traffic, Net, One, Facts);
	TestFalse(TEXT("a mover holds for nobody"), Facts.Hold.IsSet());
	TestFalse(TEXT("so its status is not the hold line"), Facts.bStatusIsHold);

	const double Stalled = Traffic->Rules.StallSeconds + 10.0;
	FGroundTrafficTestAccess Access(*Traffic);
	Access.ScriptWait(One, FTrafficResource::OfEdge(Lane), Two, Stalled);
	Access.ScriptWait(Two, FTrafficResource::OfNode(B), One, Stalled);
	Access.ScriptWait(Queued, FTrafficResource::OfEdge(Lane), One, Stalled);

	if (!TestTrue(TEXT("described"), InspectFacts::DescribeAgent(*Traffic, Net, One, Facts))) { return false; }
	TestEqual(TEXT("it waits for the other"), Facts.Hold.WaitingOn, Two);
	TestTrue(TEXT("an edge refusal is a queue"), Facts.Hold.At == EHoldAt::Behind);
	TestEqual(TEXT("for as long as its stall clock says"), Facts.Hold.StalledSeconds, Stalled);
	TestTrue(TEXT("the status IS the hold line, so the game may re-say it with a registration"), Facts.bStatusIsHold);
	TestEqual(TEXT("which names the blocker by id here"), Facts.Status,
		InspectFacts::HoldLine(Facts.Hold, FString::Printf(TEXT("aircraft %d"), Two)));
	TestEqual(TEXT("deadlocked with the other member"), Facts.DeadlockedWith, TArray<int32>{ Two });

	InspectFacts::DescribeAgent(*Traffic, Net, Two, Facts);
	TestTrue(TEXT("a node refusal is a crossing"), Facts.Hold.At == EHoldAt::Crossing);
	TestEqual(TEXT("and the other card names the first - both ends of the ring say so"), Facts.DeadlockedWith, TArray<int32>{ One });

	InspectFacts::DescribeAgent(*Traffic, Net, Queued, Facts);
	TestEqual(TEXT("queued behind the ring: holding"), Facts.Hold.WaitingOn, One);
	TestEqual(TEXT("but not IN it - the alert does not count it either"), Facts.DeadlockedWith.Num(), 0);

	// THE SAME LIST AS THE ALERT: the pair the cards report is the one cycle CurrentDeadlocks returns.
	TArray<TArray<int32>> Cycles;
	Traffic->CurrentDeadlocks(Cycles);
	TestEqual(TEXT("one ring, as the alert would see it"), Cycles.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectFactsHoldRunwayTest, "Airside.Model.InspectFacts.HoldAtRunwayNamesThePair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectFactsHoldRunwayTest::RunTest(const FString& Parameters)
{
	// A SURFACE REFUSAL NAMES THE STRIP the way the runway card does (DescribeRunway's pair), so
	// "holding short of 09/27" and a click on that runway agree. Through the actor, because a
	// runway is placed through PlaceRunway with a runway profile (TestGuide::LayRunway).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestTrue(TEXT("laid"), TestGuide::LayRunway(Actor, FVector2D(40000.0, 0.0), FVector2D(-40000.0, 0.0)))) { return false; }
	URoadNetwork& Net = *Actor->Network;
	FRoadSegmentId Runway;
	FRunwayCardFacts Card;
	for (int32 Index = 0; Index < Net.GetSegments().Num() && !Runway.IsSet(); ++Index)
	{
		if (InspectFacts::DescribeRunway(Net, Index, Card)) { Runway = Net.SegmentIdAt(Index); }
	}
	if (!TestTrue(TEXT("a runway segment"), Runway.IsSet())) { return false; }

	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 30000.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 30000.0), false);
	InspJoin(Net, A, B);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FRoutePlan Plan = TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft);
	const int32 Waiter = Traffic->DispatchAgent(&Net, Plan, UAirsideSettings::ResolveDefaultAirframe(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Waiter > 0)) { return false; }
	FGroundTrafficTestAccess(*Traffic).ScriptWait(Waiter, FTrafficResource::OfSurface(Runway), 42, 20.0);

	FAgentFacts Facts;
	if (!TestTrue(TEXT("described"), InspectFacts::DescribeAgent(*Traffic, &Net, Waiter, Facts))) { return false; }
	TestTrue(TEXT("held at the runway"), Facts.Hold.At == EHoldAt::Runway);
	TestEqual(TEXT("named by the runway card's own pair"), Facts.Hold.RunwayPair, Card.Pair);
	TestEqual(TEXT("an unknown blocker is still named - as an aircraft, today's word"), Facts.Status,
		FString::Printf(TEXT("Holding short of runway %s for aircraft 42 · 0:20"), *Card.Pair));
	return true;
}

#endif
