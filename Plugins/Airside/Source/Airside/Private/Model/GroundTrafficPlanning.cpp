// UGroundTraffic's half of space-time taxi planning (spec 2026-10-02): the requests it makes of UTaxiPlanning for an
// arrival and a departure, the per-tick tracking of every cleared aircraft, and the pushes a booked departure is due.
// The planning owner itself - table, clearances, order - is Model/TaxiPlanning.h. WHEN an aircraft is cleared stays
// this class's (DispatchArrival, DepartAgent); WHAT it is cleared on is the planner's.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DepartureAsk.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/DeparturePlanner.h"
#include "Model/LandingRun.h"
#include "Model/PushbackPlanner.h"
#include "Model/PushbackRun.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiPlanning.h"
#include "Model/RoadAgent.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficClaims.h"

namespace
{
	/** Why a taxi plan was refused, in the log's words. */
	FString TaxiRefusalText(const FTaxiPlan& Taxi)
	{
		return Taxi.Result == ETaxiPlanResult::NoRoute
			? FString::Printf(TEXT("no route it may take (%s)"), *UEnum::GetValueAsString(Taxi.RouteResult))
			: FString(TEXT("no free window round the plans already booked"));
	}
}

void UGroundTraffic::PostInitProperties()
{
	Super::PostInitProperties();
	// EVERY INSTANCE BUT THE CDO (and an archetype) MAKES ITS OWN, a duplicate included: a Transient pointer is reset to
	// the CDO's - null - by a duplication, and this runs after that reset (memory: transient subobject pointers reset on
	// duplication). NAME_None, not a fixed name: a duplication may carry the source's planning object across too, and a
	// second object of one name under one outer is a fatal error, not a replacement.
	// ENFORCED BY: Airside.Model.TaxiPlan.PlanningIsTheTrafficsOwn
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		TaxiPlanning = NewObject<UTaxiPlanning>(this, NAME_None, RF_Transient);
	}
}

uint32 UGroundTraffic::TaxiPlanRevision() const
{
	return TaxiPlanning != nullptr ? TaxiPlanning->Revision() : 0;
}

FTaxiRequest UGroundTraffic::TaxiInRequestNow(const FArrivalPlan& Plan, const FAirframe& Airframe) const
{
	// ON THE SIM CLOCK, which the plan's every window is on: the agents move on it (Advance integrates it), it pauses with
	// the sim, and it needs no world. The arrival reaches its exit when its landing - flown, not estimated - says.
	return ArrivalPlanner::TaxiInRequest(Plan, SimSeconds + FLandingRun::SecondsToVacate(Plan.End, Airframe, Plan.VacateAt));
}

EArrivalRefusal UGroundTraffic::TaxiInRefusal(const URoadNetwork& Network, const FArrivalPlan& Plan, const FAirframe& Airframe)
{
	if (!Plan.IsValid())
	{
		return Plan.Why;
	}
	if (TaxiPlanning == nullptr)
	{
		return EArrivalRefusal::None;
	}
	TArray<int32> Revoke;
	const FTaxiPlan Taxi = TaxiPlanning->PlanArrival(Network, Airframe, Rules, TaxiInRequestNow(Plan, Airframe), Revoke);
	if (Taxi.IsPlanned())
	{
		return EArrivalRefusal::None;
	}
	// WHO IS WAITING, so DiffFreedom says so when the table next frees (OnTaxiPlansFreed) - the queue's wake-up.
	bArrivalAwaitsTaxiPlan = true;
	TaxiPlanning->NoteRefused(0, FString::Printf(TEXT("arrival to stand node %d"), Plan.StandNode().Index), TaxiRefusalText(Taxi));
	return EArrivalRefusal::NoTaxiPlan;
}

bool UGroundTraffic::BookTaxiIn(int32 Id, const URoadNetwork& Network, const FTaxiPlan& Taxi, const TArray<int32>& Revoke)
{
	// ONLY ONE STILL ON ITS STAND (ruling 2), asked of the agent as well as of the clearance's stage: the stage moves
	// where the motion starts (MarkMoving), and the phase is the motion itself - two witnesses to one fact.
	TArray<int32> Parked;
	for (const int32 Departure : Revoke)
	{
		const FRoadAgent* Agent = FindAgent(Departure);
		if (Agent == nullptr || Agent->Phase == EAgentPhase::Parked)
		{
			Parked.Add(Departure);
		}
	}
	TArray<int32> Revoked;
	if (!TaxiPlanning->BookArrival(Network, Id, Taxi, Parked, SimSeconds, Revoked))
	{
		return false;
	}
	for (const int32 Departure : Revoked)
	{
		// BACK ON THE PUSH WATCH, asked whole at the next diff (revision 0 matches nothing): its turnaround heard
		// PushbackBlocked while it was booked, and hears again only through the watch.
		FPushWatch& Watch = PushWatch.Add(Departure);
		Watch.Network = &Network;
	}
	return true;
}

