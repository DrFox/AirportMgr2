#pragma once

#include "CoreMinimal.h"
#include "Model/AgentMotion.h"
#include "Model/Airframe.h"
#include "Model/LandingRun.h"
#include "Model/PushbackRun.h"
#include "Model/ReverseRun.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Model/TakeoffRun.h"
#include "Model/TrafficOccupancy.h"
#include "Model/TrafficRules.h"
#include "Model/Vehicle.h"
#include "RoadAgent.generated.h"

// Forward declared only for the friends below - see FRoadAgent::CrossingRunway.
class UGroundTraffic;
struct FClaimPass;

/**
 * WHICH BUNDLE an agent was started with - and so which of FRoadAgent's two it may read.
 *
 * NOT ETraversalClass, deliberately. Class is a ROUTING fact (which guidelines admit it,
 * what priority it gets) and tests and dispatch write it after the start; this is a fact
 * about the DATA, set by the one Start* call that stored the bundle and by nothing else. A
 * van someone re-classed as Emergency is still a vehicle, and must still read its own chassis.
 */
UENUM()
enum class EAgentBody : uint8
{
	/** Started through StartTaxi / StartArrival / StartPushback, with an FAirframe. */
	Aircraft,

	/** Started through StartDrive, with an FVehicle. */
	Vehicle
};

/**
 * Where an agent has got to. Replaces five independent bools - bArriving, bDeparting,
 * bDepartOnArrival, bParked, plus the implicit "none of the above means taxiing" - that
 * could represent illegal combinations (bArriving && bDeparting) nothing ever checked for.
 *
 * See FRoadAgent for why the states stay separate structs (FLandingRun, FRouteFollower,
 * FTakeoffRun) rather than becoming subclasses of one.
 */
UENUM()
enum class EAgentPhase : uint8
{
	/** The landing drives it, before the taxi to the stand. */
	Arriving,

	/** The follower drives it. Also the phase between a plain dispatch and any handover. */
	Taxiing,

	/** The take-off run drives it, after a taxi that ended on a runway with one armed. */
	Departing,

	/** At the stand, taxi over, running down the post-arrival shutdown pause. */
	Parked,

	/**
	 * Coming off the stand: backed down the lead-in and swung onto the taxiway. The push
	 * drives it - see FPushbackRun.
	 *
	 * NOT CALLED Pushback, and that distinction is the point of the feature rather than
	 * pedantry: a Twin Otter reverses under its own power and is not being pushed by
	 * anything. The PHASE is the manoeuvre; the SERVICE that performs it for an aeroplane
	 * which cannot manage alone is pushback, and that is the job board's business.
	 */
	Manoeuvring,

	/**
	 * A GROUND VEHICLE backing into its service bay. FReverseRun drives it.
	 *
	 * SEPARATE FROM Manoeuvring, which is an aeroplane coming off a stand. The two look alike
	 * from outside - something reversing - and are different kinematics: a pushed aeroplane
	 * pivots about its NOSE GEAR because that is where the tug couples, so the steered axle
	 * still leads. A truck backing up pivots about its FIXED axle, and can hold a tighter arc
	 * for it. Sharing a phase would mean sharing a motion law they do not share.
	 */
	Reversing,

	/** The take-off has cleared. FRoadAgent::Advance returns false from here on. */
	Gone
};

/**
 * What FRoadAgent::Advance did THIS FRAME, for UGroundTraffic::AdvanceOnce to switch on
 * instead of diffing Phase before and after the call itself (issue #105 item 6).
 *
 * None is the common case - most frames move an agent without handing it over to anything -
 * and is deliberately first so it is the default a freshly zeroed FAgentEvent starts at.
 *
 * Airborne is NOT an EAgentPhase transition: the agent is EAgentPhase::Departing both before
 * and after it fires, only FTakeoffRun::Phase moves (Rotate -> Climb) - which used to be the
 * one condition UGroundTraffic::AdvanceOnce checked directly against Agent.Departure.Phase
 * from outside, rather than something FRoadAgent::Advance told it. Folding it in here means
 * every handover this agent can report, phase-boundary or not, comes through the one channel.
 *
 * Gone mirrors Advance's own `return false`, which already tells the caller "drop this
 * agent" unambiguously - AdvanceOnce still acts on that return value, not on this event, for
 * the removal itself. It is in the enum anyway so a test can assert the SAME thing the
 * return value says, and so nothing reads "the event list" and wonders why the most final
 * event of all is missing from it.
 */
UENUM()
enum class EAgentEvent : uint8
{
	/** Nothing this frame beyond ordinary motion. */
	None,
	/** Arriving -> Taxiing: the landing vacated the strip, taxiing in. */
	Vacated,
	/** Taxiing -> Departing: reached the threshold with a departure armed, rolling. */
	LinedUp,
	/** Taxiing -> Parked: the taxi is over, the turnaround starts. */
	Parked,
	/** Manoeuvring -> Taxiing: off the stand and aligned, the taxi out starts. */
	PushedBack,
	/** Still Departing, but FTakeoffRun::Phase just reached Climb: the runway is free. */
	Airborne,
	/** Departing -> Gone: the climb cleared. See this enum's own comment on why it is here
	 *  despite Advance's `return false` already saying so. */
	Gone
};

/**
 * How far through a runway crossing a taxiing agent's BODY is. Spec §3.1's fourth route.
 *
 * AN ENUM AND NOT A PAIR OF BOOLS - this codebase's "a phase is an enum, never a set of
 * bools". The two facts being tracked ("committed to the crossing" and "a wheel is actually
 * on the asphalt") can never both be the current state, and the states are visited in one
 * order, so the illegal combination stops being representable.
 *
 * AND NOT FRoadAgent::CrossingRunway EITHER, which was the first design: a set seed used to
 * mean "holding", so the seed had to be cleared to say "not holding" and there was nowhere
 * left to record that the body had reached the strip. The seed now says WHICH runway; this
 * says whether, and how far.
 */
UENUM()
enum class ECrossingPhase : uint8
{
	/** Not crossing. CrossingRunway is unset and nothing is held for a crossing. */
	None,

	/** Past a bar, onto a step that leads to the strip, body not yet on it. The chain is
	 *  held OCCUPIED from here: the aeroplane is going to be on the asphalt shortly and
	 *  nothing may be cleared onto it in between. */
	Committed,

	/** The agent's CENTRE is on the strip. Held until the TAIL leaves it. */
	OnStrip
};

/**
 * What to do once the current taxi ends: fly a departure, or do nothing.
 *
 * DATA ABOUT AN INTENTION, not a phase - an agent taxiing toward a runway with a departure
 * armed is still Taxiing in every way that matters until it actually arrives. Splitting
 * this out of FRoadAgent's own fields is what makes that distinction checkable: before this
 * struct existed, "departure armed" was bDepartOnArrival plus three fields that had to be
 * read together, and nothing tied the four to each other.
 */
