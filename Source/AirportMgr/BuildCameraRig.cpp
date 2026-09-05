#include "BuildCameraRig.h"

double FBuildCameraRig::PitchDegrees() const
{
	// Guarded so the logarithms below are always taken of a ratio greater than one, and so
	// a details-panel edit that puts Max below Min cannot produce a NaN pitch - which would
	// send the camera somewhere unrecoverable rather than merely somewhere wrong.
	const double Low = FMath::Max(MinDistance, 1.0);
	const double High = FMath::Max(MaxDistance, Low * 1.0001);

	const double Alpha = FMath::Loge(FMath::Clamp(Distance, Low, High) / Low) / FMath::Loge(High / Low);
	return FMath::Lerp(MinPitch, MaxPitch, Alpha);
}

FVector FBuildCameraRig::CameraLocation(double PlaneZ) const
{
	const double PitchRadians = FMath::DegreesToRadians(PitchDegrees());
	const double YawRadians = FMath::DegreesToRadians(Yaw);

	// The direction the camera looks: a horizontal part shrinking as the pitch steepens,
	// and a downward part growing with it. Backing off the focus along it puts the camera
	// exactly Distance away with the focus centred, whatever the pitch.
	const FVector Look(
		FMath::Cos(PitchRadians) * FMath::Cos(YawRadians),
		FMath::Cos(PitchRadians) * FMath::Sin(YawRadians),
		-FMath::Sin(PitchRadians));

	return FVector(Focus.X, Focus.Y, PlaneZ) - Look * Distance;
}

FRotator FBuildCameraRig::CameraRotation() const
{
	// Negative pitch is downward in Unreal, and this is set on the camera actor's own
	// transform rather than through SetControlRotation: control rotation near +/-90 pitch
	// hits gimbal lock and is silently renormalised to something else. MaxPitch stays below
	// 90 partly for that reason.
	return FRotator(-PitchDegrees(), Yaw, 0.0);
}

FBuildCameraRig FBuildCameraRig::InFrame(const FVector2D& Origin, double HeadingDegrees) const
{
	const double HeadingRadians = FMath::DegreesToRadians(HeadingDegrees);

	// The same basis Pan uses: nose along the heading, right wing a quarter turn on from
	// it in Unreal's left-handed sense.
	const FVector2D Nose(FMath::Cos(HeadingRadians), FMath::Sin(HeadingRadians));
	const FVector2D Wing(-FMath::Sin(HeadingRadians), FMath::Cos(HeadingRadians));

	FBuildCameraRig World = *this;
	World.Focus = Origin + Nose * Focus.X + Wing * Focus.Y;
	World.Yaw = HeadingDegrees + Yaw;
	return World;
}

void FBuildCameraRig::Zoom(double Step, double Notches)
{
	Distance = FMath::Clamp(
		Distance * FMath::Pow(1.0 + Step, Notches),
		FMath::Max(MinDistance, 1.0),
		FMath::Max(MaxDistance, 1.0));
}

void FBuildCameraRig::Pan(double Right, double Forward, double Rate, double DeltaTime)
{
	if (Right == 0.0 && Forward == 0.0)
	{
		return;
	}

	const double YawRadians = FMath::DegreesToRadians(Yaw);

	// The camera's own axes flattened onto the road plane. Unreal is left-handed with +Y
	// to the right of +X, so the right axis is the forward one turned a quarter turn.
	const FVector2D ForwardAxis(FMath::Cos(YawRadians), FMath::Sin(YawRadians));
	const FVector2D RightAxis(-FMath::Sin(YawRadians), FMath::Cos(YawRadians));

	const double Step = Rate * Distance * DeltaTime;
	Focus += (ForwardAxis * Forward + RightAxis * Right) * Step;
}

void FBuildCameraRig::Rotate(double Degrees)
{
	// Left unnormalised on purpose - see the note on Yaw.
	Yaw += Degrees;
}

void FBuildCameraRig::EaseToward(const FBuildCameraRig& Target, double Lag, double DeltaTime)
{
	// Limits first, so editing them in the details panel during play takes effect on the
	// view rather than only on the target the view is chasing.
	MinDistance = Target.MinDistance;
	MaxDistance = Target.MaxDistance;
	MinPitch = Target.MinPitch;
	MaxPitch = Target.MaxPitch;

	// 1 - exp(-dt/lag) rather than a fixed fraction per frame: a constant fraction makes
	// the camera settle at a speed that depends on the frame rate, so the same zoom feels
	// different on a loaded machine.
	const double Alpha = (Lag > 0.0 && DeltaTime > 0.0)
		? 1.0 - FMath::Exp(-DeltaTime / Lag)
		: 1.0;

	Focus = FMath::Lerp(Focus, Target.Focus, Alpha);
	Distance = FMath::Lerp(Distance, Target.Distance, Alpha);
	Yaw = FMath::Lerp(Yaw, Target.Yaw, Alpha);
}
