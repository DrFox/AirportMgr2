#include "Present/AirsideAgentAnim.h"

#include "Present/RoadAgentActor.h"

void UAirsideAgentAnim::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	const ARoadAgentActor* Agent = Cast<ARoadAgentActor>(TryGetPawnOwner());
	if (Agent == nullptr)
	{
		// An agent is an AActor, not a APawn, so TryGetPawnOwner never finds it. Asked for
		// first anyway because it is the cheap path and costs nothing when it fails.
		Agent = Cast<ARoadAgentActor>(GetOwningActor());
	}

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

	// WHEELS: v = wr, so the rate is speed over radius. Guarded because a radius of zero is
	// a configuration mistake, and dividing by it would put NaN into a bone transform - which
	// does not show up as a fast wheel, it shows up as an aircraft that vanishes.
	//
	// AND ONLY WHILE THE WHEELS ARE ON THE GROUND. Ground speed does not fall to zero at
	// rotation - a climbing aeroplane is still travelling, and faster than it ever did on the
	// runway - so integrating it regardless spun the wheels harder than ever as the aircraft
	// climbed away. Real gear spins down over a few seconds in the airflow; stopping is not
	// that, but it is far closer than accelerating.
	if (MainWheelRadius > KINDA_SMALL_NUMBER && !bAirborne)
	{
		const float RadiansPerSecond = GroundSpeed / MainWheelRadius;
		WheelAngleDegrees = FMath::Fmod(
			WheelAngleDegrees + FMath::RadiansToDegrees(RadiansPerSecond) * DeltaSeconds, 360.0f);
	}

	// PROPELLER: RPM to degrees a second is x6 - 360 degrees over 60 seconds.
	//
	// TAKEN FROM THE MODEL, not derived from the running flag. It used to be
	// bEngineRunning ? PropellerRPM : 0, which made the propeller a switch: full speed the
	// instant an aircraft was dispatched, stopped the instant it shut down. A propeller has
	// inertia, and the model now says where it has got to - see FEnginePerformance.
	const float RPM = static_cast<float>(Motion.EngineRPM);

	// CLAMPED TO WHAT THE FRAME RATE CAN SHOW. A blade repeats every 360/N degrees, and a step
	// past half of that is indistinguishable from a smaller step the other way - which is why
	// this propeller appeared to stop at 110 fps and run backwards at 120. The step, not the
	// RPM, is what is capped: a cap expressed in RPM would still alias at a low enough frame
	// rate, because the angle per frame is what the sampling sees. See PropMaxStepPerRepeat.
	PropAngleDegrees = FMath::Fmod(
		PropAngleDegrees + PropStepDegrees(RPM, DeltaSeconds, PropBladeCount, PropMaxStepPerRepeat),
		360.0f);

	// See the header: a modelled blade at 2000 RPM strobes against a 60 Hz frame rate.
	bPropIsDisc = RPM > PropDiscRPM;
}

float UAirsideAgentAnim::PropStepDegrees(float RPM, float DeltaSeconds, int32 BladeCount,
	float MaxStepPerRepeat)
{
	// RPM to degrees a second is x6 - 360 degrees over 60 seconds.
	const float Wanted = RPM * 6.0f * DeltaSeconds;

	// A blade repeats every 360/N degrees, so that - not a full turn - is the angle the frame
	// rate has to resolve. Past half of it a step is indistinguishable from a smaller one the
	// other way, which is why this propeller appeared to stand still at 110 fps and to run
	// backwards at 120 and 144.
	const float Repeat = 360.0f / static_cast<float>(FMath::Max(BladeCount, 1));
	const float Largest = Repeat * FMath::Clamp(MaxStepPerRepeat, 0.01f, 0.5f);
	return FMath::Min(FMath::Max(Wanted, 0.0f), Largest);
}