USTRUCT()
struct AIRSIDE_API FDepartureOrder
{
	GENERATED_BODY()

	/** Where the roll starts, and the strip it starts on - see #88. */
	UPROPERTY() FRunwayEnd End;

	/**
	 * How far past the threshold the taxi joins the strip, uu. The roll starts THERE - an
	 * intersection departure - with RunwayLength minus this to reach Vr in. Zero is the
	 * backtrack case: taxied to the threshold, turned round, the whole runway ahead.
	 * See DeparturePlanner.
	 */
	UPROPERTY() double EntryOffset = 0.0;
};

/**
 * One thing driving one route: a taxi, an arrival, or a departure, whichever is current.
 *
 * STATE PATTERN OVER AN ENUM, delegating to whichever of FLandingRun / FRouteFollower /
 * FTakeoffRun is driving. The textbook version of this pattern makes each state a subclass
 * behind a common interface; that is not available here, because a USTRUCT cannot be
 * polymorphic through a UPROPERTY - there is no vtable pointer UHT will serialize - so the
 * three phase structs stay siblings and FRoadAgent::Advance itself owns the switch. Three
 * of them is also not a coincidence worth hiding behind an interface: each is a COMPLETE,
 * independently-tested world-free simulation (Airside.Model.LandingRun,
 * Airside.Model.TurnRate, Airside.Model.TakeoffRun), and a phase interface would only be
 * asking three unrelated shapes to pretend to share one.
 *
 * WORLD-FREE, like the phases it delegates to. The whole of "does this agent land, taxi,
 * park and depart correctly" is testable by calling Advance in a loop with no actor, no
 * world and no view - see Airside.Model.RoadAgent.
 *
 * Runtime only. See UGroundTraffic::Agents, and UAirsideTraffic::Views for why the view
 * that renders this is a separate, Present-layer field rather than living here.
 */
USTRUCT()
struct AIRSIDE_API FRoadAgent
{
	GENERATED_BODY()

	UPROPERTY() EAgentPhase Phase = EAgentPhase::Taxiing;

	/**
	 * How this agent rolls - the aircraft's chassis or the vehicle's, whichever it was started
	 * with. What every ground phase (follower, reverse, claims) reads.
	 *
	 * AN ACCESSOR OVER TWO PRIVATE BUNDLES rather than one field, since 2026-09-23 - see Body.
	 * A vehicle used to be an FAirframe with its climb zeroed; now it is an FVehicle, and the
	 * agent needs somewhere to hold either. UHT reflects neither TVariant nor TOptional, and
	 * every field here must be a UPROPERTY (see bDepartureArmed), so the two sit side by side
	 * and this picks one. The bundles are PRIVATE so the unused one cannot be read by mistake:
	 * a default FAirframe's climb answers IsSet() true, which is exactly the "a van reports
	 * itself landable" trap ResolveDefaultVehicle used to zero fields to avoid.
	 */
	const FChassis& Chassis() const { return Body == EAgentBody::Aircraft ? Airframe.Chassis : Vehicle.Chassis; }

	/** The aeroplane's full bundle, or null for a vehicle. The only way to read flight data. */
	const FAirframe* AsAircraft() const { return Body == EAgentBody::Aircraft ? &Airframe : nullptr; }

	/** The vehicle's bundle, or null for an aircraft. */
	const FVehicle* AsVehicle() const { return Body == EAgentBody::Vehicle ? &Vehicle : nullptr; }

	/**
	 * The aeroplane's bundle, WRITABLE, and the agent marked an aircraft - for tests that
	 * assemble an agent field by field or change a figure mid-flight to prove it is read live.
	 * Production code starts an agent through Start*; Check-Architecture rule 4 keeps this to
	 * the test modules.
	 */
	FAirframe& EditAirframeForTest() { Body = EAgentBody::Aircraft; return Airframe; }

	/** Which bundle this agent carries - see EAgentBody. */
	EAgentBody GetBody() const { return Body; }

	/** What the inspector calls it - FAirframe::TypeCode or FVehicle::TypeCode. */
	FName TypeCode() const { return Body == EAgentBody::Aircraft ? Airframe.TypeCode : Vehicle.TypeCode; }

	/**
	 * The span a route search must clear. ZERO for a vehicle, which FRouteQuery::Wingspan reads
	 * as unlimited - the answer ResolveDefaultVehicle used to reach by zeroing an airframe's.
	 */
	double Wingspan() const { return Body == EAgentBody::Aircraft ? Airframe.Wingspan : 0.0; }

	/**
	 * Drives Phase == Arriving.
	 *
	 * THE HANDOVER RUNS THE OPPOSITE WAY ROUND FROM A DEPARTURE, which is the whole reason
	 * both exist as separate structs rather than as modes: a departure is the follower then
	 * FTakeoffRun, an arrival is FLandingRun then the follower. FRoadAgent owns the switch
	 * and neither phase knows the other exists.
	 */
	UPROPERTY() FLandingRun Arrival;

	/** Drives Phase == Taxiing. */
	UPROPERTY() FRouteFollower Follower;

	/**
	 * Drives Phase == Departing.
	 *
	 * A SECOND MOTION PHASE rather than a mode inside the follower - see FTakeoffRun.
	 * FRoadAgent owns which of the two is driving it, so neither has to know the other
	 * exists.
	 */
	UPROPERTY() FTakeoffRun Departure;

	/**
	 * Drives Phase == Manoeuvring.
	 *
	 * A FOURTH MOTION PHASE rather than a mode inside the follower, for the reason the other
	 * three are separate: it is a COMPLETE, independently-tested simulation of one thing
	 * (Airside.Model.PushbackRun), and it walks its route with the body reversed, which no
	 * amount of signing FRouteFollower::Speed would express. FRoadAgent owns which of the
	 * four is driving, so none of them has to know the others exist.
	 */
	UPROPERTY() FPushbackRun Pushback;

