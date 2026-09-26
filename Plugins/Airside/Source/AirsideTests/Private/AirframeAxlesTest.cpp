#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadEntity.h"
#include "Model/TakeoffRun.h"
#include "Solve/IcaoCode.h"
#include "Testing/AirsideTestWorld.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirframeAxlesTest,
	"Airside.Model.AirframeAxles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirframeAxlesTest::RunTest(const FString& Parameters)
{
	// 1. ZERO IS "NOT MEASURED", and it must mean the old law rather than a wheelbase of
	//    nothing - every vehicle in the game relies on that, so it is asserted first.
	const FAirframe Unmeasured;
	TestFalse(TEXT("an airframe nobody measured does not claim axles"), Unmeasured.Chassis.HasAxles());
	TestEqual(TEXT("and its wheelbase is zero rather than a negative"),
		Unmeasured.Chassis.Wheelbase(), 0.0);

	// 2. A conforming airframe: origin ON the steered axle, mains aft, so the wheelbase is
	//    the distance between them and is POSITIVE whichever way the figures are signed.
	FAirframe Conforming;
	Conforming.Chassis.SteerAxleX = 0.0;
	Conforming.Chassis.FixedAxleX = -454.3;
	TestTrue(TEXT("axle figures turn the geometric law on"), Conforming.Chassis.HasAxles());
	TestEqual(TEXT("wheelbase is nose gear to main gear"), Conforming.Chassis.Wheelbase(), 454.3, 0.01);

	// 3. A DECLARED DEVIATION - origin at the FIXED axle - measures the same wheelbase. If
	//    this ever fails, the two laws disagree about the same vehicle.
	//
	//    THIS USED TO NAME THE PIPER, which was the one aircraft type declaring it, until
	//    plane7 replaced the placeholder mesh on 2026-09-21 and brought the Meridian onto the
	//    nose-gear origin. The rule is still live and still exercised by a shipped asset:
	//    UAirsideSettings::ResolveDefaultVehicle's fuel truck carries SteerAxleX 360.7 against
	//    FixedAxleX 0, because fueltruck1 is exported about its rear axle. Kept as a
	//    HAND-BUILT airframe rather than repointed at the truck, because what is being pinned
	//    is FChassis::Wheelbase's arithmetic, not any one asset's figures.
	FAirframe Deviating;
	Deviating.Chassis.SteerAxleX = 454.3;
	Deviating.Chassis.FixedAxleX = 0.0;
	TestEqual(TEXT("a main-gear origin measures the same wheelbase"),
		Deviating.Chassis.Wheelbase(), 454.3, 0.01);

	// 4. The figures survive the trip through an authored type, which is the only path the
	//    game uses - a field added to FAirframe and not copied in Airframe() is a figure
	//    that is authored and then silently dropped.
	UAircraftType* Type = TestAirframes::PiperType();
	Type->Footprint.NoseX = 385.1;
	Type->Footprint.TailX = -531.5;
	Type->SteerAxleX = 260.0;
	Type->FixedAxleX = 0.0;
	// THE FOUR FIGURES ABOVE ARE OVERWRITTEN ON PURPOSE and are not the Meridian's any more.
	// BuildPiperMeridian is used here only to get a fully populated type in one line; what is
	// being pinned is that Airframe() COPIES each field, so the values are deliberately ones
	// no real type carries. A reader who "corrects" them to the current Meridian's would be
	// testing that the builder agrees with itself.
	Type->Ground.MaxSteerDegrees = 55.0;
	Type->Ground.MaxLateralAccelUu = 200.0;

	const FAirframe Built = Type->Airframe();
	TestEqual(TEXT("SteerAxleX reaches the airframe"), Built.Chassis.SteerAxleX, 260.0, 0.01);
	TestEqual(TEXT("FixedAxleX reaches the airframe"), Built.Chassis.FixedAxleX, 0.0, 0.01);
	TestEqual(TEXT("the steering lock reaches the airframe"),
		Built.Chassis.Ground.MaxSteerDegrees, 55.0, 0.01);
	TestEqual(TEXT("the lateral accel limit reaches the airframe"),
		Built.Chassis.Ground.MaxLateralAccelUu, 200.0, 0.01);

	// BodyCentreX is DERIVED in Airframe() rather than authored, so it cannot disagree with
	// the footprint it comes from. Claims read it - see FClaimPass::CentreOf.
	TestEqual(TEXT("the body centre is derived from the footprint"),
		Built.Chassis.BodyCentreX, (385.1 + -531.5) * 0.5, 0.01);

	// THE PUSHBACK NEED MAKES THE SAME TRIP, and it is asserted here for the reason this
	// whole section exists: it is authored on the type, read in Model/, and the ONE crossing
	// between them is Airframe(). A field left out of that function is a value the simulation
	// never sees, however carefully a designer set it.
	Type->PushbackNeed = EPushbackNeed::SelfManoeuvre;
	TestEqual(TEXT("the pushback need reaches the airframe"),
		Type->Airframe().PushbackNeed, EPushbackNeed::SelfManoeuvre);

	// AND THE UNAUTHORED DEFAULT IS THE CONSERVATIVE ONE. A hand-built airframe - a test, the
	// Piper fallback - must not claim it can reverse itself: saying "needs a tug" of something
	// that does not is a missing fee, while saying "reverses itself" of an A320 is an airport
	// that never needs the depot at all. Only one of those two errors is recoverable.
	TestEqual(TEXT("an unauthored airframe needs a tug"),
		Unmeasured.PushbackNeed, EPushbackNeed::VehicleTug);

	// 5. THE MERIDIAN IS MEASURED AND NOW CONFORMS: the steered axle IS the origin and the
	//    mains are aft of it. Measured off SK_Plane7's reference pose - nosewheel at x = 0.0,
	//    wheel_L and wheel_R at x = -237.8 - where it used to be measured off
	//    SK_PiperMeridian's about the other end.
	//
	//    THE WHEELBASE IS UNCHANGED AT 2.378 m, AND THAT IS THE ASSERTION THAT MATTERS. Only
	//    the datum moved: plane7 is a rebuild of the same aeroplane, so a wheelbase that had
	//    shifted would mean the new model is a different size rather than the same one
	//    re-origined. It is the one figure that survives the whole substitution unchanged.
	UAircraftType* Meridian = TestAirframes::PiperType();
	const FAirframe Piper = Meridian->Airframe();
	TestTrue(TEXT("the Meridian steers geometrically"), Piper.Chassis.HasAxles());
	TestEqual(TEXT("its wheelbase is the measured 2.378 m, unchanged by the re-origin"),
		Piper.Chassis.Wheelbase(), 237.8, 0.1);
	TestEqual(TEXT("its steered axle is the origin, per the class convention"),
		Piper.Chassis.SteerAxleX, 0.0, 0.01);
	TestEqual(TEXT("and its mains are aft of that origin, not on it"),
		Piper.Chassis.FixedAxleX, -237.8, 0.1);
	TestTrue(TEXT("it has a steering lock to turn on"), Piper.Chassis.Ground.MaxSteerDegrees > 0.0);

	// AND IT IS NOW PITCHED ABOUT ITS MAINS, which ARoadAgentActor::SetPose keys off exactly
	// this field being non-zero. Asserted here because the sign is what makes the correction
	// move the tail DOWN and the nose UP rather than the reverse, and a positive FixedAxleX
	// would pass every other check in this function.
	TestTrue(TEXT("a non-zero, aft fixed axle is what turns the pitch-pivot correction on"),
		Piper.Chassis.FixedAxleX < 0.0);

	// 6. THE AIRLINERS STAY ON THE PIVOT LAW, deliberately: no mesh to measure against, and
	//    a published wheelbase would be a figure nobody could check on screen. They are kept
	//    rather than deleted because EntityDefinition builds the A320 in production and the
	//    stand-geometry tests size against both - they are the only large footprints here.
	UAircraftType* Airbus = NewObject<UAircraftType>();
	UAircraftType::BuildA320(Airbus);
	TestFalse(TEXT("the A320 is not measured, so it keeps the flat rate"),
		Airbus->Airframe().Chassis.HasAxles());

	return true;
}

