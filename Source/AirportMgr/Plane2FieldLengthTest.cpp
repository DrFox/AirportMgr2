#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/LandingRun.h"
#include "Model/TakeoffRun.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The same relation Airside.Model.FieldLengthsCoverTheRoll pins, for an aircraft that test
 * cannot see.
 *
 * IT LIVES HERE RATHER THAN IN AIRSIDE, and that is a layering decision rather than a
 * convenience. The Airside test checks the default airframe, whatever UAirsideContent names
 * as DefaultAircraft, and BuildPiperMeridian - all things Airside itself owns. DA_Aircraft_
 * Plane2 is game content under /Game, and a plugin test that loaded it would point the
 * dependency the wrong way; Check-Architecture.ps1 enforces that direction and has failed on
 * a COMMENT that merely named a forbidden module.
 *
 * WHY IT IS WORTH DUPLICATING THE ASSERTION. A published field length SHORTER than the roll
 * admits an aircraft to a strip it then runs off the end of - the published figure is what
 * RunwayAdmission checks, and the roll is what actually moves the aeroplane. Plane2's
 * figures are a STOL aircraft's, deliberately short, which is exactly the direction that
 * breaks this if anyone trims them further.
 *
 * The landing distance is SIMULATED rather than closed-form - FLandingRun flies a probe down
 * an unbounded runway - so it cannot be checked by arithmetic in the authoring script, only
 * here against the model itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlane2FieldLengthsTest,
	"AirportMgr.Content.Plane2FieldLengthsCoverTheRoll",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlane2FieldLengthsTest::RunTest(const FString& Parameters)
{
	const FSoftObjectPath Path(TEXT("/Game/Entities/DA_Aircraft_Plane2.DA_Aircraft_Plane2"));
	UAircraftType* Type = Cast<UAircraftType>(Path.TryLoad());
	if (Type == nullptr)
	{
		// Not a failure: a fresh checkout that has not run build_plane2_type.py has no such
		// asset, and failing then would fail the suite for want of content rather than for a
		// defect - the same shape as AirportMgr.UI.EveryActionResolvesAnIcon's skip.
		AddInfo(TEXT("DA_Aircraft_Plane2 is not present; field lengths not checked"));
		return true;
	}

	const FAirframe Airframe = Type->Airframe();
	const double Roll = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);
	const double Landing =
		FLandingRun::RequiredLandingDistance(Airframe.Ground, Airframe.Climb, Airframe.Approach)
		* FLandingRun::LandingMargin;

	TestTrue(TEXT("plane2 publishes a take-off field length"),
		Airframe.Requirements.TakeoffFieldLength > 0.0);
	TestTrue(TEXT("plane2 publishes a landing field length"),
		Airframe.Requirements.LandingFieldLength > 0.0);

	TestTrue(*FString::Printf(
		TEXT("take-off roll %.0f uu fits the published %.0f uu"),
		Roll, Airframe.Requirements.TakeoffFieldLength),
		Roll <= Airframe.Requirements.TakeoffFieldLength);

	TestTrue(*FString::Printf(
		TEXT("landing distance with margin %.0f uu fits the published %.0f uu"),
		Landing, Airframe.Requirements.LandingFieldLength),
		Landing <= Airframe.Requirements.LandingFieldLength);

	// Said out loud whether or not it passes: these are the numbers a tuning pass moves, and
	// finding them in the log beats re-deriving them.
	AddInfo(FString::Printf(
		TEXT("plane2: roll %.0f uu against published %.0f; landing %.0f against %.0f"),
		Roll, Airframe.Requirements.TakeoffFieldLength,
		Landing, Airframe.Requirements.LandingFieldLength));

	// THE POINT OF THE TYPE. A Twin Otter that needed more runway than the Meridian would be
	// a STOL aeroplane in name only, and every figure above was chosen to make it shorter.
	// 51000 and 40000 are the Meridian's published lengths (BuildPiperMeridian).
	TestTrue(TEXT("plane2 needs less runway than the Meridian, which is the whole type"),
		Airframe.Requirements.TakeoffFieldLength < 51000.0);
	return true;
}

#endif
