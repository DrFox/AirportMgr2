#include "BuildCameraComponent.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "MiniatureFocus.h"
#include "Present/AirsideOwnedLandActor.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildLog.h"

UBuildCameraComponent::UBuildCameraComponent()
{
	// Driven explicitly from ARoadBuildController::PlayerTick's UpdateView call, in the same
	// frame as input is read - a component tick would run at an unrelated point in the frame
	// and read stale WASD state.
	PrimaryComponentTick.bCanEverTick = false;

	// WatchLimits does NOT keep FCameraRigLimits' own defaults - those are the BUILD view's
	// numbers, and pressing C would otherwise open the watch camera 8000 uu out at yaw 0
	// instead of framing the aircraft. See WatchLimits' own comment for what each number
	// means; these are the exact values ApplyWatchLimits/ToggleWatchAgent used before the
	// split (issue #94 review).
	WatchLimits.MinDistance = 800.0;
	WatchLimits.MaxDistance = 20000.0;
	WatchLimits.MinPitch = 10.0;
	WatchLimits.MaxPitch = 60.0;
	WatchLimits.StartDistance = 1550.0;
	WatchLimits.StartYaw = -75.0;
	// The close-zoom speed-up is for the build view's run down to the horizon; the watch
	// camera's whole range is close, and it zoomed at one even step before this existed.
	WatchLimits.CloseZoomDistance = 0.0;
}

void UBuildCameraComponent::CreateBuildCamera(APlayerController& Owner, const ARoadNetworkActor& Target)
{
	// The actor was only ever read for this one figure - see the header's overload.
	CreateBuildCamera(Owner, Target.SurfaceZ);
}

void UBuildCameraComponent::CreateBuildCamera(APlayerController& Owner, double SurfaceZ)
{
	UWorld* World = Owner.GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// THE OWNED LAND BOUNDS THE FOCUS, set before Reset so a StartFocus outside it starts inside.
	// Looked up once here: the land is fixed for a session until land purchase exists, and that
	// feature calls SetFocusBounds when it grows the plot rather than this polling for it.
	if (const AAirsideOwnedLandActor* Land = AAirsideOwnedLandActor::Find(World))
	{
		const FBox2D Bounds = Land->GetOwnedLand();
		TargetView.FocusBounds = Bounds;
		UE_LOG(LogRoadBuild, Log, TEXT("Build camera: focus held inside the owned land (%.0f, %.0f)-(%.0f, %.0f)."),
			Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y);
	}
	TargetView.Reset(ViewLimits);

	// The view starts settled rather than easing in from wherever a default-constructed rig
	// happens to sit, which would swoop the camera across the map on possession.
	CurrentView = TargetView;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	BuildCamera = World->SpawnActor<ACameraActor>(
		CurrentView.CameraLocation(SurfaceZ), CurrentView.CameraRotation(), Params);
	if (BuildCamera == nullptr)
	{
		return;
	}

	UCameraComponent* Camera = BuildCamera->GetCameraComponent();
	Camera->SetProjectionMode(ECameraProjectionMode::Perspective);
	Camera->SetFieldOfView(static_cast<float>(FieldOfView));

	// Viewing through a camera actor takes the view away from the pawn, so the pawn's
	// mouse-look stops fighting the cursor for the same input.
	Owner.SetViewTarget(BuildCamera);

	UE_LOG(LogRoadBuild, Log,
		TEXT("Build camera: %.0f uu out at %.1f degrees. Pitch follows the zoom, %.0f to %.0f degrees."),
		CurrentView.Distance, CurrentView.PitchDegrees(), ViewLimits.MinPitch, ViewLimits.MaxPitch);
	UE_LOG(LogRoadBuild, Log,
		TEXT("Miniature focus %s: blur at infinity %.3f of the frame, f/%.1f, sharp inside %.0f uu, full at zoom <= %.0f uu, none from %.0f uu."),
		bMiniatureFocus ? TEXT("ON") : TEXT("off"), MiniatureBlurAtInfinity, MiniatureFStop,
		MiniatureSharpDistance, MiniatureFullBlurZoom, MiniatureNoBlurZoom);
	ApplyMiniatureFocus();
}

void UBuildCameraComponent::UpdateView(float DeltaTime, double Right, double Forward, double Turn, double TurnPixels,
	ARoadNetworkActor* Target)
{
	if (BuildCamera == nullptr || Target == nullptr)
	{
		return;
	}

	// WATCHING AN AIRCRAFT drives the watch rig with the same axes, and hands the camera
	// straight back when there is nothing to watch - a mode that stranded the view on a
	// despawned aircraft would leave the player looking at empty sky with no way to tell why.
	if (bWatchingAgent)
	{
		if (ARoadAgentActor* Agent = Target->GetAgentView(WatchAgentId))
		{
			WatchTarget.ApplyLimits(WatchLimits);
			WatchTarget.Pan(Right, Forward, PanRate * PanScale, DeltaTime);
			WatchTarget.Focus = WatchTarget.Focus.GetClampedToMaxSize(WatchMaxFocusOffset);
			// Keys are a rate and need DeltaTime; the mouse delta is already a per-frame
			// distance and must NOT have it - see UpdateView's own comment.
			WatchTarget.Rotate(Turn * RotateRate * DeltaTime + TurnPixels * MouseRotateRate);

			// Eased in the AIRCRAFT'S frame, then projected: the aircraft's own motion
			// reaches the camera rigidly and only the player's inputs are smoothed. Easing a
			// world-space rig towards a moving aircraft would trail it instead.
			WatchCurrent.EaseToward(WatchTarget, CameraLag, DeltaTime);

			const FVector At = Agent->GetActorLocation();
			const FBuildCameraRig World = WatchCurrent.InFrame(FVector2D(At), Agent->GetActorRotation().Yaw);
			BuildCamera->SetActorLocationAndRotation(
				World.CameraLocation(At.Z + WatchFocusHeight), World.CameraRotation());
			ApplyMiniatureFocus();
			return;
		}

		bWatchingAgent = false;
		UE_LOG(LogRoadBuild, Log, TEXT("Nothing to watch: back to the build view."));
	}

	UpdateFreeView(DeltaTime, Right, Forward, Turn, TurnPixels, Target->SurfaceZ);
}

