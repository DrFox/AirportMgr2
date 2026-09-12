#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * An aircraft type carries its own LOOK, and two types do not share one.
 *
 * THE BUG THIS EXISTS FOR, reported from play: "I just had a Twin Otter offered, it was a
 * Piper Meridian that spawned." The offer, the name, the performance figures and the refusal
 * reasons were all correctly the Twin Otter's - only the aeroplane on the runway was not.
 *
 * The cause was that UAircraftType had no mesh at all. UAirsideTraffic handed every aircraft
 * agent UAirsideContent::AgentMesh, which is ONE skeletal mesh for the whole game, and
 * nothing noticed for as long as the project had exactly one aircraft model to wear.
 *
 * SO THIS CHECKS THE TYPES AGAINST EACH OTHER, not against a literal. Asserting that plane2
 * points at SK_Plane2 would pass while every other type still pointed at the same default;
 * what makes the bug impossible is that two types resolve to DIFFERENT meshes.
 *
 * In the game module because these are /Game assets - Airside may not reach them, and
 * Check-Architecture enforces that direction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAircraftLookTest,
	"AirportMgr.Content.AircraftTypesDoNotShareOneMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
	UAircraftType* LoadType(const TCHAR* Path)
	{
		return Cast<UAircraftType>(FSoftObjectPath(Path).TryLoad());
	}
}

bool FAircraftLookTest::RunTest(const FString& Parameters)
{
	UAircraftType* Plane2 = LoadType(TEXT("/Game/Entities/DA_Aircraft_Plane2.DA_Aircraft_Plane2"));
	UAircraftType* Piper = LoadType(TEXT("/Game/Entities/DA_Aircraft_Piper.DA_Aircraft_Piper"));

	if (Plane2 == nullptr || Piper == nullptr)
	{
		// A fresh checkout that has not run the authoring scripts has neither; failing then
		// would fail the suite for want of content rather than for a defect.
		AddInfo(TEXT("Both aircraft types are not present; look not checked"));
		return true;
	}

	// THE AIRFRAME is what actually reaches the spawn - UFlight flattens the type into one
	// and the agent carries no pointer back. Checking the type's own property would pass
	// while Airframe() forgot to copy it, which is the seam that matters.
	const FAirframe Twin = Plane2->Airframe();
	const FAirframe Light = Piper->Airframe();

	TestFalse(TEXT("plane2's airframe carries a mesh, so it does not wear the game default"),
		Twin.Mesh.IsNull());

	if (!Twin.Mesh.IsNull() && !Light.Mesh.IsNull())
	{
		TestNotEqual(TEXT("two types resolve to DIFFERENT meshes - the whole defect was that "
			"every aircraft wore the same one"),
			Twin.Mesh.ToString(), Light.Mesh.ToString());
	}
	else
	{
		AddInfo(TEXT("the Piper has no mesh of its own yet and still falls back to the "
			"content default; that is legal, and plane2 having one is what stops the two "
			"being the same aeroplane"));
	}

	// THE SHORT CODE, which is not the aerodrome letter. TypeCode used to be assigned
	// UAircraftType::Code - an ICAO reference letter - so an A320 and a 737 were both "C"
	// and nothing reading it could tell two types apart.
	TestFalse(TEXT("plane2 publishes a short code"), Twin.TypeCode.IsNone());
	TestFalse(TEXT("the Piper publishes a short code"), Light.TypeCode.IsNone());
	TestNotEqual(TEXT("the two short codes differ, so TypeCode can name a type"),
		Twin.TypeCode, Light.TypeCode);

	AddInfo(FString::Printf(TEXT("plane2: %s wearing %s; piper: %s wearing %s"),
		*Twin.TypeCode.ToString(), *Twin.Mesh.ToString(),
		*Light.TypeCode.ToString(), *Light.Mesh.ToString()));
	return true;
}

#endif