int32 UGroundTraffic::RefuseNoTaxiPlan(const FAirframe& Airframe, const TCHAR* Detail)
{
	const FString Sentence = ArrivalPlanner::DescribeRefusal(EArrivalRefusal::NoTaxiPlan, Airframe.Wingspan);
	UE_LOG(LogAirsideTraffic, Warning, TEXT("%s%s"), *Sentence, Detail);
	bArrivalAwaitsTaxiPlan = true;
	OnArrivalRefused.Broadcast(EArrivalRefusal::NoTaxiPlan, Sentence);
	return 0;
}

FTaxiRequest UGroundTraffic::TaxiOutRequest(const FRoadAgent& Agent, const FAirframe& Aircraft, const FDeparturePlan& Departure,
	const FPushbackPlan* Push) const
{
	FTaxiRequest Out;
	Out.Start = Agent.GoalNode;
	Out.Goal = Departure.Route.Steps.Num() > 0 ? Departure.Route.Steps.Last().To : FGuidelineNodeId();
	Out.Holder = Agent.Id;
	Out.DepartAt = SimSeconds;
	if (Push != nullptr)
	{
		// THE PUSH'S ERRAND FOR THE TAXI ON, as PushbackPlanner searches it; the push itself timed by its own run, flown.
		Out.Errand = ERouteErrand::PushbackTaxiOut;
		Out.PushSteps = Push->PushRoute.Steps;
		Out.PushSeconds = FPushbackRun::SecondsFor(Push->PushRoute, Rules.PushSpeedFor(Aircraft.PushbackNeed), Rules.PushAccel);
	}
	else
	{
		// STRAIGHT OUT: the errand DeparturePlanner found the route under - a backtrack along the strip, or to an entry.
		Out.Errand = Departure.bBacktrack ? ERouteErrand::DepartureBacktrack : ERouteErrand::DepartureToEntry;
	}
	return Out;
}

void UGroundTraffic::TrackTaxiPlans(const URoadNetwork& Network)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	// THE LEVEL'S MARGIN IS THE TABLE'S HEADWAY - one knob, the time twin of the gap (FTaxiReservations::SetHeadway).
	TaxiPlanning->SetHeadway(Rules.TaxiPlanMargin);
	for (const int32 Holder : TaxiPlanning->Holders())
	{
		const FRoadAgent* Agent = FindAgent(Holder);
		if (Agent == nullptr)
		{
			// GONE - airborne, retired, cleared: nothing of it may be left for anyone to wait on.
			TaxiPlanning->Drop(Holder, nullptr);
			continue;
		}
		if (!TaxiPlanning->Track(*Agent, Occupancy, Rules, SimSeconds))
		{
			// ITS ROUTE CHANGED UNDER IT - the resolver's replan, a redirect, a re-offer: re-planned along the new one from
			// where it is (taxi planning PR 3), unplanned only if nothing fits. COPIED: the re-plan replaces the clearance.
			const FTaxiClearance Old = *TaxiPlanning->Find(Holder);
			if (ReplanTaxi(*Agent, Network, ETaxiUnplanned::RouteChanged,
				TEXT("its route changed under it (a replan, a redirect or a re-offer) and no plan fits the new one"), &Old))
			{
				RequeueAfterReplan(*Agent, Network, Old);
			}
		}
	}
}

