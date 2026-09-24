#pragma once

// WHAT ROLLS ON THE GROUND, split out of Model/Airframe.h on 2026-09-23. Ground performance,
// the two axles, the gear track, the body centre and the steer law - everything FRouteFollower,
// FReverseRun, FSpeedProfile and the road builders need to move or make room for ONE wheeled
// thing, and nothing about flying it.
//
// WHY A SEPARATE STRUCT AND NOT MORE FIELDS ON FAirframe: service vehicles were carried as an
// FAirframe (UAirsideSettings::ResolveDefaultVehicle called that scaffolding), so a fuel truck
// held a zeroed climb, a zeroed approach, a gear-retraction cycle and a wingspan, and every
// ground consumer took the whole aeroplane to read five fields of it. An articulated rig was
// about to add a fifth wheel and a trailer to the same bundle, which would have handed every
// A380 a kingpin. FAirframe now HAS a chassis (composition, not inheritance: an aeroplane is
// not a kind of undercarriage, and a slicing copy into a base would compile silently where a
// named member does not), and code that only rolls takes the chassis alone.
//
// ENFORCED BY: Check-Architecture rule 15 - the ground-only consumers may not name FAirframe.
//
// AirsideLog.h IS DELIBERATELY NOT INCLUDED HERE, since #176. FChassis::EffectiveSteerLaw (then FAirframe's)
// used to UE_LOG an Error inline and sat on FRouteFollower::Advance's hot path - twice per
// agent per SUBSTEP, about 3800 lines/s at 60 fps for one mis-authored type across a busy
// apron. It is a pure query now; the log moved to WarnIfSteerLawUnsupported below, called
// once per dispatch from wherever an airframe is first bound to a phase - see that
// function's own comment for exactly where, and why not FRoadAgent's own Start* methods.

#include "CoreMinimal.h"
#include "Chassis.generated.h"

/**
 * Which law moves an airframe on the ground.
 *
 * AN ENUM AND NOT A DERIVED BOOL, since 2026-09-15. It used to be inferred from
 * FChassis::HasAxles() - "has anyone measured the wheelbase?" - which meant forgetting to
 * measure a vehicle silently changed its physics. That is not hypothetical: it is the
 * 2026-09-14 report of a fuel truck driving up to a stand, stopping, swinging ninety degrees
 * on the spot and driving off. CLAUDE.md's rule is that a phase is an enum and never a set
 * of bools, for exactly this reason - an illegal state that cannot be represented cannot be
 * shipped.
 */
UENUM()
enum class ESteerLaw : uint8
{
	/**
	 * A flat yaw rate - FGroundPerformance::MaxTurnRateDegPerSec - independent of speed.
	 *
	 * What a tug, a belt loader or a pushback tractor actually does: it turns about itself,
	 * and it can do so from a standstill. THE DEFAULT, because it is the law that needs no
	 * measurements, and an airframe nobody has measured is precisely the one that must not
	 * claim to steer geometrically.
	 */
	Pivot,

	/**
	 * The kinematic bicycle model: yaw is v*sin(lock)/L about the steered axle.
	 *
	 * Two consequences the pivot law does not have, and both are load-bearing. Yaw VANISHES
	 * at a standstill, so such a vehicle cannot snap its heading while stopped - which is why
	 * a ground vehicle's MinSteeringSpeed can be zero. And a corner tighter than L/sin(lock)
	 * cannot be followed at ANY speed, because speed cancels out of the requirement - see
	 * FChassis::TightestFollowableRadius.
	 */
	RollingSteer
};

