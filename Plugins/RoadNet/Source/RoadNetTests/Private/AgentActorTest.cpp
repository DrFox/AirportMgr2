#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/StaticMesh.h"
#include "Present/RoadAgentActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	const TCHAR* AgentMeshPath = TEXT("/Game/RoadNet/Aircraft/SM_PiperMeridian.SM_PiperMeridian");

	/** Published PA-46-500TP wingspan, 13.110 m, in Unreal units. */
	constexpr double PiperWingspanUU = 1311.0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentActorTest,
	"RoadNet.Present.AgentActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentActorTest::RunTest(const FString& Parameters)
{
	// 1. THE AIRFRAME'S GEOMETRY, asserted against published dimensions rather than against
	//    itself.
	//
	//    The import script checks this too, but only when someone runs it. Here it is a
	//    standing contract: every clearance decision in the sim - whether an aircraft fits a
	//    stand, whether RouteSearch refuses a guideline as TooWide - is measured against the
	//    wingspan, so an airframe that re-exports at a different scale must fail loudly
	//    rather than quietly making all of those numbers wrong.
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, AgentMeshPath);
		if (TestNotNull(TEXT("the Piper airframe asset loads"), Mesh))
		{
			const FBox Bounds = Mesh->GetBoundingBox();
			const double Span   = Bounds.Max.Y - Bounds.Min.Y;
			const double Length = Bounds.Max.X - Bounds.Min.X;

			TestTrue(FString::Printf(
				TEXT("wingspan is the published 13.110 m, measured %.1f uu"), Span),
				FMath::Abs(Span - PiperWingspanUU) < 5.0);

			// Orientation, not just size. Span and length are close enough (1311 vs 916)
			// that a 90-degree error passes any check looking at one of them alone, and
			// the aircraft then taxis sideways down its own wingspan.
			TestTrue(TEXT("the span lies across the aircraft, not along it"), Span > Length);

			// WHICH WAY it faces. A 180-degree error keeps both extents correct and simply
			// taxis backwards. The origin is the main-gear axle, and this airframe's tail
			// reaches further aft (5.32 m) than its nose reaches forward (3.85 m).
			TestTrue(TEXT("the nose is on +X, the shorter reach from the main gear"),
				Bounds.Max.X < -Bounds.Min.X);

			// Wheels on the ground is what lets SetPose place the actor at SurfaceZ with no
			// lift of its own.
			TestTrue(FString::Printf(TEXT("the wheels rest on Z=0, measured %.1f"), Bounds.Min.Z),
				FMath::Abs(Bounds.Min.Z) < 5.0);
		}
	}

	// 2. SetPose puts the aircraft ON the road, not above it.
	//
	//    The cube this replaced had its pivot at its centre and was lifted by half its own
	//    height. The airframe's origin is already on the ground, so that same lift would fly
	//    it a metre over the taxiway - which reads as a physics or Z-fighting problem rather
	//    than as the arithmetic it is.
	{
		ARoadAgentActor* Agent = NewObject<ARoadAgentActor>(GetTransientPackage());
		if (!TestNotNull(TEXT("agent constructed"), Agent))
		{
			return false;
		}

		constexpr double SurfaceZ = 3.0;
		Agent->SetPose(FVector2D(1200.0, -400.0), FMath::DegreesToRadians(30.0), SurfaceZ);

		const FVector At = Agent->GetActorLocation();
		TestEqual(TEXT("the agent stands at the road surface height"), At.Z, SurfaceZ);
		TestEqual(TEXT("and at the position it was given"), At.X, 1200.0);
		TestEqual(TEXT("on both axes"), At.Y, -400.0);

		// Heading is yaw from +X, which is why the airframe had to be imported nose-along-X.
		TestTrue(TEXT("heading becomes yaw"),
			FMath::IsNearlyEqual(Agent->GetActorRotation().Yaw, 30.0, 0.01));
	}

	return true;
}

#endif
