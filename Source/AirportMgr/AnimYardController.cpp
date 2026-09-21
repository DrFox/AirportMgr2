#include "AnimYardController.h"

#include "AnimYard.h"
#include "BuildCameraComponent.h"
#include "RoadBuildLog.h"

#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	/**
	 * THE KEY TABLE. See YardActions' header for why there is exactly one of these.
	 *
	 * COMMA AND PERIOD FOR THE SCRUB, not the arrow keys: DefaultInput.ini binds the arrows
	 * alongside WASD on this project's axis mappings, and a scrub key that also panned the
	 * camera would drag the value and the view at once. Comma and period are unbound, sit
	 * under the right hand while the left is on WASD, and carry the < > engraving that already
	 * means "step this way" on every media transport ever made.
	 *
	 * G FOR AIRBORNE because it is the gear key in every flight simulator, and what the toggle
	 * is actually for is watching the wheels spin down and the legs fold.
	 */
	const FYardActionBinding GActions[] =
	{
		{ EKeys::SpaceBar,       EYardAction::TogglePause,    TEXT("run / pause the demo loop") },
		{ EKeys::Tab,            EYardAction::NextChannel,    TEXT("next channel") },
		{ EKeys::Period,         EYardAction::ScrubUp,        TEXT("scrub up (hold Shift: fine)") },
		{ EKeys::Comma,          EYardAction::ScrubDown,      TEXT("scrub down (hold Shift: fine)") },
		{ EKeys::G,              EYardAction::ToggleConfiguration,
			TEXT("airborne / on the wheels: gear, bays and wheels together") },
		{ EKeys::F,              EYardAction::ToggleSolo,     TEXT("solo the nearest model") },
		{ EKeys::R,              EYardAction::Reset,          TEXT("reset and resume") },
		{ EKeys::MouseScrollUp,  EYardAction::ZoomIn,         TEXT("zoom in") },
		{ EKeys::MouseScrollDown,EYardAction::ZoomOut,        TEXT("zoom out") },
	};

	/**
	 * The yard floor's height, uu.
	 *
	 * ZERO BECAUSE build_model_yard.py PUTS IT THERE - it spawns the floor plane at
	 * Vector(0,0,0) and every model on it. A constant rather than a measurement because there
	 * is nothing in the yard to measure it from: the level has no ARoadNetworkActor, which is
	 * the whole reason UBuildCameraComponent needed an overload that takes a height.
	 */
	constexpr double YardSurfaceZ = 0.0;

	/** How much of a step a fine drag is worth. */
	constexpr double FineScrubScale = 0.1;

	/**
	 * How much wider than the marks the opening shot is framed.
	 *
	 * THE MARKS ARE ORIGINS, NOT EXTENTS. Each is a nose gear or an axle, so the row's real
	 * width is the span between them PLUS half an aeroplane at each end - and the widest here
	 * is a 737 at 35.8 m across. A quarter again covers that on the yard's 185 m row and leaves
	 * the outermost models clear of the frame edge rather than touching it.
	 */
	constexpr double YardFramingMargin = 1.25;

	/**
	 * The pitch band the YARD's camera uses, degrees - shallower at both ends than the
	 * airport's 12..70.
	 *
	 * A MODEL YARD IS LOOKED AT, NOT PLANNED ON. FCameraRigLimits::MinPitch argues its 12 from
	 * keeping the horizon on screen and its 70 from "laying an airport out", and the second
	 * reason does not exist here: there is no plan to read, only models to look at from the
	 * front. At the framing distance the airport's band gives about 45 degrees, which is a
	 * view down onto the wings - the roofs of the aeroplanes, not their faces.
	 *
	 * NOT ZERO AT THE BOTTOM. Dead level collapses the row into one line and the models behind
	 * hide the ones in front; four degrees separates them without reading as a look-down.
	 */
	constexpr double YardMinPitch = 4.0;
	constexpr double YardMaxPitch = 28.0;
}

TArrayView<const FYardActionBinding> YardActions()
{
	return TArrayView<const FYardActionBinding>(GActions, UE_ARRAY_COUNT(GActions));
}

AAnimYardController::AAnimYardController()
{
	CameraComponent = CreateDefaultSubobject<UBuildCameraComponent>(TEXT("YardCamera"));
}

