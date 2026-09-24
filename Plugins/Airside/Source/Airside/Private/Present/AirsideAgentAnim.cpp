#include "Present/AirsideAgentAnim.h"

#include "Present/RoadAgentActor.h"

#include "AnimationRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

void UAirsideAgentAnim::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	// The fallback first, so the preview in the ABP editor (no agent) and every aircraft
	// roll exactly as they always did - see WheelRadius.
	WheelRadius = MainWheelRadius;

	const ARoadAgentActor* Agent = Cast<ARoadAgentActor>(GetOwningActor());
	const USkeletalMeshComponent* Component = GetSkelMeshComponent();
	if (Agent == nullptr || Component == nullptr)
	{
		return;
	}
	// A VEHICLE: the cab (dressed by SetVehicleAirframe, which marks the actor first) or one
	// of its trailers (whose FTowLinkView exists before its anim class is set).
	if (!Agent->IsVehicle() && Agent->FindTowLinkView(Component) == nullptr)
	{
		return;
	}
	if (const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset())
	{
		WheelRadius = WheelHubRadius(Mesh->GetRefSkeleton(), MainWheelRadius);
	}
}

void UAirsideAgentAnim::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	// THE OWNING ACTOR, not TryGetPawnOwner: an agent is an AActor, never an APawn, so that
	// path could not succeed - and casting a pawn to this actor was a compile warning.
	const ARoadAgentActor* Agent = Cast<ARoadAgentActor>(GetOwningActor());

	if (Agent == nullptr)
	{
		// Previewing in the Animation Blueprint editor, where there is no agent. Everything
		// holds its last value rather than snapping to zero, so a preview looks parked
		// instead of broken.
		return;
	}

	const FAgentMotion& Motion = Agent->GetMotion();

	GroundSpeed = static_cast<float>(Motion.GroundSpeed);
	bAirborne = Motion.bAirborne;

	// THE GEAR, COPIED AND NOT DERIVED - the model ran the cycle, doors and truck and all, and
	// this is where it got to. See FRoadAgent::AdvanceGear.
	//
	// UNPACKED FROM ONE STRUCT INTO THREE FLOATS, which is the only place in the project that
	// happens and is deliberate: FAgentMotion carries an FGearPose because the model always
	// writes the three together, while an Animation Blueprint reads each one by NAME off a
	// variable getter. Nesting them here would break every shipped graph.
	GearDownFraction = static_cast<float>(Motion.GearPose.GearDownFraction);
	BayDoorOpenFraction = static_cast<float>(Motion.GearPose.BayDoorOpenFraction);
	TruckLevelFraction = static_cast<float>(Motion.GearPose.TruckLevelFraction);

	// ONE RULE, THREE TIMES. Each fraction counts DOWNNESS, OPENNESS or LEVELNESS - its own
	// rest state, which is the bind pose - and each angle counts travel away from it. See
	// AngleFromRestFraction, which carries the sign trap this used to ship.
	GearAngleDegrees = AngleFromRestFraction(GearDownFraction, GearRetractedAngleDegrees);
	BayDoorAngleDegrees = AngleFromRestFraction(BayDoorOpenFraction, BayDoorClosedAngleDegrees);
	TruckTiltAngleDegrees = AngleFromRestFraction(TruckLevelFraction, TruckTiltedAngleDegrees);

	// A TRAILER'S INSTANCE READS ITS OWN LINK. Every mesh on the agent shares one owning actor,
	// so without this a trailer's wheels would roll at the CAB's speed and its turntable steer
	// by the CAB's wheel - see TowbarAngleDegrees. The actor answers which component is which.
	if (const FTowLinkView* Tow = Agent->FindTowLinkView(GetSkelMeshComponent()))
	{
		SteerAngleDegrees = 0.0f;
		TowbarAngleDegrees = (Motion.Tow.IsValidIndex(Tow->TowbarLink) && Motion.Tow.IsValidIndex(Tow->Link))
			? RelativeYawDegrees(Motion.Tow[Tow->TowbarLink].Heading, Motion.Tow[Tow->Link].Heading)
			: 0.0f;
		// The AXLE's travel, not the cab's speed - see FTowLinkView::RolledUu.
		WheelAngleDegrees = WheelAngleFromTravel(Tow->RolledUu, WheelRadius);
	}
	else
	{
		// COPIED, NOT DERIVED. The model steered with this exact angle - see
		// FRouteFollower::SteerDegrees - so the wheel the player watches is the one that turned
		// the aeroplane rather than a second opinion about it.
		SteerAngleDegrees = static_cast<float>(Motion.SteerAngleDegrees);
		TowbarAngleDegrees = 0.0f;

		// WHEELS: v = wr on the ground; DECAYED, NOT DROPPED, in the air (#107 item 8) - see
		// WheelStepDegrees and FAgentMotion::GroundSpeed's own header.
		WheelAngleDegrees = FMath::Fmod(
			WheelAngleDegrees
				+ WheelStepDegrees(GroundSpeed, WheelRadius, bAirborne, DeltaSeconds,
					WheelSpinDownSeconds, WheelRateDegPerSec),
			360.0f);
	}

	// PROPELLER: RPM to degrees a second is x6 - 360 degrees over 60 seconds.
	//
	// TAKEN FROM THE MODEL, not derived from the running flag. It used to be
	// bEngineRunning ? PropellerRPM : 0, which made the propeller a switch: full speed the
	// instant an aircraft was dispatched, stopped the instant it shut down. A propeller has
	// inertia, and the model now says where it has got to - see FEnginePerformance.
	const float RPM = static_cast<float>(Motion.EngineRPM);

	// BOTH CAPPED NOW (#107 item 7): RPM first, THEN the per-frame step. A blade repeats every
	// 360/N degrees, and a step past half of that is indistinguishable from a smaller step
	// the other way - which is why this propeller appeared to stop at 110 fps and run
	// backwards at 120. Capping only the step (the original fix for that) made the apparent
	// speed rise with the frame rate instead: a fixed degrees-per-frame ceiling times a
	// higher frame rate is a higher degrees-per-second rate, which read from play as the
	// propeller changing speed with the camera. Capping the RATE (PropDisplayCapRPM) fixes
	// that; the step clamp (PropMaxStepPerRepeat) stays as the guard against aliasing at a
	// frame rate low enough to hitch, which a rate cap alone cannot prevent.
	PropAngleDegrees = FMath::Fmod(
		PropAngleDegrees
			+ PropStepDegrees(RPM, DeltaSeconds, PropBladeCount, PropMaxStepPerRepeat, PropDisplayCapRPM),
		360.0f);

	// See the header: a modelled blade at 2000 RPM strobes against a 60 Hz frame rate.
	bPropIsDisc = RPM > PropDiscRPM;
}

