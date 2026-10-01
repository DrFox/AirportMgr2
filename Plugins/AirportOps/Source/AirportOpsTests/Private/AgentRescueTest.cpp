#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/AgentRescue.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Model/RoadGuideline.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * FTestAirport's runway, exit, taxiway and TWO stands, an aircraft taxiing from the exit to the
	 * first, and the flight board relayed exactly as UOpsRuntime::OnAgentPhase relays it - published
	 * into a bus inside the broadcast and heard when Advance drains it (#436), so a test that retires
	 * the aeroplane sees the Gone the board would hear, when it would hear it. World-free.
	 */
	struct FRescueField
	{
		FTestAirport Airport;
		/** The phase changes' bus - see Build. By value: the field is a test local and never copied. */
		FOpsEventBus Bus;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		UAgentRescue* Rescue = nullptr;
		UFlight* Flight = nullptr;
		int32 Plane = 0;

		bool Build()
		{
			const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
			FTestAirportOptions Options;
			Options.StandCount = 2;
			Airport = FTestAirport::Build(Airframe, Options);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Rescue = NewObject<UAgentRescue>(GetTransientPackage());
			Rescue->FlightBoard = Board;

			const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*Airport.Net, Airport.ExitAt,
				ETraversalClass::Aircraft, 200.0);
			if (!Exit.IsSet() || Airport.Stands.Num() < 2)
			{
				return false;
			}
			const FRoutePlan Plan = TestGraph::Probe(*Airport.Net, Exit, Airport.Pose(Airport.Stands[0]), ETraversalClass::Aircraft);
			if (!Plan.IsValid())
			{
				return false;
			}
			Plane = Traffic->DispatchAgent(Airport.Net, Plan, Airframe, ETraversalClass::Aircraft, 0.0);

			Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = Airframe;
			Flight->AgentId = Plane;
			Flight->SetPhaseForTest(EFlightPhase::TaxiIn);
			Board->AddOffer(*Clock, Flight);

			// PUBLISHED, THEN DRAINED - what UOpsRuntime does (#436). This relay used to call the board inside the
			// traffic's broadcast, an ordering production never has.
			UFlightBoard* Bound = Board;
			URoadNetwork* Graph = Airport.Net;
			USimClock* Time = Clock;
			Bus.BeginWiring();
			Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [Bound, Graph, Time](const FAgentPhaseEvent& E)
			{
				Bound->OnAgentPhase(*Graph, *Time, E);
			});
			Bus.EndWiring();
			Traffic->OnAgentPhaseChanged.AddLambda([this](const FAgentTransition& Transition)
			{
				Bus.Publish(FAgentPhaseEvent{ Transition });
			});
			return Plane > 0;
		}

		void Advance(double Seconds)
		{
			for (double T = 0.0; T < Seconds; T += 0.05)
			{
				Traffic->Advance(0.05, Airport.Net);
				Bus.Drain();
			}
		}

		const FRoadAgent* Agent() const { return Traffic->FindAgent(Plane); }
	};
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueAircraftDespawnCancelsFlightTest,
	"AirportOps.Model.AgentRescue.AircraftDespawnCancelsFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueAircraftDespawnCancelsFlightTest::RunTest(const FString& Parameters)
{
	// A DESPAWNED AEROPLANE DID NOT DEPART. FlightPhaseFromTransition books a Retired as Departed, as it books the
	// Gone off the climb - right for one that flew off the runway, a lie for one the player deleted on a taxiway - so the flight is
	// Cancelled first and unhooked, and the Gone that follows moves nothing.
	FRescueField Field;
	if (!TestTrue(TEXT("an aircraft taxiing in, flying a flight"), Field.Build())) { return false; }
	Field.Advance(2.0);
	if (!TestNotNull(TEXT("still taxiing"), Field.Agent())) { return false; }

	TestTrue(TEXT("Despawn is always offered"),
		Field.Rescue->CanUnstick(*Field.Traffic, Field.Plane, EUnstickAction::Despawn).bAllowed);
	const FUnstickVerdict Done = Field.Rescue->Unstick(*Field.Traffic, *Field.Airport.Net, *Field.Clock,
		Field.Plane, EUnstickAction::Despawn);
	TestTrue(TEXT("done"), Done.bAllowed);
	TestNull(TEXT("the aircraft is gone"), Field.Agent());
	TestEqual(TEXT("its flight is CANCELLED, not Departed"), Field.Flight->GetPhase(), EFlightPhase::Cancelled);
	TestEqual(TEXT("and unhooked from the agent"), Field.Flight->AgentId, INDEX_NONE);
	TestNull(TEXT("the board no longer finds it by agent"), Field.Board->FindByAgentForTest(Field.Plane));
	TestEqual(TEXT("it is in history"), Field.Board->GetHistoryCountForTest(), 1);
	TestTrue(TEXT("and it holds nothing - its stand claim went with it"),
		Field.Traffic->HolderOfNode(Field.Airport.Pose(Field.Airport.Stands[0])) != Field.Plane);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueAircraftFindStandTest,
	"AirportOps.Model.AgentRescue.AircraftFindStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueAircraftFindStandTest::RunTest(const FString& Parameters)
{
	// A STRANDED AEROPLANE'S MIDDLE ACTION IS A STAND - it has no depot (user, 2026-09-29). Refused
	// while it moves (Replan's job), done once it is stranded, and it then actually parks on a stand.
	FRescueField Field;
	if (!TestTrue(TEXT("an aircraft taxiing in"), Field.Build())) { return false; }
	Field.Advance(2.0);

	const FUnstickVerdict Moving = Field.Rescue->CanUnstick(*Field.Traffic, Field.Plane, EUnstickAction::SendHome);
	TestFalse(TEXT("refused while it is moving"), Moving.bAllowed);
	TestFalse(TEXT("and says why"), Moving.Why.IsEmpty());
	TestTrue(TEXT("Replan is offered while it moves"),
		Field.Rescue->CanUnstick(*Field.Traffic, Field.Plane, EUnstickAction::Replan).bAllowed);

	if (!TestTrue(TEXT("stranded"), FGroundTrafficTestAccess(*Field.Traffic).Strand(Field.Plane))) { return false; }
	Field.Advance(0.2);
	if (!TestEqual(TEXT("its phase is Stranded"), Field.Agent()->Phase, EAgentPhase::Stranded)) { return false; }

	const FUnstickVerdict Done = Field.Rescue->Unstick(*Field.Traffic, *Field.Airport.Net, *Field.Clock,
		Field.Plane, EUnstickAction::SendHome);
	if (!TestTrue(FString::Printf(TEXT("done (%s)"), *Done.Why.ToString()), Done.bAllowed)) { return false; }
	TestEqual(TEXT("taxiing again"), Field.Agent()->Phase, EAgentPhase::Taxiing);

	for (int32 I = 0; I < 6000 && Field.Agent() != nullptr && Field.Agent()->Phase == EAgentPhase::Taxiing; ++I)
	{
		Field.Advance(0.05);
	}
	const FRoadAgent* P = Field.Agent();
	if (!TestNotNull(TEXT("still there"), P)) { return false; }
	TestEqual(TEXT("it parks"), P->Phase, EAgentPhase::Parked);
	TestTrue(TEXT("on a stand's pose"), P->GoalNode == Field.Airport.Pose(Field.Airport.Stands[0])
		|| P->GoalNode == Field.Airport.Pose(Field.Airport.Stands[1]));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueReplanTest,
	"AirportOps.Model.AgentRescue.Replan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueReplanTest::RunTest(const FString& Parameters)
{
	// TWO HALVES: a moving aircraft already on its best route is REFUSED with that sentence - a
	// "replan" that changes nothing must not report success, or the player is told a stuck agent is
	// fixed - and a stranded one is put back on the pavement toward its own stand.
	FRescueField Field;
	if (!TestTrue(TEXT("an aircraft taxiing in"), Field.Build())) { return false; }
	Field.Advance(2.0);
	const FUnstickVerdict Same = Field.Rescue->Unstick(*Field.Traffic, *Field.Airport.Net, *Field.Clock,
		Field.Plane, EUnstickAction::Replan);
	TestFalse(TEXT("nothing better: refused"), Same.bAllowed);
	TestFalse(TEXT("with a reason"), Same.Why.IsEmpty());
	TestEqual(TEXT("and still taxiing"), Field.Agent()->Phase, EAgentPhase::Taxiing);

	FGroundTrafficTestAccess(*Field.Traffic).Strand(Field.Plane);
	Field.Advance(0.2);
	const FGuidelineNodeId Goal = Field.Agent()->GoalNode;
	const FUnstickVerdict Done = Field.Rescue->Unstick(*Field.Traffic, *Field.Airport.Net, *Field.Clock,
		Field.Plane, EUnstickAction::Replan);
	TestTrue(FString::Printf(TEXT("stranded: done (%s)"), *Done.Why.ToString()), Done.bAllowed);
	TestEqual(TEXT("taxiing"), Field.Agent()->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("to the goal it had"), Field.Agent()->GoalNode, Goal);
	return true;
}

namespace AgentRescueHeld
{
	/**
	 * A fork, the route-change tests' own: A (0,0) east to B (3000,0), on to C (6000,0) and E (9000,0), and B north to
	 * D (3000,3000) and D on to C - so an agent refused BC has a way round by D, and one that is not refused has no
	 * better route than the one it is on. Authored edges; a van (a zero-size body fits every edge and turn here).
	 */
	struct FFork
	{
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UAgentRescue* Rescue = nullptr;
		FGuidelineNodeId A, B, C, D, E;
		FGuidelineEdgeId BC;

		void Build()
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			A = TestGraph::Node(*Net, 0.0, 0.0);
			B = TestGraph::Node(*Net, 3000.0, 0.0);
			C = TestGraph::Node(*Net, 6000.0, 0.0);
			D = TestGraph::Node(*Net, 3000.0, 3000.0);
			E = TestGraph::Node(*Net, 9000.0, 0.0);
			TestGraph::FJoinOptions Authored;
			Authored.bDerived = false;
			TestGraph::Join(*Net, A, B, Authored);
			BC = TestGraph::Join(*Net, B, C, Authored);
			TestGraph::Join(*Net, B, D, Authored);
			TestGraph::Join(*Net, D, C, Authored);
			TestGraph::Join(*Net, C, E, Authored);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Rescue = NewObject<UAgentRescue>(GetTransientPackage());
		}

		/** A van dispatched From -> E the short way (by BC), standing at From - dispatched, never advanced, so stopped. */
		int32 Van(FGuidelineNodeId From)
		{
			FVehicle Van;
			Van.Chassis.Ground.MaxTurnRateDegPerSec = 90.0;
			return Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, From, E, ETraversalClass::GroundVehicle), Van,
				ETraversalClass::GroundVehicle, 0.0);
		}

		/** Whether AgentId's route still takes BC. */
		bool TakesBC(int32 AgentId) const
		{
			const FRoadAgent* Agent = Traffic->FindAgent(AgentId);
			return Agent != nullptr && Agent->Follower.Plan.Steps.ContainsByPredicate(
				[this](const FRouteStep& Step) { return Step.Edge == BC; });
		}
	};
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueReplanHonoursTheResolverBoundTest,
	"AirportOps.Model.AgentRescue.ReplanHonoursTheResolverBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueReplanHonoursTheResolverBoundTest::RunTest(const FString& Parameters)
{
	// ONE BAN (#429): the Unstick's Replan turns a held agent round what holds it exactly when the deadlock resolver
	// would - UGroundTraffic::ReplanAroundBlocker, the resolver's own per-agent step. It used to copy the resolver's ban
	// and drop its UPPER BOUND: an agent stopped a whole edge short of the step it was refused was turned "round" it
	// from a node it was nowhere near - which the resolver refuses precisely because the alternative is another edge
	// out of THAT node. Both vans are refused edge BC (a phantom holder); one stands at B, one 3000 uu back at A.
	using namespace AgentRescueHeld;
	FFork F;
	F.Build();
	constexpr int32 Phantom = 4242;

	// AT ITS BLOCK: standing at B, refused BC (its step 0) - 0 uu to the node, inside gap + half a footprint. Turned.
	const int32 AtB = F.Van(F.B);
	if (!TestTrue(TEXT("a van at B, going by BC"), AtB > 0 && F.TakesBC(AtB))) { return false; }
	FGroundTrafficTestAccess(*F.Traffic).ScriptWait(AtB, FTrafficResource::OfEdge(F.BC), Phantom, 30.0, 0);
	const FUnstickVerdict Turned = F.Rescue->Unstick(*F.Traffic, *F.Net, *F.Clock, AtB, EUnstickAction::Replan);
	TestTrue(FString::Printf(TEXT("held AT its block: the Unstick turns it round (%s)"), *Turned.Why.ToString()), Turned.bAllowed);
	TestFalse(TEXT("and its route no longer takes the edge that refused it - it goes by D"), F.TakesBC(AtB));

	// FAR FROM ITS BLOCK: standing at A, refused BC (its step 1), 3000 uu ahead - past gap + half a footprint + the
	// node's reach. The resolver would not turn it; neither does the Unstick. Before #429 part 2 it banned BC and
	// sent the van by D from a node 3000 uu away.
	const int32 AtA = F.Van(F.A);
	if (!TestTrue(TEXT("a van at A, going by BC"), AtA > 0 && F.TakesBC(AtA))) { return false; }
	FGroundTrafficTestAccess(*F.Traffic).ScriptWait(AtA, FTrafficResource::OfEdge(F.BC), Phantom, 30.0, 1);
	const FUnstickVerdict Far = F.Rescue->Unstick(*F.Traffic, *F.Net, *F.Clock, AtA, EUnstickAction::Replan);
	TestFalse(TEXT("held a whole edge short of its block: not turned round it - a fresh search finds the route it has"), Far.bAllowed);
	TestTrue(TEXT("so its route still takes BC (red when the rescue had no upper bound: it went by D)"), F.TakesBC(AtA));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueRefusalsTest,
	"AirportOps.Model.AgentRescue.Refusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueRefusalsTest::RunTest(const FString& Parameters)
{
	// THE MENU AND THE ACTION AGREE: every refusal CanUnstick gives, Unstick gives too, with the same
	// sentence and nothing changed - they share Decide. Parked is the phase every action but Despawn
	// refuses for an aircraft at a stand.
	FRescueField Field;
	if (!TestTrue(TEXT("an aircraft taxiing in"), Field.Build())) { return false; }
	for (int32 I = 0; I < 6000 && Field.Agent()->Phase != EAgentPhase::Parked; ++I)
	{
		Field.Advance(0.05);
	}
	if (!TestEqual(TEXT("parked on its stand"), Field.Agent()->Phase, EAgentPhase::Parked)) { return false; }

	for (const EUnstickAction Action : { EUnstickAction::Replan, EUnstickAction::SendHome })
	{
		const FUnstickVerdict Asked = Field.Rescue->CanUnstick(*Field.Traffic, Field.Plane, Action);
		const FUnstickVerdict Ran = Field.Rescue->Unstick(*Field.Traffic, *Field.Airport.Net, *Field.Clock, Field.Plane, Action);
		const FString Name = UEnum::GetValueAsString(Action);
		TestFalse(FString::Printf(TEXT("%s refused on a stand"), *Name), Asked.bAllowed);
		TestFalse(FString::Printf(TEXT("%s: the action refuses too"), *Name), Ran.bAllowed);
		TestEqual(FString::Printf(TEXT("%s: with the menu's own reason"), *Name), Ran.Why.ToString(), Asked.Why.ToString());
		TestEqual(FString::Printf(TEXT("%s: still parked"), *Name), Field.Agent()->Phase, EAgentPhase::Parked);
	}
	TestFalse(TEXT("an unknown agent is refused"),
		Field.Rescue->CanUnstick(*Field.Traffic, 9999, EUnstickAction::Despawn).bAllowed);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueHeldPushTest,
	"AirportOps.Model.AgentRescue.HeldPushRefusalMatchesTheCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueHeldPushTest::RunTest(const FString& Parameters)
{
	// A HELD PUSH'S UNSTICK SAYS WHAT ITS CARD SAYS (#501 review). A push that ended where its taxi out cannot begin holds
	// for a way to the runway, and its card reads "No way to the runway - waiting"; Replan refused it with "Coming off its
	// stand - wait for it to finish", which it was not doing. Mid-push the old sentence stands; held, the card's words.
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	FTestAirportOptions Options;
	Options.StandCount = 2;
	const FTestAirport Air = FTestAirport::Build(Airframe, Options);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	UAgentRescue* Rescue = NewObject<UAgentRescue>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*Air.Net, Air.Threshold - FVector2D(1000.0, 0.0), Airframe, 0.0);
	if (!TestTrue(TEXT("an arrival is admitted"), Id > 0)) { return false; }
	for (int32 Tick = 0; Tick < 20 * 900 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(0.05, Air.Net);
	}
	if (!TestEqual(TEXT("parked"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked)) { return false; }
	if (!TestEqual(TEXT("it departs"), Traffic->DepartAgent(Id, *Air.Net), EDepartureRefusal::None)) { return false; }
	for (int32 Tick = 0; Tick < 30; ++Tick) { Traffic->Advance(1.0 / 30.0, Air.Net); }
	if (!TestTrue(TEXT("pushing back, not holding"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Manoeuvring
		&& !Traffic->FindAgent(Id)->IsHoldingForTaxiOut())) { return false; }
	const FString Pushing = Rescue->CanUnstick(*Traffic, Id, EUnstickAction::Replan).Why.ToString();
	TestTrue(FString::Printf(TEXT("mid-push, Replan says it is coming off its stand ('%s')"), *Pushing),
		Pushing.StartsWith(TEXT("Coming off its stand")));

	// THE RUNWAY'S LINES GO MID-PUSH (Airside.Model.Traffic.HeldTaxiOut.MidPushRunwayLossHolds' edit): it holds at the push's end.
	TArray<FGuidelineNodeId> OnRunway;
	for (int32 Index = 0; Index < Air.Net->GetGuidelineNodes().Num(); ++Index)
	{
		const FGuidelineNode& Node = Air.Net->GetGuidelineNodes()[Index];
		if (Node.bAlive && FMath::Abs(Node.Position.Y) <= 3000.0)
		{
			OnRunway.Add(Air.Net->GuidelineNodeIdAt(Index));
		}
	}
	for (const FGuidelineNodeId Node : OnRunway) { Air.Net->RemoveGuidelineNode(Node); }
	Traffic->OnGraphRebuilt(*Air.Net);
	for (int32 Tick = 0; Tick < 30 * 300 && !Traffic->FindAgent(Id)->IsHoldingForTaxiOut(); ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Air.Net);
	}
	const FRoadAgent* Held = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("its push ran out and it holds, still manoeuvring"),
		Held->IsHoldingForTaxiOut() && Held->Phase == EAgentPhase::Manoeuvring)) { return false; }

	const FUnstickVerdict Verdict = Rescue->CanUnstick(*Traffic, Id, EUnstickAction::Replan);
	const FString Card = InspectFacts::StatusOf(*Held);
	TestFalse(TEXT("Replan is refused - the retry is what moves it, once a line reaches it"), Verdict.bAllowed);
	TestTrue(FString::Printf(TEXT("in the card's words (card '%s', refusal '%s')"), *Card, *Verdict.Why.ToString()),
		Card.StartsWith(TEXT("No way to the runway")) && Verdict.Why.ToString().StartsWith(TEXT("No way to the runway")));
	// FIND STAND, ITS OWN WORDS (#501 re-review): it is a departure, and there is no stand to seek.
	const FString Home = Rescue->CanUnstick(*Traffic, Id, EUnstickAction::SendHome).Why.ToString();
	TestTrue(FString::Printf(TEXT("Find stand says it is departing, not why it cannot move ('%s')"), *Home),
		Home.StartsWith(TEXT("Departing - it waits for a way to the runway")));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRescueRuntimeForwardsTest,
	"AirportOps.Present.UnstickForwards",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRescueRuntimeForwardsTest::RunTest(const FString& Parameters)
{
	// THE SEAM, AT THE COMPOSITION: UOpsRuntime::CanUnstick / Unstick supply the ATTACHED actor's
	// traffic, network and clock to UAgentRescue. Every AgentRescue test above hands those in by hand,
	// so they would all stay green with the runtime's forwarders reading a null model - which is what
	// the driver calls. Spawned actor, real agent, real runtime.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>(GetTransientPackage());
	TestFalse(TEXT("unattached: refused, not a crash"), Runtime->CanUnstick(1, EUnstickAction::Despawn).bAllowed);
	TestFalse(TEXT("unattached: Unstick refused too"), Runtime->Unstick(1, EUnstickAction::Despawn).bAllowed);

	Runtime->Attach(Actor);
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
		UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
	TestNotNull(TEXT("the rescue is the runtime's"), Runtime->GetAgentRescue());
	TestTrue(TEXT("attached: Despawn offered for the real agent"), Runtime->CanUnstick(Id, EUnstickAction::Despawn).bAllowed);
	TestTrue(TEXT("attached: Despawn runs"), Runtime->Unstick(Id, EUnstickAction::Despawn).bAllowed);
	TestNull(TEXT("and the agent is gone from the ATTACHED model"), Actor->GetTraffic()->GetModel()->FindAgent(Id));
	return true;
}

#endif