	/**
	 * The back-out a ground vehicle is making, when Phase is Reversing.
	 *
	 * ARMED FROM INSIDE THE TAXI, unlike Pushback, which is armed from outside by whoever
	 * decides an aeroplane is leaving. A reverse leg is part of a route the vehicle is already
	 * driving - one span of a stand's four-leg cycle - so the agent finds it for itself, cuts it
	 * out of the plan with RouteSearch::Section, and picks the taxi up again afterwards.
	 */
	UPROPERTY() FReverseRun Reverse;

private:
	/**
	 * How fast this vehicle backs up, uu/s. Copied in at dispatch, like ShutdownPause, because
	 * FRoadAgent is world-free and cannot read the rules for itself.
	 *
	 * PRIVATE, WRITTEN ONLY THROUGH StampRules (issue #295) - AdmitDispatched used to set this
	 * by hand, and DispatchArrival never set it at all (see StampRules's own comment). A test
	 * that needs a specific figure calls StampRules with an FTrafficRules of its own, the same
	 * door production uses.
	 */
	UPROPERTY() double ReverseSpeed = 0.0;

public:
	/** Read-only outside StampRules - see ReverseSpeed's own comment. */
	double GetReverseSpeed() const { return ReverseSpeed; }

private:
	/**
	 * The step of Follower.Plan the taxi resumes at once the current reverse span ends, and
	 * INDEX_NONE when the reverse is the last thing the route does.
	 *
	 * A STEP, NOT A DISTANCE, and that correction cost a PIE session. FRouteFollower::Start's
	 * third parameter is InitialSpeed - it has no Travelled, and always begins a plan at its
	 * beginning. Handing it a resume distance there restarted the route at the SERVICE POINT
	 * with an absurd speed, so the truck backed out correctly, snapped back to the aeroplane
	 * facing away from it, and drove the reverse arm forwards. Reported as exactly that.
	 *
	 * So the remainder is CUT instead, with RouteSearch::Section, and the follower is given a
	 * plan whose beginning is where it should start.
	 *
	 * PRIVATE (issue #295): written and read only inside RoadAgent.cpp (TryArmReverseLeg arms
	 * it, Advance's reverse-handover branch consumes it), so no mutator is needed - the whole
	 * point of #174's pattern is a door for the OUTSIDE writers this field never had.
	 */
	UPROPERTY() int32 ResumeStep = INDEX_NONE;

public:
	/**
	 * RPM at or above which a powerback may begin. Copied from FTrafficRules at StartPushback
	 * because this struct is world-free and cannot read the rules for itself - the same
	 * reason ShutdownPause is a copy rather than a lookup. Zero for anything on a tug bar.
	 */
	UPROPERTY() double PushbackThrustRPM = 0.0;

	/** The route to fly once an arrival has vacated. Planned at dispatch, so a landing
	 *  cannot be armed for a stand it has no way of reaching. */
	UPROPERTY() FRoutePlan TaxiInPlan;

	/**
	 * The route to taxi once a PUSH has finished. The exact mirror of TaxiInPlan above, and
	 * planned at dispatch for the same reason: an aeroplane must never be pushed somewhere it
	 * cannot then taxi out of.
	 *
	 * A SECOND ROUTE IS UNAVOIDABLE HERE. A push reverses onto the arm of the junction the
	 * departure does NOT take, so when it ends the aeroplane is standing somewhere the
	 * departure route never visits - the route planned from the stand no longer begins where
	 * the aeroplane is. See PushbackPlanner::Plan.
	 */
	UPROPERTY() FRoutePlan TaxiOutPlan;

	/** What to fly once the current taxi ends, if anything. See FDepartureOrder. */
	UPROPERTY() FDepartureOrder DepartureOrder;

	/**
	 * Armed at dispatch when the route's goal was a runway threshold.
	 *
	 * A BOOL BESIDE THE STRUCT, not TOptional<FDepartureOrder>: TOptional is not
	 * UHT-reflectable, and every FRoadAgent field must be a UPROPERTY because FRoadAgent
	 * itself lives inside a UPROPERTY TArray (UGroundTraffic::Agents) that only
	 * serializes what UHT can see.
	 */
	UPROPERTY() bool bDepartureArmed = false;

	/**
	 * No stand could be found for this aircraft: it stops at the end of what remains of its
	 * route and is re-offered one whenever a stand may have freed. INTENT DATA, like
	 * FDepartureOrder, not a phase - a waiting aircraft is Taxiing to its prefix's end and
	 * then Parked there, and either is true while it waits. Set only by the rebuild path
	 * (UGroundTraffic::ReResolvePlan) in v1; cleared by the re-offer (ReofferStands).
	 */
	UPROPERTY() bool bAwaitingStand = false;

	/**
	 * Arms the wait: GOAL AND FLAG TOGETHER, so bAwaitingStand can never be true without the
	 * node the re-offer pass (ReofferStands) searches from - the invariant this codebase's
	 * review flagged as maintained only by convention (issue #174). Goal is usually the same
	 * node it already was (the rebuild path re-arms in place); taking it as a parameter
	 * rather than reading GoalNode inside the method means a caller cannot arm the wait
	 * without saying, at the call site, what it is waiting FROM.
	 */
	void SetAwaitingStand(FGuidelineNodeId Goal) { GoalNode = Goal; bAwaitingStand = true; }

	/** Ends the wait - a stand was found, or the agent moved on some other way. The new
	 *  goal, if there is one, is set separately (SetGoal/SetGoalFrom): a caller that just
	 *  found a stand already knows where it is sending the agent. */
	void ClearAwaitingStand() { bAwaitingStand = false; }

	/**
	 * The engine is turning. NOT the same question as whether the aircraft is moving -
	 * orthogonal to Phase, because an engine can run in ANY phase: idling while parked and
	 * taxiing, at full power while departing, even while arriving (an arrival appears on
	 * final with it already turning). See FEnginePerformance for the RPM this commands.
	 *
	 * THIS WAS ONCE INFERRED FROM MOVEMENT - the propeller stopped whenever the aircraft
	 * did - which was wrong at both ends: an aircraft holding short with its engine idling
	 * is the commonest thing on an airport, and one that had actually shut down could not
	 * be expressed at all. The answer here is STATE, not a guess made from the speed.
	 */
	UPROPERTY() bool bEngineRunning = false;

private:
	/** Where the propeller has actually got to, RPM. Trails bEngineRunning - see
	 *  FEnginePerformance. Advanced by AdvanceEngine every frame, whichever phase is
	 *  driving.
	 *
	 *  PRIVATE (issue #295): RedirectAgent used to read it into a local, restart the taxi (which
	 *  resets it to zero as part of a cold start) and write the local back when the engine had
	 *  not actually stopped - see RestoreEngineRPM's own comment for why that restore is now a
	 *  named call instead of a bare field write. */
	UPROPERTY() double EngineRPM = 0.0;

public:
	/** Read-only outside StartEngineAtSpeed/RestoreEngineRPM/AdvanceEngine - see EngineRPM's
	 *  own comment. */
	double GetEngineRPM() const { return EngineRPM; }

	/**
	 * Restores EngineRPM to a figure captured before a cold start overwrote it - RedirectAgent's
	 * one caller, for an agent whose engine had not actually stopped (bEngineRunning false does
	 * not mean the propeller has stopped turning; AdvanceEngine spools it down over
	 * SpoolDownSeconds). See RedirectAgent's own comment for the fuller story; this is the door
	 * that used to be a bare `Agent.EngineRPM = PriorRPM` (issue #295).
	 */
	void RestoreEngineRPM(double RPM) { EngineRPM = RPM; }

	/**
	 * Where the gear has got to. Advanced by AdvanceGear every frame, whichever phase is
	 * driving - the same arrangement as EngineRPM above and for the same reason.
	 */
	UPROPERTY() EGearPhase GearPhase = EGearPhase::Down;