float UAirsideAgentAnim::WheelStepDegrees(float GroundSpeed, float Radius, bool bAirborne,
	float DeltaSeconds, float SpinDownSeconds, float& InOutRateDegPerSec)
{
	if (Radius <= KINDA_SMALL_NUMBER)
	{
		// A radius of zero is a configuration mistake, and dividing by it would put NaN into
		// a bone transform - which does not show up as a fast wheel, it shows up as an
		// aircraft that vanishes.
		InOutRateDegPerSec = 0.0f;
		return 0.0f;
	}

	if (!bAirborne)
	{
		// ON THE GROUND: read straight off ground speed every frame, v = wr - the tyre has no
		// inertia of its own here, it is being driven by contact with the tarmac.
		InOutRateDegPerSec = FMath::RadiansToDegrees(GroundSpeed / Radius);
	}
	else if (SpinDownSeconds > KINDA_SMALL_NUMBER)
	{
		// AIRBORNE: DECAYED, NOT DROPPED. This used to be gated on !bAirborne outright, which
		// contradicts FAgentMotion::GroundSpeed's own header - "a wheel that stopped the
		// instant the aircraft lifted off would snap from spinning to still in one frame,
		// which is the one thing real wheels visibly do not do" - and that is exactly what it
		// did: a ~12,000 deg/s wheel at rotation stopped dead in a single frame.
		//
		// THE FLARE'S OWN LAW (h' = -h/tau, FLandingRun::Advance), applied to the rate
		// instead of a height: proportional to what is left, so it eases toward zero rather
		// than running at a fixed rate until it arrives and then holding. Bounded in time by
		// SpinDownSeconds the same way the flare is bounded by FlareTimeConstantSeconds.
		//
		// TOWARD ZERO, NOT Max AGAINST IT. This was FMath::Max(Decayed, 0), which is only a
		// decay for a wheel turning forwards: GroundSpeed became signed on 2026-09-20 so a
		// reversing vehicle rolls its wheels back, and Max against zero would return zero on
		// the first airborne frame - reinstating, for anything backing up, the exact snap this
		// spin-down exists to prevent. Clamping on the SIDE IT STARTED also stops the decay
		// overshooting through zero on a long frame and driving the wheel the other way.
		const float Decayed =
			InOutRateDegPerSec - (InOutRateDegPerSec / SpinDownSeconds) * DeltaSeconds;
		InOutRateDegPerSec = InOutRateDegPerSec >= 0.0f ? FMath::Max(Decayed, 0.0f)
		                                                : FMath::Min(Decayed, 0.0f);
	}
	else
	{
		// SpinDownSeconds <= 0 is authored as "stop dead" - a choice, not a division by a
		// number close to zero.
		InOutRateDegPerSec = 0.0f;
	}

	return InOutRateDegPerSec * DeltaSeconds;
}