bool UGroundTraffic::ReplanTaxi(const FRoadAgent& Agent, const URoadNetwork& Network, ETaxiUnplanned Cause, const TCHAR* Why,
	const FTaxiClearance* Old)
{
	const FAirframe* Aircraft = Agent.AsAircraft();
	if (TaxiPlanning == nullptr || Aircraft == nullptr)
	{
		return false;
	}
	// WHAT IT WAS CLEARED FOR: from its old clearance, else from the unplanned record a retry keeps.
	const FTaxiUnplanned* Record = TaxiPlanning->FindUnplanned(Agent.Id);
	const ETaxiClearanceKind Kind = Old != nullptr ? Old->Kind : Record != nullptr ? Record->Kind
		: Agent.bDepartureArmed ? ETaxiClearanceKind::TaxiOut : ETaxiClearanceKind::TaxiIn;
	ERouteErrand Errand = Old != nullptr ? Old->QueueErrand : Record != nullptr ? Record->Errand : ERouteErrand::Unset;
	if (Errand == ERouteErrand::Unset)
	{
		// THE TAXI-IN ERRAND FROM ITS ONE OWNER (ArrivalPlanner::TaxiInRequest - rule 4), not spelled here.
		Errand = Kind == ETaxiClearanceKind::TaxiIn ? ArrivalPlanner::TaxiInRequest(FArrivalPlan(), 0.0).Errand
			: ERouteErrand::DepartureToEntry;
	}
	const FString Because = Why;
	auto Fail = [this, &Agent, Cause, &Because, Kind, Errand]()
	{
		TaxiPlanning->MarkUnplanned(Agent.Id, Cause, Because, Kind, Errand);
		return false;
	};
	const double Margin = FMath::Max(0.0, Rules.TaxiPlanMargin);
	const double Cap = FMath::Max(Aircraft->Chassis.Ground.Taxi.SpeedCap, 1.0);

	// PARKED: its stand held for ever, as a plan that arrived there holds it; a departure booked to push later is back
	// on the push watch, re-planned when it is asked again (as a revoked one is).
	if (Agent.Phase == EAgentPhase::Parked)
	{
		if (Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
		{
			TaxiPlanning->Drop(Agent.Id, nullptr);
			return false;
		}
		FTaxiPlan Stay;
		Stay.Result = ETaxiPlanResult::Planned;
		Stay.Passes = { { FTaxiResource::Node(Agent.GoalNode), { Agent.Id, SimSeconds, FTaxiReservations::Forever } } };
		const bool bHeld = TaxiPlanning->Book(Network, Agent.Id, Kind, Stay, ETaxiClearanceStage::Done, FRoutePlan(), SimSeconds);
		if (Old != nullptr && Old->Stage == ETaxiClearanceStage::Booked)
		{
			FPushWatch& Watch = PushWatch.Add(Agent.Id);
			Watch.Network = &Network;
		}
		return bHeld;
	}

	FTaxiRequest Request;
	Request.Errand = Errand;
	Request.Holder = Agent.Id;
	FRoutePlan Live;
	int32 Prefix = 0;
	TArray<FTaxiPass> PrefixPasses;
	FRoutePlan PushRoute;
	TArray<FTaxiResource> PushOnly;
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	switch (Agent.Phase)
	{
	case EAgentPhase::Taxiing:
	{
		// FROM THE END OF THE STEP IT IS ON, when it gets there at its speed (floored: a stopped aircraft still sets off);
		// that step is held from now until it leaves - the prefix.
		Live = Agent.PlanInProgress();
		const double T = FClaimPass::CentreOf(Agent);
		const int32 Current = CurrentStep(Live, T);
		if (!Live.Steps.IsValidIndex(Current))
		{
			return Fail();
		}
		const FRouteStep& On = Live.Steps[Current];
		const double Speed = FMath::Abs(Agent.SpeedAlongPlan());
		double Reach = SimSeconds + FMath::Max(0.0, On.EndDistance - T) / FMath::Max(Speed, 0.25 * Cap);
		const ETaxiWay Way = On.bReversed ? ETaxiWay::BToA : ETaxiWay::AToB;
		Prefix = Current + 1;
		FTaxiPlan Tail;
		if (Prefix == Live.Steps.Num())
		{
			// ON ITS LAST STEP: nothing left to plan but staying - its goal, and the step under it, for ever.
			Tail.Result = ETaxiPlanResult::Planned;
			Tail.Passes = { { FTaxiResource::Node(On.To), { Agent.Id, Reach - Margin, FTaxiReservations::Forever } } };
			Tail.PushAt = Reach;
			Tail.Arrival = Reach;
			PrefixPasses.Add({ FTaxiResource::Edge(On.Edge), { Agent.Id, SimSeconds, FTaxiReservations::Forever, Way } });
		}
		else
		{
			// SOMEBODY ON THE NODE AHEAD WHEN IT WOULD GET THERE: it waits for them where it is, on its own step.
			double Shift = 0.0;
			if (!TaxiPlanning->GetTable().EarliestFit(FTaxiResource::Node(On.To), ETaxiWay::Any, Reach, Reach + 0.001, Agent.Id, Shift))
			{
				return Fail();
			}
			Reach += Shift;
			Request.Start = On.To;
			Request.Goal = Live.Steps.Last().To;
			Request.DepartAt = Reach;
			Request.bStartsRolling = Shift <= 0.0 && Speed > 100.0;
			Request.bMayWaitAtStart = FTaxiPlanner::CanHoldAt(Network, Rules, On.Edge, On.To);
			Request.Along.Append(Live.Steps.GetData() + Prefix, Live.Steps.Num() - Prefix);
			Tail = TaxiPlanning->bRefuseReplansForTest ? FTaxiPlan() : TaxiPlanning->Plan(Network, *Aircraft, Rules, Request);
			if (!Tail.IsPlanned())
			{
				return Fail();
			}
			// THE STEP IT IS ON, from now - or, behind one ahead of it the same way, a headway after that one (the order's
			// FIFO) - until it leaves.
			const double To = Tail.PushAt + Margin;
			double Behind = 0.0;
			TaxiPlanning->GetTable().EarliestFit(FTaxiResource::Edge(On.Edge), Way, SimSeconds, To, Agent.Id, Behind);
			const double From = FMath::Min(SimSeconds + Behind, To - 0.001);
			PrefixPasses.Add({ FTaxiResource::Edge(On.Edge), { Agent.Id, From, To, Way } });
		}
		return TaxiPlanning->BookAlong(Network, Agent.Id, Kind, ETaxiClearanceStage::Moving, Live, Prefix, Tail, PrefixPasses,
			PushRoute, PushOnly, SimSeconds) || Fail();
	}
	case EAgentPhase::Arriving:
		// ON FINAL: from its exit, rolling, at the vacate time its first plan had - the landing is flown, not re-estimated.
		if (Old == nullptr || !Agent.TaxiInPlan.IsValid())
		{
			return Fail();
		}
		Live = Agent.TaxiInPlan;
		Request.DepartAt = Old->Plan.PushAt;
		Request.bMayWaitAtStart = false;
		Request.bStartsRolling = true;
		break;
	case EAgentPhase::Manoeuvring:
	{
		// PUSHING: from where the push ends, when it ends; the push's ground held from now till then, released at the
		// hand-over as a planned push's is (PushOnly).
		if (!Agent.TaxiOutPlan.IsValid())
		{
			return Fail();
		}
		Live = Agent.TaxiOutPlan;
		PushRoute = Agent.Pushback.Plan;
		const double PushLeft = FMath::Max(0.0, PushRoute.Length - Agent.Pushback.Travelled)
			/ FMath::Max(Rules.PushSpeedFor(Aircraft->PushbackNeed), 1.0);
		Request.DepartAt = SimSeconds + PushLeft;
		for (const FRouteStep& Step : PushRoute.Steps)
		{
			for (const FTaxiResource& Resource : { FTaxiResource::Edge(Step.Edge), FTaxiResource::Node(Step.To) })
			{
				PrefixPasses.Add({ Resource, { Agent.Id, SimSeconds, Request.DepartAt + Margin, ETaxiWay::Any } });
				PushOnly.Add(Resource);
			}
		}
		break;
	}
	case EAgentPhase::Parked:      // above
	case EAgentPhase::Departing:   // lined up: the runway's own
	case EAgentPhase::Reversing:   // never an aircraft on a taxiway
	case EAgentPhase::Gone:
	case EAgentPhase::Stranded:
		TaxiPlanning->Drop(Agent.Id, nullptr);
		return false;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	Request.Start = StepFromNode(Live, 0);
	Request.Goal = Live.Steps.Num() > 0 ? Live.Steps.Last().To : FGuidelineNodeId();
	Request.Along = Live.Steps;
	const FTaxiPlan Tail = TaxiPlanning->bRefuseReplansForTest ? FTaxiPlan() : TaxiPlanning->Plan(Network, *Aircraft, Rules, Request);
	if (!Tail.IsPlanned() || !TaxiPlanning->BookAlong(Network, Agent.Id, Kind, ETaxiClearanceStage::Moving, Live, 0, Tail,
		PrefixPasses, PushRoute, PushOnly, SimSeconds))
	{
		return Fail();
	}
	return true;
}

void UGroundTraffic::RequeueAfterReplan(const FRoadAgent& Agent, const URoadNetwork& Network, const FTaxiClearance& Old)
{
	// STILL QUEUEING, if its route still ends short of the entry: the entry re-found by where it was when the handle died
	// in a rebuild, as the re-resolve re-finds every node.
	const FTaxiClearance* Now = TaxiPlanning->Find(Agent.Id);
	if (Now == nullptr || !Old.QueueFor.IsSet() || Now->Plan.Route.Steps.Num() == 0)
	{
		return;
	}
	FGuidelineNodeId Entry = Old.QueueFor;
	if (Network.GetGuidelineNode(Entry) == nullptr)
	{
		Entry = RouteSearch::FindNearestNode(Network, Old.QueueAt, Agent.Class, Rules.ResolveRadius);
	}
	if (Entry.IsSet() && Now->Plan.Route.Steps.Last().To != Entry)
	{
		TaxiPlanning->SetQueued(Agent.Id, Entry, Old.QueueErrand, Old.QueueAt);
	}
}

void UGroundTraffic::ReplanAfterRebuild(const URoadNetwork& Network, const TArray<int32>& Order, const TMap<int32, FTaxiClearance>& Old)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	// THE MOVING FIRST, IN THE ORDER THEY HELD - each planned round those already re-booked, as each was booked behind
	// them before - then the parked. Nothing else runs inside OnGraphRebuilt, so nobody new is admitted ahead of them.
	int32 Replanned = 0;
	int32 Lost = 0;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		for (const int32 Id : Order)
		{
			const FRoadAgent* Agent = FindAgent(Id);
			const FTaxiClearance* Was = Old.Find(Id);
			if (Agent == nullptr || Was == nullptr || (Agent->Phase == EAgentPhase::Parked) != (Pass == 1))
			{
				continue;
			}
			if (ReplanTaxi(*Agent, Network, ETaxiUnplanned::LayoutEdit, TEXT("a layout edit rebuilt the taxiways under it"), Was))
			{
				RequeueAfterReplan(*Agent, Network, *Was);
				++Replanned;
			}
			else
			{
				Lost += TaxiPlanning->FindUnplanned(Id) != nullptr ? 1 : 0;
			}
		}
	}
	if (Order.Num() > 0)
	{
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: layout rebuilt - %d plan(s) re-made in their old order, %d unplanned"),
			Replanned, Lost);
	}
}

