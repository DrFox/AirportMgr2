#pragma once

#include "CoreMinimal.h"
#include "Model/LandingRun.h"
#include "Model/RoadEntity.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Model/TakeoffRun.h"
#include "RoadAgent.generated.h"

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

	/** The take-off has cleared. FRoadAgent::Advance returns false from here on. */
	Gone
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

	/** Where the roll starts, road-plane XY. */
	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;

	/** Unit vector from the threshold toward the far end. */
	UPROPERTY() FVector2D Direction = FVector2D::ZeroVector;

	/** Runway available beyond the threshold, uu. */
	UPROPERTY() double RunwayLength = 0.0;
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
 * Runtime only. See UAirsideTraffic::Agents and FAgentSlot for why the view that renders
 * this is a separate, Present-layer field rather than living here.
 */
USTRUCT()
struct AIRSIDE_API FRoadAgent
{
	GENERATED_BODY()

	UPROPERTY() EAgentPhase Phase = EAgentPhase::Taxiing;

	/**
	 * Every fact about this aeroplane, in one place. See FAirframe for why this replaced
	 * four separate parameters (Ground, Climb, Approach, Engine) plus a bare Wingspan.
	 */
	UPROPERTY() FAirframe Airframe;

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

	/** The route to fly once an arrival has vacated. Planned at dispatch, so a landing
	 *  cannot be armed for a stand it has no way of reaching. */
	UPROPERTY() FRoutePlan TaxiInPlan;

	/** What to fly once the current taxi ends, if anything. See FDepartureOrder. */
	UPROPERTY() FDepartureOrder DepartureOrder;

	/**
	 * Armed at dispatch when the route's goal was a runway threshold.
	 *
	 * A BOOL BESIDE THE STRUCT, not TOptional<FDepartureOrder>: TOptional is not
	 * UHT-reflectable, and every FRoadAgent field must be a UPROPERTY because FRoadAgent
	 * itself lives inside a UPROPERTY TArray (UAirsideTraffic::Agents) that only
	 * serializes what UHT can see.
	 */
	UPROPERTY() bool bDepartureArmed = false;

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

	/** Where the propeller has actually got to, RPM. Trails bEngineRunning - see
	 *  FEnginePerformance. Advanced by AdvanceEngine every frame, whichever phase is
	 *  driving. */
	UPROPERTY() double EngineRPM = 0.0;

	/**
	 * Seconds still to run on the post-arrival pause before the engine is shut down.
	 *
	 * Counted down only once Phase == Parked. Zero means nothing is pending - either it has
	 * not parked yet, or the shutdown has already happened.
	 */
	UPROPERTY() double ShutdownCountdown = 0.0;

	/**
	 * How long the post-arrival pause runs, seconds. Copied from
	 * ARoadNetworkActor::ShutdownPauseSeconds at dispatch, because this struct is world-free
	 * and cannot read an actor's UPROPERTY for itself.
	 */
	UPROPERTY() double ShutdownPause = 10.0;

	/**
	 * What Advance last reported, kept so a frame where the driving phase DECLINES (no
	 * route, or a polyline too short to have a direction) can hand back the same motion
	 * rather than an unset FVector2D - which is how this project has twice put things at the
	 * world origin. See Advance.
	 */
	UPROPERTY() FAgentMotion LastMotion;

	/**
	 * Spools the propeller one frame toward whatever the engine has been commanded to do.
	 *
	 * Separate from Advance because it happens in ALL phases - taxiing, rolling, climbing,
	 * parked - and an engine that only spooled while one of them was driving would stop dead
	 * the moment an aircraft changed phase.
	 */
	void AdvanceEngine(double DeltaSeconds);

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
	bool StartArrival(const FVector2D& Threshold, const FVector2D& Direction, double RunwayLength,
		const FAirframe& InAirframe, double VacateAt, const FRoutePlan& InTaxiInPlan);

	/** Starts a plain taxi with no prior landing: Phase becomes Taxiing. */
	void StartTaxi(const FRoutePlan& Plan, const FAirframe& InAirframe);

	/** Arms a departure for the taxi currently under way. See FDepartureOrder. */
	void ArmDeparture(const FVector2D& Threshold, const FVector2D& Direction, double RunwayLength);

	/**
	 * Advances whichever phase is current by one frame, and reports what to show.
	 *
	 * OWNS EVERY HANDOVER: Arriving -> Taxiing on vacate, Taxiing -> Departing when the taxi
	 * has arrived with a departure armed, Taxiing -> Parked otherwise on arrival, Parked
	 * counts down and clears bEngineRunning once, Departing -> Gone when the take-off has
	 * cleared.
	 *
	 * Returns true with a motion to show; false only once Phase == Gone, which is also the
	 * caller's signal to destroy the view and drop the agent - see ARoadNetworkActor::Tick.
	 */
	bool Advance(double DeltaSeconds, FAgentMotion& OutMotion);
};
