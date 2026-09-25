#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "CoreMinimal.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Model/Vehicle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * utility1 towing fuelTrailer1's content resolves - the sibling of RigContentTest.cpp's
 * AirportMgr.Content.RigResolves, for the drawbar chain rather than the semi-trailer rig (spec
 * 2026-09-24 revision, section 4).
 *
 * IN THE GAME MODULE, beside RigContentTest and PlotKitContentTest - these read /Game assets
 * (DA_AirsideContent, SK_Utility1, SK_FuelTrailer1), which Airside may not reach
 * (Check-Architecture enforces the direction).
 *
 * THE CHAIN HAS TWO LINKS, AND LINK LENGTHS ARE MEASURED, NOT ASSERTED BY NAME (CLAUDE.md,
 * "measured beats typed"; the task-3b brief's own words: "the test must compare link lengths
 * against the skeleton's bone distances"). Every figure is read straight off the IMPORTED
 * skeletons via USkeletalMesh::GetComposedRefPoseMatrix - the full reference-skeleton
 * hierarchy, walked and composed by the engine once and cached, unlike
 * Tools/Python/import_fueltrailer1.py's own unreal.AnimPose route: that one silently DROPS
 * SK_Utility1's 'hitch' bone (measured 2026-09-24 - AnimPose.get_bone_names reports 8 of its 9
 * bones, no error), which is why this C++ test does not reuse that Python mechanism at all and
 * instead uses the API GetComposedRefPoseMatrix's own engine implementation shows composes
 * over EVERY raw bone in FReferenceSkeleton, regardless of skin weighting.
 *
 * HORIZONTAL (X, Y) DISTANCE, NOT THE FULL 3D ONE: FTowLink and VehicleSweep both work in the
 * ROAD PLANE (VehicleSweep.h's own FLink/FLinkPose are FVector2D-based). The towbar's hitch
 * and axle bones sit at the coupling height (Z ~30.8 uu, the train height the whole chain
 * couples at) while the body's own axle (the rear wheels, 'root') is on the ground (Z 0); a 3D
 * distance would fold that vertical separation into the body link's Length, which is not a
 * fact FTowLink represents. import_fueltrailer1.py's own report_tow_chain measures the same
 * way, for the same reason.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUtilityTowContentResolvesTest,
	"AirportMgr.Content.UtilityTowResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
	/** Horizontal (X, Y) distance between two bones' composed reference-pose origins. */
	double HorizontalBoneDistance(const USkeletalMesh& A, FName BoneA, const USkeletalMesh& B, FName BoneB)
	{
		const FVector OriginA = A.GetComposedRefPoseMatrix(BoneA).GetOrigin();
		const FVector OriginB = B.GetComposedRefPoseMatrix(BoneB).GetOrigin();
		return FVector2D(OriginA.X - OriginB.X, OriginA.Y - OriginB.Y).Size();
	}
}

