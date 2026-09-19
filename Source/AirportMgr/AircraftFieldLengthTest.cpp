#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/LandingRun.h"
#include "Model/TakeoffRun.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The same relation Airside.Model.FieldLengthsCoverTheRoll pins, for the aircraft that test
 * cannot see.
 *
 * IT LIVES HERE RATHER THAN IN AIRSIDE, and that is a layering decision rather than a
 * convenience. The Airside test checks the default airframe, whatever UAirsideContent names
 * as DefaultAircraft, and BuildPiperMeridian - all things Airside itself owns. The
 * DA_Aircraft_* assets are game content under /Game, and a plugin test that loaded them would
 * point the dependency the wrong way; Check-Architecture.ps1 enforces that direction and has
 * failed on a COMMENT that merely named a forbidden module.
 *
 * WHY IT IS WORTH DUPLICATING THE ASSERTION. A published field length SHORTER than the roll
 * admits an aircraft to a strip it then runs off the end of - the published figure is what
 * RunwayAdmission checks, and the roll is what actually moves the aeroplane.
 *
 * A TABLE RATHER THAN A TEST PER TYPE, since plane3 landed on 2026-09-18. The rule is a
 * property of the PIPELINE, not of one aeroplane: every type authored by a build_*_type.py
 * publishes figures a tuning pass can trim, and the two types break it from opposite
 * directions - plane2's lengths are a STOL aircraft's, deliberately short, and plane3's are
 * an airliner's against an acceleration four times plane2's. One name here also means the
 * next type is a row, which is the only way a list like this stays in step with the content.
 *
 * The landing distance is SIMULATED rather than closed-form - FLandingRun flies a probe down
 * an unbounded runway - so it cannot be checked by arithmetic in the authoring scripts, only
 * here against the model itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAircraftFieldLengthsTest,
	"AirportMgr.Content.FieldLengthsCoverTheRoll",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAircraftFieldLengthsTest::RunTest(const FString& Parameters)
{
	struct FPublished
	{
		const TCHAR* Path;
		const TCHAR* Label;

		/** An upper bound the type's own character demands, uu, or 0 for no claim. */
		double TakeoffCeiling;

		/** Why that ceiling is the point of the type, quoted when it fails. */
		const TCHAR* Character;
	};

	// 51000 and 40000 are the Meridian's published lengths (BuildPiperMeridian), and they are
	// the yardstick both ways: plane2 is a STOL aeroplane and MUST beat them, plane3 is an
	// airliner and is expected not to - which is why it carries no ceiling rather than a
	// generous one. A ceiling nobody could fail is a test that measures nothing.
	const FPublished Published[] = {
		{ TEXT("/Game/Entities/DA_Aircraft_Plane2.DA_Aircraft_Plane2"), TEXT("plane2"), 51000.0,
		  TEXT("a Twin Otter that needed more runway than the Meridian would be a STOL "
			   "aeroplane in name only") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane3.DA_Aircraft_Plane3"), TEXT("plane3"), 0.0,
		  nullptr },
	};

	for (const FPublished& Each : Published)
	{
		UAircraftType* Type = Cast<UAircraftType>(FSoftObjectPath(Each.Path).TryLoad());
		if (Type == nullptr)
		{
			// Not a failure: a fresh checkout that has not run the authoring script has no
			// such asset, and failing then would fail the suite for want of content rather
			// than for a defect - the same shape as AirportMgr.UI.EveryActionResolvesAnIcon's
			// skip.
			AddInfo(FString::Printf(TEXT("%s is not present; field lengths not checked"),
				Each.Path));
			continue;
		}

		const FAirframe Airframe = Type->Airframe();
		const double Roll = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);
		const double Landing =
			FLandingRun::RequiredLandingDistance(Airframe.Ground, Airframe.Climb, Airframe.Approach)
			* FLandingRun::LandingMargin;

		TestTrue(*FString::Printf(TEXT("%s publishes a take-off field length"), Each.Label),
			Airframe.Requirements.TakeoffFieldLength > 0.0);
		TestTrue(*FString::Printf(TEXT("%s publishes a landing field length"), Each.Label),
			Airframe.Requirements.LandingFieldLength > 0.0);

		TestTrue(*FString::Printf(
			TEXT("%s: take-off roll %.0f uu fits the published %.0f uu"),
			Each.Label, Roll, Airframe.Requirements.TakeoffFieldLength),
			Roll <= Airframe.Requirements.TakeoffFieldLength);

		TestTrue(*FString::Printf(
			TEXT("%s: landing distance with margin %.0f uu fits the published %.0f uu"),
			Each.Label, Landing, Airframe.Requirements.LandingFieldLength),
			Landing <= Airframe.Requirements.LandingFieldLength);

		// Said out loud whether or not it passes: these are the numbers a tuning pass moves,
		// and finding them in the log beats re-deriving them.
		AddInfo(FString::Printf(
			TEXT("%s: roll %.0f uu against published %.0f; landing %.0f against %.0f"),
			Each.Label, Roll, Airframe.Requirements.TakeoffFieldLength,
			Landing, Airframe.Requirements.LandingFieldLength));

		if (Each.TakeoffCeiling > 0.0)
		{
			TestTrue(Each.Character,
				Airframe.Requirements.TakeoffFieldLength < Each.TakeoffCeiling);
		}
	}

	return true;
}

#endif