/**
 * One POWER SETTING: what the airframe does when it is being flown a particular way.
 *
 * Taxi, take-off and landing are three different machines as far as motion is concerned -
 * an aircraft that accelerates onto a runway at its taxi rate never gets airborne - so the
 * numbers are grouped by the regime they belong to rather than flattened onto the airframe
 * where a caller would have to remember which one it was holding.
 *
 * ONLY TAXI EXISTS TODAY, and deliberately. A Takeoff or Landing field here would be
 * authored numbers that nothing reads, which is the failure this codebase has shipped three
 * times over - see CLAUDE.md. They become fields the day something flies, and the shape is
 * already right for that.
 *
 * Braking is here alongside thrust because both are "how fast can this change speed", and
 * the two are not equal: wheel brakes beat a propeller, which is why Decel is the larger.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FGroundRegime
{
	GENERATED_BODY()

	/** How hard it can speed up, uu per second squared. 100 is 1 m/s2. */
	UPROPERTY(EditAnywhere) double Accel = 100.0;

	/** How hard it can slow down. Larger than Accel: brakes beat thrust. */
	UPROPERTY(EditAnywhere) double Decel = 200.0;

	/** The speed this regime works up to, uu per second. 1000 is 10 m/s. */
	UPROPERTY(EditAnywhere) double SpeedCap = 1000.0;

	bool IsSet() const { return Accel > 0.0 && Decel > 0.0 && SpeedCap > 0.0; }
};