	/**
	 * How far into a raise or a lower, seconds. Meaningless in Down and Up, and reset to
	 * zero on arrival at either so it cannot be read as a stale position.
	 */
	UPROPERTY() double GearCycleSeconds = 0.0;

	/**
	 * Seconds still to run on the post-arrival pause before the engine is shut down.
	 *
	 * Counted down only once Phase == Parked. Zero means nothing is pending - either it has
	 * not parked yet, or the shutdown has already happened.
	 */
	UPROPERTY() double ShutdownCountdown = 0.0;

private:
	/**
	 * How long the post-arrival pause runs, seconds. Copied from
	 * ARoadNetworkActor::ShutdownPauseSeconds at dispatch, because this struct is world-free
	 * and cannot read an actor's UPROPERTY for itself.
	 *
	 * PRIVATE, WRITTEN ONLY THROUGH StampRules (issue #295) - see ReverseSpeed's own comment;
	 * the two are stamped together. No getter: nothing outside RoadAgent.cpp reads it today.
	 */
	UPROPERTY() double ShutdownPause = 10.0;

public:
	/**
	 * What Advance last reported, kept so a frame where the driving phase DECLINES (no
	 * route, or a polyline too short to have a direction) can hand back the same motion
	 * rather than an unset FVector2D - which is how this project has twice put things at the
	 * world origin. See Advance.
	 *
	 * LEFT PUBLIC (issue #295's review): the one external WRITE (GroundTrafficRebuild.cpp's
	 * drive-side rejoin) goes through RebaseLastMotionPosition below, but a dozen-plus READ
	 * call sites across InspectFacts.cpp, AirsideTraffic.cpp, TrafficClaims.cpp and
	 * GroundTraffic(Rebuild).cpp would each need converting to a getter for no behaviour
	 * change - the "more than ~10 sites" exception the brief for this issue allows. See the
	 * PR body for the count.
	 */
	UPROPERTY() FAgentMotion LastMotion;

	/**
	 * The ROAD-PLANE position: for an airborne aircraft this is its ground track, and
	 * anything drawn under it (a selection ring, say) sits at the same point. Right for
	 * every aircraft on the ground, which is where anything selectable long enough to matter
	 * is. Exists so a caller outside Model/ (FSelectTool) names what it wants rather than
	 * reaching LastMotion.Position itself (#104).
	 */
	FVector2D GroundPosition() const { return LastMotion.Position; }

	/**
	 * Re-seats LastMotion.Position only, leaving Heading/Altitude/GroundSpeed/etc as they are -
	 * FPlanReResolver::ReResolvePlan's rejoin-after-a-drive-side-flip case, where RestartTaxi's
	 * own fallback pose (the new plan's first point) is wrong for a vehicle that is actually
	 * part-way along it. NAMED rather than a bare `Agent.LastMotion.Position = At` (issue #295)
	 * so the one external write of this field is a call, not a direct reach into it.
	 */
	void RebaseLastMotionPosition(const FVector2D& At) { LastMotion.Position = At; }

	/**
	 * Where each link of the vehicle's tow has its axle, road-plane XY - one per FTowLink, and
	 * EMPTY for anything rigid. Laid straight behind the cab at StartDrive and stepped with the
	 * cab in every tow sub-step after (see FollowAndTow); NEVER re-derived from the route, which
	 * is what lets a trailer lag a corner the way a real one does (spec §1).
	 *
	 * AXLES AND NOT HITCH ANGLES, although the spec first said "angle": the pursuit steps an
	 * axle position (VehicleSweep::StepChain, the same call Trace makes), and an angle would be
	 * converted to a point and back every sub-step - a rounding the router never makes.
	 */
	UPROPERTY() TArray<FVector2D> TowAxles;

	/** The link that jack-knifed, which stopped this agent, or INDEX_NONE. See JackknifedLink. */
	int32 GetJackknifedLink() const { return JackknifedLink; }

	/**
	 * The plan this agent is walking, how far along it, and how fast - from whichever struct
	 * is actually driving.
	 *
	 * THREE ACCESSORS AND NOT THREE DIRECT READS OF Follower, because since the push there are
	 * TWO structs that can be walking an agent along a route. Reading Agent.Follower.Travelled
	 * on a manoeuvring agent returns whatever the taxi IN left there - a stale distance on a
	 * live plan, which claims ground the aeroplane is nowhere near. Airside.Model.ClaimCentre
	 * measured that at 99 000 uu against a real 1 500.
	 *
	 * NOT A CASE FOR EVERY PHASE. An arrival and a departure are not on a route at all, and
	 * their callers branch away before they reach these (see FClaimPass::Run's first arm). The
	 * follower is the answer for everything else, which leaves every vehicle's path unchanged.
	 */
	const FRoutePlan& PlanInProgress() const
	{
		return Phase == EAgentPhase::Manoeuvring ? Pushback.Plan : Follower.Plan;
	}
	double DistanceAlongPlan() const
	{
		return Phase == EAgentPhase::Manoeuvring ? Pushback.Travelled : Follower.Travelled;
	}
	double SpeedAlongPlan() const
	{
		return Phase == EAgentPhase::Manoeuvring ? Pushback.Speed : Follower.Speed;
	}

	/**
	 * Rebases the follower's driven distance after a route's DRIVEN HISTORY is trimmed ahead of
	 * it - ExtendRoute's own KeepBehind, which drops whole steps behind the agent and must move
	 * Travelled back by exactly what they measured, or the follower's cursor and the trimmed
	 * plan's own step distances disagree from the very first tick after the trim.
	 *
	 * A NAMED CALL, NOT `Agent.Follower.Travelled -= Dropped` AT THE CALL SITE (issue #295):
	 * Follower is a public sub-phase struct FRoadAgent does not otherwise reach through by
	 * hand, and this was the one place outside RoadAgent.cpp that did.
	 */
	void RebaseTravelled(double Dropped) { Follower.Travelled -= Dropped; }

	/** True while a route-walking phase is driving: a taxi, or a push off a stand. The one
	 *  question FClaimPass::Run, the rebuild and the deadlock resolver all used to spell as
	 *  "Phase == Taxiing", which silently excluded the push. */
	bool IsOnRoute() const
	{
		return Phase == EAgentPhase::Taxiing || Phase == EAgentPhase::Manoeuvring;
	}

	/** Stable identity for the agent's lifetime, assigned by UGroundTraffic::Admit. 0 means
	 *  unassigned and is never handed out. Was FAgentSlot::Id before the Mediator moved to
	 *  Model/ and the slot struct went with the view pointer it existed to carry.
	 *
	 *  LEFT PUBLIC, WITH AssignId AS ADMIT'S OWN DOOR (issue #295): a great many test fixtures
	 *  build an FRoadAgent by hand and set this directly (a scripted scenario, not production
	 *  discipline on itself - the same exemption issue #174's own fields give test modules), so
	 *  privatising the field itself would touch far more than the one production writer this
	 *  is really about. Admit uses AssignId; everything else keeps constructing test agents the
	 *  way it always has. */
	UPROPERTY() int32 Id = 0;

