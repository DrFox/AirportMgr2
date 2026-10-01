#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/SkeletalMesh.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "AirsideTestFixtures.h"
#include "Entities/AircraftType.h"
#include "Model/RoadEntity.h"
#include "Present/RoadAgentActor.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/**
	 * The airframe, from the configured content set rather than a path written here.
	 *
	 * A literal path in this file would be a ninth copy of the mistake the content set
	 * exists to remove: it would keep passing after a content move only until it did not,
	 * and it would fail as "the airframe is the wrong size" rather than "it is somewhere
	 * else now". Null when nothing is configured, which the caller reports as a skip.
	 */
	USkeletalMesh* AgentActorTestAirframe()
	{
		const UAirsideContent* Content = UAirsideSettings::GetContent();
		return Content != nullptr ? Content->AgentMesh.LoadSynchronous() : nullptr;
	}

	/** Published PA-46-500TP wingspan, 13.110 m, in Unreal units. */
	constexpr double PiperWingspanUU = 1311.0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentActorTest,
	"Airside.Present.AgentActor",
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
		USkeletalMesh* Mesh = AgentActorTestAirframe();
		if (TestNotNull(TEXT("the Piper airframe asset loads"), Mesh))
		{
			const FBox Bounds = Mesh->GetBounds().GetBox();
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

			// Wheels on the ground is what lets SetMotion place the actor at SurfaceZ with no
			// lift of its own.
			TestTrue(FString::Printf(TEXT("the wheels rest on Z=0, measured %.1f"), Bounds.Min.Z),
				FMath::Abs(Bounds.Min.Z) < 5.0);
		}
	}

	// 2. SetMotion puts the aircraft ON the road, not above it.
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

		// DRESSED FIRST, which is the contract now. The airframe used to be resolved by path
		// in the constructor, so a bare agent arrived wearing it; it is pushed in by whoever
		// spawns the agent instead, because a path in C++ is a reference the editor cannot
		// fix up when content moves. An undressed agent is the placeholder cube - asserted
		// below, because the lift SetMotion applies depends on which of the two it is.
		Agent->SetAirframe(AgentActorTestAirframe());

		FAgentMotion Motion;
		Motion.Position = FVector2D(1200.0, -400.0);
		Motion.Heading = FMath::DegreesToRadians(30.0);
		Agent->SetMotion(Motion, SurfaceZ);

		const FVector At = Agent->GetActorLocation();
		TestEqual(TEXT("the agent stands at the road surface height"), At.Z, SurfaceZ);
		TestEqual(TEXT("and at the position it was given"), At.X, 1200.0);
		TestEqual(TEXT("on both axes"), At.Y, -400.0);

		// PITCHING KEEPS THE MAINS ON THE GROUND - measured in
		// Airside.Present.MeridianPitchesAboutItsMains, which is the one pitched check (#462 M28:
		// this test used to repeat the same SetMotion arithmetic with plane2's mains typed in as
		// -454.3, differing from the Meridian's only in where the pivot came from). What stays
		// here is the LEVEL case, which is a different property: the pivot must cost a level
		// aircraft nothing.
		//
		// ITS OWN ACTOR, not the one above: SetMotion is the only way to pose an agent, so a
		// test that borrowed it would leave it pitched and pointing somewhere else for every
		// assertion that follows - which is precisely what it did on the first run here.
		ARoadAgentActor* Flaring = NewObject<ARoadAgentActor>(GetTransientPackage());
		if (TestNotNull(TEXT("a flaring agent constructs"), Flaring))
		{
			Flaring->SetAirframe(AgentActorTestAirframe());

			// LEVEL FIRST, AND NOT POINTING DOWN +X. A pivot correction differenced against
			// the unrotated offset rather than the yawed one is zero at heading zero and
			// wrong everywhere else - it displaced agents sideways by up to twice the
			// wheelbase, reported from play as tracking way off the centre line, and the
			// first version of this test could not see it because it faced +X.
			FAgentMotion Level;
			Level.Position = FVector2D(700.0, -250.0);
			Level.Heading = FMath::DegreesToRadians(40.0);
			Level.PitchPivotX = -454.3;
			Flaring->SetMotion(Level, SurfaceZ);

			TestEqual(TEXT("a level aircraft stands exactly where it was put, whatever its "
				"heading"), Flaring->GetActorLocation().X, 700.0, 0.01);
			TestEqual(TEXT("on both axes"), Flaring->GetActorLocation().Y, -250.0, 0.01);
		}

		// AN UNMEASURED AIRFRAME PITCHES ABOUT ITS ORIGIN exactly as it did before any of
		// this - a zero pivot is the identity, and every vehicle and both airliners take it.
		ARoadAgentActor* Unmeasured = NewObject<ARoadAgentActor>(GetTransientPackage());
		if (TestNotNull(TEXT("an unmeasured agent constructs"), Unmeasured))
		{
			Unmeasured->SetAirframe(AgentActorTestAirframe());

			FAgentMotion Pitched;
			Pitched.PitchDegrees = 8.0;
			Unmeasured->SetMotion(Pitched, SurfaceZ);
			TestEqual(TEXT("with no pivot measured, the origin still sits on the surface"),
				Unmeasured->GetActorLocation().Z, SurfaceZ);
		}

			// And an UNDRESSED one is lifted, because the cube's pivot is at its centre while the
		// airframe's is on the ground. Getting this backwards flies the aircraft a metre above
		// the taxiway, which reads as a physics bug rather than as the arithmetic it is.
		ARoadAgentActor* Bare = NewObject<ARoadAgentActor>(GetTransientPackage());
		if (TestNotNull(TEXT("a second agent constructs"), Bare))
		{
			Bare->SetMotion(FAgentMotion(), SurfaceZ);
			TestTrue(TEXT("an agent with no airframe stands on the cube's lift"),
				Bare->GetActorLocation().Z > SurfaceZ);
		}

	// Heading is yaw from +X, which is why the airframe had to be imported nose-along-X.
		TestTrue(TEXT("heading becomes yaw"),
			FMath::IsNearlyEqual(Agent->GetActorRotation().Yaw, 30.0, 0.01));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeridianPitchesAboutItsMainsTest,
	"Airside.Present.MeridianPitchesAboutItsMains",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMeridianPitchesAboutItsMainsTest::RunTest(const FString& Parameters)
{
	// THE ONE BEHAVIOUR CHANGE IN plane7's IMPORT, pinned - everything else about that model
	// swap was a change of numbers.
	//
	// ARoadAgentActor::SetPose corrects for pitch by holding FAgentMotion::PitchPivotX still,
	// and FRoadAgent copies that straight off FChassis::FixedAxleX. A ZERO PIVOT IS EXACTLY
	// ZERO CORRECTION, so while the Meridian's origin WAS its main-gear axle - which is what
	// SM_PiperMeridian was imported about - this aeroplane was silently exempt from the whole
	// mechanism. plane7 is exported about the nose gear, so FixedAxleX is -237.8 and the
	// Meridian is corrected like every other measured airframe.
	//
	// WHAT THIS TEST UNIQUELY PINS is the ACTOR'S half. Reverting BuildPiperMeridian - which
	// TestAirframes::PiperType() is a one-line wrapper for - to a main-gear origin is caught by
	// Airside.Model.AirframeAxles section 5, which compares the builder with the shipped plane7
	// asset and its rig; it used to leave every assertion there passing on a self-consistent type.
	// What nothing else measures is that the pivot the type carries is the one SetPose holds
	// still while the body pitches: a correct type under a broken correction is a Meridian whose
	// tail sinks through the tarmac during the flare - a defect that was reported from play once
	// already, for plane2, and described as "the rear wheels push into the ground on landing".
	UAircraftType* Type = TestAirframes::PiperType();
	const FAirframe Meridian = Type->Airframe();

	// THE ONE PITCHED CHECK, since #462 M28. Airside.Present.AgentActor used to repeat this
	// arithmetic with plane2's mains typed in (-454.3) - the same SetMotion call, the same
	// three assertions, differing only in where the pivot came from - so this is the survivor
	// and the pivot is the type's, so the figure that reaches SetPose is the one the aeroplane
	// carries.
	// WHAT THE ARITHMETIC IS FOR, carried over from there:
	//
	// PITCHING KEEPS THE MAINS ON THE GROUND, which is the whole of the flare looking right.
	// Reported from play: "the rear wheels now push into the ground on the plane landing, as
	// if the pivot point for the flare has changed" - and it had. Pitch rotates about the
	// ACTOR ORIGIN, and plane2's origin moved to the nose gear when it was re-exported into
	// UAircraftType's local space (2026-09-13). A nose-up attitude about a pivot at the very
	// front levers everything behind it downwards: at 8 degrees plane2's mains went 63 uu
	// under the tarmac. An aeroplane pitches about its MAIN GEAR on the ground, so that is
	// the pivot.
	//
	// 1. THE TYPE HAS A PIVOT TO BE CORRECTED ABOUT. Asserted before anything is drawn,
	//    because if this is zero the actor assertions below would pass trivially: an
	//    uncorrected aeroplane keeps its ORIGIN on the surface, and the origin is what
	//    GetActorLocation reports.
	TestTrue(FString::Printf(TEXT("the Meridian declares a main gear aft of its origin "
		"(%.1f uu)"), Meridian.Chassis.FixedAxleX), Meridian.Chassis.FixedAxleX < -100.0);

	constexpr double SurfaceZ = 40.0;
	constexpr double PitchDegrees = 8.0;

	ARoadAgentActor* Flaring = NewObject<ARoadAgentActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("a flaring Meridian constructs"), Flaring))
	{
		return false;
	}
	Flaring->SetAirframe(AgentActorTestAirframe());

	// THE PIVOT COMES FROM THE TYPE, NOT FROM A LITERAL: the figure BuildPiperMeridian carries
	// is the one that arrives, so the test cannot keep passing against a number the aeroplane
	// no longer has.
	//
	// ON A DIAGONAL, and that is the yaw-frame bug's case: a pivot correction differenced
	// against the unrotated offset rather than the yawed one is zero at heading zero and wrong
	// everywhere else - it displaced agents sideways by up to twice the wheelbase, reported
	// from play as tracking way off the centre line, and the first version of the test this
	// one replaces could not see it because it faced +X. (The level case, which that bug also
	// breaks, stays in Airside.Present.AgentActor.)
	FAgentMotion Pitched;
	Pitched.Position = FVector2D(-800.0, 1500.0);
	Pitched.Heading = FMath::DegreesToRadians(115.0);
	Pitched.PitchDegrees = PitchDegrees;
	Pitched.PitchPivotX = Meridian.Chassis.FixedAxleX;
	Flaring->SetMotion(Pitched, SurfaceZ);

	// 2. THE MAINS STAY ON THE TARMAC. Asked of the actor's own transform rather than
	//    recomputed here, so what is measured is what the renderer will draw.
	const FVector Mains = Flaring->GetActorTransform().TransformPosition(
		FVector(Pitched.PitchPivotX, 0.0, 0.0));
	TestTrue(FString::Printf(TEXT("a flaring Meridian keeps its mains on the surface "
		"(Z %.1f against %.1f)"), Mains.Z, SurfaceZ),
		FMath::Abs(Mains.Z - SurfaceZ) < 1.0);

	// 3. AND THE NOSE ACTUALLY RISES, or assertion 2 would pass on an aeroplane that refused
	//    to pitch at all. 237.8 uu of wheelbase at 8 degrees lifts the nose gear 33 uu, and
	//    THE NOSE GEAR IS THE ORIGIN now - so this is the assertion that would have been
	//    false before plane7, when the correction was the identity and the origin stayed
	//    exactly on the surface.
	const double Lift = Flaring->GetActorLocation().Z - SurfaceZ;
	const double Want = -Meridian.Chassis.FixedAxleX * FMath::Sin(FMath::DegreesToRadians(PitchDegrees));
	TestTrue(FString::Printf(TEXT("while its nose gear lifts by the wheelbase times sin(pitch)"
		" - %.1f uu against %.1f"), Lift, Want), FMath::Abs(Lift - Want) < 1.0);

	// 4. AND THE MAINS DO NOT SLIDE OFF THE LINE THE MODEL PUT THEM ON. Pitching may raise
	//    the nose; it may not move the aircraft sideways off its centreline.
	const FVector2D MainsPlan(Mains.X, Mains.Y);
	const FVector2D Expected = Pitched.Position + FVector2D(
		FMath::Cos(Pitched.Heading), FMath::Sin(Pitched.Heading)) * Pitched.PitchPivotX;
	TestTrue(FString::Printf(TEXT("and stay on the route point's own line (%.1f uu away)"),
		FVector2D::Distance(MainsPlan, Expected)),
		FVector2D::Distance(MainsPlan, Expected) < 1.0);

	return true;
}

#endif