float UAirsideAgentAnim::PropStepDegrees(float RPM, float DeltaSeconds, int32 BladeCount,
	float MaxStepPerRepeat, float DisplayCapRPM)
{
	// THE RATE IS CAPPED, NOT THE STEP - fps-independent by construction, because this feeds
	// straight into a degrees-per-second figure rather than being sized against one frame's
	// worth of it. NEVER RAISES the rate: a genuinely slow propeller (idling, spooling) is
	// still shown turning at its real speed, only ever brought DOWN toward it.
	const float Displayed = FMath::Min(RPM, FMath::Max(DisplayCapRPM, 0.0f));

	// RPM to degrees a second is x6 - 360 degrees over 60 seconds.
	const float Wanted = Displayed * 6.0f * DeltaSeconds;

	// A blade repeats every 360/N degrees, so that - not a full turn - is the angle the frame
	// rate has to resolve. Past half of it a step is indistinguishable from a smaller one the
	// other way, which is why this propeller appeared to stand still at 110 fps and to run
	// backwards at 120 and 144.
	const float Repeat = 360.0f / static_cast<float>(FMath::Max(BladeCount, 1));
	const float Largest = Repeat * FMath::Clamp(MaxStepPerRepeat, 0.01f, 0.5f);
	return FMath::Min(FMath::Max(Wanted, 0.0f), Largest);
}

float UAirsideAgentAnim::AngleFromRestFraction(float RestFraction, float TravelledAngle)
{
	// ONE MINUS THE FRACTION, because every fraction in FGearPose counts how far the part is
	// toward its REST state - down, open, level - and the angle counts travel AWAY from that
	// state, which is the bind pose. Getting it the other way round parks an aeroplane on a
	// folded leg with its bay shut round the wheels, which is a state the mesh can express
	// perfectly happily and which has been on screen once. See the header.
	return (1.0f - RestFraction) * TravelledAngle;
}

float UAirsideAgentAnim::RelativeYawDegrees(double Heading, double RelativeTo)
{
	// FindDeltaAngleRadians wraps to -PI..PI - see the header for why the raw difference is wrong.
	return static_cast<float>(FMath::RadiansToDegrees(FMath::FindDeltaAngleRadians(RelativeTo, Heading)));
}

float UAirsideAgentAnim::WheelAngleFromTravel(double TravelUu, float Radius)
{
	if (Radius <= 0.0f)
	{
		return 0.0f;
	}
	// In DOUBLE until after the Fmod - see the header.
	return static_cast<float>(FMath::Fmod(FMath::RadiansToDegrees(TravelUu / Radius), 360.0));
}

float UAirsideAgentAnim::WheelHubRadius(const FReferenceSkeleton& Skeleton, float Fallback)
{
	for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone)
	{
		if (!Skeleton.GetBoneName(Bone).ToString().StartsWith(TEXT("wheel")))
		{
			continue;
		}
		// The HUB's height in the reference pose IS the radius: z = 0 is the contact plane.
		const double Hub = FAnimationRuntime::GetComponentSpaceTransformRefPose(Skeleton, Bone).GetTranslation().Z;
		return Hub > 0.0 ? static_cast<float>(Hub) : Fallback;
	}
	return Fallback;
}