bool FUtilityTowContentResolvesTest::RunTest(const FString& Parameters)
{
	const FResolvedTowView View = UAirsideSettings::ResolveUtilityTowView();

	if (!TestNotNull(TEXT("utility1's mesh (SK_Utility1) resolves"), View.Cab.Mesh.Get()))
	{
		return false;
	}
	TestNotNull(TEXT("utility1's Animation Blueprint (ABP_Utility1) resolves"), View.Cab.AnimClass.Get());

	const FVehicle Utility = UAirsideSettings::ResolveUtilityTowVehicle();
	if (!TestEqual(TEXT("ResolveUtilityTowView names one entry per ResolveUtilityTowVehicle Tow link"),
		View.Links.Num(), Utility.Tow.Num()))
	{
		return false;
	}
	// THIS TEST'S OWN ASSUMPTION, CHECKED RATHER THAN TAKEN ON FAITH (RigContentTest.cpp's own
	// pattern): it reads Links[0] as the towbar and Links[1] as fuelTrailer1 below, which only
	// means what it says while the chain is exactly two links, a bar then a body.
	if (!TestTrue(TEXT("the drawbar chain is TWO links (a towbar, then a body), as authored"),
		Utility.Tow.Num() == 2))
	{
		return false;
	}

	// LINK 0, THE TOWBAR - a BAR (FTowLink::BodyFront and BodyRear both zero), so its resolved
	// view is EMPTY by contract (FResolvedTowView's own comment) rather than merely unset:
	// fuelTrailer1 is ONE skinned asset covering the towbar and the body together, so there is
	// no second mesh for this entry to ever resolve to.
	TestNull(TEXT("Link 0 (the towbar) has no mesh of its own - it is a bar, and fuelTrailer1's "
		"one mesh resolves at Link 1"), View.Links[0].Mesh.Get());
	const FTowLink& TowbarLink = Utility.Tow[0];
	TestTrue(TEXT("Link 0 (towbar) is a bar: BodyFront == BodyRear == 0"),
		TowbarLink.BodyFront == 0.0 && TowbarLink.BodyRear == 0.0);

	// LINK 1, THE BODY - fuelTrailer1's own mesh and ABP.
	const FResolvedAgentView& Trailer = View.Links[1];
	if (!TestNotNull(TEXT("the trailer mesh (fuelTrailer1) resolves at Link 1"), Trailer.Mesh.Get()))
	{
		return false;
	}
	TestNotNull(TEXT("the trailer's Animation Blueprint (ABP_FuelTrailer1) resolves"), Trailer.AnimClass.Get());

	// MEASURED AGAINST THE SKELETON, THE WHOLE POINT OF THIS TEST. utility1's own mesh must
	// resolve too - View.Cab.Mesh, checked above - to measure its 'hitch' bone against
	// Tow[0].HitchX.
	const USkeletalMesh* UtilityMesh = View.Cab.Mesh.Get();
	const USkeletalMesh* TrailerMesh = Trailer.Mesh.Get();
	const FTowLink& BodyLink = Utility.Tow[1];

	// Link 0's HitchX: utility1's OWN 'hitch' bone, along utility1's body from ITS fixed
	// (rear) axle - which IS utility1's origin (SPEC.md: "origin at the rear axle centre"),
	// so the bone's composed X *is* HitchX with no further offset - see FTowLink's own comment
	// ("a drawbar eye behind the rear axle") and ResolveUtilityTowVehicle's.
	const double HitchX = UtilityMesh->GetComposedRefPoseMatrix(TEXT("hitch")).GetOrigin().X;
	TestTrue(*FString::Printf(
			TEXT("Tow[0].HitchX (%.1f uu) matches utility1's 'hitch' bone (%.1f uu) within 1 uu"),
			TowbarLink.HitchX, HitchX),
		FMath::Abs(TowbarLink.HitchX - HitchX) <= 1.0);

	// Link 0's Length: the towbar's own hitch ('tow_eye') to its own axle ('towbar_yaw'),
	// horizontal.
	const double TowbarLength = HorizontalBoneDistance(*TrailerMesh, TEXT("towbar_yaw"), *TrailerMesh, TEXT("tow_eye"));
	TestTrue(*FString::Printf(
			TEXT("Tow[0].Length (%.1f uu) matches |towbar_yaw - tow_eye| (%.1f uu) within 1 uu"),
			TowbarLink.Length, TowbarLength),
		FMath::Abs(TowbarLink.Length - TowbarLength) <= 1.0);

	// Link 1's Length: the body's own hitch (towbar_yaw, the SAME bone Link 0's axle is) to
	// its own axle ('root', the rear wheels' centre - the mesh's own origin), horizontal.
	const double BodyLength = HorizontalBoneDistance(*TrailerMesh, TEXT("towbar_yaw"), *TrailerMesh, TEXT("root"));
	TestTrue(*FString::Printf(
			TEXT("Tow[1].Length (%.1f uu) matches |towbar_yaw - root| (%.1f uu) within 1 uu"),
			BodyLink.Length, BodyLength),
		FMath::Abs(BodyLink.Length - BodyLength) <= 1.0);

	return true;
}

#endif