// MeasuredTypes[] LIVED HERE UNTIL ISSUE #293: sixteen hand-typed paths, each with a comment
// naming the ONE FACT that justified including it. FootprintMatchesTheMesh,
// MeasuredTypesFitTheirLettersRow, FieldLengthsCoverTheRoll and PushbackNeedsAuthored below
// all now call EveryAircraftType() (Testing/AirsideTestWorld.h) instead - the asset registry
// answers "every UAircraftType with a mesh" directly, so a new aeroplane needs no fifth hand
// list to join, and the gap that cost this file its issue (Plane4 and Plane7 absent from
// AircraftFieldLengthTest.cpp's table - see FieldLengthsCoverTheRoll below) cannot recur the
// same way. A comment that only justified INCLUSION had nothing left to say once inclusion
// stopped being a choice - but the FACTS those comments carried are kept here rather than
// dropped, since several are what a numeric constant elsewhere (IcaoCode's, mostly) was
// calibrated against:
//
//   - plane6 (777-300ER) is the first BOGIE rig: its main leg carries three axles, so its
//     rig names wheel_L1..L3 and has no bone called wheel_L at all - see LeftMainWheel below.
//   - plane7 (PA-46 Meridian) could not join a measured-figures list while its mesh was
//     SM_PiperMeridian, a placeholder imported about the main gear rather than the nose; it
//     declared a deviation from the steered-axle-is-the-origin convention that every other
//     type conforms to. Replaced 2026-09-21.
//   - plane8 (A380-800) is the second bogie rig and the first with TWO units a side: its
//     rig numbers wheel_L1..L2 on the wing bogie, wheel_L3..L5 on the body bogie, so
//     LeftMainWheel finds wheel_L1 the way it finds plane6's.
//   - plane9 (A320-200) BINDS CODE C'S NOSE: its 508.4 uu nose overhang is what
//     IcaoCode::MaxNoseFwdForLetter(Code C) is sized from.
//   - plane10 (Grand Caravan) is the first measured Code B single: its 15.88 m span clears
//     Code A's 15 m by 0.88.
//   - plane11 (A350-1000) BINDS CODE E'S TAIL: its rudder at 6901.3 uu aft of the stop mark
//     is what IcaoCode::MaxTailAftForLetter(Code E) 6902 is sized from.
//   - plane12 (PA-28-180 Cherokee) is the first rig exported OFF-LEVEL, 5.02 deg nose-up on
//     its gear - its footprint is a plan shadow of a pitched mesh.
//   - plane13 (757-300) is the first measured Code D type and the first two-axle bogie: its
//     38.05 m span clears Code C's 36 m by 2.05, and until it Code D had no modelled
//     aeroplane to be checked by.
//   - plane14 (Phenom 300) is the first measured Code B JET: its 15.91 m span clears Code
//     A's 15 m by 0.91 across the winglets.
//   - plane15 (Cirrus SR22) is the first rig whose NOSEWHEEL CASTERS: its steer bone turns
//     about a vertical axis at the leg foot rather than about a raked leg.
//   - plane16 (Baron 58) is the first Code A rig whose gear RETRACTS, and the second
//     exported off-level (2.44 deg nose-up); its axles are measured with the gear DOWN.