void UGroundTraffic::RetryUnplanned(const URoadNetwork& Network)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	// ARRIVED, GONE, OR OFF THE TAXIWAYS: no longer unplanned (spec §3 - the alert clears on arrival).
	for (const int32 Id : TaxiPlanning->UnplannedHolders())
	{
		const FRoadAgent* Agent = FindAgent(Id);
		if (Agent == nullptr || (Agent->Phase != EAgentPhase::Taxiing && Agent->Phase != EAgentPhase::Arriving
			&& Agent->Phase != EAgentPhase::Manoeuvring))
		{
			TaxiPlanning->ClearUnplanned(Id);
		}
	}
	// THE REST TRIED AGAIN when the table has moved - a plan along the route it drives, from where it is now.
	for (const int32 Id : TaxiPlanning->UnplannedDueRetry(SimSeconds))
	{
		const FRoadAgent* Agent = FindAgent(Id);
		const FTaxiUnplanned* Record = TaxiPlanning->FindUnplanned(Id);
		if (Agent == nullptr || Record == nullptr || Agent->Phase != EAgentPhase::Taxiing)
		{
			continue;
		}
		TaxiPlanning->NoteUnplannedTried(Id, SimSeconds);
		const FString Why = Record->Why;
		ReplanTaxi(*Agent, Network, Record->Cause, *Why, nullptr);
	}
}


