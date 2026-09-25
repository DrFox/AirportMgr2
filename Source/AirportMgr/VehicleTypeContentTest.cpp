#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimYardCatalogue.h"
#include "AnimYardMotion.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SkeletalMeshComponent.h"
#include "Content/AirsideSettings.h"
#include "Engine/SkeletalMesh.h"
#include "Entities/VehicleType.h"
#include "Model/Airframe.h"
#include "Present/RoadAgentActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The DA_Vehicle_* assets against the meshes they describe, the dispatch path they shadow and
 * the bench that shows them.
 *
 * IN THE GAME MODULE, NOT AIRSIDE, for AircraftFieldLengthTest.cpp's reason: these load /Game
 * content, and a plugin test that did would point the dependency the wrong way.
 */
namespace
{
	/** Every UVehicleType in the project, loaded. */
	TArray<const UVehicleType*> EveryVehicleType()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();
		// WAITED FOR, as AnimYardCatalogue waits: an unfinished scan answers with nothing, and
		// every loop below would then be green over an empty list.
		Registry.WaitForCompletion();

		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UVehicleType::StaticClass()->GetClassPathName(), Assets);

		TArray<const UVehicleType*> Out;
		for (const FAssetData& Data : Assets)
		{
			if (const UVehicleType* Type = Cast<UVehicleType>(Data.GetAsset()))
			{
				Out.Add(Type);
			}
		}
		return Out;
	}

	/** A bone's reference-pose position on the vehicle, uu, or unset if the rig has no such bone. */
	TOptional<FVector> BoneAt(const USkeletalMesh& Mesh, const TCHAR* Name)
	{
		const FReferenceSkeleton& Ref = Mesh.GetRefSkeleton();
		const TArray<FTransform>& Pose = Ref.GetRefBonePose();
		const int32 Index = Ref.FindBoneIndex(FName(Name));
		if (Index == INDEX_NONE)
		{
			return TOptional<FVector>();
		}
		// UP THE PARENT CHAIN - a front wheel hangs off its steer bone, so its own translation
		// is not where it is on the vehicle. Airside.Content.VehicleFootprintMatchesTheMesh
		// learned this first.
		FTransform At = Pose[Index];
		for (int32 Parent = Ref.GetParentIndex(Index); Parent != INDEX_NONE; Parent = Ref.GetParentIndex(Parent))
		{
			At = At * Pose[Parent];
		}
		return TOptional<FVector>(At.GetLocation());
	}

	/**
	 * The fixed axle's X: the rear axle of a rigid vehicle, the axle group's CENTRE on a
	 * trailer. Read from the wheel bones by the fleet's own naming (wheel_RL, or wheel_<n>L
	 * for a numbered group), which airside_anim.BONE_RULES already relies on.
	 */
	TOptional<double> FixedAxleOf(const USkeletalMesh& Mesh)
	{
		if (const TOptional<FVector> Rear = BoneAt(Mesh, TEXT("wheel_RL")))
		{
			return TOptional<double>(Rear->X);
		}
		double Sum = 0.0;
		int32 Count = 0;
		for (int32 Axle = 1; Axle <= 6; ++Axle)
		{
			if (const TOptional<FVector> Wheel = BoneAt(Mesh, *FString::Printf(TEXT("wheel_%dL"), Axle)))
			{
				Sum += Wheel->X;
				++Count;
			}
		}
		return Count > 0 ? TOptional<double>(Sum / Count) : TOptional<double>();
	}

	/**
	 * Push a motion in and let the graph run on it - AnimYardRigTest's Settle, PREFIXED because
	 * the module is a unity build: two anonymous-namespace Settles in one batch are C2084.
	 */
	void VehicleTypeSettle(ARoadAgentActor& Agent, USkeletalMeshComponent& Component, const FAgentMotion& Motion,
		double Seconds)
	{
		Agent.SetMotion(Motion, 0.0);
		Component.TickAnimation(static_cast<float>(Seconds), /*bNeedsValidRootMotion*/ false);
		Component.RefreshBoneTransforms();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleTypesAreInTheYardTest,
	"AirportMgr.Content.VehicleTypes.EveryTypeIsInTheYard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleTypesAreInTheYardTest::RunTest(const FString& Parameters)
{
	// THE SEAM THIS BRANCH ADDED: AnimYardCatalogue enumerates UVehicleType. A type that is
	// authored but not enumerated stands still in the yard, labelled "undriven", which reads as
	// a vehicle with no graph rather than as a catalogue that never looked.
	const TArray<const UVehicleType*> Types = EveryVehicleType();

	// FOUR ON 2026-09-25 - the fuel bowser, the catering truck, the baggage cart and the
	// curtain trailer. A floor rather than an exact count, so the next type is not a test edit.
	TestTrue(*FString::Printf(TEXT("the project carries the ground vehicle types (%d found)"), Types.Num()),
		Types.Num() >= 4);

	TArray<FYardRigEntry> Rigs;
	AnimYardCatalogue::EveryRig(Rigs);

	for (const UVehicleType* Type : Types)
	{
		const FString Who = Type->GetName();
		TestFalse(*FString::Printf(TEXT("%s has a Code for the inspector"), *Who), Type->Code.IsNone());

		USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		UClass* AnimClass = Type->AnimClass.LoadSynchronous();
		TestNotNull(*FString::Printf(TEXT("%s names a mesh that loads"), *Who), Mesh);
		TestNotNull(*FString::Printf(TEXT("%s names an Animation Blueprint that loads"), *Who), AnimClass);
		if (Mesh == nullptr || AnimClass == nullptr)
		{
			continue;
		}

		const FYardRigEntry* Entry = Rigs.FindByPredicate(
			[Mesh](const FYardRigEntry& Rig) { return Rig.Mesh == Mesh; });
		if (!TestNotNull(*FString::Printf(TEXT("%s's mesh is in the yard catalogue"), *Who), Entry))
		{
			continue;
		}

		// DECLARED BY THE TYPE, not by UAirsideContent. The fuel truck is named by both, and
		// the type is meant to win - see EveryRig. A catalogue that kept the content entry
		// would still drive the truck, so only the declarer tells the two apart.
		TestEqual(*FString::Printf(TEXT("%s's mesh is declared by the type itself"), *Who),
			Entry->DeclaredBy, Type->GetName());
		TestTrue(*FString::Printf(TEXT("%s is catalogued as a vehicle"), *Who), Entry->Rig.bIsVehicle);

		// THE SEAM EveryRigDrivesItsBones SKIPS ON: towed means "does not steer", and nothing
		// else does. A catalogue that dropped the flag would fail the rig test's steer check on
		// the cart; one that set it everywhere would silently stop checking every truck's.
		TestEqual(*FString::Printf(TEXT("%s steers exactly when it is not towed"), *Who),
			Entry->Rig.bSteers, !Type->bTowed);
		TestTrue(*FString::Printf(TEXT("%s is catalogued with its own graph"), *Who), Entry->Rig.AnimClass == AnimClass);
	}

	// ONE SUBJECT PER MESH. Two entries for one mesh would put two agents on one mark.
	TSet<const USkeletalMesh*> Seen;
	for (const FYardRigEntry& Rig : Rigs)
	{
		bool bAlready = false;
		Seen.Add(Rig.Mesh, &bAlready);
		TestFalse(*FString::Printf(TEXT("%s is catalogued once"), *GetNameSafe(Rig.Mesh)), bAlready);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleTypesAxlesMatchTheMeshTest,
	"AirportMgr.Content.VehicleTypes.AxlesMatchTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleTypesAxlesMatchTheMeshTest::RunTest(const FString& Parameters)
{
	// EVERY FIGURE ON A TYPE THAT DESCRIBES ITS MESH IS HELD TO THE MESH. build_vehicle_types.py
	// measures them off the .glb, and the import could still disagree - it resized the fuel
	// truck's vertices and its bones by different amounts once (import_fueltruck.py records
	// it). A wheelbase off by 15% is invisible until a corner, where it crabs.
	const TArray<const UVehicleType*> Types = EveryVehicleType();
	if (Types.Num() == 0)
	{
		AddError(TEXT("no vehicle types - nothing measured"));
		return false;
	}

	int32 Measured = 0;
	for (const UVehicleType* Type : Types)
	{
		const USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		if (Mesh == nullptr)
		{
			continue; // EveryTypeIsInTheYard reports it
		}
		const FString Who = Type->GetName();
		const FVehicle& V = Type->Geometry;

		// A CENTIMETRE, the tolerance every mesh-vs-figure test here uses: both come from one
		// measurement, so anything larger is a step that was not re-run.
		const TOptional<double> Fixed = FixedAxleOf(*Mesh);
		if (TestTrue(*FString::Printf(TEXT("%s's rig has wheel bones to find a fixed axle on"), *Who), Fixed.IsSet()))
		{
			TestEqual(*FString::Printf(TEXT("%s: FixedAxleX is the rig's fixed axle"), *Who),
				V.Chassis.FixedAxleX, Fixed.GetValue(), 1.0);
		}

		const TOptional<FVector> Steer = BoneAt(*Mesh, TEXT("steer_FL"));
		if (Steer.IsSet())
		{
			TestEqual(*FString::Printf(TEXT("%s: SteerAxleX is steer_FL's X"), *Who),
				V.Chassis.SteerAxleX, Steer->X, 1.0);
		}
		else
		{
			// NO STEERED AXLE, NO WHEELBASE. A trailer declares none; one that did would hand
			// the follower a bicycle model for a vehicle that cannot steer.
			TestEqual(*FString::Printf(TEXT("%s has no steer bone, so no steered axle"), *Who),
				V.Chassis.SteerAxleX, V.Chassis.FixedAxleX, 1.0);
		}

		// THE BODY'S ENDS ARE THE MESH'S ENDS. Width is allowed to be LESS than the mesh's,
		// never more: FVehicle::BodyWidth excludes wing mirrors, which overhang a kerb legally.
		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		const double Front = Bounds.Origin.X + Bounds.BoxExtent.X;
		const double Rear = Bounds.Origin.X - Bounds.BoxExtent.X;
		TestEqual(*FString::Printf(TEXT("%s: BodyFrontX is the mesh's front"), *Who), V.BodyFrontX, Front, 1.0);
		TestEqual(*FString::Printf(TEXT("%s: BodyRearX is the mesh's rear"), *Who), V.BodyRearX, Rear, 1.0);
		TestTrue(*FString::Printf(TEXT("%s: BodyWidth %.1f is measured and within the mesh's %.1f"),
			*Who, V.BodyWidth, Bounds.BoxExtent.Y * 2.0),
			V.BodyWidth > 0.0 && V.BodyWidth <= Bounds.BoxExtent.Y * 2.0 + 1.0);
		++Measured;
	}

	TestTrue(TEXT("at least one type was measured"), Measured > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleTypesResolveViewWithNoBranchTest,
	"AirportMgr.Content.VehicleTypes.ResolveViewNamesNoBranch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleTypesResolveViewWithNoBranchTest::RunTest(const FString& Parameters)
{
	// THE PIN FOR #308: UAirsideSettings::ResolveVehicleViewFor used to be a TypeCode ladder
	// naming two HARD-CODED vehicles (RIG, UTILITY); every other FVehicle - every UVehicleType
	// asset here - fell through it to the rigid default and never wore its own mesh unless it
	// happened to be the one fuel truck the ladder's fallback also drew. This asks
	// ResolveVehicleViewFor for every authored type's own Vehicle() and checks it resolved
	// THAT type's own mesh, with no TypeCode, no asset name and no branch anywhere on the path -
	// the shape that would go red if the ladder ever came back for a fifth vehicle.
	const TArray<const UVehicleType*> Types = EveryVehicleType();
	if (!TestTrue(TEXT("at least one vehicle type to check"), Types.Num() > 0))
	{
		return false;
	}

	int32 Checked = 0;
	for (const UVehicleType* Type : Types)
	{
		USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		UClass* AnimClass = Type->AnimClass.LoadSynchronous();
		if (Mesh == nullptr)
		{
			continue; // EveryTypeIsInTheYard already reports a type with no mesh
		}
		const FString Who = Type->GetName();

		const FVehicle Vehicle = Type->Vehicle();
		if (!TestEqual(*FString::Printf(TEXT("%s: Vehicle() carries its own Mesh"), *Who),
			Vehicle.Mesh.LoadSynchronous(), Mesh))
		{
			continue;
		}

		const FResolvedTowView View = UAirsideSettings::ResolveVehicleViewFor(Vehicle);
		TestEqual(*FString::Printf(TEXT("%s: resolves its own mesh"), *Who), View.Cab.Mesh.Get(), Mesh);
		TestTrue(*FString::Printf(TEXT("%s: resolves its own Animation Blueprint"), *Who),
			View.Cab.AnimClass == AnimClass);
		++Checked;
	}
	TestTrue(TEXT("at least one type was actually checked"), Checked > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleTypesFuelTruckAgreesWithDispatchTest,
	"AirportMgr.Content.VehicleTypes.FuelTruckAgreesWithDispatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleTypesFuelTruckAgreesWithDispatchTest::RunTest(const FString& Parameters)
{
	// TWO STATEMENTS OF ONE TRUCK UNTIL M3 MOVES DISPATCH ONTO THE TYPE. ResolveDefaultVehicle
	// types the dispatched truck's figures in C++ beside their reasons, and DA_Vehicle_FuelTruck1
	// states them again so the fleet has one shape. UVehicleType's header names the gap; this is
	// what stops the two drifting while it is open. Delete it the day ResolveDefaultVehicle
	// reads the type.
	const FResolvedAgentView View = UAirsideSettings::ResolveVehicleView();
	if (View.Mesh == nullptr)
	{
		AddInfo(TEXT("no rigged vehicle configured; nothing to agree with"));
		return true;
	}

	const UVehicleType* Truck = nullptr;
	for (const UVehicleType* Type : EveryVehicleType())
	{
		if (Type->Mesh.LoadSynchronous() == View.Mesh)
		{
			Truck = Type;
		}
	}
	if (!TestNotNull(TEXT("a vehicle type describes the dispatched truck's mesh"), Truck))
	{
		return false;
	}

	const FVehicle Typed = Truck->Vehicle();
	const FVehicle Dispatched = UAirsideSettings::ResolveDefaultVehicle();
	TestEqual(TEXT("TypeCode"), Typed.TypeCode, Dispatched.TypeCode);
	TestEqual(TEXT("SteerAxleX"), Typed.Chassis.SteerAxleX, Dispatched.Chassis.SteerAxleX, 0.1);
	TestEqual(TEXT("FixedAxleX"), Typed.Chassis.FixedAxleX, Dispatched.Chassis.FixedAxleX, 0.1);
	TestTrue(TEXT("SteerLaw"), Typed.Chassis.SteerLaw == Dispatched.Chassis.SteerLaw);
	TestEqual(TEXT("MaxSteerDegrees"), Typed.Chassis.Ground.MaxSteerDegrees, Dispatched.Chassis.Ground.MaxSteerDegrees, 0.01);
	TestEqual(TEXT("BodyWidth"), Typed.BodyWidth, Dispatched.BodyWidth, 0.1);
	TestEqual(TEXT("BodyFrontX"), Typed.BodyFrontX, Dispatched.BodyFrontX, 0.1);
	TestEqual(TEXT("BodyRearX"), Typed.BodyRearX, Dispatched.BodyRearX, 0.1);
	TestTrue(TEXT("the type names the dispatched truck's graph too"),
		Truck->AnimClass.LoadSynchronous() == View.AnimClass);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleTypesWorkingPartsMoveTest,
	"AirportMgr.Content.VehicleTypes.WorkingPartsMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleTypesWorkingPartsMoveTest::RunTest(const FString& Parameters)
{
	// EveryRigDrivesItsBones ASKS ONLY ABOUT WHEELS AND STEERING, so a catering truck whose
	// graph rolled its wheels and never lifted its box would pass it. This asks about the
	// working parts, and asks in the direction each must go - the gear test's lesson that "did
	// it move" passes a mirrored rig.
	//
	// KEYED ON BONE NAMES THE RIG ALREADY DECLARES (box_lift, platform, tow_eye, beacon), not
	// on a table of which asset has what: a rig that has the bone is checked, and one that
	// renamed it fails the counts at the bottom rather than passing quietly.
	FAirsideTestWorld TestWorld(/*bSpawnActor*/ false);
	if (TestWorld.World == nullptr) { AddError(TEXT("no test world")); return false; }

	int32 Lifts = 0, Platforms = 0, Towbars = 0, Beacons = 0;

	for (const UVehicleType* Type : EveryVehicleType())
	{
		USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		UClass* AnimClass = Type->AnimClass.LoadSynchronous();
		if (Mesh == nullptr || AnimClass == nullptr)
		{
			continue;
		}
		const FString Who = Type->GetName();
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const auto IndexOf = [&Ref](const TCHAR* Name) { return Ref.FindBoneIndex(FName(Name)); };

		ARoadAgentActor* Agent = TestWorld.World->SpawnActor<ARoadAgentActor>();
		if (Agent == nullptr) { AddError(TEXT("no agent")); return false; }
		Agent->SetVehicleAirframe(Mesh, AnimClass, FVector(600.0, 250.0, 300.0));
		USkeletalMeshComponent* Component = Agent->FindComponentByClass<USkeletalMeshComponent>();
		if (Component == nullptr) { AddError(FString::Printf(TEXT("%s: no component"), *Who)); Agent->Destroy(); continue; }
		Component->InitAnim(/*bForceReinit*/ true);

		FYardMotion Bench;
		Bench.Reset();
		const auto PoseAt = [&](double Body, double Seconds)
		{
			Bench.BodyFraction = Body;
			VehicleTypeSettle(*Agent, *Component, Bench.ToAgentMotion(FGearPerformance()), Seconds);
			return Component->GetComponentSpaceTransforms();
		};

		const TArray<FTransform> Stowed = PoseAt(0.0, 1.0 / 60.0);
		const TArray<FTransform> Lifted = PoseAt(FYardMotion::BodyLiftShare, 1.0 / 60.0);
		const TArray<FTransform> Out = PoseAt(1.0, 1.0 / 60.0);

		// THE BOX GOES UP: Lift clip, 1.10 -> 3.80 m on box_lift. Two metres is well short of it
		// and far past anything a mis-bound clip would produce by accident.
		const int32 Box = IndexOf(TEXT("box_lift"));
		if (Box != INDEX_NONE && Box < Out.Num())
		{
			++Lifts;
			const double Rose = Lifted[Box].GetLocation().Z - Stowed[Box].GetLocation().Z;
			TestTrue(*FString::Printf(TEXT("%s: the box rises %.0f uu at full lift - wanted over 200; "
				"zero means the Lift clip is not evaluated at LiftClipTimeSeconds"), *Who, Rose), Rose > 200.0);
		}

		// THE PLATFORM RUNS FORWARD, with the box already up: +180 uu along +X.
		const int32 Platform = IndexOf(TEXT("platform"));
		if (Platform != INDEX_NONE && Platform < Out.Num())
		{
			++Platforms;
			const double Ran = Out[Platform].GetLocation().X - Lifted[Platform].GetLocation().X;
			TestTrue(*FString::Printf(TEXT("%s: the platform runs %.0f uu FORWARD - wanted 150 to 200; "
				"negative is a mirrored axis, zero an unwired node"), *Who, Ran), Ran > 150.0 && Ran < 200.0);
			const double Sank = Out[Platform].GetLocation().Z - Lifted[Platform].GetLocation().Z;
			TestTrue(*FString::Printf(TEXT("%s: the platform stays level as it runs out (%.1f uu on Z)"), *Who, Sank),
				FMath::Abs(Sank) < 5.0);
		}

		// THE TOWBAR LIFTS ITS EYE. Raised 70 degrees about the pivot 0.89 m behind the eye,
		// the eye climbs about 84 cm; the wrong sign would drive it into the ground.
		const int32 Eye = IndexOf(TEXT("tow_eye"));
		if (Eye != INDEX_NONE && Eye < Out.Num())
		{
			++Towbars;
			const double Rose = Out[Eye].GetLocation().Z - Stowed[Eye].GetLocation().Z;
			TestTrue(*FString::Printf(TEXT("%s: the tow eye rises %.0f uu as the towbar stows - "
				"wanted over 50; negative is the towbar driven into the ground"), *Who, Rose), Rose > 50.0);
		}

		// THE BEACON TURNS WITH NOTHING ELSE MOVING. A quarter second at rest.
		const int32 Beacon = IndexOf(TEXT("beacon"));
		if (Beacon != INDEX_NONE && Beacon < Out.Num())
		{
			++Beacons;
			const TArray<FTransform> Before = PoseAt(0.0, 1.0 / 60.0);
			const TArray<FTransform> After = PoseAt(0.0, 0.25);
			const double Turned = FMath::RadiansToDegrees(
				Before[Beacon].GetRotation().AngularDistance(After[Beacon].GetRotation()));
			TestTrue(*FString::Printf(TEXT("%s: the beacon turns %.1f degrees in a quarter second "
				"parked - wanted over 30"), *Who, Turned), Turned > 30.0);
		}

		Agent->Destroy();
	}

	// EACH PART WAS FOUND ON SOME RIG - otherwise the checks above measured nothing.
	TestTrue(TEXT("some rig has a box lift (catering1)"), Lifts > 0);
	TestTrue(TEXT("some rig has a platform (catering1)"), Platforms > 0);
	TestTrue(TEXT("some rig has a towbar eye (baggageCart1)"), Towbars > 0);
	TestTrue(TEXT("some rig has a beacon (fueltruck1, catering1)"), Beacons > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
