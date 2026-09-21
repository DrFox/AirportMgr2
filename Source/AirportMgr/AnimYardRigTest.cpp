#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimYardCatalogue.h"
#include "AnimYardMotion.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Model/AgentMotion.h"
#include "Present/RoadAgentActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Every bone's component-space transform, as the graph last left it. */
	TArray<FTransform> PoseOf(const USkeletalMeshComponent& Component)
	{
		return Component.GetComponentSpaceTransforms();
	}

	/**
	 * Which bones changed between two poses, by index.
	 *
	 * THE WHOLE TRANSFORM, ROTATION INCLUDED, and the first form of this compared POSITIONS
	 * alone under a comment claiming "every driven bone in this fleet has geometry hanging off
	 * it". That was a guess, and measuring refuted it: every rig failed the wheel check,
	 * including SK_FuelTruck1, whose graph is known good. A WHEEL BONE IS A LEAF THAT SPINS
	 * ABOUT ITS OWN AXIS - its origin does not move a millimetre, and there is no child bone
	 * below it whose origin could. Only plane2's steer check passed, because its steer bone
	 * carries the roll bone offset along the strut.
	 *
	 * Rotation would pass on a graph that drove the ROOT, since that turns every bone - but
	 * that shows up as one enormous moved set rather than as a false negative, and the
	 * different-sets assertion below is what would catch it.
	 */
	TSet<int32> MovedBones(const TArray<FTransform>& Before, const TArray<FTransform>& After)
	{
		TSet<int32> Moved;
		const int32 Count = FMath::Min(Before.Num(), After.Num());
		for (int32 Bone = 0; Bone < Count; ++Bone)
		{
			// Bone maths is float, so an untouched bone does not come back bit-identical across
			// two evaluations; a driven one turns by degrees.
			if (!Before[Bone].Equals(After[Bone], 1e-3))
			{
				Moved.Add(Bone);
			}
		}
		return Moved;
	}

	/** Push a motion in and let the graph run on it. */
	void Settle(ARoadAgentActor& Agent, USkeletalMeshComponent& Component, const FAgentMotion& Motion,
		double Seconds)
	{
		Agent.SetMotion(Motion, 0.0);
		Component.TickAnimation(static_cast<float>(Seconds), /*bNeedsValidRootMotion*/ false);
		Component.RefreshBoneTransforms();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardEveryRigDrivesItsBonesTest,
	"AirportMgr.View.AnimYard.EveryRigDrivesItsBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardEveryRigDrivesItsBonesTest::RunTest(const FString& Parameters)
{
	// THE UNWIRED BONE. An Animation Blueprint built on UAirsideAgentAnim can be perfectly
	// valid, compile clean, load clean and drive NOTHING: the Transform (Modify) Bone nodes
	// are hand-wired in the editor - build_fueltruck_anim.py says so in as many words, "the
	// Transform (Modify) Bone nodes are a few minutes in the editor against the bone names
	// this script prints" - and a graph with the node missing, or with its Rotation pin left
	// unconnected, is a model that stands in bind pose while every number the bench prints is
	// correct. Nothing else in this project would notice.
	//
	// IT ASKS "DID ANYTHING MOVE", NOT "DID gear_nose MOVE". A table of expected bone names
	// per rig would be a second declaration of what each graph does - authored by hand, from
	// the same .glb the graph was wired against, and wrong the first time a rig is re-exported.
	// What can be asserted without one is sharper than it sounds: a rig that ignores a channel
	// moves no bone at all on it, which is exactly the failure.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	TArray<FYardRigEntry> Rigs;
	AnimYardCatalogue::EveryRig(Rigs);

	// THE CATALOGUE BEING EMPTY IS A FAILURE, not an empty pass. An asset registry that had
	// not finished scanning would hand back nothing, and a loop over nothing is green - the
	// "a green test may measure nothing" case, in the form most likely to occur here.
	if (Rigs.Num() == 0)
	{
		AddError(TEXT("the catalogue found no drivable rigs at all - either the content moved "
			"or the asset registry answered before it had scanned"));
		return false;
	}

	AddInfo(FString::Printf(TEXT("%d rig(s) to check"), Rigs.Num()));

	for (const FYardRigEntry& Entry : Rigs)
	{
		if (Entry.Mesh == nullptr || Entry.Rig.AnimClass == nullptr)
		{
			continue;
		}

		const FString Who = FString::Printf(TEXT("%s (%s)"), *Entry.Mesh->GetName(), *Entry.DeclaredBy);

		// THROUGH ARoadAgentActor, not a bare component. UAirsideAgentAnim reads its values off
		// the owning agent and returns early on anything else, so a component parented to
		// nothing would hold every value at its default and this test would measure the bind
		// pose against itself.
		ARoadAgentActor* Agent = TestWorld.World->SpawnActor<ARoadAgentActor>();
		if (Agent == nullptr) { AddError(TEXT("no agent")); return false; }

		if (Entry.Rig.bIsVehicle)
		{
			Agent->SetVehicleAirframe(Entry.Mesh, Entry.Rig.AnimClass, FVector(600.0, 250.0, 300.0));
		}
		else
		{
			Agent->SetAirframe(Entry.Mesh, Entry.Rig.AnimClass);
		}

		USkeletalMeshComponent* Component = Agent->FindComponentByClass<USkeletalMeshComponent>();
		if (Component == nullptr || Component->GetSkeletalMeshAsset() == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: the agent was not dressed with the mesh"), *Who));
			continue;
		}

		// THE GRAPH HAS TO BE LIVE before any of this means anything: an anim instance is
		// created when the mesh and class are both set, and InitAnim is what makes the
		// component evaluate one.
		Component->InitAnim(/*bForceReinit*/ true);
		if (Component->GetAnimInstance() == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: no anim instance was created, so the Animation "
				"Blueprint is not driving this mesh at all"), *Who));
			continue;
		}

		FYardMotion Bench;
		Bench.Reset();

		// 1. THE RESTING POSE, which is what the yard shows a parked model in.
		Settle(*Agent, *Component, Bench.ToAgentMotion(Entry.Rig.Gear), 1.0 / 60.0);
		const TArray<FTransform> Parked = PoseOf(*Component);
		if (Parked.Num() == 0)
		{
			AddError(FString::Printf(TEXT("%s: the component evaluated no bones"), *Who));
			continue;
		}

		// 2. THE WHEELS. Half a second at taxi speed is several turns of a 0.2 m wheel, so
		// anything hanging off a wheel bone is somewhere else entirely by the end of it.
		Bench.Reset();
		Bench.GroundSpeed = Bench.TaxiSpeed;
		Settle(*Agent, *Component, Bench.ToAgentMotion(Entry.Rig.Gear), 0.5);
		const TSet<int32> ByWheels = MovedBones(Parked, PoseOf(*Component));

		TestTrue(*FString::Printf(TEXT("%s: rolling the wheels moves at least one bone - if this "
			"is red the graph is not applying WheelAngleDegrees to anything"), *Who),
			ByWheels.Num() > 0);

		// 3. THE STEERING, from the parked pose again so the two are measured against one datum.
		Bench.Reset();
		Bench.SteerDegrees = Bench.MaxSteerDegrees;
		Settle(*Agent, *Component, Bench.ToAgentMotion(Entry.Rig.Gear), 1.0 / 60.0);
		const TSet<int32> BySteer = MovedBones(Parked, PoseOf(*Component));

		TestTrue(*FString::Printf(TEXT("%s: steering moves at least one bone - if this is red the "
			"graph is not applying SteerAngleDegrees to anything"), *Who),
			BySteer.Num() > 0);

		// 4. THE TWO ARE NOT THE SAME BONE. build_fueltruck_anim.py's own warning: the steer
		// bones are the PARENTS of the front roll bones, "wire the steer node before the wheel
		// node it carries, and never the two onto one bone" - and a single Transform (Modify)
		// Bone applies its rotations in a fixed order, so a shared bone wobbles instead of
		// steering. Identical moved-bone sets is what that mistake looks like from outside.
		//
		// A SUBSET IS EXPECTED AND FINE: the steered wheel's own children move under both.
		// EQUAL sets are the failure, because then nothing moved under one that did not move
		// under the other, and the two channels are driving exactly the same thing.
		if (ByWheels.Num() > 0 && BySteer.Num() > 0)
		{
			TestFalse(*FString::Printf(TEXT("%s: the wheels and the steering move DIFFERENT bones "
				"- identical sets means both channels are wired to one bone"), *Who),
				ByWheels.Difference(BySteer).IsEmpty() && BySteer.Difference(ByWheels).IsEmpty());
		}

		Agent->Destroy();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