/**
 * How an airframe MOVES on the ground. A property of the aircraft, never of the pavement.
 *
 * The distinction matters and this project has already had to make it once, the other way
 * round: painted geometry - a stand's lead-in sweep, a taxiway fillet - is sized for the
 * LARGEST type admitted and is deliberately not per-aircraft, because concrete cannot be
 * repoured per movement. A turn RATE is the opposite. Nothing about the taxiway decides
 * how fast a nosewheel can be slewed; the airframe does, and an A320 and a Piper differ by
 * a factor of two and a half.
 *
 * Here in Model/ rather than on UAircraftType so FRouteFollower can take one. The Entities
 * layer depends on Model/ and never the reverse - the same reason FEntityFootprint lives here.
 *
 * Distances are uu, and a uu is a centimetre.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FGroundPerformance
{
	GENERATED_BODY()

	/** Moving about the airport. */
	UPROPERTY(EditAnywhere) FGroundRegime Taxi;

	/**
	 * Full power down a runway. SpeedCap here is ROTATION SPEED - the speed at which the
	 * nose comes up - which is exactly what "the speed this regime works up to" means for a
	 * take-off roll.
	 *
	 * Decel is the ABORT: a rejected take-off is braking from near Vr, which is harder than
	 * anything a taxi asks for.
	 */
	UPROPERTY(EditAnywhere) FGroundRegime Takeoff;

	/**
	 * Wheels down after touchdown. SpeedCap here is Vref - the speed the approach is flown
	 * at - which is what "the speed this regime works up to" means read backwards: it is the
	 * speed the regime STARTS from and brakes away.
	 *
	 * Decel is wheel braking with reverse or beta, which is harder than a rejected take-off
	 * because there is no question of stopping and going again. Accel is small and real: an
	 * aircraft that has slowed too far still has to keep rolling to steer, and the same
	 * MinSteeringSpeed rule applies on a runway as anywhere else.
	 */
	UPROPERTY(EditAnywhere) FGroundRegime Landing;

	/**
	 * The slowest this type can be kept rolling WHILE STEERING - non-zero for an aircraft,
	 * and ZERO for anything that can stop mid-turn.
	 *
	 * A wheeled aircraft cannot yaw without rolling: a prop or a fan produces thrust along
	 * the airframe, and a nosewheel steers the direction that thrust is taken in. It has no
	 * way to pivot on the spot. So a turn that cannot be made at speed is made at a crawl,
	 * which is what a pilot riding the brakes against idle thrust actually does.
	 *
	 * A GROUND VEHICLE SETS THIS TO ZERO and means it. A truck's wheels are driven and
	 * steered independently of any thrust line; it can stop with the wheel turned and pull
	 * away again. The van was authored at 50 uu/s until 2026-09-15 for a reason that had
	 * stopped being true - "a follower allowed to stop dead mid-turn would snap its heading
	 * round" - which is a PIVOT-law artefact. Under the rolling-steer law MaxStep is
	 * proportional to speed (FRouteFollower), so at zero the heading cannot move at all, let
	 * alone snap.
	 *
	 * WAS CALLED MinSteeringSpeed, and was FOUR THINGS AT ONCE - FSpeedProfile's own comment said
	 * as much: "THREE RULES ALL REPORT MinSteeringSpeed and the inspector panel cannot tell them
	 * apart". This is the only one of the four that is a fact about an airframe. The others
	 * now live where they belong: FRouteFollower::ProgressEpsilon guards the two loops that
	 * divide by their own progress, and a corner too tight for the steering lock logs a
	 * warning instead of quietly reporting this number.
	 *
	 * Not a floor on speed in general: an aircraft parked at its destination is stopped.
	 * This bounds only what a TURN may slow it to.
	 */
	UPROPERTY(EditAnywhere) double MinSteeringSpeed = 50.0;

	/**
	 * How fast the nose can be swung, in DEGREES per second.
	 *
	 * Outside the regime because it is a fact about the STEERING rather than about a power
	 * setting - the same nosewheel, at the same speed, slews at the same rate whatever the
	 * throttle is doing.
	 *
	 * Degrees because this is authored and read by hand, matching the convention the
	 * service-point builders use; FRouteFollower converts once, on Start.
	 *
	 * It is v/R at the tightest turn the steering allows, taken at the speed a pilot would
	 * take it - so it is derived from two published figures rather than dialled in until it
	 * looked right. See UAircraftType::PiperMeridianGround for that arithmetic.
	 */
	UPROPERTY(EditAnywhere) double MaxTurnRateDegPerSec = 10.0;

	/**
	 * The steering lock, degrees either side of straight ahead.
	 *
	 * READ ONLY BY THE ROLLING-STEER LAW - see FChassis::HasAxles. Where there is a
	 * wheelbase it REPLACES MaxTurnRateDegPerSec rather than capping it, and the difference
	 * matters: the airliner's hand-tuned 8 deg/s binds at every ordinary bend (a 30 m turn
	 * at 5 m/s needs 9.5), so keeping the old figure as a ceiling would defeat the geometry
	 * it was standing in for. MaxTurnRateDegPerSec above says as much itself - it is "v/R at
	 * the tightest turn the steering allows", which is this, once the wheelbase is known.
	 *
	 * 60 degrees is tiller range. Rudder-pedal steering is nearer 10 and is a different
	 * manoeuvre, not what a taxi turn uses.
	 */
	UPROPERTY(EditAnywhere) double MaxSteerDegrees = 60.0;

	/**
	 * What a turn may pull sideways, uu/s^2. 147 is 0.15 g.
	 *
	 * THIS IS WHAT LIMITS CORNER SPEED once steering is geometric, and the reason is worth
	 * stating because it replaces a yaw-rate cap that looked like physics and was not.
	 * Required yaw is v/R, available yaw is v*sin(lock)/L - speed cancels, so whether a
	 * corner can be FOLLOWED is purely geometric (R >= L/sin(lock)) and says nothing about
	 * how fast it may be taken. What actually stops an aircraft rounding a 15 m bend at taxi
	 * speed is tyre side load and the cabin, which is this.
	 *
	 * Authored per type rather than left to this default, for the reason UAirsideSettings
	 * gives about the van's figures: this is where a type's performance is DECIDED, and a
	 * reader must be able to see it without opening another header.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0")) double MaxLateralAccelUu = 147.0;

	/** False when nothing was authored, so a caller can fall back rather than freeze an agent. */
	bool IsSet() const
	{
		// Landing is NOT required here. This answers "can this thing move about an airport",
		// which every agent needs; an arrival additionally checks Landing.IsSet() for itself,
		// so an airframe with no landing figures declines to land rather than failing to taxi.
		//
		// MinSteeringSpeed IS NOT CHECKED, since 2026-09-15. Zero is a legitimate authored
		// value - it is what every ground vehicle says, because a truck can stop with the
		// wheel turned - and requiring it non-zero would have frozen the van outright, since
		// ArrivalPlanner and FRoadAgent both branch on exactly this call. A van that never
		// moves reads as a routing bug and would have been hunted as one. See
		// Airside.Model.SteeringFloorZeroStillTaxis.
		return Taxi.IsSet() && MaxTurnRateDegPerSec > 0.0;
	}
};