void UGroundTraffic::StartDuePushes(const URoadNetwork& Network)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	for (const int32 Id : TaxiPlanning->DuePushes(SimSeconds))
	{
		const FRoadAgent* Agent = FindAgent(Id);
		const FTaxiClearance* Clearance = TaxiPlanning->Find(Id);
		if (Agent == nullptr || Clearance == nullptr || Agent->Phase != EAgentPhase::Parked)
		{
			TaxiPlanning->Drop(Id, TEXT("its push never came - the aircraft left its stand another way"));
			continue;
		}
		// ITS TURN BY THE ORDER, and the ground under the push free of bodies - the claim table's own check, as
		// DepartAgent's: a push is granted whole or not at all. Not yet: asked again next step - a list of a few due
		// pushes, read, not a plan made.
		if (TaxiPlanning->PushWaitingFor(Id) != 0
			|| (Clearance->PushRoute.IsValid() && !IsPushGroundFree(Id, Clearance->PushRoute, Clearance->PushRoute.Length)))
		{
			continue;
		}
		// COPIED: starting the push announces a phase change, and a listener may touch the clearance map.
		const FRoutePlan Push = Clearance->PushRoute;
		const FRoutePlan Taxi = Clearance->Plan.Route;
		UE_LOG(LogAirsideTaxiPlan, Log, TEXT("TaxiPlan: agent %d %s as planned"), Id,
			Push.IsValid() ? TEXT("pushes back") : TEXT("taxis out"));
		if (!StartPlannedDeparture(Id, Network, Push, Taxi))
		{
			TaxiPlanning->Drop(Id, TEXT("its push could not be started"));
		}
	}
}

