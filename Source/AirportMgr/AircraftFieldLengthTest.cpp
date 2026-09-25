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
		// plane1 CARRIES THE SAME CEILING AS plane2 FOR A DIFFERENT REASON. It is not a STOL
		// aeroplane - a Twin Otter out-STOLs a 172 and the fleet's short-field floor stays
		// plane2's - but the LIGHTEST aeroplane in the game needing more runway than a
		// 500 hp turboprop single would be wrong on its face. Its 49,700 clears the
		// Meridian's 51,000 by 1,300 uu, which is thin; that is the claim, and a thin one
		// is still a claim. Both figures are published constants rather than computed, so
		// this only moves when somebody edits a table on purpose.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane1.DA_Aircraft_Plane1"), TEXT("plane1"), 51000.0,
		  TEXT("a Cessna 172 that needed more runway than the Meridian would have the "
			   "lightest aeroplane in the game asking more of a field than a turboprop") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane2.DA_Aircraft_Plane2"), TEXT("plane2"), 51000.0,
		  TEXT("a Twin Otter that needed more runway than the Meridian would be a STOL "
			   "aeroplane in name only") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane3.DA_Aircraft_Plane3"), TEXT("plane3"), 0.0,
		  nullptr },
		// plane5's CEILING IS THE Q400's PUBLISHED LENGTH, NOT THE MERIDIAN's, and that is the
		// first ceiling here that measures a type against another MODELLED type rather than
		// against the paper yardstick. The King Air exists to be the rung between the Twin
		// Otter and the Q400: it demands TARMAC, which no lighter type does, and it demands
		// noticeably less of it than the Q400 does. Lose the second half and the first half
		// buys nothing - paving would admit nothing that lengthening had not already admitted,
		// and the type would be a Q400 that carries eleven people.
		//
		// 140200 is build_plane3_type.py's REQUIREMENTS["takeoff_field_length"]. Typed here
		// because a test may not import a Python table; if plane3's figure moves, this fails
		// and says so, which is the behaviour wanted from a second copy that cannot be avoided.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane5.DA_Aircraft_Plane5"), TEXT("plane5"), 140200.0,
		  TEXT("a King Air that needed as much runway as a Q400 would not be the rung between "
			   "the Twin Otter and the Q400 - paving would admit nothing that lengthening had "
			   "not already admitted") },
		// plane6 CARRIES NO CEILING, AND IT IS THE FIRST ROW HERE WITH NOTHING ABOVE IT TO BE
		// SHORTER THAN. Every ceiling above is "this type must ask less of a field than that
		// one"; the 777-300ER is the largest aeroplane in the game, so the honest claim about
		// it runs the other way - it must ask MORE than the 737 - and this struct has no
		// column for a floor. Left as a claim nobody makes rather than as a generous ceiling
		// that could not fail, which is the mistake the Meridian-yardstick note warns against.
		// The roll-against-published check below is real and is the reason the row is here at
		// all: 3,120 m is by far the largest published figure in the game, and a tuning pass
		// that trimmed the take-off accel could put the roll past it without anything else
		// noticing.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane6.DA_Aircraft_Plane6"), TEXT("plane6"), 0.0,
		  nullptr },
		// plane8 ALSO CARRIES NO CEILING, AND NOT FOR plane6's REASON. The A380's published
		// take-off field length (~3,000 m, Airbus AC 3-3-1) is SHORTER than the 777-300ER's
		// 3,120 - four engines on 575 t against two on 351 t - so "must ask less than the 777"
		// would be a TRUE claim this row could make. It is not made, because the point of a
		// ceiling here is a rung on the field-length ladder, and the A380 is not a rung on it:
		// what refuses this type is a 45 m runway and a Code E stand, not 120 m of pavement.
		// Its gameplay claim is width, and this struct has no column for width; a ceiling
		// that happened to hold would be read as the ladder having one more step than it has.
		// The roll-against-published check below is real, as it is for plane6.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane8.DA_Aircraft_Plane8"), TEXT("plane8"), 0.0,
		  nullptr },
		// plane11's CEILING IS THE 777-300ER's PUBLISHED TAKE-OFF LENGTH - plane9's argument
		// one letter up. The A350-1000 and the 777 share Code E and its stands; what tells
		// them apart on the field is that the A350 asks less runway (2,750 m against 3,120),
		// so it is the widebody a field admits first. 312000 is build_plane6_type.py's
		// REQUIREMENTS["takeoff_field_length"], typed here for the reason plane5's row gives.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane11.DA_Aircraft_Plane11"), TEXT("plane11"), 312000.0,
		  TEXT("an A350-1000 that needed as much runway as a 777-300ER would erase the one "
			   "field difference between the two Code E twins") },
		// plane9's CEILING IS THE 737-800's PUBLISHED TAKE-OFF LENGTH. The A320 and the 737
		// share Code C and its stands; what tells them apart on the field is that the A320
		// asks less runway (2,100 m against 2,316). Lose that and it is a 737 with a
		// different nose. 231600 is build_plane4_type.py's REQUIREMENTS["takeoff_field_length"],
		// typed here for the reason plane5's row gives.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane9.DA_Aircraft_Plane9"), TEXT("plane9"), 231600.0,
		  TEXT("an A320 that needed as much runway as a 737-800 would erase the one field "
			   "difference between the two Code C jets") },
		// plane10's CEILING IS THE KING AIR's PUBLISHED TAKE-OFF LENGTH. Both are Code B
		// turboprops; the Caravan is the one that stays on GRASS, and it must also ask less
		// runway (740 m against 1,006) or paving and lengthening would buy the same thing.
		// 100600 is build_plane5_type.py's REQUIREMENTS["takeoff_field_length"], typed here
		// for the reason plane5's row gives.
		// plane12 CARRIES plane1's CEILING FOR plane1's REASON: a four-seat trainer that
		// needed more runway than the Meridian would be wrong on its face. 50,000 clears it by
		// 1,000 uu - thin, and the claim.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane12.DA_Aircraft_Plane12"), TEXT("plane12"), 51000.0,
		  TEXT("a Cherokee that needed more runway than the Meridian would have a club "
			   "trainer asking more of a field than a turboprop") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane10.DA_Aircraft_Plane10"), TEXT("plane10"), 100600.0,
		  TEXT("a Caravan that needed as much runway as a King Air would be a grass-strip "
			   "type that only paved fields could take") },
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
		const double Roll = FTakeoffRun::RequiredRoll(Airframe.Chassis.Ground, Airframe.Climb);
		const double Landing =
			FLandingRun::RequiredLandingDistance(Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach)
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
