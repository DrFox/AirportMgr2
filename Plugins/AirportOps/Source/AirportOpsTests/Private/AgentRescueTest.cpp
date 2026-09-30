#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/AgentRescue.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
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
