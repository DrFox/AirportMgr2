#pragma once

#include "CoreMinimal.h"
#include "TrafficRules.generated.h"

// FORWARD DECLARED, NOT INCLUDED (issue #175): FootprintFor/GapFor/PushSpeedFor only need
// these two enums as PARAMETER TYPES here, and an enum class with an explicit underlying
// type needs no more than that to be forward declared - the values themselves are read only
// in TrafficRules.cpp, which includes RoadTraffic.h and RoadEntity.h for them. Pulling in
// RoadEntity.h from this header would have re-created the very fan-out issue #175 exists to
// cut: RoadNetworkActor.h's by-value FTrafficRules member does not need to know what an
// EPushbackNeed is, only that FTrafficRules has a function that takes one.
enum class ETraversalClass : uint8;
enum class EPushbackNeed : uint8;

/**
 * The numbers the arbiter works with. Spec 2026-09-06 §2.3.
 *
 * Footprint and gap live HERE, per class, and not on FAirframe: the airframe has no
 * length figure today, and a second copy of a performance number is the drift this
 * codebase's "one struct per thing" rule exists to prevent. A per-type length is a later
 * refinement with one owner.
 *
 * MOVED OFF Model/GroundTraffic.h (issue #175): TrafficClaims.h included that whole header -
 * UGroundTraffic, FDeadlockResolver, FPlanReResolver and all - solely to name this one
 * struct in two member references, and RoadNetworkActor.h did the same for one by-value
 * UPROPERTY. GroundTraffic.h still includes this header, so every one of the 47 existing
 * includers of it keeps compiling unchanged; TrafficClaims.h and RoadNetworkActor.h are the
 * two switched to this header alone.
 */
USTRUCT()
struct AIRSIDE_API FTrafficRules
{
	GENERATED_BODY()

	/** How much of the line an agent's body covers, uu. Half ahead of Travelled, half behind. */
	UPROPERTY(EditAnywhere) double AircraftFootprint = 1000.0;

	/**
	 * 620, which is fueltruck1's own length: 6.200 m, the one dimension that model's README
	 * fixes exactly. It was 500, chosen before there was a truck to measure.
	 *
	 * IT HAS TO MATCH THE MESH, because UAirsideContent::VehicleMesh says the placeholder box
	 * is sized from this figure so that "what is on screen is the length the arbiter actually
	 * keeps clear". A 6.2 m truck reserving 5 m is that promise broken in the direction that
	 * hurts: the arbiter would let a second agent into road this one is occupying.
	 */
	UPROPERTY(EditAnywhere) double VehicleFootprint = 850.0;

	/** Clear line kept ahead of the nose, beyond the braking distance, uu. */
	UPROPERTY(EditAnywhere) double AircraftGap = 1500.0;
	UPROPERTY(EditAnywhere) double VehicleGap = 300.0;

	/**
	 * How fast a push off a stand runs, uu/s. 1 uu is 1 cm - see UAircraftType::MainWheelRadius.
	 *
	 * ON THE RULES AND NOT THE AIRFRAME, unlike the braking figure the claim window reads:
	 * push speed is a property of what is doing the PUSHING, so a hand tug is slower than a
	 * tug vehicle whatever it has on the bar. That is also why they are named for the tug
	 * rather than for the aeroplane. Slice 2 moves the two tug figures onto the depot's own
	 * vehicle types and leaves SelfManoeuvre here, where it belongs.
	 *
	 * FIGURES, NOT MEASUREMENTS. Nothing about a real tug is modelled yet; these exist so the
	 * manoeuvre reads at the right pace on screen, and they are EditAnywhere so it can be
	 * tuned against what the player actually sees rather than against a specification.
	 */
	UPROPERTY(EditAnywhere) double SelfManoeuvrePushSpeed = 200.0;  // 2.0 m/s, on the engine
	UPROPERTY(EditAnywhere) double HandTugPushSpeed       = 80.0;   // 0.8 m/s, walking pace
	UPROPERTY(EditAnywhere) double VehicleTugPushSpeed    = 150.0;  // 1.5 m/s

	/**
	 * How fast a ground vehicle backs out of a service point, uu/s.
	 *
	 * HERE RATHER THAN ON THE VEHICLE, with the push speeds, because it is the same kind of
	 * figure and answers the same question: how fast a manoeuvre reads on screen. A crawl by
	 * nature - nobody reverses beside an aeroplane at taxi speed - and EditAnywhere so it can be
	 * tuned against what the player watches.
	 */
	UPROPERTY(EditAnywhere) double ServiceReverseSpeed    = 100.0;  // 1.0 m/s

	/** Into and out of a push, uu/s^2. Gentle: a towbar does not snatch. */
	UPROPERTY(EditAnywhere) double PushAccel              = 30.0;   // 0.3 m/s^2

	/**
	 * Within this of the parked heading, the way out is forward and no push is needed, degrees.
	 *
	 * A MEASUREMENT OF THE GROUND AHEAD, not a property of the stand: it answers a
	 * taxi-through stand, a taxiway a player happened to draw past a stand, and a graph
	 * rebuilt since the aeroplane parked, all with one question. Nothing in Model/ reads
	 * UEntityDefinition::bTaxiThrough for this, and that is deliberate.
	 */
	UPROPERTY(EditAnywhere) double StraightOutDegrees     = 45.0;

