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
	if (MainWheelRadius > KINDA_SMALL_NUMBER)
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
	PropAngleDegrees = FMath::Fmod(PropAngleDegrees + RPM * 6.0f * DeltaSeconds, 360.0f);

	// See the header: a modelled blade at 2000 RPM strobes against a 60 Hz frame rate.
	bPropIsDisc = RPM > PropDiscRPM;
}
