#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "CoreMinimal.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Model/Vehicle.h"
#include "Present/AirsideAgentAnim.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The articulated rig's content resolves: both meshes, both Animation Blueprints, and the
 * trailer mesh is the length ResolveRigVehicle's own Tow link says it should be.
 *
 * IN THE GAME MODULE, beside PlotKitContentTest - these read /Game assets (DA_AirsideContent,
 * SK_TruckCab1, SK_TankTrailer1), which Airside may not reach (Check-Architecture enforces the
 * direction).
 *
 * THE LENGTH IS MEASURED, NOT ASSERTED BY NAME (CLAUDE.md, "measured beats typed"):
 * Rig.Tow[0].Length + BodyFront + BodyRear is the geometric length
 * UAirsideSettings::ResolveRigVehicle's own comment records having measured off
 * tankTrailer1.glb (1317.0 uu, 2026-09-24) - comparing it against the IMPORTED mesh's bounds
 * catches a re-export, or a re-typed figure, that drifted from the asset this content set now
 * names, rather than leaving that only to whichever route test happens to exercise the rig.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigContentResolvesTest,
	"AirportMgr.Content.RigResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigContentResolvesTest::RunTest(const FString& Parameters)
{
	const FResolvedTowView View = UAirsideSettings::ResolveRigView();

	if (!TestNotNull(TEXT("the cab mesh (truckCab1) resolves"), View.Cab.Mesh.Get()))
	{
		return false;
	}
	TestNotNull(TEXT("the cab's Animation Blueprint (ABP_TruckCab1) resolves"), View.Cab.AnimClass.Get());

	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	if (!TestEqual(TEXT("ResolveRigView names one entry per ResolveRigVehicle Tow link"),
		View.Links.Num(), Rig.Tow.Num()))
	{
		return false;
	}
	// THIS TEST'S OWN ASSUMPTION, CHECKED RATHER THAN TAKEN ON FAITH: it reads Links[0] as
	// tankTrailer1 below, which only means what it says while the rig has exactly one link.
	if (!TestTrue(TEXT("the rig is a one-link (semi-trailer) chain, as authored"), Rig.Tow.Num() == 1))
	{
		return false;
	}

	const FResolvedAgentView& Trailer = View.Links[0];
	if (!TestNotNull(TEXT("the trailer mesh (tankTrailer1) resolves"), Trailer.Mesh.Get()))
	{
		return false;
	}
	TestNotNull(TEXT("the trailer's Animation Blueprint (ABP_TankTrailer1) resolves"), Trailer.AnimClass.Get());

	// Hitch-to-axle plus how far the body reaches ahead of the hitch and behind the axle - see
	// FTowLink's own comment - is the trailer's overall length.
	const FTowLink& Link = Rig.Tow[0];
	const double ExpectedLengthUu = Link.Length + Link.BodyFront + Link.BodyRear;
	const double MeshLengthUu = Trailer.Mesh->GetBounds().BoxExtent.X * 2.0;
	TestTrue(*FString::Printf(
			TEXT("the trailer mesh's length (%.1f uu) is within 10%% of Tow[0]'s geometry (%.1f uu)"),
			MeshLengthUu, ExpectedLengthUu),
		FMath::Abs(MeshLengthUu - ExpectedLengthUu) <= ExpectedLengthUu * 0.10);

	// THE COUPLING POINT ITSELF, NOT JUST OVERALL LENGTH (final-fix-brief item 1): the check
	// above only confirms the trailer mesh is roughly the right SIZE - two meshes could each
	// pass it while their coupling bones sat anywhere, and the drawn kingpin would not sit on
	// the drawn fifth wheel. MEASURED against the imported skeletons, not asserted by name
	// (CLAUDE.md, "measured beats typed"; UtilityTowContentTest.cpp's own pattern,
	// GetComposedRefPoseMatrix - the full composed reference-skeleton hierarchy).
	const USkeletalMesh* CabMesh = View.Cab.Mesh.Get();
	const USkeletalMesh* TrailerMesh = Trailer.Mesh.Get();

	// truckCab1's OWN 'fifth_wheel' bone (the kingpin SOCKET - see AirsideContent.h's own
	// comment on RigCabMesh) sits Tow[0].HitchX ahead of the cab's rear (fixed) axle, which is
	// the cab mesh's own origin - the same convention ResolveRigVehicle's comment records
	// having measured.
	const double FifthWheelX = CabMesh->GetComposedRefPoseMatrix(TEXT("fifth_wheel")).GetOrigin().X;
	TestTrue(*FString::Printf(
			TEXT("truckCab1's 'fifth_wheel' bone (%.1f uu) matches Tow[0].HitchX (%.1f uu) within 1 uu"),
			FifthWheelX, Link.HitchX),
		FMath::Abs(FifthWheelX - Link.HitchX) <= 1.0);

	// tankTrailer1's OWN 'kingpin' bone sits Tow[0].Length ahead of the trailer mesh's own
	// origin (the tandem axle centre - checked below).
	const double KingpinX = TrailerMesh->GetComposedRefPoseMatrix(TEXT("kingpin")).GetOrigin().X;
	TestTrue(*FString::Printf(
			TEXT("tankTrailer1's 'kingpin' bone (%.1f uu) matches Tow[0].Length (%.1f uu) within 1 uu"),
			KingpinX, Link.Length),
		FMath::Abs(KingpinX - Link.Length) <= 1.0);

	// THE TRAILER MESH'S ORIGIN IS THE TANDEM AXLE CENTRE (import_rig.py's convention, which
	// is what Length is measured FROM above) - checked against the axle bones themselves
	// rather than taken on the import script's say-so: the tandem centre is the horizontal
	// midpoint of axle 1 (wheel_1L/wheel_1R) and axle 2 (wheel_2L/wheel_2R), which must sit at
	// the mesh's local (0, 0) for Link.Length to mean what FTowLink's own comment says it does.
	const FVector Wheel1L = TrailerMesh->GetComposedRefPoseMatrix(TEXT("wheel_1L")).GetOrigin();
	const FVector Wheel1R = TrailerMesh->GetComposedRefPoseMatrix(TEXT("wheel_1R")).GetOrigin();
	const FVector Wheel2L = TrailerMesh->GetComposedRefPoseMatrix(TEXT("wheel_2L")).GetOrigin();
	const FVector Wheel2R = TrailerMesh->GetComposedRefPoseMatrix(TEXT("wheel_2R")).GetOrigin();
	const FVector2D TandemCentre = FVector2D(
		Wheel1L.X + Wheel1R.X + Wheel2L.X + Wheel2R.X,
		Wheel1L.Y + Wheel1R.Y + Wheel2L.Y + Wheel2R.Y) / 4.0;
	TestTrue(*FString::Printf(
			TEXT("tankTrailer1's tandem axle centre (%.1f, %.1f) is at the mesh's own origin - ")
			TEXT("the link Axle - within 1 uu"),
			TandemCentre.X, TandemCentre.Y),
		TandemCentre.Size() <= 1.0);

	return true;
}