	/** The one production door Id is assigned through - UGroundTraffic::Admit, and nowhere
	 *  else. See Id's own comment for why the field stays public rather than gaining a friend.
	 *  ENFORCED BY: Check-Architecture.ps1 rule 4 (allowed-callers, 'FRoadAgent::AssignId'
	 *  row) - a second production caller fails it. */
	void AssignId(int32 NewId) { Id = NewId; }

	/** How this agent moves. Vehicles were dispatched with no class at all before M2, which
	 *  is why priority could not be applied to them.
	 *
	 *  LEFT PUBLIC (issue #295's review): set once, at birth, by DispatchArrival/AdmitDispatched
	 *  - no partner field it must agree with, unlike the invariant pairs this issue's mutators
	 *  protect - and read at dozens of sites (TraversalPriority(Agent.Class) alone, across the
	 *  claim pass, the deadlock resolver and the rebuild) that a getter would not make any
	 *  safer. Check-Architecture's generalised write-ban (this issue, item 5) allow-lists the
	 *  two birth sites by name rather than the whole file, so a future direct write of any
	 *  OTHER field in GroundTraffic.cpp still fails. */
	UPROPERTY() ETraversalClass Class = ETraversalClass::Aircraft;

	/** Where the current route is going, so a replan can aim at the same place. See
	 *  SetGoalFrom, which is how this should be set from a fresh plan, and SetGoal below
	 *  for a caller that already has the node rather than a plan to take it from. */
	UPROPERTY() FGuidelineNodeId GoalNode;

	/**
	 * Repoints the goal directly - a rebuild re-pointing a dead handle to where the same
	 * position now resolves, a stand found for a waiter. NOT SetGoalFrom's replacement:
	 * that derives the goal from a plan's own last step; this is for a caller that already
	 * has the node itself. Issue #174 routed the rebuild's hand-written `Agent.GoalNode = X`
	 * through this so the field has one production writer's worth of call sites to check
	 * rather than five.
	 */
	void SetGoal(FGuidelineNodeId Goal) { GoalNode = Goal; }

	// --- Written by UGroundTraffic's arbitration each tick; read by Advance ------------
	//
	// Arbitration writes, motion reads: there is no second evaluator of where the agent
	// may go, only one input into the one follower.
	//
	// PRIVATE, WITH Refuse AND ClearArbitration AS THE ONLY WRITERS (issue #174). ApplyClaims
	// used to set StopWithin, WaitingOn, BlockedStep and BlockedResource one field at a time,
	// and GroundTrafficRebuild.cpp hand-typed the very three-field reset ClearArbitration
	// exists for, verbatim, rather than calling it - the bug this friend-and-mutator pair
	// exists to make impossible again. Getters below cover every outside reader (the deadlock
	// resolver, InspectFacts, the stall clock in AdvanceOnce) so nothing that used to read the
	// field directly loses the ability to; only the WRITE is now one call.
private:
	/** Distance beyond which the follower may not go this tick. See FRouteFollower::Advance. */
	UPROPERTY() double StopWithin = TNumericLimits<double>::Max();

	/** Id of the agent holding what this one was refused, or 0. The wait-for graph's edge. */
	UPROPERTY() int32 WaitingOn = 0;

	/** Index into Follower.Plan.Steps of the step whose resource refused this agent, or -1.
	 *  Names the node a deadlock replan starts from (the step's FROM node). */
	UPROPERTY() int32 BlockedStep = -1;

	/**
	 * WHAT refused this agent at BlockedStep - the node, edge or runway segment - so the
	 * deadlock resolver can ban the right thing. Banning the step's edge alone was the first
	 * attempt and was wrong for a node: the search walked round the block and re-entered the
	 * occupied node from its other arm, and the aircraft "turned around" at a bar to wait
	 * on the same holder from the other side (PIE, 2026-09-06). Meaningless when BlockedStep
	 * is -1.
	 */
	UPROPERTY() FTrafficResource BlockedResource;

public:
	/** Read-only outside Refuse/ClearArbitration - see StopWithin's own comment. */
	double GetStopWithin() const { return StopWithin; }

	/** Read-only outside Refuse/ClearArbitration - see WaitingOn's own comment. */
	int32 GetWaitingOn() const { return WaitingOn; }

	/** Read-only outside Refuse/ClearArbitration - see BlockedStep's own comment. */
	int32 GetBlockedStep() const { return BlockedStep; }

	/** Read-only outside Refuse/ClearArbitration - see BlockedResource's own comment. */
	const FTrafficResource& GetBlockedResource() const { return BlockedResource; }

	/**
	 * Refuses this agent at Step: what stopped it (Resource), how far it may still travel
	 * (NewStopWithin) and who holds the thing (BlockerId) - the quadruple FClaimPass::
	 * ApplyClaims used to write field by field (issue #174), which let a caller update three
	 * of the four and leave the last one answering last tick's question. One call, so the
	 * four either move together or not at all.
	 */
	void Refuse(int32 Step, const FTrafficResource& Resource, double NewStopWithin, int32 BlockerId);

	/**
	 * Resets StopWithin, WaitingOn and BlockedStep to "nothing is refusing this agent" - the
	 * three-line reset that was hand-written, verbatim, at HoldRunwayOnly, ReleaseForDeadPlan
	 * and ApplyClaims's not-held branch (issue #82). LastOverlaps is NOT included: it is either
	 * reset alongside this one by the caller (HoldRunwayOnly, ReleaseForDeadPlan) or has
	 * already been overwritten with this pass's freshly-computed overlaps before the caller
	 * gets here (ApplyClaims) - folding it in here would stomp that computed value.
	 */
	void ClearArbitration();

private:
	/**
	 * Everyone this agent was reported as OVERLAPPING on the last claim pass - two bodies
	 * standing on one node or one runway. Throttles that Warning to the transition.
	 *
	 * A FIELD OF ITS OWN rather than reusing WaitingOn, which was the first attempt and was
	 * wrong: WaitingOn names the FIRST refusal in route order, so an overlap that is not the
	 * first refusal never matched it and the Warning fired on every single tick.
	 *
	 * A LIST rather than the single last id, which was the second attempt: an agent can
	 * overlap two things in one pass (its own node and the runway under it), and keeping
	 * only the last let the other one re-log every tick. Never more than a few entries.
	 *
	 * PRIVATE, WRITTEN ONLY THROUGH SetLastOverlaps (issue #295) - FClaimPass::ApplyClaims
	 * used to reach in and assign it directly.
	 */
	UPROPERTY() TArray<int32> LastOverlaps;

public:
	/** Read-only outside SetLastOverlaps - see LastOverlaps' own comment. */
	const TArray<int32>& GetLastOverlaps() const { return LastOverlaps; }