/**
 * One wheeled thing's ground geometry and performance - see the top of this file.
 *
 * Held by FAirframe::Chassis for an aircraft. Taken ALONE by everything that only rolls, so a
 * follower cannot read a wingspan it has no business with, and a vehicle need not pretend to
 * be an aeroplane to be followed.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FChassis
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FGroundPerformance Ground;

	/**
	 * The STEERED axle, uu along local +X. Nose gear on an aircraft, front axle on a vehicle.
	 *
	 * ZERO MEANS THE ORIGIN IS THAT AXLE, which is UAircraftType's documented local space:
	 * "origin at the NOSE GEAR, +X forward, +Y starboard". Non-zero is a DECLARED deviation.
	 *
	 * NO AIRCRAFT TYPE DECLARES ONE ANY MORE. BuildPiperMeridian did - it asked for this field
	 * by name, "that offset belongs here as a field and not as a constant at the call site" -
	 * until plane7 replaced the placeholder mesh it was measured off and brought it onto the
	 * nose-gear origin with the rest of the fleet. The live declarer is now
	 * UAirsideSettings::ResolveDefaultVehicle's fuel truck, which carries SteerAxleX 360.7
	 * against FixedAxleX 0: fueltruck1 is exported about its rear axle because that is what a
	 * front-steered truck pivots about, so on a VEHICLE the deviation is the model's own
	 * choice rather than an importer's accident.
	 *
	 * Named for the axle rather than the nose gear because vehicles carry this struct too,
	 * and a truck has no nose gear.
	 */
	UPROPERTY(EditAnywhere) double SteerAxleX = 0.0;

	/**
	 * The FIXED axle, uu along local +X. Main gear, or a vehicle's rear axle. Negative on a
	 * conforming airframe, since the mains sit aft of the nose gear.
	 */
	UPROPERTY(EditAnywhere) double FixedAxleX = 0.0;

	/**
	 * Distance BETWEEN the two main wheels, uu - the gear track, measured across.
	 *
	 * Zero means UNMEASURED, exactly as Wheelbase() being zero does, and HasMainGearTrack()
	 * below is the same question HasAxles() asks. Nothing is derived from a guess: an
	 * unmeasured airframe falls back to one contact point on the centreline rather than to
	 * a fabricated track, because a made-up track puts wheels where the aeroplane has none
	 * and everything downstream inherits the fiction.
	 *
	 * Across, not half-across. A datasheet quotes the track as the full figure between the
	 * wheels and this is the number that gets typed in from one, so halving it here would
	 * be an invitation to type the half.
	 */
	UPROPERTY(EditAnywhere) double MainGearTrack = 0.0;

	/** "Has anyone measured the gear track?" - the sibling of HasAxles. */
	bool HasMainGearTrack() const { return MainGearTrack > KINDA_SMALL_NUMBER; }

	/**
	 * The body's plan centre, uu along local +X. Derived from the footprint in UAircraftType::Airframe().
	 *
	 * NOT THE ORIGIN, and that is why it exists. The traffic model's claim windows are
	 * measured from the agent's centre - see FClaimPass::FClaimWindow, "the agent's CENTRE,
	 * never its nose" - and used Follower.Travelled for it. That was roughly true while mesh
	 * origins sat mid-fuselage, and became wrong by half a length the moment plane2 was
	 * re-exported about its nose gear.
	 */
	UPROPERTY(EditAnywhere) double BodyCentreX = 0.0;

	/**
	 * Nose gear to main gear, always positive, zero when unmeasured.
	 *
	 * THE LAW SELECTOR, through HasAxles below. An airframe with a wheelbase steers
	 * geometrically; one without keeps the flat MaxTurnRateDegPerSec. That is not a
	 * migration crutch - a van is authored at 90 deg/s and would need 83 degrees of lock at
	 * its creep speed, so it is pivoting rather than steering, and a bicycle model would
	 * cripple every service vehicle on the airport.
	 */
	double Wheelbase() const { return FMath::Abs(SteerAxleX - FixedAxleX); }

	/**
	 * Which law moves this airframe - DECLARED, not inferred from whether the axles happen
	 * to have been filled in. See ESteerLaw for the bug that inference caused.
	 */
	UPROPERTY(EditAnywhere) ESteerLaw SteerLaw = ESteerLaw::Pivot;

	/**
	 * True when the axle figures can actually support the rolling-steer law.
	 *
	 * NO LONGER THE LAW SELECTOR, since 2026-09-15 - it is now the DATA CHECK that
	 * EffectiveSteerLaw runs against the declared law. Kept rather than inlined because
	 * "are these axles measured" and "how does this thing steer" are two questions, and
	 * collapsing them into one predicate is what caused the pivoting truck.
	 */
	bool HasAxles() const { return Wheelbase() > KINDA_SMALL_NUMBER; }

	/**
	 * The law this airframe will actually be moved by: the declared one, UNLESS the data
	 * cannot support it.
	 *
	 * TWO STATEMENTS THAT MUST AGREE, and UE gives no way to make them one - the law is a
	 * UPROPERTY and the wheelbase is two more. So the consumer checks identity rather than
	 * trusting either, which is CLAUDE.md's rule for the case where a second list is forced
	 * on us. A RollingSteer airframe with no wheelbase would divide by zero in
	 * TightestFollowableRadius and yaw without bound in FRouteFollower; falling back to Pivot
	 * is wrong but survivable.
	 *
	 * PURE, SINCE #176. This used to UE_LOG an Error right here and got called twice per
	 * agent per SUBSTEP from FRouteFollower::Advance - about 3800 lines/s at 60 fps for one
	 * mis-authored type, the "log stops being read" failure the traffic code elsewhere
	 * throttles carefully. See NeedsSteerLawWarning and WarnIfSteerLawUnsupported below for
	 * where the log went instead: once per dispatch, not once per frame.
	 *
	 * THE ONLY LEGAL WAY TO ASK. Read SteerLaw directly and the check is bypassed.
	 */
	ESteerLaw EffectiveSteerLaw() const
	{
		if (SteerLaw == ESteerLaw::RollingSteer && !HasAxles())
		{
			return ESteerLaw::Pivot;
		}
		return SteerLaw;
	}

	/**
	 * The same question EffectiveSteerLaw answers by falling back, asked directly and with
	 * no side effect, so a caller can decide WHEN to say so rather than EffectiveSteerLaw
	 * saying it on every one of the many calls a single dispatch makes.
	 *
	 * See WarnIfSteerLawUnsupported, the one production caller.
	 */
	bool NeedsSteerLawWarning() const
	{
		return SteerLaw == ESteerLaw::RollingSteer && !HasAxles();
	}

	/**
	 * The tightest arc this airframe can follow AT ANY SPEED, uu. Zero when it pivots.
	 *
	 * Required yaw is v/R and available yaw is v*sin(lock)/L, so SPEED CANCELS: a corner is
	 * followable at every speed or at none, and the threshold is L/sin(lock). FSpeedProfile
	 * drops an agent to MinSteeringSpeed below it, and FStandLayoutBuild rounds a stand lane's corners
	 * to it so it does not have to.
	 *
	 * ZERO for an airframe with no measured axles. That one steers on the flat
	 * MaxTurnRateDegPerSec instead and has no such threshold - see HasAxles - so zero means
	 * "nothing to clear", not "clears nothing".
	 *
	 * THE TESTS DELIBERATELY DO NOT CALL THIS. They restate the arithmetic, for the reason
	 * Airside.Model.ServiceRoadFilletClearsTheTruckLock gives at its own copy: a helper that
	 * both the production code and its test called could be wrong in one place and agree with
	 * itself.
	 */
	double TightestFollowableRadius() const
	{
		const double Lock = FMath::Sin(FMath::DegreesToRadians(
			FMath::Clamp(Ground.MaxSteerDegrees, 0.0, 90.0)));
		return (EffectiveSteerLaw() == ESteerLaw::RollingSteer && Lock > KINDA_SMALL_NUMBER)
			? Wheelbase() / Lock : 0.0;
	}

	/**
	 * The tightest arc this airframe can hold GOING BACKWARDS, uu. Zero when it does not steer
	 * geometrically, exactly as TightestFollowableRadius reports zero.
	 *
	 * L/tan(lock), NOT L/sin(lock), and the difference is the whole reason a service bay is
	 * affordable. Forwards, the body pivots about the STEERED axle and the arc the steered
	 * wheels describe has radius L/sin(lock). Backwards it pivots about the FIXED axle, whose
	 * arc is L/tan(lock) - strictly smaller for any lock under 90 degrees, since tan exceeds
	 * sin there. For the 6.2 m bowser that is 361 uu against 510, about 30% less room.
	 *
	 * WHY THAT MATTERS RATHER THAN BEING A CURIOSITY: a bay is a dead end, so a vehicle either
	 * backs into it or the bay is a drive-through needing TWO forward corners. Backing in needs
	 * less room than either, which is why real aprons do it and why the 2026-09-16 rethink
	 * stopped trying to thread a drivable lane past the aeroplane.
	 *
	 * NOT A SPEED LIMIT. Like its forward sibling this is kinematic: an arc tighter than this
	 * cannot be reversed along at any speed, because speed cancels out of the requirement.
	 */
	double TightestReversibleRadius() const
	{
		const double Lock = FMath::Tan(FMath::DegreesToRadians(
			FMath::Clamp(Ground.MaxSteerDegrees, 0.0, 90.0)));
		return (EffectiveSteerLaw() == ESteerLaw::RollingSteer && Lock > KINDA_SMALL_NUMBER)
			? Wheelbase() / Lock : 0.0;
	}
};