/**
 * Every existing ground vehicle's wheel spin changed the moment WheelHubRadius shipped
 * (final-fix-brief item 2): every vehicle used to spin on MainWheelRadius's class default (the
 * Meridian aircraft's own figure), and now spins on its own measured hub instead - correct, but
 * unpinned until now. Pins WheelHubRadius's OUTPUT against each mesh's own hub, MEASURED off
 * the imported skeleton rather than typed twice - see AirsideAgentAnim.h's own comment ("Static
 * so it is testable against a skeleton alone").
 *
 * fueltruck1 (the bowser, ResolveVehicleView) is the visible case: its hub sits well above the
 * ~21 uu fallback, so its wheels now turn about 1.8x SLOWER than before this branch (2.3x on the
 * pre-#279 mesh, whose hub was 47.95) - see the
 * spec's REVISED note (2026-09-24-articulated-rig-forward-design.md section 2, "Animation").
 * truckCab1 is pinned alongside it since it is the other rig mesh WheelHubRadius now drives.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleWheelHubRadiusPinnedTest,
	"AirportMgr.Content.VehicleWheelHubRadiusPinned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleWheelHubRadiusPinnedTest::RunTest(const FString& Parameters)
{
	const FResolvedAgentView FuelTruck = UAirsideSettings::ResolveVehicleView();
	if (!TestNotNull(TEXT("fueltruck1's mesh (SK_FuelTruck1) resolves"), FuelTruck.Mesh.Get()))
	{
		return false;
	}
	const FResolvedTowView Rig = UAirsideSettings::ResolveRigView();
	if (!TestNotNull(TEXT("the cab mesh (SK_TruckCab1) resolves"), Rig.Cab.Mesh.Get()))
	{
		return false;
	}

	// A FALLBACK THAT COULD NEVER PASS AS A REAL HUB: 1.0 uu, so a test that somehow measured
	// nothing (Hub <= 0 on every wheel* bone) fails loudly through WheelHubRadius returning
	// this exact sentinel, rather than a plausible-looking number nobody would question.
	constexpr float ImpossibleFallback = 1.0f;

	// MEASURED 2026-09-25 off the imported skeletons - typed here, once, so a re-export or a
	// change to WheelHubRadius itself is caught rather than only noticed by eye. A wrong guess
	// here fails loudly with WheelHubRadius's actual measured output in the message (below).
	// fueltruck1 RE-PINNED 2026-09-25 (47.95 -> 38.40): #279 moved the bowser onto the rigidCab1
	// model, a new mesh with its own, smaller wheels - the pin caught the swap, as it is meant to.
	constexpr float ExpectedFuelTruck1HubZ = 38.40f;
	constexpr float ExpectedTruckCab1HubZ = 53.80f;

	const float FuelTruck1HubZ =
		UAirsideAgentAnim::WheelHubRadius(FuelTruck.Mesh->GetRefSkeleton(), ImpossibleFallback);
	TestTrue(*FString::Printf(
			TEXT("fueltruck1's measured wheel hub (%.2f uu) matches the pinned figure (%.2f uu) within 0.5 uu"),
			FuelTruck1HubZ, ExpectedFuelTruck1HubZ),
		FMath::Abs(FuelTruck1HubZ - ExpectedFuelTruck1HubZ) <= 0.5f);

	const float TruckCab1HubZ =
		UAirsideAgentAnim::WheelHubRadius(Rig.Cab.Mesh->GetRefSkeleton(), ImpossibleFallback);
	TestTrue(*FString::Printf(
			TEXT("truckCab1's measured wheel hub (%.2f uu) matches the pinned figure (%.2f uu) within 0.5 uu"),
			TruckCab1HubZ, ExpectedTruckCab1HubZ),
		FMath::Abs(TruckCab1HubZ - ExpectedTruckCab1HubZ) <= 0.5f);

	return true;
}

#endif
