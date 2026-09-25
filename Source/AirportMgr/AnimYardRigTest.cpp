#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimYardCatalogue.h"
#include "AnimYardMotion.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Model/Airframe.h"
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

		// 3. THE STEERING - unless the rig is towed and so has no steering of its own to drive
		// (FYardRig::bSteers). Skipped BY NAME in the log, so a rig that stopped steering by
		// accident cannot hide here: only a UVehicleType marked bTowed takes this branch.
		if (!Entry.Rig.bSteers)
		{
			AddInfo(FString::Printf(TEXT("%s: towed - its steering follows the tow, not "
				"SteerAngleDegrees, so the steer checks do not apply"), *Who));
			Agent->Destroy();
			continue;
		}

		// From the parked pose again so the two are measured against one datum.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimYardGearFoldsIntoTheAirframeTest,
	"AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnimYardGearFoldsIntoTheAirframeTest::RunTest(const FString& Parameters)
{
	// EVERY RIG IN THE FLEET FOLDED ITS GEAR OUTBOARD, THROUGH THE WING, AND SHIPPED.
	//
	// Blender is right-handed and UE is left-handed, so the glTF import MIRRORS every
	// bone-local rotation: the model's positive is the graph's negative. The ROLLING bones
	// have always carried a -1 for it - "the rigs turn the other way about their own axis" -
	// and the TRAVELLING bones never did, because until the animation bench there was nothing
	// in the editor that drove a retract where a human could look. plane4 had been wrong since
	// 2026-09-19, plane5 and plane7 since they were written, and plane6's 777 is what showed
	// it. Fixed in Tools/wire_plane<N>_anim.py; pinned here.
	//
	// EveryRigDrivesItsBones ABOVE CANNOT CATCH THIS and says so in its own terms: it asks
	// "did anything move", deliberately, because a table of expected bone names would be a
	// second declaration of each graph. A leg swinging the wrong way moves exactly as many
	// bones as one swinging the right way.
	//
	// "A RETRACTING WHEEL GOES UP" WAS THE FIRST DRAFT OF THIS TEST AND IT MEASURED NOTHING.
	// Measured before it was written: on every rig in the fleet the wheel rises at BOTH signs,
	// because the leg pivots about a trunnion above and outboard of it and either rotation
	// lifts it. plane6's left wheel rises 274 uu the wrong way and 494 uu the right way. A
	// test asserting "higher than it started" would have been green on the very defect it was
	// written for - which is the a-green-test-may-measure-nothing failure, caught here only
	// because the probe was run against both signs before the assertion was chosen.
	//
	// WHAT DISCRIMINATES IS SIDEWAYS: a main leg that folds the wrong way swings OUTBOARD,
	// away from the fuselage and through the wing it is supposed to retract into. plane6 goes
	// 494 uu outboard, plane4 116 uu, plane7 72 uu, against a few uu of noise. So the rule is
	// that no wheel ends a retraction FARTHER from the centreline than it began - which needs
	// no per-rig table, because it is a fact about undercarriages rather than about models.
	//
	// IT DOES NOT DISCRIMINATE A FORE-AFT LEG, AND plane5 IS ONE. A King Air's mains fold
	// FORWARD into the nacelles, so both signs leave |Y| unchanged and plane5 passes this
	// either way. That is named rather than papered over: a forward/aft flip on such a rig is
	// still uncaught, and closing it would mean each rig declaring which way its bay faces -
	// the second declaration EveryRigDrivesItsBones above refuses to write, for reasons that
	// apply here unchanged. Three of the four retractable rigs are covered, including all
	// three that were wrong.
	//
	// A RIG WHOSE GEAR REALLY DOES RETRACT OUTBOARD would fail this, and should: no aeroplane
	// in this fleet does, and the day one arrives the right move is to make that a declared
	// property of the rig rather than to widen the tolerance here.
	//
	// The rise is asserted too, but as a SANITY bound rather than as the discriminator - a
	// leg that has not moved at all fails it, and that is a different fault from a mirrored
	// one.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	TArray<FYardRigEntry> Rigs;
	AnimYardCatalogue::EveryRig(Rigs);
	if (Rigs.Num() == 0)
	{
		AddError(TEXT("the catalogue found no drivable rigs at all"));
		return false;
	}

	// A COUNT, BECAUSE MOST OF THE CATALOGUE HAS NO GEAR. Three ground vehicles and two
	// fixed-gear aeroplanes are skipped by FGearPerformance::IsSet, and a loop that skipped
	// EVERY row would be green while measuring nothing - the failure a-green-test-may-measure
	// -nothing describes, in the form most available here.
	int32 Retractable = 0;

	for (const FYardRigEntry& Entry : Rigs)
	{
		if (Entry.Mesh == nullptr || Entry.Rig.AnimClass == nullptr || !Entry.Rig.Gear.IsSet())
		{
			continue;
		}
		++Retractable;

		const FString Who = FString::Printf(TEXT("%s (%s)"), *Entry.Mesh->GetName(), *Entry.DeclaredBy);

		ARoadAgentActor* Agent = TestWorld.World->SpawnActor<ARoadAgentActor>();
		if (Agent == nullptr) { AddError(TEXT("no agent")); return false; }
		Agent->SetAirframe(Entry.Mesh, Entry.Rig.AnimClass);

		USkeletalMeshComponent* Component = Agent->FindComponentByClass<USkeletalMeshComponent>();
		if (Component == nullptr || Component->GetSkeletalMeshAsset() == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: the agent was not dressed with the mesh"), *Who));
			Agent->Destroy();
			continue;
		}
		Component->InitAnim(/*bForceReinit*/ true);
		if (Component->GetAnimInstance() == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: no anim instance"), *Who));
			Agent->Destroy();
			continue;
		}

		FYardMotion Bench;
		Bench.Reset();
		Bench.bAirborne = true;
		Settle(*Agent, *Component, Bench.ToAgentMotion(Entry.Rig.Gear), 1.0 / 60.0);
		const TArray<FTransform> Down = PoseOf(*Component);

		// FULLY STOWED, not part way. FYardMotion::GearCycleFraction is 0 down and locked,
		// 1 stowed, and it walks FGearPerformance::FractionsAt - the one evaluator - so this
		// is the same curve the game flies rather than a straight line the test invented.
		Bench.GearCycleFraction = 1.0;
		Settle(*Agent, *Component, Bench.ToAgentMotion(Entry.Rig.Gear), 1.0 / 60.0);
		const TArray<FTransform> Stowed = PoseOf(*Component);

		// THE WHEEL BONES, BY NAME, AND THE NAMES ARE NOT A PER-RIG TABLE. Every rig in this
		// fleet names its rolling bones wheel_* and nosewheel, which airside_anim.BONE_RULES
		// already relies on to decide what drives them - so reading the same convention here
		// adds no declaration that the pipeline did not already make. A rig that renamed them
		// would come out with zero wheels and fail the guard below rather than pass quietly.
		const FReferenceSkeleton& Rig = Entry.Mesh->GetRefSkeleton();
		int32 Checked = 0;
		for (int32 Bone = 0; Bone < Rig.GetNum() && Bone < Stowed.Num(); ++Bone)
		{
			const FString Name = Rig.GetBoneName(Bone).ToString();
			if (!Name.StartsWith(TEXT("wheel_")) && Name != TEXT("nosewheel"))
			{
				continue;
			}
			++Checked;

			const FVector Before = Down[Bone].GetTranslation();
			const FVector After = Stowed[Bone].GetTranslation();
			const double Rose = After.Z - Before.Z;
			const double Outboard = FMath::Abs(After.Y) - FMath::Abs(Before.Y);

			// THE DISCRIMINATOR. 10 uu of slack, against the 72 uu that the least-wrong rig
			// in the fleet moved - three quarters of an order of magnitude, so this is not a
			// figure anyone has to tune.
			TestTrue(*FString::Printf(
				TEXT("%s: %s does not swing OUTBOARD as the gear stows - it moved %+.0f uu "
					"away from the centreline, which is a leg folding out through the wing "
					"rather than up into it. That is the mirrored-rotation sign error: the "
					"bone wants a -1 in Tools/wire_%s_anim.py's PLAN."),
				*Who, *Name, Outboard, *Entry.Mesh->GetName().Mid(3).ToLower()),
				Outboard < 10.0);

			// AND THE SANITY BOUND. Half the wheel's own hub height: a leg that has genuinely
			// travelled has climbed most of its own length, while a graph that drove nothing
			// leaves this at zero. Not the discriminator - see the header - because both signs
			// clear it comfortably.
			const double Floor = Before.Z * 0.5;
			TestTrue(*FString::Printf(
				TEXT("%s: %s rises when the gear stows - it moved %+.0f uu on Z, needed more "
					"than %.0f. Zero here means the graph applied no gear angle at all."),
				*Who, *Name, Rose, Floor), Rose > Floor);
		}

		TestTrue(*FString::Printf(TEXT("%s: its rig has wheel bones to measure - none found, so "
			"nothing above was checked"), *Who), Checked > 0);
		AddInfo(FString::Printf(TEXT("%s: %d wheel bone(s) checked"), *Who, Checked));

		Agent->Destroy();
	}

	TestTrue(TEXT("at least one retractable-gear aircraft was found - if this is red the "
		"catalogue or FGearPerformance::IsSet changed and this test measured nothing"),
		Retractable > 0);
	AddInfo(FString::Printf(TEXT("%d retractable-gear rig(s) of %d in the catalogue"),
		Retractable, Rigs.Num()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