	/** The one door LastOverlaps is written through - an empty array is the reset FClaimPass::
	 *  HoldRunwayOnly/ReleaseForDeadPlan make; ApplyClaims's own end-of-pass call hands it this
	 *  pass's real overlaps. ENFORCED BY: Check-Architecture.ps1 rule 6 (agent field writes) -
	 *  FClaimPass is a friend and could otherwise reach in and assign the field directly. */
	void SetLastOverlaps(TArray<int32> Overlaps) { LastOverlaps = MoveTemp(Overlaps); }

private:
	/** Seconds stopped with WaitingOn set. Deadlock detection looks once this passes the rule.
	 *  PRIVATE, WITH AccrueStall/ResetStall AS THE ONLY WRITERS (issue #295 - the field was
	 *  still public despite both mutators existing since issue #174). */
	UPROPERTY() double StalledSeconds = 0.0;

public:
	/** Adds to the stall clock. AdvanceOnce's own ternary used to write this field by hand
	 *  each tick (issue #174) - one arm of it is this call, the other is ResetStall. */
	void AccrueStall(double DeltaSeconds) { StalledSeconds += DeltaSeconds; }

	/** Zeroes the stall clock: the wait stopped, whether it resolved or the agent left it. */
	void ResetStall() { StalledSeconds = 0.0; }

	/** Read-only outside AccrueStall/ResetStall - see StalledSeconds' own comment. */
	double GetStalledSeconds() const { return StalledSeconds; }

private:
	/** SimSeconds of the last replan attempt by the deadlock resolver; -1e9 = never.
	 *  PRIVATE, WRITTEN ONLY THROUGH StampResolveAttempt (issue #295) - FDeadlockResolver::
	 *  StampCycle and OnGraphRebuilt's rebuild-time reset used to assign it directly. */
	UPROPERTY() double LastResolveAttempt = -1.0e9;

public:
	/** Read-only outside StampResolveAttempt - see LastResolveAttempt's own comment. */
	double GetLastResolveAttempt() const { return LastResolveAttempt; }

	/** The one door LastResolveAttempt is written through - a deadlock retry's own stamp
	 *  (FDeadlockResolver::StampCycle) or a rebuild's reset to "never" (OnGraphRebuilt, so an
	 *  unresolvable jam does not wait out the rest of its retry window after the player has
	 *  just built the fix for it). ENFORCED BY: Check-Architecture.ps1 rule 6 (agent field
	 *  writes) and, unlike LastOverlaps, plain private access too - FDeadlockResolver is not
	 *  a friend of FRoadAgent. */
	void StampResolveAttempt(double SimSeconds) { LastResolveAttempt = SimSeconds; }

	/** Runway segments this agent occupies in a phase that is not a taxi: an arrival from
	 *  StartArrival until Vacated, a departure from the handover until Gone. */
	UPROPERTY() TArray<FRoadSegmentId> RunwayHeld;

	/** Holds this chain from now - an arrival's touchdown (DispatchArrival) or a departure's
	 *  line-up (the LinedUp handover). See RunwayHeld. */
	void HoldRunway(const TArray<FRoadSegmentId>& Chain) { RunwayHeld = Chain; }

	/** Releases whatever runway this agent held - the vacate (to the crossing rule) or the
	 *  Airborne handover (to nobody; the strip is simply free). */
	void ReleaseRunway() { RunwayHeld.Reset(); }

private:
	/** The chain a taxi ending on a runway will hold once it becomes a departure.
	 *  PRIVATE, WRITTEN ONLY THROUGH ArmDepartureRunway/DisarmDeparture (issue #295) -
	 *  ArmDepartureIfRunway used to assign and .Reset() it directly. */
	UPROPERTY() TArray<FRoadSegmentId> DepartureRunway;

public:
	/** Read-only outside ArmDepartureRunway/DisarmDeparture - see DepartureRunway's own
	 *  comment. */
	const TArray<FRoadSegmentId>& GetDepartureRunway() const { return DepartureRunway; }

	/** Arms the chain a departure will hold once the taxi that is heading for a runway reaches
	 *  it - ArmDepartureIfRunway's one caller, alongside ArmDeparture itself. */
	void ArmDepartureRunway(TArray<FRoadSegmentId> Chain) { DepartureRunway = MoveTemp(Chain); }

	/**
	 * Whether a runway crossing is current. Public read of the private CrossingPhase/
	 * CrossingRunway pair below, for callers outside UGroundTraffic (InspectFacts, tests).
	 */
	bool IsCrossing() const { return CrossingPhase != ECrossingPhase::None; }

	/** How far through a crossing this agent's BODY is. See ECrossingPhase. Read-only outside
	 *  UGroundTraffic - see BeginCrossing/EndCrossing. */
	ECrossingPhase GetCrossingPhase() const { return CrossingPhase; }

	/** Which runway a current crossing is over. See CrossingRunway. Read-only outside
	 *  UGroundTraffic - see BeginCrossing/EndCrossing. */
	FRoadSegmentId GetCrossingRunway() const { return CrossingRunway; }

	/**
	 * Arms or advances a runway crossing: CrossingRunway becomes Seed and CrossingPhase becomes
	 * InPhase, together - see CrossingRunway for why they must agree. Also how an already-
	 * Committed crossing advances to OnStrip: call it again with the same seed and the new
	 * phase, rather than writing CrossingPhase alone and leaving CrossingRunway to be read as
	 * "still valid" by assumption.
	 */
	void BeginCrossing(FRoadSegmentId Seed, ECrossingPhase InPhase);

	/** Ends a runway crossing: CrossingRunway and CrossingPhase both go back to unset/None
	 *  together, for the same reason BeginCrossing sets them together. */
	void EndCrossing();

private:
	/**
	 * Seed of a runway chain this agent is physically ON while taxiing, after passing a
	 * holding-position bar or vacating a landing. Unset when none. Spec §3.1's fourth route.
	 *
	 * SEPARATE FROM RunwayHeld, which is the chain a NON-taxiing agent owns: this one is
	 * held by an agent that is crossing, and it is released by geometry (the tail clearing
	 * the strip) rather than by a phase change. A seed rather than the expanded chain
	 * because the chain is re-expanded per tick anyway, and a rebuild may have changed it.
	 *
	 * WHICH CHAIN, NOT WHETHER. CrossingPhase says whether the hold applies; this says which
	 * runway it is over. Reading IsSet() as "holding" is the bug the phase exists to end.
	 *
	 * PRIVATE, WITH UGroundTraffic AND FClaimPass AS FRIENDS (issue #82; FClaimPass added in
	 * #84 when the claim pass moved off UGroundTraffic). The friendship is for the many
	 * existing READS in the claim pass (Agent.CrossingPhase == ...), which stay direct field
	 * access rather than a getter call at every one. WRITES go through BeginCrossing/
	 * EndCrossing everywhere, including inside these friends - the friendship makes that a
	 * convention this type documents, not a rule the compiler can enforce on its own friend.
	 */
	UPROPERTY() FRoadSegmentId CrossingRunway;