void AAnimYardController::BeginPlay()
{
	Super::BeginPlay();

	if (CachedYard == nullptr)
	{
		for (TActorIterator<AAnimYard> It(GetWorld()); It; ++It)
		{
			CachedYard = *It;
			break;
		}
	}

	if (CachedYard == nullptr)
	{
		// SAID OUT LOUD. Without a yard actor every key silently does nothing, which is
		// indistinguishable from the keys not being bound - and this project's own notes call
		// that the most expensive failure mode it has.
		UE_LOG(LogRoadBuild, Warning,
			TEXT("Anim yard: no AAnimYard in this level, so nothing will move. "
				"Re-run Tools/Python/build_model_yard.py to place one."));
	}

	// AIMED BEFORE THE CAMERA IS MADE, because CreateBuildCamera is what resets the rig and the
	// reset is what reads the limits. The other order builds the camera from last frame's aim.
	AimAtTheAircraft();

	if (CameraComponent != nullptr)
	{
		CameraComponent->CreateBuildCamera(*this, YardSurfaceZ);
	}

	// THE BANNER IS GENERATED FROM THE TABLE, never typed. A banner and a binding written in
	// different breaths is exactly how "4 routes" came to be advertised while EKeys::Four went
	// nowhere; printing the same array the binding loop walks makes that unrepresentable.
	FString Keys;
	for (const FYardActionBinding& Action : YardActions())
	{
		Keys += FString::Printf(TEXT("\n    %-12s %s"), *Action.Key.GetDisplayName().ToString(), Action.Help);
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Anim yard controller ready. WASD/QE to fly, middle-drag to turn.%s"), *Keys);
}

void AAnimYardController::AimAtTheAircraft()
{
	if (CameraComponent == nullptr || Yard() == nullptr)
	{
		return;
	}

	FBox2D Marks(ForceInit);
	double HeadingDegrees = 0.0;
	if (!Yard()->AircraftFraming(Marks, HeadingDegrees))
	{
		// Nothing driven to aim at. The rig's own default - the world origin - is as good an
		// answer as exists, and saying so beats a camera pointed at a row that is not there.
		UE_LOG(LogRoadBuild, Warning,
			TEXT("Anim yard: no driven aircraft to aim the opening view at; using the origin."));
		return;
	}

	FCameraRigLimits& Limits = CameraComponent->ViewLimits;

	// ORBIT ABOUT THE MIDDLE OF THE ROW, so panning and rotating from the opening shot turn
	// about what you are looking at rather than about a corner of it.
	Limits.StartFocus = Marks.GetCenter();

	// FACING THEM IS THE OPPOSITE OF THEIR HEADING. Yaw is the direction the camera LOOKS
	// along, so looking at the noses of a row facing H means looking along H + 180 - and
	// CameraLocation then backs the camera off the far side, which puts it in front of them.
	// Derived from the models' own facing rather than assuming +X: nothing in the yard script
	// rotates a model today, and a framing that silently depended on that would break quietly
	// the day one does.
	Limits.StartYaw = HeadingDegrees + 180.0;

	// THE DISTANCE IS SOLVED FROM THE ROW AND THE LENS, not chosen. Half the row subtends half
	// the horizontal field of view at exactly this range, so every model is in shot with the
	// margin to spare - and adding a seventh aeroplane widens the shot by itself.
	//
	// THE LONGER SIDE OF THE MARKS IS THE ROW. It is a line of models, so its bounding box is
	// long one way and near zero the other, whichever axis the row happens to run along.
	const FVector2D Size = Marks.GetSize();
	const double Span = FMath::Max(Size.X, Size.Y);
	const double HalfFovRadians = FMath::DegreesToRadians(CameraComponent->FieldOfView * 0.5);
	const double Fit = Span * 0.5 / FMath::Max(FMath::Tan(HalfFovRadians), KINDA_SMALL_NUMBER);
	Limits.StartDistance = FMath::Max(Fit * YardFramingMargin, Limits.MinDistance);

	// See YardMinPitch: the airport's band is argued from laying an airport out, which is not
	// what happens here.
	Limits.MinPitch = YardMinPitch;
	Limits.MaxPitch = YardMaxPitch;

	UE_LOG(LogRoadBuild, Log,
		TEXT("Anim yard view: %.0f uu in front of %d m of aircraft centred on (%.0f, %.0f), "
			"looking along %.0f degrees."),
		Limits.StartDistance, FMath::RoundToInt(Span / 100.0),
		Limits.StartFocus.X, Limits.StartFocus.Y, Limits.StartYaw);
}

void AAnimYardController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (InputComponent == nullptr)
	{
		return;
	}

	// EVERY key comes from the one table, exactly as ARoadBuildController binds from
	// BuildActions(). A key that exists without an entry cannot be documented; an entry
	// without a key is the bug this arrangement makes impossible.
	for (const FYardActionBinding& Action : YardActions())
	{
		if (!Action.Key.IsValid())
		{
			continue;
		}
		InputComponent->BindKey(Action.Key, IE_Pressed, this, &AAnimYardController::OnActionKey);
	}
}