bool UGroundTraffic::StartPlannedDeparture(int32 AgentId, const URoadNetwork& Network, const FRoutePlan& PushRoute,
	const FRoutePlan& TaxiRoute)
{
	const int32 Index = FindIndex(AgentId);
	const FAirframe* Aircraft = Index != INDEX_NONE ? Agents[Index].AsAircraft() : nullptr;
	if (Aircraft == nullptr)
	{
		return false;
	}
	if (!PushRoute.IsValid())
	{
		// DepartOrdered, NOT Redirected: this is the taxi OUT, and the flight board reads it so (review M4) - it used to
		// ask the live agent whether a departure was armed, a drain later.
		if (!RedirectAgent(AgentId, &Network, TaxiRoute, EAgentEvent::DepartOrdered))
		{
			return false;
		}
		TaxiPlanning->MarkMoving(AgentId);   // as the push's, below
		return true;
	}

	FRoadAgent& Agent = Agents[Index];
	const EAgentPhase Before = Agent.Phase;
	// COPIED, because StartPushback assigns the agent's own airframe from its argument and
	// Aircraft points at that very field.
	const FAirframe Own = *Aircraft;
	if (!Agent.StartPushback(PushRoute, TaxiRoute, Own,
		Rules.PushSpeedFor(Own.PushbackNeed), Rules.PushAccel,
		Own.Engine.MaxRPM * Rules.PowerbackRPMFraction))
	{
		return false;
	}

	// THE GOAL IS THE TAXI OUT'S, not the push's. The claim pass reserves the node an agent is
	// heading FOR, and a push that claimed its own end would have the aeroplane reserving a
	// patch of taxiway as though it were a stand. What it is going to is the runway.
	//
	// ReleaseGoal THEN TakeGoal (issue #295), not a hand-spelled copy of TakeGoal's three
	// calls: this used to re-type SetGoalFrom/ArmDepartureIfRunway/ClaimGoalNodeAtDispatch here
	// and never called ReleaseGoal at all, so a departing aeroplane kept its OWN stand's node
	// claimed against the re-offer pass for the rest of the session - the exact drift
	// RedirectAgent and ExtendRoute already avoid by sharing this same pair.
	ReleaseGoal(Agent, AgentId);
	TakeGoal(Agent, AgentId, &Network, TaxiRoute);

	// THE NEED IS NAMED even though nothing branches on it yet. Slice 1 pushes all three the
	// same way and nobody is doing the pushing, so this line is the only place the gap between
	// "needs a tug" and "has one" is visible at all.
	// MOVING FROM THIS CALL, not from the next tick's Track: an arrival dispatched between ticks must not revoke it.
	TaxiPlanning->MarkMoving(AgentId);
	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Agent %d pushing back %.0f uu, then %.0f uu to taxi out, %s"),
		AgentId, PushRoute.Length, TaxiRoute.Length,
		*UEnum::GetValueAsString(Own.PushbackNeed));

	Announce(TransitionOf(Agent, Before, EAgentEvent::DepartOrdered));
	// #169: the occupancy revision was bumped in TakeGoal, with the claim - unconditional,
	// unlike the broadcast above. See RedirectAgent's own copy of this comment.
	return true;
}

