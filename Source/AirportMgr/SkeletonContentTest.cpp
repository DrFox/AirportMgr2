#include "Animation/Skeleton.h"
#include "Content/AirsideSettings.h"
#include "CoreMinimal.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SAVED SKELETON HOLDS EVERY BONE ITS MESH HAS - for the towing fleet's four meshes
 * (SK_Utility1, SK_FuelTrailer1, SK_TruckCab1, SK_TankTrailer1), the ones whose coupling bones
 * (hitch, fifth_wheel, tow_eye) the tow chain reads.
 *
 * WHY: a reimport that adds a bone writes it to the mesh AND to its USkeleton, which is a
 * separate package. reimport_utility1.py saved only the mesh, so SK_Utility1_Skeleton stayed on
 * disk without 'hitch' (fixed by hand in e5879b88, 2026-09-25): the editor merged the bone back
 * in on load and asked to save, every session. Tools/Python/airside_import.py's
 * save_mesh_and_skeleton is the fix; this is what goes red if a script stops calling it.
 *
 * TWO WAYS TO BE STALE, both checked. USkeleton::BuildLinkupData MERGES a mesh's missing bones
 * into the skeleton IN MEMORY the first time anything links the two (an anim instance, a
 * component) and marks the skeleton's package dirty. So a bone missing from the reference
 * skeleton is a stale save that nothing has touched yet, and a DIRTY skeleton package in a
 * fresh test process is a stale save that something already patched over in memory. Asking only
 * the first would pass once any earlier test in the run had spawned the utility.
 *
 * IN THE GAME MODULE, beside RigContentTest: it reads /Game assets through the content set,
 * which Airside may not reach.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkeletonHoldsEveryMeshBoneTest,
	"AirportMgr.Content.SkeletonHoldsEveryMeshBone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSkeletonHoldsEveryMeshBoneTest::RunTest(const FString& Parameters)
{
	// THROUGH THE RESOLVERS, never by path: Content/ resolves each asset in one function.
	TArray<const USkeletalMesh*> Meshes;
	for (const FResolvedTowView& View : { UAirsideSettings::ResolveUtilityTowView(), UAirsideSettings::ResolveRigView() })
	{
		Meshes.Add(View.Cab.Mesh.Get());
		for (const FResolvedAgentView& Link : View.Links)
		{
			// The towbar link draws on the trailer's own bone and has no mesh of its own.
			if (Link.Mesh.Get() != nullptr)
			{
				Meshes.Add(Link.Mesh.Get());
			}
		}
	}
	TestEqual(TEXT("four meshes: utility1, fuelTrailer1, truckCab1, tankTrailer1"), Meshes.Num(), 4);

	for (const USkeletalMesh* Mesh : Meshes)
	{
		if (!TestNotNull(TEXT("the mesh resolves"), Mesh)) { continue; }
		const USkeleton* Skeleton = Mesh->GetSkeleton();
		if (!TestNotNull(*FString::Printf(TEXT("%s has a Skeleton"), *Mesh->GetName()), Skeleton)) { continue; }

		const FReferenceSkeleton& MeshBones = Mesh->GetRefSkeleton();
		const FReferenceSkeleton& SkeletonBones = Skeleton->GetReferenceSkeleton();
		TArray<FString> Missing;
		for (int32 Bone = 0; Bone < MeshBones.GetRawBoneNum(); ++Bone)
		{
			const FName Name = MeshBones.GetBoneName(Bone);
			if (SkeletonBones.FindBoneIndex(Name) == INDEX_NONE)
			{
				Missing.Add(Name.ToString());
			}
		}
		TestTrue(*FString::Printf(TEXT("every one of %s's %d bones is in %s's reference skeleton - missing: %s"),
				*Mesh->GetName(), MeshBones.GetRawBoneNum(), *Skeleton->GetName(),
				Missing.Num() > 0 ? *FString::Join(Missing, TEXT(", ")) : TEXT("none")),
			Missing.Num() == 0);
		TestFalse(*FString::Printf(TEXT("%s's package is not dirty - the saved skeleton was not patched from the mesh on load"),
				*Skeleton->GetName()),
			Skeleton->GetPackage()->IsDirty());
	}
	return true;
}

#endif