/**
 * Logs, once per call, if Chassis cannot support its declared steer law - see
 * FChassis::NeedsSteerLawWarning.
 *
 * A FREE FUNCTION DEFINED IN Chassis.cpp, not a member here, so this header can stay free
 * of AirsideLog.h - see the top of this file for why that mattered. Call it ONCE, from
 * wherever an airframe is first bound to a phase, rather than from the hot path that reads
 * EffectiveSteerLaw every frame: FRouteFollower::Start and ::Replace, and FLandingRun::Begin,
 * do this. FRoadAgent's own StartTaxi / StartArrival / StartPushback - the sites the issue
 * that split this file names first - are NOT it: they were mid-refactor under a concurrent
 * change (#174) when this was written, so the call sits one layer down instead, at the
 * structs FRoadAgent's Start* methods already delegate the airframe to. The net effect is
 * the same: once per dispatch or replan, never once per Advance.
 *
 * NO TYPE NAME IN THE LINE since 2026-09-23. It used to print FAirframe::TypeCode; a chassis
 * has no name, and threading one through FRouteFollower::Start only to print it would be a
 * parameter nothing moves by. The two axle figures still identify the mis-authored row.
 */
AIRSIDE_API void WarnIfSteerLawUnsupported(const FChassis& Chassis);