	/** How far through a crossing this agent's BODY is. See ECrossingPhase. Private for the
	 *  same reason as CrossingRunway, and by the same friends. */
	UPROPERTY() ECrossingPhase CrossingPhase = ECrossingPhase::None;

	friend class UGroundTraffic;
	friend struct FClaimPass;

public:
	/**
	 * Spools the propeller one frame toward whatever the engine has been commanded to do.
	 *
	 * Separate from Advance because it happens in ALL phases - taxiing, rolling, climbing,
	 * parked - and an engine that only spooled while one of them was driving would stop dead
	 * the moment an aircraft changed phase.
	 */
	void AdvanceEngine(double DeltaSeconds);

	/**
	 * Moves the gear one frame, and starts or finishes a cycle when the aircraft passes a
	 * cue height.
	 *
	 * Beside AdvanceEngine and called next to it for the same reason: it happens in ALL
	 * phases. A cycle that only advanced inside the Departing branch would freeze the doors
	 * half open the moment a departure handed over.
	 *
	 * READS LastMotion.Altitude, which is LAST frame's height. A frame of lag on a cue that
	 * is crossed once per flight is not worth restructuring Advance for - the alternative is
	 * moving this call below a switch that returns early in four of its branches.
	 */
	void AdvanceGear(double DeltaSeconds);

	/** The whole undercarriage pose - see FGearPerformance::FractionsAt, which produces it. */
	FGearPose GearPose() const;

	/**
	 * The engine is running and already at speed, as it is for an aeroplane that has spent a
	 * turnaround on a stand before taxiing out.
	 *
	 * StartTaxi deliberately starts a PLAIN DISPATCH from cold, so the propeller winds up as
	 * the aircraft first moves. That is wrong for a departure: the engines were started
	 * during the turnaround, minutes before the aeroplane rolled, and starting from zero
	 * there meant the propeller was still winding up while the aircraft was already taxiing
	 * at full speed - which is what was reported. Called by the redirect that sends a parked
	 * aircraft to the runway.
	 */
	void StartEngineAtSpeed();

	/**
	 * What to show for this agent right now: where it is, and what it is doing.
	 *
	 * A pure function of the agent's own state, so Airside.Present.AgentMotion can ask it
	 * directly with no world involved.
	 */
	FAgentMotion DescribeMotion(const FVector2D& At, double Heading,
		double Altitude = 0.0, double PitchDegrees = 0.0) const;

	/**
	 * Arms an arrival: Phase becomes Arriving. False, and leaves the agent untouched, when
	 * this runway cannot take this aircraft - see FLandingRun::Start.
	 */
	bool StartArrival(const FRunwayEnd& End, const FAirframe& InAirframe, double VacateAt,
		const FRoutePlan& InTaxiInPlan);

	/** Starts a plain taxi with no prior landing: Phase becomes Taxiing. An aircraft. */
	void StartTaxi(const FRoutePlan& Plan, const FAirframe& InAirframe);

	/**
	 * StartTaxi for a service vehicle: Phase becomes Taxiing, Body becomes Vehicle.
	 *
	 * A SEPARATE NAME AND NOT AN OVERLOAD, so a call site says which kind of thing it is
	 * sending - and a braced or default-constructed argument cannot silently pick one.
	 */
	void StartDrive(const FRoutePlan& Plan, const FVehicle& InVehicle);

	/**
	 * Restarts a taxi along a new plan with the bundle this agent already carries, whichever
	 * kind it is - UGroundTraffic::RedirectAgent's "a redirect changes where it goes, not what
	 * it is". Replaces that function copying Agent.Airframe out and handing it back to
	 * StartTaxi, which cannot be written for an agent that may hold either bundle.
	 *
	 * InitialHeading, when set, is the body heading to start from instead of the line's own at
	 * the start: UGroundTraffic::RedirectAgent passes a TOW's current heading, see there.
	 */
	void RestartTaxi(const FRoutePlan& Plan, double InitialTravelled = 0.0,
		TOptional<double> InitialHeading = TOptional<double>());

	/**
	 * Sends a parked aeroplane off its stand: Phase becomes Manoeuvring. False, and leaves
	 * the agent untouched, when the plan cannot be pushed along - see FPushbackRun::Start.
	 *
	 * THE ENGINE IS STARTED HERE AND NOT IN StartTaxi. Real practice is "push and start": the
	 * crew spools up WHILE the tug pushes, and the spool routinely outlasts the manoeuvre.
	 * AdvanceEngine already runs first and unconditionally every frame whatever phase is
	 * driving, so moving the cold start to here is the whole of it - and the handover into
	 * the follower must then NOT go through StartTaxi, which writes EngineRPM back to zero.
	 *
	 * ThrustRPM is the RPM at or above which a POWERBACK may begin; it is ignored for
	 * anything on a tug bar, which the tug moves whatever the propeller is doing.
	 */
	bool StartPushback(const FRoutePlan& PushPlan, const FRoutePlan& InTaxiOutPlan,
		const FAirframe& InAirframe, double PushSpeed, double PushAccel, double ThrustRPM);

	/** Arms a departure for the taxi currently under way. See FDepartureOrder. */
	void ArmDeparture(const FRunwayEnd& End, double EntryOffset = 0.0);

	/** Disarms it: the route no longer ends on the runway it was armed for (ArmDepartureIfRunway).
	 *  CLEARS DepartureRunway TOO (issue #295): an armed departure with no chain to hold, or a
	 *  chain nobody is armed to take, is the same half-set state issue #174 already closed for
	 *  the arbitration fields. ArmDepartureIfRunway used to reset both by hand - a mutator call
	 *  plus a hand-written `.Reset()` beside it - which is exactly the shape that drifts; one
	 *  call keeps the pair from coming apart again. */
	void DisarmDeparture()
	{
		bDepartureArmed = false;
		DepartureOrder = FDepartureOrder();
		DepartureRunway.Reset();
	}

	/**
	 * Copies the rule figures every admit path must stamp before the agent's first tick:
	 * ShutdownPause and ReverseSpeed. FRoadAgent is world-free and cannot read FTrafficRules
	 * or ARoadNetworkActor's own UPROPERTY for itself, so whoever admits it copies both in -
	 * see ReverseSpeed and ShutdownPause's own comments for why each is a copy rather than a
	 * lookup.
	 *
	 * ONE CALL FOR BOTH FIGURES (issue #295), not one hand-written assignment per admit site:
	 * DispatchArrival used to set ShutdownPause alone and AdmitDispatched set both, so an
	 * arrival never received ReverseSpeed at all - harmless while nothing an arrival does
	 * reads it, and exactly the "the copy that nobody set" shape CLAUDE.md warns about the
	 * day something does read it. Every admit path calls this now, whether or not the figure
	 * applies to what is being admitted - see Airside.Model.Traffic.ArrivalReceivesReverseSpeed.
	 */
	void StampRules(const FTrafficRules& Rules, double ShutdownPauseSeconds)
	{
		ShutdownPause = ShutdownPauseSeconds;
		ReverseSpeed = Rules.ServiceReverseSpeed;
	}