void AAnimYardController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (CameraComponent == nullptr)
	{
		return;
	}

	// THE SAME AXES ARoadBuildController READS, so the yard flies the way the airport does and
	// there is nothing new to learn to use it.
	const double Right = (IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
	const double Forward = (IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
	const double Turn = (IsInputKeyDown(EKeys::E) ? 1.0 : 0.0) - (IsInputKeyDown(EKeys::Q) ? 1.0 : 0.0);

	double TurnPixels = 0.0;
	if (IsInputKeyDown(EKeys::MiddleMouseButton))
	{
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		GetInputMouseDelta(MouseX, MouseY);
		TurnPixels = MouseX;
	}

	// UpdateFreeView, NOT UpdateView: there is no ARoadNetworkActor in the yard and no agent
	// worth riding. See the overload's own comment.
	CameraComponent->UpdateFreeView(DeltaTime, Right, Forward, Turn, TurnPixels, YardSurfaceZ);
}

void AAnimYardController::Do(EYardAction Action)
{
	// THE CAMERA ACTIONS NEED NO YARD, and refusing them when the level has no bench in it
	// would leave the player unable even to look around while working out why.
	if (Action == EYardAction::ZoomIn || Action == EYardAction::ZoomOut)
	{
		if (CameraComponent != nullptr)
		{
			CameraComponent->ZoomBy(Action == EYardAction::ZoomIn ? 1.0 : -1.0);
		}
		return;
	}

	AAnimYard* TheYard = Yard();
	if (TheYard == nullptr)
	{
		return;
	}

	FYardMotion& Bench = TheYard->EditMotion();

	switch (Action)
	{
	case EYardAction::TogglePause:
		Bench.bPaused = !Bench.bPaused;
		break;

	case EYardAction::NextChannel:
	{
		// WRAPS. A caret that stopped at the last channel would leave it reachable only by
		// counting presses from a fresh start.
		const TArrayView<const EYardChannel> Channels = FYardMotion::Channels();
		int32 At = Channels.IndexOfByKey(CaretChannel);
		At = At == INDEX_NONE ? 0 : (At + 1) % Channels.Num();
		CaretChannel = Channels[At];
		break;
	}

	case EYardAction::ScrubUp:
		Bench.Scrub(CaretChannel, Bench.ChannelStep(CaretChannel) * ScrubScale());
		break;

	case EYardAction::ScrubDown:
		Bench.Scrub(CaretChannel, -Bench.ChannelStep(CaretChannel) * ScrubScale());
		break;

	case EYardAction::ToggleConfiguration:
		Bench.ToggleConfiguration();
		break;

	case EYardAction::ToggleSolo:
		if (TheYard->Solo() != nullptr)
		{
			TheYard->SetSolo(nullptr);
		}
		else if (CameraComponent != nullptr)
		{
			// THE CAMERA'S FOCUS IS "the model you are looking at" for an orbiting top-down
			// rig - the point the view is centred on, which is what the player aimed at.
			TheYard->SetSolo(TheYard->NearestSubject(CameraComponent->ViewFocus()));
		}
		break;

	case EYardAction::Reset:
		Bench.Reset();
		TheYard->SetSolo(nullptr);
		break;

	default:
		break;
	}

	// ONE LINE PER ACTION, and it names the whole state rather than what just changed. The
	// point of it is what happens AFTER the session: a screenshot of a rig in a wrong pose is
	// only evidence if the log says what the rig was being told at the time, and reconstructing
	// that from a sequence of deltas is exactly the work nobody does.
	UE_LOG(LogRoadBuild, Log,
		TEXT("Anim yard: %s | stage %s | caret %s = %.2f | speed %.0f steer %.1f gear %.2f RPM %.0f | %s | solo %s"),
		Bench.bPaused ? TEXT("PAUSED") : TEXT("running"),
		FYardMotion::StageName(Bench.CurrentStage()),
		FYardMotion::ChannelName(CaretChannel), Bench.Value(CaretChannel),
		Bench.GroundSpeed, Bench.SteerDegrees, Bench.GearCycleFraction, Bench.EngineRPM,
		Bench.bAirborne ? TEXT("airborne") : TEXT("on the wheels"),
		TheYard->Solo() != nullptr ? *AAnimYard::NameOf(TheYard->Solo()) : TEXT("none"));
}

void AAnimYardController::OnActionKey(FKey Key)
{
	for (const FYardActionBinding& Action : YardActions())
	{
		if (Action.Key == Key)
		{
			Do(Action.Action);
			return;
		}
	}
}

double AAnimYardController::ScrubScale() const
{
	// POLLED RATHER THAN BOUND. Shift is a modifier on a held drag, not an action of its own,
	// and binding it would put a tenth entry in the key table that does nothing on its own.
	const bool bFine = IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);
	return bFine ? FineScrubScale : 1.0;
}
