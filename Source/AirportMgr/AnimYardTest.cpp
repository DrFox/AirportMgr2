#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimYard.h"
#include "AnimYardMotion.h"

#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Present/AirsideAgentAnim.h"
#include "Present/RoadAgentActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A REAL RIGGED MESH, because ARoadAgentActor::SetAirframe returns early on a null one -
	 * "the cube stands, deliberately" - so a test that adopted mesh-less actors would watch the
	 * whole dressing path being skipped and call it a pass. The fuel truck is the fleet's only
	 * rigged ground vehicle and is in the repository.
	 */
	USkeletalMesh* LoadTruck()
	{
		return Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr,
			TEXT("/Game/Vehicles/FuelTruck1/SK_FuelTruck1")));
	}

	ASkeletalMeshActor* PlaceModel(UWorld& World, const FVector& Where, USkeletalMesh* Mesh)
	{
		ASkeletalMeshActor* Actor = World.SpawnActor<ASkeletalMeshActor>(Where, FRotator::ZeroRotator);
		if (Actor != nullptr && Mesh != nullptr)
		{
			Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
		}
		return Actor;
	}

	/** The catalogue answer for a mesh the bench knows how to drive. */
	void RigTheTruck(FYardRig& Out)
	{
		// UAirsideAgentAnim ITSELF, not an Animation Blueprint built on it. The C++ base drives
		// no bones - there is no graph - but it is a real UAnimInstance of the right type, which
		// is exactly what this test is about: that the class the catalogue named reaches the
		// component. Whether a graph moves the right BONES is a different question, and a
		// different test, against real content.
		Out.AnimClass = UAirsideAgentAnim::StaticClass();
		Out.bIsVehicle = true;
		Out.BoxSizeUu = FVector(900.0, 250.0, 320.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardAdoptsPlacedModelsTest,
	"AirportMgr.View.AnimYard.AdoptsPlacedModels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardAdoptsPlacedModelsTest::RunTest(const FString& Parameters)
{
	// THE ADOPTION SEAM. M_ModelYard is authored by build_model_yard.py and its models are
	// plain ASkeletalMeshActors standing in bind pose; UAirsideAgentAnim only ever runs on an
	// ARoadAgentActor, because NativeUpdateAnimation casts its owner to one and returns early
	// otherwise. Adoption is the whole of how the first becomes the second, and nothing else in
	// the project exercises it.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	USkeletalMesh* Truck = LoadTruck();
	if (Truck == nullptr)
	{
		AddError(TEXT("SK_FuelTruck1 did not load; the yard has no rigged mesh to adopt"));
		return false;
	}

	ASkeletalMeshActor* Rigged = PlaceModel(*TestWorld.World, FVector(0.0, 0.0, 0.0), Truck);
	ASkeletalMeshActor* AlsoRigged = PlaceModel(*TestWorld.World, FVector(1500.0, -300.0, 0.0), Truck);
	ASkeletalMeshActor* Unknown = PlaceModel(*TestWorld.World, FVector(3000.0, 0.0, 0.0), nullptr);

	AAnimYard* Yard = TestWorld.World->SpawnActor<AAnimYard>();
	if (Yard == nullptr) { AddError(TEXT("no yard")); return false; }

	Yard->RigResolver = [Truck](USkeletalMesh* Mesh, FYardRig& Out)
	{
		if (Mesh != Truck) { return false; }
		RigTheTruck(Out);
		return true;
	};

	const int32 Driven = Yard->AdoptSubjects();

	// 1. EVERY MODEL IS A SUBJECT, DRIVEN OR NOT. The one the catalogue cannot place still has
	// to be known about, because the HUD labels it "no anim class" - and a still model that the
	// bench never noticed is indistinguishable from a rig whose graph does nothing.
	TestEqual(TEXT("all three placed models become subjects"), Yard->Subjects().Num(), 3);
	TestEqual(TEXT("the two the catalogue knows are driven"), Driven, 2);

	// 2. A DRIVEN SUBJECT GETS AN AGENT ON ITS OWN MARK. The transform comes from the placed
	// actor rather than being recomputed, so build_model_yard.py stays the one authority on
	// where a model stands - its layout is measured off each mesh's own bounds.
	for (ASkeletalMeshActor* Source : { Rigged, AlsoRigged })
	{
		const ARoadAgentActor* Agent = Yard->AgentFor(Source);
		if (Agent == nullptr)
		{
			AddError(FString::Printf(TEXT("%s was not given an agent"), *Source->GetName()));
			continue;
		}

		TestTrue(*FString::Printf(TEXT("%s's agent stands on its mark"), *Source->GetName()),
			Agent->GetActorLocation().Equals(Source->GetActorLocation(), 1e-3));
		TestTrue(*FString::Printf(TEXT("%s is hidden, so the bind-pose copy is not left standing "
			"inside the animated one"), *Source->GetName()), Source->IsHidden());
	}

	// 3. AN UNKNOWN MODEL IS LEFT ALONE AND LEFT VISIBLE. Hiding it would delete it from the
	// yard; replacing it with a cube would say the content is broken. It stands as it is, and
	// the HUD says why it is not moving.
	TestNull(TEXT("a model with no rig gets no agent"), Yard->AgentFor(Unknown));
	TestFalse(TEXT("a model with no rig stays visible"), Unknown->IsHidden());

	// 4. ADOPTION IS IDEMPOTENT. BeginPlay calls it, and anything that calls it again - a
	// re-scan key, a test - must not stack a second agent on every mark.
	const int32 Again = Yard->AdoptSubjects();
	TestEqual(TEXT("a second adoption drives nothing new"), Again, 0);
	TestEqual(TEXT("and adds no subjects"), Yard->Subjects().Num(), 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardDrivesItsSubjectsTest,
	"AirportMgr.View.AnimYard.DrivesItsSubjects",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardDrivesItsSubjectsTest::RunTest(const FString& Parameters)
{
	// THE OTHER HALF OF THE SEAM: a yard that adopts and then never pushes anything in is a
	// row of agents standing in bind pose, which looks exactly like a rig with no graph.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	USkeletalMesh* Truck = LoadTruck();
	if (Truck == nullptr)
	{
		AddError(TEXT("SK_FuelTruck1 did not load; the yard has no rigged mesh to adopt"));
		return false;
	}

	ASkeletalMeshActor* Source = PlaceModel(*TestWorld.World, FVector::ZeroVector, Truck);
	AAnimYard* Yard = TestWorld.World->SpawnActor<AAnimYard>();
	if (Yard == nullptr) { AddError(TEXT("no yard")); return false; }

	Yard->RigResolver = [Truck](USkeletalMesh* Mesh, FYardRig& Out)
	{
		if (Mesh != Truck) { return false; }
		RigTheTruck(Out);
		return true;
	};
	Yard->AdoptSubjects();

	ARoadAgentActor* Agent = Yard->AgentFor(Source);
	if (Agent == nullptr) { AddError(TEXT("the truck was not adopted")); return false; }

	// 1. A TICK REACHES THE AGENT. Four seconds in the demo loop is the steering sweep, where
	// speed and steer are both non-zero - so one assertion cannot pass by accident on a
	// channel that happens to be zero at the sampled instant.
	Yard->Tick(4.0f);

	const FAgentMotion& Pushed = Agent->GetMotion();
	TestTrue(TEXT("the loop has the wheels turning by the steering sweep"), Pushed.GroundSpeed > 0.0);
	TestTrue(TEXT("and the nosewheel deflected"), FMath::Abs(Pushed.SteerAngleDegrees) > 0.0);

	// 2. WHAT THE AGENT GOT IS WHAT THE YARD'S OWN MOTION SAYS, field for field. This is what
	// fails if a later hand gives the yard a second copy of a channel - the whole argument for
	// FYardMotion being one producer.
	const FAgentMotion Expected = Yard->GetMotion().ToAgentMotion(FGearPerformance());
	TestEqual(TEXT("speed reaches the agent"), Pushed.GroundSpeed, Expected.GroundSpeed, 1e-9);
	TestEqual(TEXT("steer reaches the agent"), Pushed.SteerAngleDegrees, Expected.SteerAngleDegrees, 1e-9);
	TestEqual(TEXT("RPM reaches the agent"), Pushed.EngineRPM, Expected.EngineRPM, 1e-9);

	// 3. EVERY SUBSEQUENT TICK MOVES IT ON. A yard that pushed once and then held would look
	// identical on the first frame and wrong on every one after it.
	const double Before = Pushed.SteerAngleDegrees;
	Yard->Tick(1.0f);
	TestTrue(TEXT("a second tick advances the loop rather than holding the first frame"),
		!FMath::IsNearlyEqual(Agent->GetMotion().SteerAngleDegrees, Before, 1e-6));

	// 4. PAUSED MEANS PAUSED ALL THE WAY DOWN. The pause lives on FYardMotion, and it is the
	// yard's Tick that has to honour it - a Tick that pushed regardless would freeze the HUD's
	// numbers while the models carried on moving.
	Yard->EditMotion().bPaused = true;
	const double Held = Agent->GetMotion().SteerAngleDegrees;
	Yard->Tick(2.0f);
	TestEqual(TEXT("a paused yard pushes the same values"),
		Agent->GetMotion().SteerAngleDegrees, Held, 1e-9);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