	/**
	 * Sets GoalNode from a plan's own last step, or clears it when the plan has none.
	 *
	 * DispatchArrival, DispatchAgent and RedirectAgent each wrote this same ternary by hand
	 * (issue #82) - a caller handing this agent a fresh plan calls this instead of repeating
	 * "Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : FGuidelineNodeId()" a fourth time.
	 */
	void SetGoalFrom(const FRoutePlan& Plan);

	/**
	 * Advances whichever phase is current by one frame, and reports what to show.
	 *
	 * OWNS EVERY HANDOVER: Arriving -> Taxiing on vacate, Taxiing -> Departing when the taxi
	 * has arrived with a departure armed, Taxiing -> Parked otherwise on arrival, Parked
	 * counts down and clears bEngineRunning once, Departing -> Gone when the take-off has
	 * cleared, and (since issue #105 item 6) FTakeoffRun::Phase reaching Climb while still
	 * Departing. OutEvent names whichever of these happened THIS FRAME - EAgentEvent::None on
	 * every other frame, which is most of them - so UGroundTraffic::AdvanceOnce can switch on
	 * what happened instead of diffing Phase (or, for the climb, a takeoff sub-phase nothing
	 * outside this function used to have a name for) before and after the call itself.
	 *
	 * Returns true with a motion to show; false only once Phase == Gone, which is also the
	 * caller's signal to destroy the view and drop the agent - see ARoadNetworkActor::Tick.
	 * OutEvent is EAgentEvent::Gone on that same call, redundantly with the return value - see
	 * EAgentEvent::Gone's own comment for why it is declared anyway.
	 */
	bool Advance(double DeltaSeconds, FAgentMotion& OutMotion, EAgentEvent& OutEvent);

private:
	/** Which of the two bundles below is live - see EAgentBody. Written only by the Start* calls. */
	UPROPERTY() EAgentBody Body = EAgentBody::Aircraft;

	/**
	 * Every fact about this aeroplane, in one place, when Body is Aircraft. See FAirframe for
	 * why this replaced four separate parameters (Ground, Climb, Approach, Engine) plus a bare
	 * Wingspan. Read through Chassis() / AsAircraft(), never directly - see Chassis().
	 */
	UPROPERTY() FAirframe Airframe;

	/** The vehicle's bundle, when Body is Vehicle. Read through Chassis() / AsVehicle(). */
	UPROPERTY() FVehicle Vehicle;

	/**
	 * Which link of the tow folded past VehicleSweep::MaxHitchRadians, or INDEX_NONE. Set by
	 * FollowAndTow, cleared by StartDrive; while set the agent holds where it folded.
	 *
	 * AN INDEX, NOT A bool: "jack-knifed" and "which link" are one fact, and the log and the
	 * test both want the second. NOT AN EAgentPhase either: a folded rig is still Taxiing as far
	 * as dispatch, claims and the rebuild are concerned - it holds its ground and the route it
	 * was on - and a new phase would be a case in every switch on Phase for a bug detector
	 * forward driving never trips (spec §1).
	 */
	UPROPERTY() int32 JackknifedLink = INDEX_NONE;

	/**
	 * Follower.Advance, plus the tow. RIGID: exactly the one call it always was, so nothing
	 * without a trailer moves differently. TOWING: the frame is cut into sub-steps no longer
	 * than VehicleSweep::TraceStep at the vehicle's speed cap, and each sub-step moves the cab
	 * and then steps the chain behind it - the router's step length, so the trailer drives the
	 * path Trace gated it on, and a long frame is many short ones rather than one long pull.
	 *
	 * The stop point is honoured ACROSS the sub-steps: each is handed what is left of
	 * StopWithin, not all of it again, or N sub-steps could each creep up to the full distance.
	 */
	bool FollowAndTow(double DeltaSeconds, FVector2D& OutAt, double& OutHeading);

	/**
	 * The ONE closing act of a route: Phase becomes Parked, the shutdown pause starts, and
	 * the motion handed back is re-described from (At, Heading) so it agrees with the phase
	 * just entered.
	 *
	 * WRITTEN ONCE HERE AFTER ISSUE #289. Both closing sites - a taxi arriving with no
	 * departure armed, and a reverse leg that turns out to be the last thing the route has
	 * left to drive - used to write Phase/ShutdownCountdown/the re-describe out by hand, and
	 * one of the two (the reverse leg) skipped the re-describe: it handed back the stale
	 * LastMotion from the driving phase's last tick instead, which still carried THAT
	 * phase's own GroundSpeed. A panel reading "Parked, 0.4 m/s" for one frame is exactly the
	 * bug the taxi-arrival copy of this code was written to stop (Airside.Model.InspectFacts
	 * caught it originally); it came back on the OTHER path because there were two copies of
	 * the ritual to keep in step instead of one. Airside.Model.RoadAgent.
	 * ReverseLastLegParksAtRest pins the reverse-leg case.
	 *
	 * ZEROES Follower.Speed ONCE, HERE, rather than at each closing site as before: DescribeMotion's
	 * own switch also answers zero for Parked/Gone directly (see there), but that only fixes what
	 * the PANEL reads. SpeedAlongPlan() and the tow seed copies (GroundTraffic.cpp,
	 * GroundTrafficRebuild.cpp) read Follower.Speed directly, with no phase guard, so a stale taxi
	 * speed left in it is a fact a parked agent can still be caught telling other code - the shape
	 * this project has shipped as a regression before.
	 */
	void Park(const FVector2D& At, double Heading, FAgentMotion& OutMotion);

	/**
	 * Checks whether the taxi has reached a reverse leg and, if so, drives the whole
	 * handover: arms FReverseRun, or stops the agent and says why it refused. Split out of
	 * Advance (issue #174 - Advance was 462 lines, and this block alone was over a hundred
	 * of them) with NO change to what it does: same scan, same conditions, same log lines.
	 *
	 * RETURNS WHETHER IT HANDLED THE FRAME, not whether a reverse armed - true covers both
	 * the armed case and the refused-and-stopped one, because either way Advance's caller
	 * has nothing left to do this tick; false means the step under the agent is not a
	 * reverse leg (or is not yet reached) and Advance should fall through to the follower.
	 *
	 * STILL SCANS Follower.Plan.Steps FROM THE TOP EVERY CALL, exactly as the inline block
	 * did - the scan itself is NOT cached here. Issue #190's medium item is caching this as
	 * a NextReverseLegStep computed once in Start/on a plan change; folding that in here
	 * too would make one issue's diff answer for another's measurement. This is the
	 * extraction only - see the PR for #174.
	 */
	bool TryArmReverseLeg(const FVector2D& At, double Heading, FAgentMotion& OutMotion);
};
