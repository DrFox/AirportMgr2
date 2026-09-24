#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "CoreMinimal.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Model/Vehicle.h"

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

	return true;
}

#endif