void UBuildCameraComponent::UpdateFreeView(float DeltaTime, double Right, double Forward, double Turn,
	double TurnPixels, double SurfaceZ)
{
	// ITS OWN GUARD. UpdateView above checks this before it ever reaches here, but the bench
	// calls this directly and a camera that has not been created yet is the ordinary state on
	// the first frame after possession.
	if (BuildCamera == nullptr)
	{
		return;
	}

	TargetView.ApplyLimits(ViewLimits);
	TargetView.Pan(Right, Forward, PanRate * PanScale, DeltaTime);
	// Keys are a rate and need DeltaTime; the mouse delta is already a per-frame distance and
	// must NOT have it - see UpdateView's own comment.
	TargetView.Rotate(Turn * RotateRate * DeltaTime + TurnPixels * MouseRotateRate);
	CurrentView.EaseToward(TargetView, CameraLag, DeltaTime);

	BuildCamera->SetActorLocationAndRotation(
		CurrentView.CameraLocation(SurfaceZ), CurrentView.CameraRotation());
	ApplyMiniatureFocus();
}

void UBuildCameraComponent::ApplyMiniatureFocus()
{
	if (BuildCamera == nullptr)
	{
		return;
	}

	FPostProcessSettings& Settings = BuildCamera->GetCameraComponent()->PostProcessSettings;

	FMiniatureFocus Focus;
	Focus.BlurAtInfinity = MiniatureBlurAtInfinity;
	Focus.FStop = MiniatureFStop;
	Focus.SharpDistanceUu = MiniatureSharpDistance;
	Focus.FullBlurZoomUu = MiniatureFullBlurZoom;
	Focus.NoBlurZoomUu = MiniatureNoBlurZoom;

	// ActiveRig().Distance IS the camera-to-look-at distance in both rigs - CameraLocation
	// backs off the focus by exactly Distance along the view direction - so it is the zoom
	// the fade reads, with no trace needed. It is no longer the FOCUS: that is the fixed
	// SharpDistance, so a watched aircraft 15 m away stays sharp however it is framed.
	const double FocusUu = Focus.SharpDistanceUu;
	const double Sensor = bMiniatureFocus ? Focus.SensorWidthMm(ActiveRig().Distance, FieldOfView) : 0.0;

	// A zero sensor means off (or a degenerate input): clear the overrides, so the level's
	// post-process volume decides, rather than leaving the last frame's lens in place.
	const bool bOn = Sensor > 0.0;
	Settings.bOverride_DepthOfFieldFocalDistance = bOn;
	Settings.bOverride_DepthOfFieldSensorWidth = bOn;
	Settings.bOverride_DepthOfFieldFstop = bOn;
	if (bOn)
	{
		Settings.DepthOfFieldFocalDistance = static_cast<float>(FocusUu);
		Settings.DepthOfFieldSensorWidth = static_cast<float>(Sensor);
		Settings.DepthOfFieldFstop = static_cast<float>(MiniatureFStop);
	}
}

void UBuildCameraComponent::SetPlayerSpeedScales(double Pan, double Zoom)
{
	PanScale = FMath::Max(0.0, Pan);
	ZoomScale = FMath::Max(0.0, Zoom);
}

void UBuildCameraComponent::ZoomBy(double Notches)
{
	// The wheel drives whichever rig owns the camera. Zooming the hidden build view while
	// watching would be a surprise stored up for the moment the watch ends.
	FBuildCameraRig& View = bWatchingAgent ? WatchTarget : TargetView;
	View.ApplyLimits(bWatchingAgent ? WatchLimits : ViewLimits);
	// Capped where ZoomStep's own ClampMax is: a notch that more than doubles the distance is a jump.
	View.Zoom(FMath::Min(ZoomStep * ZoomScale, 0.9), Notches);

	UE_LOG(LogRoadBuild, Log, TEXT("%s %.0f uu out, %.1f degrees"),
		bWatchingAgent ? TEXT("Watch") : TEXT("View"), View.Distance, View.PitchDegrees());
}

bool UBuildCameraComponent::ToggleWatchAgent(const ARoadNetworkActor& Target, int32 PreferredAgentId)
{
	if (!bWatchingAgent && Target.GetAgentView(PreferredAgentId) == nullptr)
	{
		// Refused. The caller says so out loud - silently staying on the build camera is
		// indistinguishable from the key not being bound, which is a class of confusion this
		// project has paid for.
		return false;
	}

	bWatchingAgent = !bWatchingAgent;
	if (bWatchingAgent)
	{
		WatchAgentId = PreferredAgentId;
		// Reset on every entry rather than resuming: C is "show me the aircraft", and a view
		// left zoomed into a wheel last time would answer with a wheel.
		WatchTarget.Reset(WatchLimits);
		WatchCurrent = WatchTarget;
	}

	return true;
}