void UGroundTraffic::PlanTaxiOut(const FRoadAgent& Agent, const FAirframe& Aircraft, const URoadNetwork& Network,
	FDepartureAsk& Ask) const
{
	// PUSHBACK IS GATED ON A PLAN (spec 2026-10-02 §1): no taxi plan to the runway, no push - refused as the push ground
	// is, a refusal other traffic clears, and the push watch re-asks it when the taxi table moves.
	const FTaxiRequest Request = TaxiOutRequest(Agent, Aircraft, Ask.Plan, Ask.bStraightOut ? nullptr : &Ask.Push);
	Ask.Taxi = TaxiPlanning->Plan(Network, Aircraft, Rules, Request);
	if (Ask.Taxi.IsPlanned())
	{
		return;
	}

	// A DEPARTURE QUEUES (measured on M_ScaleGatwick, 2026-10-02): its entry is booked FOR EVER by the departure ahead
	// until that one lines up - every plan ends where its aircraft can stay (spec §2) - so a plan that must reach the entry
	// had each departure wait on its stand for the whole of the one before's taxi: 20 minutes' mean wait at 40 mov/h. So
	// it is planned AS FAR AS IT CAN STAY - the latest node on its way where it may hold, off the runway - and the rest is
	// booked when the entry frees (ExtendQueuedDepartures). Holding there for ever is itself a place it can stay, so the
	// rule holds; and since nothing is planned through a node held for ever, no arrival can come to wait on it.
	// ENFORCED BY: Airside.Model.TaxiPlan.RefusesWhenNothingFree (a node held for ever refuses a plan through it)
	const FRoutePlan& Way = Ask.bStraightOut ? Ask.Plan.Route : Ask.Push.TaxiOutRoute;
	int32 Tried = 0;
	for (int32 Index = Way.Steps.Num() - 2; Index >= 0 && Tried < Rules.TaxiPlanQueueCandidates; --Index)
	{
		const FRouteStep& Step = Way.Steps[Index];
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
		if (Edge == nullptr || Network.IsRunwaySegment(Edge->DerivedFrom) || !FTaxiPlanner::CanHoldAt(Network, Rules, Step.Edge, Step.To))
		{
			continue;
		}
		++Tried;
		FTaxiRequest Short = Request;
		Short.Goal = Step.To;
		FTaxiPlan Queued = TaxiPlanning->Plan(Network, Aircraft, Rules, Short);
		// AND IT MAY STAND THERE AS IT ARRIVES (review of #528, measured on M_ScaleGatwick at 40 mov/h, 2026-10-02): the
		// planner may reach the node along another edge than this route's, and a goal is admitted along any - so a departure
		// queued at the end of a lane too short for the node behind's reach still claimed that node, which its plan had
		// released, and the arrival booked through it next waited on it while it waited on that arrival (resolver, twice).
		const bool bStands = Queued.IsPlanned() && Queued.Route.Steps.Num() > 0
			&& FTaxiPlanner::CanHoldAt(Network, Rules, Queued.Route.Steps.Last().Edge, Step.To);
		if (bStands)
		{
			Ask.Taxi = MoveTemp(Queued);
			Ask.QueueFor = Request.Goal;
			Ask.QueueErrand = Request.Errand;
			return;
		}
	}
	Ask.Why = EDepartureRefusal::PushbackBlocked;
	Ask.bNoTaxiPlan = true;
}

EDepartureRefusal UGroundTraffic::ClearDeparture(int32 AgentId, const URoadNetwork& Network, const FDepartureAsk& Ask)
{
	// BOOKED, THEN STARTED - NOW, OR WHEN ITS PLAN SAYS. A plan that holds it on its stand first is a clearance to push
	// later: booked (an arrival may still take it - ruling 2), and refused for now as push ground, which is what the
	// caller waits on; StartDuePushes starts it, and the phase change it announces is the caller's news.
	//
	// AND A PUSH DUE NOW GOES ONLY IF IT IS ITS TURN (measured on M_ScaleGatwick at 80 mov/h, 2026-10-02): its plan was
	// timed round an arrival booked through the push's end; the arrival was late, the push went at its planned time, and
	// the aeroplane stood on the line the arrival was due down - each waiting on the other. So it is booked first, and
	// the push's whole ground asked of the order as StartDuePushes asks it; a straight-out departure is ordered as it
	// taxis (UTaxiPlanning::OrderHold), from its stand.
	const FRoutePlan PushRoute = Ask.bStraightOut ? FRoutePlan() : Ask.Push.PushRoute;
	if (!TaxiPlanning->Book(Network, AgentId, ETaxiClearanceKind::TaxiOut, Ask.Taxi, ETaxiClearanceStage::Booked, PushRoute,
		SimSeconds))
	{
		return WatchForTaxiPlan(AgentId, Network, PushRoute);
	}
	if (Ask.QueueFor.IsSet())
	{
		const FGuidelineNode* Entry = Network.GetGuidelineNode(Ask.QueueFor);
		TaxiPlanning->SetQueued(AgentId, Ask.QueueFor, Ask.QueueErrand, Entry != nullptr ? Entry->Position : FVector2D::ZeroVector);
	}
	// "NOW" WITHIN THE WINDOW'S OWN MARGIN, never more (review of #528 finding 7): the push ground is booked from a margin
	// before PushAt, so starting that much early is inside the booking; a fixed second was not, under a margin below it.
	const bool bNow = Ask.Taxi.PushAt <= SimSeconds + FMath::Min(1.0, FMath::Max(0.0, Rules.TaxiPlanMargin))
		&& (Ask.bStraightOut || TaxiPlanning->PushWaitingFor(AgentId) == 0);
	if (!bNow)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d cleared to %s in %.0f s, when its taxi plan's ground is due"),
			AgentId, Ask.bStraightOut ? TEXT("taxi out") : TEXT("push back"), Ask.Taxi.PushAt - SimSeconds);
		return EDepartureRefusal::PushbackBlocked;
	}
	if (!StartPlannedDeparture(AgentId, Network, PushRoute, Ask.Taxi.Route))
	{
		TaxiPlanning->Drop(AgentId, TEXT("its departure could not be started"));
		return EDepartureRefusal::NoRoute;
	}
	return EDepartureRefusal::None;
}

