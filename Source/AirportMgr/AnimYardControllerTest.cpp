#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimYard.h"
#include "AnimYardController.h"
#include "AnimYardMotion.h"
#include "BuildCameraComponent.h"

#include "Animation/SkeletalMeshActor.h"
#include "Camera/CameraActor.h"
#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Present/AirsideAgentAnim.h"
#include "Present/RoadAgentActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	USkeletalMesh* LoadTruckMesh()
	{
		return Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr,
			TEXT("/Game/Vehicles/FuelTruck1/SK_FuelTruck1")));
	}

	ASkeletalMeshActor* PlaceRiggedModel(UWorld& World, const FVector& Where, USkeletalMesh* Mesh)
	{
		ASkeletalMeshActor* Actor = World.SpawnActor<ASkeletalMeshActor>(Where, FRotator::ZeroRotator);
		if (Actor != nullptr)
		{
			Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
		}
		return Actor;
	}

	/** A yard whose catalogue answer is "yes, with the C++ base class" for anything rigged. */
	AAnimYard* SpawnYardDriving(UWorld& World, USkeletalMesh* Mesh)
	{
		AAnimYard* Yard = World.SpawnActor<AAnimYard>();
		if (Yard == nullptr) { return nullptr; }

		Yard->RigResolver = [Mesh](USkeletalMesh* Candidate, FYardRig& Out)
		{
			if (Candidate != Mesh) { return false; }
			Out.AnimClass = UAirsideAgentAnim::StaticClass();
			Out.bIsVehicle = true;
			return true;
		};
		return Yard;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardEveryActionHasAKeyTest,
	"AirportMgr.View.AnimYard.EveryActionHasAKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardEveryActionHasAKeyTest::RunTest(const FString& Parameters)
{
	// CHECK WHERE THE LIST IS CONSUMED. This project has shipped the same bug three times - a
	// list of things declared and never read, and a banner advertising "4 routes" while
	// EKeys::Four went nowhere. The bench's help text is drawn from YardActions() and its keys
	// are bound from YardActions(), so the two cannot disagree about which keys exist; what
	// they CAN disagree about is whether the binding loop ran at all, which is what this asks.
	//
	// BY NAME, NOT BY COUNT. A count matches just as well when one action is bound twice and
	// another not at all.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	AAnimYardController* Controller = TestWorld.World->SpawnActor<AAnimYardController>();
	if (Controller == nullptr) { AddError(TEXT("no controller")); return false; }

	// A PlayerInput IS REQUIRED before the input component exists - the same precedent
	// PlayerTickBuildsOneContextTest records: a bare SpawnActor never creates one.
	Controller->InitInputSystem();
	if (Controller->InputComponent == nullptr)
	{
		AddError(TEXT("the controller has no input component, so nothing could be bound"));
		return false;
	}

	TSet<FName> Bound;
	for (const FInputKeyBinding& Binding : Controller->InputComponent->KeyBindings)
	{
		Bound.Add(Binding.Chord.Key.GetFName());
	}

	TestTrue(TEXT("the bench declares some actions at all"), YardActions().Num() > 0);

	for (const FYardActionBinding& Action : YardActions())
	{
		TestTrue(*FString::Printf(TEXT("%s is bound to a key (%s)"),
			Action.Help, *Action.Key.GetDisplayName().ToString()),
			Bound.Contains(Action.Key.GetFName()));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardActionsDriveTheYardTest,
	"AirportMgr.View.AnimYard.ActionsDriveTheYard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardActionsDriveTheYardTest::RunTest(const FString& Parameters)
{
	// A BOUND KEY THAT DOES NOTHING is the other half of the same failure, and the half a
	// binding check cannot see. Every action is invoked here and its effect measured on the
	// yard's own motion.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	AAnimYard* Yard = TestWorld.World->SpawnActor<AAnimYard>();
	AAnimYardController* Controller = TestWorld.World->SpawnActor<AAnimYardController>();
	if (Yard == nullptr || Controller == nullptr) { AddError(TEXT("no yard or controller")); return false; }
	Controller->SetYardForTest(Yard);

	// 1. PAUSE IS A TOGGLE, not a latch. The loop is running when the bench opens.
	TestFalse(TEXT("the demo loop is running to start with"), Yard->GetMotion().bPaused);
	Controller->Do(EYardAction::TogglePause);
	TestTrue(TEXT("the pause action pauses"), Yard->GetMotion().bPaused);
	Controller->Do(EYardAction::TogglePause);
	TestFalse(TEXT("and pressing it again resumes"), Yard->GetMotion().bPaused);

	// 2. THE CARET WALKS EVERY CHANNEL AND WRAPS. A caret that stopped at the last channel
	// would leave the last one reachable only by pressing Tab exactly the right number of
	// times from a fresh start.
	TSet<EYardChannel> Seen;
	const int32 ChannelCount = FYardMotion::Channels().Num();
	for (int32 i = 0; i < ChannelCount; ++i)
	{
		Seen.Add(Controller->Caret());
		Controller->Do(EYardAction::NextChannel);
	}
	TestEqual(TEXT("Tab reaches every channel in one pass"), Seen.Num(), ChannelCount);
	TestEqual(TEXT("and wraps back to where it started"),
		static_cast<int32>(Controller->Caret()), static_cast<int32>(FYardMotion::Channels()[0]));

	// 3. A SCRUB MOVES THE CARET'S CHANNEL BY EXACTLY ONE STEP, in the direction pressed.
	// One step and not two is what makes a held key walk a value at a readable rate.
	Controller->Do(EYardAction::Reset);
	const EYardChannel Caret = Controller->Caret();
	const double Step = Yard->GetMotion().ChannelStep(Caret);
	const double Before = Yard->GetMotion().Value(Caret);

	Controller->Do(EYardAction::ScrubUp);
	TestEqual(TEXT("scrub up moves the caret's channel one step up"),
		Yard->GetMotion().Value(Caret), Before + Step, 1e-9);

	Controller->Do(EYardAction::ScrubDown);
	TestEqual(TEXT("scrub down brings it back"), Yard->GetMotion().Value(Caret), Before, 1e-9);

	// 4. AIRBORNE IS A TOGGLE AND IT TAKES CONTROL, exactly as a scrub does - otherwise the
	// next frame of the demo loop puts it straight back.
	Controller->Do(EYardAction::Reset);
	TestFalse(TEXT("reset leaves it on the ground"), Yard->GetMotion().bAirborne);
	Controller->Do(EYardAction::ToggleConfiguration);
	TestTrue(TEXT("the airborne action lifts it off the wheels"), Yard->GetMotion().bAirborne);
	TestTrue(TEXT("and pauses, so the loop cannot undo it on the next frame"),
		Yard->GetMotion().bPaused);
	Controller->Do(EYardAction::ToggleConfiguration);
	TestFalse(TEXT("and puts it back down"), Yard->GetMotion().bAirborne);

	// 5. RESET PARKS EVERYTHING AND RESUMES. See FYardMotion::Reset for why resuming is part
	// of it rather than a second key.
	Controller->Do(EYardAction::ScrubUp);
	Controller->Do(EYardAction::Reset);
	TestFalse(TEXT("reset resumes the loop"), Yard->GetMotion().bPaused);
	TestEqual(TEXT("reset parks the caret's channel"), Yard->GetMotion().Value(Controller->Caret()), 0.0, 1e-9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardSoloDrivesOneRigTest,
	"AirportMgr.View.AnimYard.SoloDrivesOneRig",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardSoloDrivesOneRigTest::RunTest(const FString& Parameters)
{
	// SOLO IS FOR A CLOSE-PACKED ROW. Everything else freezes AT THE PARKED POSE rather than
	// wherever it happened to be, because a row frozen mid-steering-sweep is a row of wrong
	// answers to compare the soloed rig against.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	USkeletalMesh* Truck = LoadTruckMesh();
	if (Truck == nullptr) { AddError(TEXT("SK_FuelTruck1 did not load")); return false; }

	ASkeletalMeshActor* Near = PlaceRiggedModel(*TestWorld.World, FVector(0.0, 0.0, 0.0), Truck);
	ASkeletalMeshActor* Far = PlaceRiggedModel(*TestWorld.World, FVector(9000.0, 0.0, 0.0), Truck);

	AAnimYard* Yard = SpawnYardDriving(*TestWorld.World, Truck);
	if (Yard == nullptr) { AddError(TEXT("no yard")); return false; }
	Yard->AdoptSubjects();

	ARoadAgentActor* NearAgent = Yard->AgentFor(Near);
	ARoadAgentActor* FarAgent = Yard->AgentFor(Far);
	if (NearAgent == nullptr || FarAgent == nullptr) { AddError(TEXT("both models must be driven")); return false; }

	// 1. WITH NO SOLO, EVERY RIG GETS THE SAME MOTION. That lockstep is the bench's whole
	// method: nine controls to compare the tenth against.
	Yard->Tick(4.0f);
	TestEqual(TEXT("unsoloed, both rigs steer alike"),
		NearAgent->GetMotion().SteerAngleDegrees, FarAgent->GetMotion().SteerAngleDegrees, 1e-9);
	TestTrue(TEXT("and the steering is actually deflected, so the comparison means something"),
		FMath::Abs(NearAgent->GetMotion().SteerAngleDegrees) > 0.0);

	// 2. SOLO DRIVES ONE AND PARKS THE REST.
	Yard->SetSolo(Near);
	Yard->Tick(1.0f);
	TestTrue(TEXT("the soloed rig keeps moving"), FMath::Abs(NearAgent->GetMotion().GroundSpeed) > 0.0);
	TestEqual(TEXT("the rest are parked, not frozen mid-sweep"),
		FarAgent->GetMotion().SteerAngleDegrees, 0.0, 1e-9);
	TestEqual(TEXT("with their wheels stopped"), FarAgent->GetMotion().GroundSpeed, 0.0, 1e-9);

	// 3. AND THE SOLOED RIG STAYS ON ITS OWN MARK. Soloing is a change of what is driven, not
	// of where anything stands.
	TestTrue(TEXT("solo does not move the soloed model"),
		NearAgent->GetActorLocation().Equals(Near->GetActorLocation(), 1e-3));
	TestTrue(TEXT("nor the parked ones"),
		FarAgent->GetActorLocation().Equals(Far->GetActorLocation(), 1e-3));

	// 4. CLEARING IT PUTS THE ROW BACK IN LOCKSTEP.
	Yard->SetSolo(nullptr);
	Yard->Tick(1.0f);
	TestEqual(TEXT("clearing solo returns the row to lockstep"),
		NearAgent->GetMotion().SteerAngleDegrees, FarAgent->GetMotion().SteerAngleDegrees, 1e-9);

	// 5. THE NEAREST SUBJECT TO A POINT IS WHAT THE SOLO KEY PICKS - the controller hands it
	// the camera's focus, and this is the part of that decision that can be measured without
	// a camera.
	TestEqual(TEXT("a point beside the first model picks the first"),
		Yard->NearestSubject(FVector2D(500.0, 0.0)), static_cast<const AActor*>(Near));
	TestEqual(TEXT("a point beside the second picks the second"),
		Yard->NearestSubject(FVector2D(8000.0, 0.0)), static_cast<const AActor*>(Far));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardCameraNeedsNoAirportTest,
	"AirportMgr.View.AnimYard.CameraNeedsNoAirport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardCameraNeedsNoAirportTest::RunTest(const FString& Parameters)
{
	// THE SEAM THE BENCH NEEDED. UBuildCameraComponent took an ARoadNetworkActor for one
	// reason - SurfaceZ, a double - and the model yard has no airport in it at all. The
	// overload that takes the height directly is what lets the bench reuse the game's own
	// camera feel instead of growing a second one; this is the test that fails if it is ever
	// quietly re-coupled.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	AAnimYardController* Controller = TestWorld.World->SpawnActor<AAnimYardController>();
	if (Controller == nullptr) { AddError(TEXT("no controller")); return false; }

	UBuildCameraComponent* Camera = Controller->Camera();
	if (Camera == nullptr) { AddError(TEXT("the bench controller has no camera component")); return false; }

	Camera->CreateBuildCamera(*Controller, /*SurfaceZ*/ 0.0);

	ACameraActor* Spawned = Camera->CameraActorForTest();
	if (Spawned == nullptr)
	{
		AddError(TEXT("no camera actor was spawned, so the yard has nothing to look through"));
		return false;
	}

	// NOT ASSERTED: that the camera became the controller's view target. SetViewTarget goes
	// through APlayerCameraManager, which a bare SpawnActor in a test world never creates - so
	// the assertion would measure the test's own setup rather than this seam. What the seam
	// promises is that a camera is placed above the height it was handed, with no airport
	// involved, and that is what is measured instead.
	TestTrue(TEXT("the camera is placed above the surface height it was given"),
		Spawned->GetActorLocation().Z > 0.0);

	// PANNING MOVES IT. A camera that spawned and then ignored every input would satisfy the
	// assertion above and be useless to look at a row of models with.
	const FVector Before = Spawned->GetActorLocation();
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Camera->UpdateFreeView(1.0f / 60.0f, /*Right*/ 1.0, /*Forward*/ 0.0, /*Turn*/ 0.0,
			/*TurnPixels*/ 0.0, /*SurfaceZ*/ 0.0);
	}
	TestTrue(TEXT("panning right moves the yard camera"),
		!Spawned->GetActorLocation().Equals(Before, 1.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardCameraStartsInFrontOfTheAircraftTest,
	"AirportMgr.View.AnimYard.CameraStartsInFrontOfTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardCameraStartsInFrontOfTheAircraftTest::RunTest(const FString& Parameters)
{
	// WHY THE PLAYERSTART DOES NOT DECIDE THIS, reported from play 2026-09-21: moving it moves
	// nothing. AAnimYardGameMode sets DefaultPawnClass to null, so no pawn is ever spawned at
	// it, and FBuildCameraRig::Reset snaps Focus to the WORLD ORIGIN - between the two rows,
	// looking at neither. The opening view is decided here instead, and it is DERIVED from
	// where the models actually stand rather than typed, so re-running build_model_yard.py with
	// a new model in the row re-aims the camera by itself.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	USkeletalMesh* Truck = LoadTruckMesh();
	if (Truck == nullptr) { AddError(TEXT("SK_FuelTruck1 did not load")); return false; }

	// A ROW OF THREE "AIRCRAFT" ALONG Y AT X = 2500, and one VEHICLE far off on the other side
	// at X = -2500 - the yard's real shape. The vehicle is here to be ignored: it is the thing
	// that drags a naive "centre of everything" framing back to the origin, which is the view
	// being fixed.
	const double RowX = 2500.0;
	ASkeletalMeshActor* Near = PlaceRiggedModel(*TestWorld.World, FVector(RowX, -8000.0, 0.0), Truck);
	ASkeletalMeshActor* Mid = PlaceRiggedModel(*TestWorld.World, FVector(RowX, 0.0, 0.0), Truck);
	ASkeletalMeshActor* Far = PlaceRiggedModel(*TestWorld.World, FVector(RowX, 8000.0, 0.0), Truck);
	ASkeletalMeshActor* Vehicle = PlaceRiggedModel(*TestWorld.World, FVector(-2500.0, 0.0, 0.0), Truck);

	AAnimYard* Yard = TestWorld.World->SpawnActor<AAnimYard>();
	if (Yard == nullptr) { AddError(TEXT("no yard")); return false; }

	// The same mesh dresses both, so the rig's bIsVehicle is what tells them apart - which is
	// exactly what the framing has to read.
	Yard->RigResolver = [Truck, Vehicle](USkeletalMesh* Mesh, FYardRig& Out)
	{
		if (Mesh != Truck) { return false; }
		Out.AnimClass = UAirsideAgentAnim::StaticClass();
		Out.bIsVehicle = false;
		return true;
	};
	Yard->AdoptSubjects();
	Yard->SetSubjectIsVehicleForTest(Vehicle, true);

	AAnimYardController* Controller = TestWorld.World->SpawnActor<AAnimYardController>();
	if (Controller == nullptr) { AddError(TEXT("no controller")); return false; }
	Controller->SetYardForTest(Yard);

	Controller->AimAtTheAircraft();
	UBuildCameraComponent* Camera = Controller->Camera();
	if (Camera == nullptr) { AddError(TEXT("no camera component")); return false; }
	Camera->CreateBuildCamera(*Controller, 0.0);

	ACameraActor* Spawned = Camera->CameraActorForTest();
	if (Spawned == nullptr) { AddError(TEXT("no camera actor")); return false; }

	const FVector Eye = Spawned->GetActorLocation();
	const FRotator Look = Spawned->GetActorRotation();

	// 1. IN FRONT OF THEM. The models are unrotated and UAircraftType's local space is "origin
	// at the NOSE GEAR, +X forward", so the noses point along world +X and "in front" is a
	// GREATER X than the row stands at. Nothing in the yard script rotates a model, so this is
	// a fact about the level rather than an assumption about it.
	TestTrue(*FString::Printf(TEXT("the camera is in front of the row, not behind it "
		"(camera X %.0f against the row's %.0f)"), Eye.X, RowX), Eye.X > RowX);

	// 2. FACING THEM. Looking back along -X is yaw 180.
	TestEqual(TEXT("the camera looks back down the row's nose line"),
		FMath::UnwindDegrees(Look.Yaw), 180.0, 1.0);

	// 3. ABOVE THE FLOOR AND LOOKING DOWN A LITTLE. Not a plan view - the whole point is to see
	// the aircraft from the front - but not at zero either, or the row collapses into one line
	// and the models behind hide the ones in front.
	TestTrue(*FString::Printf(TEXT("the camera is above the floor (%.0f uu)"), Eye.Z), Eye.Z > 0.0);
	TestTrue(*FString::Printf(TEXT("and is looking down, but not steeply (%.1f degrees)"), -Look.Pitch),
		-Look.Pitch > 0.0 && -Look.Pitch < 35.0);

	// 4. EVERY AIRCRAFT IS IN SHOT. This is the assertion that makes the distance mean
	// something: a camera aimed correctly but standing too close frames one aeroplane, and the
	// bench's whole method is comparing the row against itself.
	const double HalfFov = Camera->FieldOfView * 0.5;
	const FVector2D EyeXY(Eye.X, Eye.Y);
	const FVector2D Forward(FMath::Cos(FMath::DegreesToRadians(Look.Yaw)),
		FMath::Sin(FMath::DegreesToRadians(Look.Yaw)));

	for (const ASkeletalMeshActor* Aircraft : { Near, Mid, Far })
	{
		const FVector At = Aircraft->GetActorLocation();
		FVector2D ToModel = FVector2D(At.X, At.Y) - EyeXY;
		ToModel.Normalize();

		const double OffAxis = FMath::RadiansToDegrees(FMath::Acos(
			FMath::Clamp(FVector2D::DotProduct(Forward, ToModel), -1.0, 1.0)));

		TestTrue(*FString::Printf(TEXT("%s is inside the opening shot (%.1f degrees off axis, "
			"half-FOV is %.1f)"), *Aircraft->GetName(), OffAxis, HalfFov), OffAxis < HalfFov);
	}

	// 5. THE VEHICLE ROW DOES NOT PULL THE SHOT. Its mark is 5000 uu the other side of the
	// origin; a framing that averaged every subject would sit the camera between the two rows
	// facing nothing, which is what the world-origin default already did.
	TestTrue(TEXT("the ground-vehicle row does not drag the camera back towards the origin"),
		Eye.X > RowX);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
