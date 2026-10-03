// UGroundTraffic's half of space-time taxi planning (spec 2026-10-02): the requests it makes of UTaxiPlanning for an
// arrival and a departure, the per-tick tracking of every cleared aircraft, and the pushes a booked departure is due.
// The planning owner itself - table, clearances, order - is Model/TaxiPlanning.h. WHEN an aircraft is cleared stays
// this class's (DispatchArrival, DepartAgent); WHAT it is cleared on is the planner's.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DepartureAsk.h"
#include "Model/DeparturePlanner.h"
#include "Model/LandingRun.h"
#include "Model/PushbackPlanner.h"
#include "Model/PushbackRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiPlanning.h"

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
	// TO THE ENTRY ITSELF: held a while there, not for ever (TaxiPlanEntryHold). A queue's end stays for ever - it waits there.
	FTaxiPlan Taxi = Ask.Taxi;
	if (!Ask.QueueFor.IsSet())
	{
		UTaxiPlanning::CapEntryHold(Taxi, Rules.TaxiPlanEntryHold);
	}
	if (!TaxiPlanning->Book(Network, AgentId, ETaxiClearanceKind::TaxiOut, Taxi, ETaxiClearanceStage::Booked, PushRoute,
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
		FTaxiPlan Ext = TaxiPlanning->Plan(Network, *Aircraft, Rules, Rest);
		UTaxiPlanning::CapEntryHold(Ext, Rules.TaxiPlanEntryHold);
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