EDepartureRefusal UGroundTraffic::WatchForTaxiPlan(int32 AgentId, const URoadNetwork& Network, const FRoutePlan& PushRoute)
{
	// NO TAXI PLAN TO THE RUNWAY FITS round the plans booked: other traffic's to clear, as the push ground is - so on the
	// push watch, keyed on the taxi table too (FPushWatch::TaxiRevision). Said once per wake, as the ground's refusal is.
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d cannot leave its stand yet: no taxi plan to the runway fits round those booked."),
		AgentId);
	RefreshRunwaySeeds(Network, /*bForce*/ false);
	FPushWatch& Watch = PushWatch.Add(AgentId);
	Watch.PushRoute = PushRoute;
	Watch.Network = &Network;
	Watch.EditRevision = Network.GetEditRevision();
	Watch.GuidelineRevision = Network.GetGuidelineRevision();
	Watch.TaxiRevision = TaxiPlanRevision();
	Watch.AskedAt = SimSeconds;
	Watch.bTaxiBlocked = true;
	RunwaysHeldNow(Network, Watch.RunwaysHeld);
	return EDepartureRefusal::PushbackBlocked;
}

void UGroundTraffic::ExtendQueuedDepartures(const URoadNetwork& Network)
{
	if (TaxiPlanning == nullptr)
	{
		return;
	}
	for (const int32 Id : TaxiPlanning->QueuedDueAsk(SimSeconds))
	{
		const FRoadAgent* Agent = FindAgent(Id);
		const FTaxiClearance* Clearance = TaxiPlanning->Find(Id);
		const FAirframe* Aircraft = Agent != nullptr ? Agent->AsAircraft() : nullptr;
		if (Clearance == nullptr || Aircraft == nullptr || Agent->Phase != EAgentPhase::Taxiing || Clearance->Plan.Route.Steps.Num() == 0)
		{
			continue;
		}
		TaxiPlanning->NoteExtendAsked(Id, SimSeconds);
		// THE REST, FROM WHERE ITS PLAN ENDS - its holding node - no earlier than it is due there.
		FTaxiRequest Rest;
		Rest.Start = Clearance->Plan.Route.Steps.Last().To;
		Rest.Goal = Clearance->QueueFor;
		Rest.Errand = Clearance->QueueErrand;
		Rest.Holder = Id;
		Rest.DepartAt = FMath::Max(SimSeconds, Clearance->Plan.Arrival);
		const FTaxiPlan Ext = TaxiPlanning->Plan(Network, *Aircraft, Rules, Rest);
		if (!Ext.IsPlanned() || !TaxiPlanning->Extend(Network, Id, Ext, SimSeconds))
		{
			continue;
		}
		// AND DRIVEN: spliced onto the route it is on, in place - it never stops to be redirected (ExtendRoute), its goal
		// moving to the entry, its take-off armed there (TakeGoal's ArmDepartureIfRunway).
		if (!ExtendRoute(Id, &Network, Ext.Route))
		{
			TaxiPlanning->Drop(Id, TEXT("its way on to the runway could not be spliced onto its route"));
			continue;
		}
		if (const FRoadAgent* Extended = FindAgent(Id))
		{
			TaxiPlanning->AdoptRoute(Id, Extended->PlanInProgress());
		}
	}
}