	/**
	 * Fraction of MaxRPM a SelfManoeuvre airframe needs before it will move, 0..1.
	 *
	 * A POWERBACK IS THE ENGINE DOING THE WORK, so it cannot begin until there is thrust. An
	 * aeroplane on a tug bar moves from the first frame whatever its propeller is doing,
	 * because the tug supplies the force - which is the ONE place in this slice where the
	 * pushback need changes what happens, and it is justified because it is a fact about the
	 * aeroplane rather than about a tug that does not exist yet.
	 *
	 * HERE AND NOT ON FEnginePerformance, which is per-type authored content: this is a rule
	 * about when a manoeuvre may begin, not a fact about any engine, and putting it there
	 * would mean re-authoring every aircraft asset to carry a number none of them vary.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double PowerbackRPMFraction = 0.6;

	/** Weight on held length in the routing cost. See FRouteQuery::CongestionWeight. */
	UPROPERTY(EditAnywhere) double CongestionWeight = 2.0;

	/**
	 * What a runway edge costs, as a multiple of its length, on an errand whose policy allows
	 * one at all (FRoutePolicy::bPenaliseRunways).
	 *
	 * TEN IS AN ARGUMENT, NOT A MEASUREMENT: a taxiway detour is rarely ten times the strip
	 * it parallels, so ten sends a route round whenever round exists, and still lets the
	 * strip win when it is the only way through. It has NOT been judged against a real
	 * airport - see the spec's section 10.
	 *
	 * ClampMin 1.0: below one, a runway edge would cost less than its own chord and the
	 * search's straight-line heuristic would stop being admissible, silently.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0")) double RunwayPenalty = 10.0;

	/** Stopped-and-waiting this long before deadlock detection looks. A normal junction
	 *  wait must never trip it. */
	UPROPERTY(EditAnywhere) double StallSeconds = 3.0;

	/** An unresolvable waiter re-tries its replan this often, sim seconds. */
	UPROPERTY(EditAnywhere) double RetrySeconds = 5.0;

	/** After a graph rebuild, how near a live node must be to a step's end to be it. */
	UPROPERTY(EditAnywhere) double ResolveRadius = 25.0;

	/**
	 * The longest step the model will take in one go, in sim seconds.
	 *
	 * THE FRAME IS NOT THE STEP. UAirsideTraffic hands this the frame time multiplied by the
	 * player's speed, so at x8 a 16 ms frame arrives as 133 ms of simulation - and an
	 * aeroplane crossing 133 ms of ground in one jump can pass the waypoint it was turning
	 * onto and be pulled back onto the line next frame. That is the rubber-banding reported
	 * from play, and it appeared at x2 and got worse from there while x1 looked perfect,
	 * which is exactly the signature of a step that scales with the multiplier.
	 *
	 * Substepping costs arbitration and motion passes in proportion to the speed multiplier,
	 * which is the right place to spend: the player asked for more simulation per second.
	 *
	 * MOVED HERE FROM UGroundTraffic (#107 item 6): a UPROPERTY(EditAnywhere) on that
	 * Transient, non-instanced UObject never reached the Details panel - the exact trap
	 * RoadNetworkActor.h documents for Presenter/Facade/Traffic, one layer further down
	 * (UGroundTraffic itself is a subobject of a subobject, neither exposed EditAnywhere).
	 * FTrafficRules already IS the level-authored knob (ARoadNetworkActor::TrafficRules,
	 * copied into the model every tick by UAirsideTraffic::Advance - see its header), so
	 * living here instead makes both fields reachable for free.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.001")) double MaxSubstepSeconds = 1.0 / 30.0;

	/**
	 * The most substeps one call will take, whatever the delta.
	 *
	 * A CEILING RATHER THAN A PROMISE. A frame that hitches badly - a level loading, a
	 * breakpoint - would otherwise ask for hundreds of steps and hitch the next frame too,
	 * which is the spiral that turns one stutter into a freeze. Past this the delta is
	 * divided evenly (see Advance), so every step is longer than MaxSubstepSeconds: slightly
	 * wrong every step beats compounding.
	 *
	 * SIZED FROM THE SPEED LADDER (#107 item 4), not merely for a hitch: USimClock's ladder
	 * (AirportOps/SimClock.h) reaches X32, and UAirsideTraffic::Advance's caller hands it the
	 * real frame time TIMES that multiplier every frame, hitch or not. X32 at 30 fps - the
	 * slowest rate this project treats as ordinary play - is 1.067 s of sim time needing 32
	 * steps to hold MaxSubstepSeconds; the OLD default of 8 clamped that to 8 steps of 133 ms
	 * each, four times the documented target, on EVERY frame at that speed - the same
	 * rubber-banding this substep split exists to remove, just moved to a higher speed
	 * setting instead of fixed. 32 keeps a genuine hitch exactly as bounded as before; it
	 * only stops ordinary top-speed play from being treated as one.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1")) int32 MaxSubsteps = 32;

	double FootprintFor(ETraversalClass Class) const;
	double GapFor(ETraversalClass Class) const;

	/** Which of the three push speeds above applies. The ONE consumer that has to agree with
	 *  EPushbackNeed - see its body for why that matters. */
	double PushSpeedFor(EPushbackNeed Need) const;
};