/**
 * The bone index of a rig's LEFT MAIN WHEEL, or INDEX_NONE.
 *
 * "wheel_L" UP TO plane5 AND "wheel_L1" ONCE A LEG CARRIES A BOGIE. Every aeroplane in this
 * fleet before the 777 has one main wheel a side; a 777 main leg carries three axles, so
 * plane6's rig names wheel_L1..L3 and there is no bone called wheel_L on it. The literal this
 * replaces made "one main wheel a side" a silent precondition of being measurable at all -
 * the test would have reported a MISSING BONE, which reads as a broken rig rather than as an
 * assumption in the test.
 *
 * ANY OF THE BOGIE'S WHEELS WOULD DO for what the caller wants. The hub HEIGHT is the wheel
 * radius, and three wheels on one axle beam are at one height - which is itself asserted, on
 * the authoring side, by build_aircraft_type.py's axles_and_radius. The lowest-numbered is
 * taken so the choice is a rule rather than whatever the bone order happens to be, and
 * Tools/Python/airside_anim.axle_anchor picks by the same rule for the same reason.
 */
static int32 LeftMainWheel(const FReferenceSkeleton& Rig)
{
	const int32 Single = Rig.FindBoneIndex(TEXT("wheel_L"));
	if (Single != INDEX_NONE)
	{
		return Single;
	}
	for (int32 Axle = 1; Axle <= 4; ++Axle)
	{
		const int32 Found = Rig.FindBoneIndex(*FString::Printf(TEXT("wheel_L%d"), Axle));
		if (Found != INDEX_NONE)
		{
			return Found;
		}
	}
	return INDEX_NONE;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFootprintMatchesTheMeshTest,
	"Airside.Content.FootprintMatchesTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFootprintMatchesTheMeshTest::RunTest(const FString& Parameters)
{
	// THE HALF-RUN PIPELINE, pinned. build_plane<N>_type.py measures the export and writes
	// the DA; re-import the mesh without re-running it and the figures describe an aeroplane
	// that no longer exists. Nothing on screen shows that - the model looks right and its
	// clearance envelope is somewhere else - so it has to be a test.
	//
	// EVERY MEASURED TYPE, not just plane2. The half-run is a property of the PIPELINE, so
	// the second aeroplane to go through it inherits the same exposure the moment it exists,
	// and a test naming one asset would have gone on passing while the other drifted. The list
	// is EveryAircraftType(), shared with MeasuredTypesFitTheirLettersRow.
	for (UAircraftType* Type : EveryAircraftType())
	{
		// THE BARE PACKAGE PATH, not GetPathName()'s "/Game/.../DA_Aircraft_Plane1.
		// DA_Aircraft_Plane1" - every literal path elsewhere in this file (Expected[],
		// Judgement[], StaticLoadObject calls) is the bare form, and a mismatched suffix here
		// would make the coverage Sets below never match anything they should.
		const FString Path = Type->GetOutermost()->GetName();

		USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		if (!TestNotNull(*FString::Printf(TEXT("%s: the mesh it names loads"), *Path), Mesh))
		{
			continue;
		}

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		const double MeshNoseX = Bounds.Origin.X + Bounds.BoxExtent.X;
		const double MeshTailX = Bounds.Origin.X - Bounds.BoxExtent.X;

		// A centimetre either way: both come from the same measurement, so anything larger is
		// a pipeline that was not re-run rather than a rounding difference.
		TestEqual(*FString::Printf(TEXT("%s: the authored nose matches the mesh"), *Path),
			Type->Footprint.NoseX, MeshNoseX, 1.0);
		TestEqual(*FString::Printf(TEXT("%s: the authored tail matches the mesh"), *Path),
			Type->Footprint.TailX, MeshTailX, 1.0);

		// AND THE ORIGIN IS THE NOSE GEAR, which is what makes the steered axle zero. A mesh
		// re-exported about some other point would pass both checks above and still steer
		// from the wrong end of the aeroplane.
		TestEqual(*FString::Printf(TEXT("%s: the steered axle is the origin, per the class "
			"convention"), *Path), Type->SteerAxleX, 0.0, 1.0);
		TestTrue(*FString::Printf(TEXT("%s: its mains are measured, aft of that origin"), *Path),
			Type->FixedAxleX < -100.0);

		// AND THE WHEEL THE ANIMATION SPINS IS THE WHEEL THE MODEL CARRIES. The radius is
		// measured by the authoring script and copied into the Anim Blueprint's defaults, so
		// it is the one figure in this pipeline that lives in two assets - and a type left on
		// UAircraftType's 21 uu default turns its wheels at whatever rate that implies.
		//
		// MEASURED AGAINST THE RIG, WHICH IT DID NOT USED TO BE. This check read
		// "FMath::Abs(MainWheelRadius - 21.0) > 1.0" - not the Meridian's default, therefore
		// measured - and that is a proxy rather than the thing. plane1 is what exposed it: a
		// Cessna 172's hub sits at 19.8 uu, so a correctly measured type passed by 1.2 uu
		// against a tolerance of 1.0, and the next model to carry a 20 uu wheel would have
		// failed for being right. The hub's HEIGHT in the reference pose IS the radius -
		// z = 0 is the contact plane, which airside_import.report_bounds refuses an import
		// for missing by more than 10 uu - so the rig can be asked directly, and a stale
		// figure now fails whatever value it happens to hold.
		const FReferenceSkeleton& Rig = Mesh->GetRefSkeleton();
		const int32 LeftWheel = LeftMainWheel(Rig);
		if (TestTrue(*FString::Printf(TEXT("%s: its rig has a left main wheel bone to "
			"measure against"), *Path), LeftWheel != INDEX_NONE))
		{
			const double HubHeight = FAnimationRuntime::GetComponentSpaceTransformRefPose(
				Rig, LeftWheel).GetTranslation().Z;
			TestEqual(*FString::Printf(TEXT("%s: the authored main wheel radius is the "
				"height of the hub the rig carries"), *Path),
				Type->MainWheelRadius, HubHeight, 0.5);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeasuredTypesFitTheirLettersRowTest,
	"Airside.Content.MeasuredTypesFitTheirLettersRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMeasuredTypesFitTheirLettersRowTest::RunTest(const FString& Parameters)
{
	// THE ASSET HALF OF Airside.Entities.EveryAirframeFitsItsLettersRow, and it exists because
	// that test names its own gap: it builds its cases from the C++ BUILDERS, so a type that
	// exists only as a DA_Aircraft_* uasset is invisible to it. Every MEASURED type is such a
	// type. That gap was not theoretical - plane6's 777-300ER reaches 6799 uu aft of the stop
	// mark against a Code E row authored at 6700, and nothing in the suite would have said so.
	//
	// WHAT GOES WRONG IF THIS IS RED. IcaoCode's row is what a STAND is laid out from - its
	// width, its depth, its wingtip clearance and where its GSE road runs. A type larger than
	// its letter admits parks with its tail, nose or wing over ground the layout treated as
	// clear, and nothing on screen distinguishes that from a stand that is simply tight.
	//
	// THE SAME FOUR CHECKS, DELIBERATELY, rather than a subset chosen because these types are
	// modelled: tail, nose, span-to-letter and the wing band. The two tests are one rule
	// applied to two kinds of type, and a divergence between them would be a second rule
	// nobody decided on.
	for (UAircraftType* Type : EveryAircraftType())
	{
		const FString Path = Type->GetOutermost()->GetName();

		// PARSED, NOT TRUSTED - UAircraftType::Code is an FName a human can edit, and a
		// mistyped letter would otherwise have every figure below measured against Code C.
		const TOptional<EIcaoCode> Parsed = IcaoCode::Parse(Type->Code.ToString());
		if (!TestTrue(*FString::Printf(TEXT("%s: its Code '%s' is a recognised ICAO letter"),
			*Path, *Type->Code.ToString()), Parsed.IsSet()))
		{
			continue;
		}
		const EIcaoCode Code = *Parsed;
		const TCHAR* Letter = IcaoCode::ToLetter(Code);

		// CONVERTED TO NOSE-GEAR COORDINATES FIRST, because the row is stated about the stop
		// mark and a footprint is stated about whatever origin its type declares. Every
		// measured type converts by zero today - FootprintMatchesTheMesh asserts SteerAxleX is
		// zero for all of them - and the conversion stays for the reason its twin in
		// StandLayoutTest gives: the first draft there left it out on the same reasoning and
		// was wrong by 85 uu on the first run.
		const double ToStopMark = Type->SteerAxleX;
		const double Nose = Type->Footprint.NoseX - ToStopMark;
		const double Tail = Type->Footprint.TailX - ToStopMark;

		TestTrue(
			*FString::Printf(TEXT("%s: its tail at %.0f is within code %s's %.0f"),
				*Path, Tail, Letter, IcaoCode::MaxTailAftForLetter(Code)),
			Tail >= -IcaoCode::MaxTailAftForLetter(Code));
		TestTrue(
			*FString::Printf(TEXT("%s: its nose at %.0f is within code %s's %.0f"),
				*Path, Nose, Letter, IcaoCode::MaxNoseFwdForLetter(Code)),
			Nose <= IcaoCode::MaxNoseFwdForLetter(Code));
		TestEqual(
			*FString::Printf(TEXT("%s: its measured span of %.0f uu is code %s's, which is the "
				"letter it is authored at"), *Path, Type->Footprint.Wingspan, Letter),
			IcaoCode::LetterForWingspan(Type->Footprint.Wingspan), FString(Letter));

		const double WingLine = Type->Footprint.WingX - ToStopMark;
		TestTrue(
			*FString::Printf(TEXT("%s: its wing line at %.0f is inside code %s's %.0f .. %.0f"),
				*Path, WingLine, Letter,
				IcaoCode::WingAftForLetter(Code), IcaoCode::WingFwdForLetter(Code)),
			WingLine >= IcaoCode::WingAftForLetter(Code)
				&& WingLine <= IcaoCode::WingFwdForLetter(Code));

		// SAID OUT LOUD WHETHER OR NOT IT PASSES, because the useful question when one of these
		// fails is "by how much", and re-deriving it means loading the asset by hand.
		AddInfo(FString::Printf(
			TEXT("%s is code %s: nose %.0f/%.0f, tail %.0f/%.0f, span %.0f, wing %.0f"),
			*Path, Letter, Nose, IcaoCode::MaxNoseFwdForLetter(Code),
			-Tail, IcaoCode::MaxTailAftForLetter(Code), Type->Footprint.Wingspan, WingLine));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackNeedsAuthoredTest,
	"Airside.Content.PushbackNeedsAuthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackNeedsAuthoredTest::RunTest(const FString& Parameters)
{
	// WHAT THE SHIPPED AIRCRAFT ACTUALLY SAY, asserted against the assets rather than trusted
	// to an authoring script's own report. Both headless save APIs report success while
	// writing nothing, so a MARKER line in a python log is evidence that the script RAN, not
	// that the value landed. This is the only check that survives the script.
	//
	// IT IS ALSO THE PROGRESSION, pinned. The light types reverse themselves, so an early
	// airport needs no Pushback depot at all; the airliners need a tug, so accepting one is
	// what forces the building. That is the whole gameplay point of EPushbackNeed, and it is
	// a content decision - which means nothing but a content test can defend it.
	//
	// THE REASONS ARE NOT HERE (issue #293). Every row below used to carry its own "Why" -
	// fourteen strings that were also typed, verbatim, in build_pushback_needs.py's NEEDS
	// table. build_pushback_needs.py's docstring already says which to believe if they ever
	// disagreed; keeping BOTH copies meant a plane whose reasoning was edited in one place
	// and not the other disagreed silently. The reason now lives once, in
	// aircraft/<key>.py's pushback_need/pushback_reason - Type->PushbackNeed against Need
	// below is the check that survives the authoring script; the WHY is Python's to carry.
	struct FExpected
	{
		const TCHAR* Path;
		EPushbackNeed Need;
	};

	const FExpected Expected[] = {
		{ TEXT("/Game/Entities/DA_Aircraft_Plane1"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane2"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane3"), EPushbackNeed::SelfManoeuvre },
		// PLANE4 AND PLANE5 WERE ABSENT FROM THIS TABLE ENTIRELY UNTIL ISSUE #293's coverage
		// sweep below found the gap - present in NEITHER this table nor
		// build_pushback_needs.py's, so both a 737-800 and a King Air loaded on FAirframe's
		// silent VehicleTug default with nothing asserting it was a decision. plane4
		// (VehicleTug: a jet with no reverse-pitch prop, plane9's and plane14's reasoning)
		// and plane5 (SelfManoeuvre: a King Air beta-ranges on its PT6s, plane10's Caravan
		// reasoning one size up) are now authored - see aircraft/plane4.py, aircraft/plane5.py.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane4"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane5"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane6"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane7"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane8"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane9"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane10"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane11"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane12"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane13"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane14"), EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane15"), EPushbackNeed::SelfManoeuvre },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane16"), EPushbackNeed::SelfManoeuvre },
		// THE TWO PAPER TYPES. No aircraft/<key>.py spec exists for either - EntityDefinition
		// builds them in C++ - so they stay hand-typed here and in build_pushback_needs.py's
		// own small PAPER_NEEDS residual rather than growing a spec file for two entries.
		{ TEXT("/Game/Entities/DA_Aircraft_A320"),   EPushbackNeed::VehicleTug },
		{ TEXT("/Game/Entities/DA_Aircraft_B738"),   EPushbackNeed::VehicleTug },
	};

	TSet<FString> Covered;
	for (const FExpected& Each : Expected)
	{
		Covered.Add(Each.Path);

		UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
			UAircraftType::StaticClass(), nullptr, Each.Path));
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Each.Path), Type))
		{
			continue;
		}

		TestEqual(*FString::Printf(TEXT("%s carries its authored pushback need - see "
			"Tools/Python/aircraft for why"), Each.Path), Type->PushbackNeed, Each.Need);

		// AND IT SURVIVES THE FLATTENING for this particular asset, not just for the
		// hand-built type in AirframeAxles above: the game reads FAirframe, never the DA.
		TestEqual(*FString::Printf(TEXT("%s carries it into the airframe"), Each.Path),
			Type->Airframe().PushbackNeed, Each.Need);
	}

	// EVERY TYPE WITH A MESH HAS AN AUTHORED NEED - the coverage check plane4 and plane5's
	// absence above proves was missing. Expected[] is typed rather than read off
	// EveryAircraftType() (a C++ test may not import Python's pushback_need to get an
	// independent gold value from), but its COVERAGE is checked against the registry: a
	// seventeenth aeroplane with no row here fails this loop rather than passing silently
	// the way plane4 and plane5 did.
	for (const UAircraftType* Type : EveryAircraftType())
	{
		TestTrue(*FString::Printf(TEXT("%s has a row in this test's Expected[] table - a type "
			"with a mesh and no authored pushback need is exactly the #293 gap"),
			*Type->GetOutermost()->GetName()),
			Covered.Contains(Type->GetOutermost()->GetName()));
	}

	return true;
}

/**
 * The same relation Airside.Model.FieldLengthsCoverTheRoll pins (FieldLengthTest.cpp, for the
 * default airframe and BuildPiperMeridian), for every /Game/Entities/DA_Aircraft_* content
 * asset - MOVED HERE FROM Source/AirportMgr/AircraftFieldLengthTest.cpp by issue #293.
 *
 * THAT FILE'S OWN LAYERING CLAIM WAS FALSE, and issue #293 is what caught it: its header
 * comment said "a plugin test that loaded [/Game assets] would point the dependency the
 * wrong way; Check-Architecture.ps1 enforces that direction" - but `grep Game
 * Check-Architecture.ps1` finds no such rule, and AirframeAxlesTest.cpp above (MeasuredTypes,
 * Expected[]), GearCycleTest.cpp:557 and StarterMapProbeTest.cpp:41 all already load
 * /Game/Entities/DA_Aircraft_* by path from this same plugin test module. A comment that
 * states a fact about OTHER code with no lint or test enforcing it is exactly what
 * CLAUDE.md's `// ENFORCED BY:` convention exists to catch, and this one enforced nothing -
 * it was simply wrong, and one of the two false-claim finds this issue's fix asked for.
 *
 * A published field length SHORTER than the roll admits an aircraft to a strip it then runs
 * off the end of - the published figure is what RunwayAdmission checks, and the roll is what
 * actually moves the aeroplane. The landing distance is SIMULATED rather than closed-form -
 * FLandingRun flies a probe down an unbounded runway - so it cannot be checked by arithmetic
 * in the authoring scripts, only here against the model itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFieldLengthsCoverTheRollContentTest,
	"Airside.Content.FieldLengthsCoverTheRoll",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFieldLengthsCoverTheRollContentTest::RunTest(const FString& Parameters)
{
	// THE ROLL CHECK RUNS FOR EVERY TYPE WITH A MESH, not a hand-picked twelve of sixteen.
	// AircraftFieldLengthTest.cpp's own Published[] table was silently missing Plane4 and
	// Plane7 - nobody had asked the roll-vs-published question of either - which is exactly
	// the shape EveryAircraftType() closes: the asset registry, not a list a human keeps in
	// step by hand.
	for (UAircraftType* Type : EveryAircraftType())
	{
		const FString Path = Type->GetOutermost()->GetName();
		const FAirframe Airframe = Type->Airframe();
		const double Roll = FTakeoffRun::RequiredRoll(Airframe.Chassis.Ground, Airframe.Climb);
		const double Landing =
			FLandingRun::RequiredLandingDistance(Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach)
			* FLandingRun::LandingMargin;

		TestTrue(*FString::Printf(TEXT("%s publishes a take-off field length"), *Path),
			Airframe.Requirements.TakeoffFieldLength > 0.0);
		TestTrue(*FString::Printf(TEXT("%s publishes a landing field length"), *Path),
			Airframe.Requirements.LandingFieldLength > 0.0);
		TestTrue(*FString::Printf(
			TEXT("%s: take-off roll %.0f uu fits the published %.0f uu"),
			*Path, Roll, Airframe.Requirements.TakeoffFieldLength),
			Roll <= Airframe.Requirements.TakeoffFieldLength);
		TestTrue(*FString::Printf(
			TEXT("%s: landing distance with margin %.0f uu fits the published %.0f uu"),
			*Path, Landing, Airframe.Requirements.LandingFieldLength),
			Landing <= Airframe.Requirements.LandingFieldLength);

		// Said out loud whether or not it passes: these are the numbers a tuning pass moves,
		// and finding them in the log beats re-deriving them.
		AddInfo(FString::Printf(
			TEXT("%s: roll %.0f uu against published %.0f; landing %.0f against %.0f"),
			*Path, Roll, Airframe.Requirements.TakeoffFieldLength,
			Landing, Airframe.Requirements.LandingFieldLength));
	}

	// THE JUDGEMENT ROWS - a fleet-progression CLAIM ("this type must ask less of a field
	// than that one"), which is a game-design fact about a PAIR of aeroplanes and cannot come
	// off the asset registry the way the roll check above does. Shrunk to {Path,
	// CeilingTypePath, Why} (issue #293): the old table TYPED the sibling's published figure
	// as a literal ("140200 is aircraft/plane3.py's REQUIREMENTS[...]"), which is exactly the
	// kind of second copy that goes stale the day the sibling's own figure is tuned. This
	// reads Requirements.TakeoffFieldLength off the live sibling asset instead.
	struct FJudgement
	{
		const TCHAR* Path;
		// nullptr: this type makes no ceiling claim - see Why for the reason.
		const TCHAR* CeilingTypePath;
		const TCHAR* Why;
	};

	const FJudgement Judgement[] = {
		// plane1 CARRIES THE SAME CEILING AS plane2 FOR A DIFFERENT REASON. It is not a STOL
		// aeroplane, but the LIGHTEST aeroplane in the game needing more runway than a
		// turboprop would be wrong on its face.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane1"), TEXT("/Game/Entities/DA_Aircraft_Plane7"),
		  TEXT("a Cessna 172 that needed more runway than the Meridian would have the "
			   "lightest aeroplane in the game asking more of a field than a turboprop") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane2"), TEXT("/Game/Entities/DA_Aircraft_Plane7"),
		  TEXT("a Twin Otter that needed more runway than the Meridian would be a STOL "
			   "aeroplane in name only") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane3"), nullptr, nullptr },
		// PLANE4 IS A NEW ROW - issue #293's own finding. AircraftFieldLengthTest.cpp never
		// named it at all, so nothing had ever asked whether the 737-800 fits its own roll,
		// let alone whether anything should be shorter than it. It makes no independent
		// ceiling claim today (nothing below it is meant to beat it, and plane9's row above
		// is the one claim that uses ITS figure); the roll-vs-published loop above is what
		// this row's existence was missing.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane4"), nullptr, nullptr },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane5"), TEXT("/Game/Entities/DA_Aircraft_Plane3"),
		  TEXT("a King Air that needed as much runway as a Q400 would not be the rung between "
			   "the Twin Otter and the Q400 - paving would admit nothing that lengthening had "
			   "not already admitted") },
		// plane6 CARRIES NO CEILING: the 777-300ER is the largest aeroplane in the game, so
		// the honest claim runs the other way (it must ask MORE than the 737), and this
		// struct has no column for a floor.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane6"), nullptr, nullptr },
		// PLANE7 IS A NEW ROW TOO - the Meridian is the YARDSTICK plane1, plane2 and plane12
		// are judged against, and nothing constrains it from above or below in turn.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane7"), nullptr, nullptr },
		// plane8 CARRIES NO CEILING FOR A DIFFERENT REASON THAN plane6's: the A380's gameplay
		// claim is width, and this struct has no column for width.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane8"), nullptr, nullptr },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane9"), TEXT("/Game/Entities/DA_Aircraft_Plane4"),
		  TEXT("an A320 that needed as much runway as a 737-800 would erase the one field "
			   "difference between the two Code C jets") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane10"), TEXT("/Game/Entities/DA_Aircraft_Plane5"),
		  TEXT("a Caravan that needed as much runway as a King Air would be a grass-strip "
			   "type that only paved fields could take") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane11"), TEXT("/Game/Entities/DA_Aircraft_Plane6"),
		  TEXT("an A350-1000 that needed as much runway as a 777-300ER would erase the one "
			   "field difference between the two Code E twins") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane12"), TEXT("/Game/Entities/DA_Aircraft_Plane7"),
		  TEXT("a Cherokee that needed more runway than the Meridian would have a club "
			   "trainer asking more of a field than a turboprop") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane13"), TEXT("/Game/Entities/DA_Aircraft_Plane11"),
		  TEXT("a 757-300 that needed as much runway as an A350-1000 would make Code D a rung "
			   "a field could skip") },
		// plane14 CARRIES NO CEILING, plane3's case: the one comparison its character offers
		// (the King Air on Code B) is a claim no tuning pass could be told apart from noise.
		{ TEXT("/Game/Entities/DA_Aircraft_Plane14"), nullptr, nullptr },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane15"), TEXT("/Game/Entities/DA_Aircraft_Plane10"),
		  TEXT("an SR22 that needed as much runway as a Caravan would be a four-seat Code A "
			   "single asking a Code B turboprop's field") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane16"), TEXT("/Game/Entities/DA_Aircraft_Plane5"),
		  TEXT("a Baron that needed as much runway as a King Air would make the piston twin "
			   "and the turboprop twin one rung") },
	};

	TSet<FString> JudgementCovered;
	for (const FJudgement& Each : Judgement)
	{
		JudgementCovered.Add(Each.Path);
		if (Each.CeilingTypePath == nullptr)
		{
			continue;
		}

		UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
			UAircraftType::StaticClass(), nullptr, Each.Path));
		UAircraftType* Ceiling = Cast<UAircraftType>(StaticLoadObject(
			UAircraftType::StaticClass(), nullptr, Each.CeilingTypePath));
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Each.Path), Type)
			|| !TestNotNull(*FString::Printf(TEXT("%s's ceiling type %s loads"), Each.Path,
				Each.CeilingTypePath), Ceiling))
		{
			continue;
		}

		TestTrue(Each.Why,
			Type->Airframe().Requirements.TakeoffFieldLength
				< Ceiling->Airframe().Requirements.TakeoffFieldLength);
	}

	// EVERY TYPE WITH A MESH HAS A JUDGEMENT ROW, even if that row's answer is "no ceiling
	// claim" - the same coverage shape PushbackNeedsAuthored's Covered check enforces above,
	// for the same reason: Plane4 and Plane7 were missing from this table's ancestor with
	// nothing to notice.
	for (const UAircraftType* Type : EveryAircraftType())
	{
		TestTrue(*FString::Printf(TEXT("%s has a row in this test's Judgement[] table"),
			*Type->GetOutermost()->GetName()),
			JudgementCovered.Contains(Type->GetOutermost()->GetName()));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleFootprintMatchesTheMeshTest,
	"Airside.Content.VehicleFootprintMatchesTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleFootprintMatchesTheMeshTest::RunTest(const FString& Parameters)
{
	// FootprintMatchesTheMesh above guards the AIRCRAFT figures against their mesh. This is
	// the same guard for the ground vehicle, and it exists because the coupling is the same
	// and just as invisible: FTrafficRules::VehicleFootprint is a hand-typed length that has
	// to equal the vehicle mesh's, because UAirsideContent::VehicleMesh promises the box is
	// sized from it so that what is on screen is "the length the arbiter actually keeps
	// clear". Re-export fueltruck1 a little longer and nothing on screen changes - the truck
	// looks right and reserves the wrong amount of road, so the arbiter admits a second agent
	// into road this one is occupying.
	//
	// THE MESH COMES FROM THE CONTENT SET rather than a path written here, so this follows
	// whichever vehicle is configured instead of asserting a fact about one asset.
	const USkeletalMesh* Mesh = UAirsideSettings::ResolveVehicleView().Mesh;
	if (Mesh == nullptr)
	{
		// NOT A FAILURE. A project with no rigged vehicle configured has nothing to
		// disagree with - and the automation projects that carry no content set are exactly
		// that, which is why ResolveDefaultAirframe has a Piper fallback at all.
		AddInfo(TEXT("no rigged vehicle configured; nothing to compare the footprint against"));
		return true;
	}

	const FTrafficRules Rules;
	const double MeshLengthUu = Mesh->GetBounds().BoxExtent.X * 2.0;

	// A centimetre either way, the tolerance FootprintMatchesTheMesh uses and for its reason:
	// both figures come from one measurement, so anything larger is a step that was not
	// re-run rather than rounding.
	TestEqual(TEXT("VehicleFootprint matches the configured vehicle mesh's length"),
		Rules.VehicleFootprint, MeshLengthUu, 1.0);

	// AND THE AXLES MATCH THE RIG, which is what makes the truck STEER rather than pivot.
	//
	// ResolveDefaultVehicle types these figures in - there is no UVehicleType asset to author
	// them on yet - so they are exactly the "authored number describing a mesh" this test
	// exists to pin. Get them wrong and nothing looks broken until a corner: a wheelbase of
	// zero turns HasAxles() off and the truck spins about its rear axle, which is how this
	// was found.
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const TArray<FTransform>& Pose = Ref.GetRefBonePose();

	auto BoneX = [&Ref, &Pose](const TCHAR* Name) -> TOptional<double>
	{
		const int32 Index = Ref.FindBoneIndex(FName(Name));
		if (Index == INDEX_NONE)
		{
			return TOptional<double>();
		}
		// Up the parent chain, because a bone's ref pose is relative to its parent - and the
		// front wheels hang off their steer bones, so their own translation is not where they
		// are on the truck.
		FTransform At = Pose[Index];
		for (int32 Parent = Ref.GetParentIndex(Index); Parent != INDEX_NONE;
			Parent = Ref.GetParentIndex(Parent))
		{
			At = At * Pose[Parent];
		}
		return TOptional<double>(At.GetLocation().X);
	};

	const TOptional<double> FrontX = BoneX(TEXT("steer_FL"));
	const TOptional<double> RearX = BoneX(TEXT("wheel_RL"));
	if (!FrontX.IsSet() || !RearX.IsSet())
	{
		AddError(TEXT("the configured vehicle has no steer_FL/wheel_RL bones to measure - "
			"either the rig was renamed or a different vehicle is configured, and "
			"ResolveDefaultVehicle's axle figures are describing something else"));
		return false;
	}

	const FVehicle Van = UAirsideSettings::ResolveDefaultVehicle();
	TestTrue(TEXT("the service vehicle steers geometrically rather than pivoting"),
		Van.Chassis.HasAxles());
	TestEqual(TEXT("SteerAxleX matches the rig's front axle"), Van.Chassis.SteerAxleX, FrontX.GetValue(), 1.0);
	TestEqual(TEXT("FixedAxleX matches the rig's rear axle"), Van.Chassis.FixedAxleX, RearX.GetValue(), 1.0);

	return true;
}

#endif
