#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Solve/IcaoCode.h"
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
	//    UAirsideSettings::ResolveDefaultVehicle's fuel truck carries SteerAxleX 494.5 against
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

/**
 * Every aeroplane authored by a build_plane<N>_type.py - the ones whose figures are MEASURED
 * off an export rather than typed into a C++ builder.
 *
 * ONE LIST, BECAUSE TWO TESTS READ IT. FootprintMatchesTheMesh pins the authored figures
 * against the mesh they were measured from; MeasuredTypesFitTheirLettersRow pins them against
 * IcaoCode's table. Both are properties of the PIPELINE rather than of one aeroplane, so a
 * type that joined one list and not the other would be half-covered with nothing saying which
 * half - which is the failure CLAUDE.md names under "check where a list is CONSUMED", and
 * which airside_import.py's FLEET already records paying for once.
 *
 * NOT THE PAPER TYPES. DA_Aircraft_A320 and DA_Aircraft_B738 carry no mesh and are built in
 * C++, so there is nothing here to measure them against; Airside.Entities.
 * EveryAirframeFitsItsLettersRow covers those through their builders.
 */
static const TCHAR* const MeasuredTypes[] = {
	TEXT("/Game/Entities/DA_Aircraft_Plane1"),
	TEXT("/Game/Entities/DA_Aircraft_Plane2"),
	TEXT("/Game/Entities/DA_Aircraft_Plane3"),
	TEXT("/Game/Entities/DA_Aircraft_Plane4"),
	TEXT("/Game/Entities/DA_Aircraft_Plane5"),
	// plane6 IS THE ROW THAT BROKE THE wheel_L LOOKUP BELOW, and it is here rather than
	// excused because the half-run is exactly as available to a 777 as to a 172. A 777 main
	// leg carries a three-axle bogie, so its rig names wheel_L1..L3 and has no bone called
	// wheel_L at all - see LeftMainWheel.
	TEXT("/Game/Entities/DA_Aircraft_Plane6"),
	// THE MERIDIAN JOINS THE LIST, which it could not while its mesh was a placeholder
	// imported about the main gear: the steered-axle check asserts the class convention, and
	// that type declared a deviation from it. plane7 conforms, so the half-run guard now
	// covers every modelled aeroplane in the game rather than five of six. It is also the only
	// row whose footprint is typed in C++ as well as authored - see
	// Tools/Python/build_plane7_type.py, which checks the same four figures from the other
	// direction.
	TEXT("/Game/Entities/DA_Aircraft_Plane7"),
	// THE A380 IS THE SECOND BOGIE ROW AND THE FIRST WITH TWO UNITS A SIDE. Its rig numbers
	// the wheels per side - wheel_L1..L2 on the wing bogie, wheel_L3..L5 on the body bogie -
	// precisely so that LeftMainWheel below finds wheel_L1 the way it finds plane6's; the
	// models repo names the MESHES by unit and the BONES by the game's rule, and says so.
	TEXT("/Game/Entities/DA_Aircraft_Plane8"),
};

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
 * the authoring side, by build_plane6_type.py's axles_and_radius. The lowest-numbered is
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
	// is MeasuredTypes, shared with MeasuredTypesFitTheirLettersRow.
	for (const TCHAR* Path : MeasuredTypes)
	{
		UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
			UAircraftType::StaticClass(), nullptr, Path));
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Path), Type))
		{
			continue;
		}

		USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
		if (!TestNotNull(*FString::Printf(TEXT("%s: the mesh it names loads"), Path), Mesh))
		{
			continue;
		}

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		const double MeshNoseX = Bounds.Origin.X + Bounds.BoxExtent.X;
		const double MeshTailX = Bounds.Origin.X - Bounds.BoxExtent.X;

		// A centimetre either way: both come from the same measurement, so anything larger is
		// a pipeline that was not re-run rather than a rounding difference.
		TestEqual(*FString::Printf(TEXT("%s: the authored nose matches the mesh"), Path),
			Type->Footprint.NoseX, MeshNoseX, 1.0);
		TestEqual(*FString::Printf(TEXT("%s: the authored tail matches the mesh"), Path),
			Type->Footprint.TailX, MeshTailX, 1.0);

		// AND THE ORIGIN IS THE NOSE GEAR, which is what makes the steered axle zero. A mesh
		// re-exported about some other point would pass both checks above and still steer
		// from the wrong end of the aeroplane.
		TestEqual(*FString::Printf(TEXT("%s: the steered axle is the origin, per the class "
			"convention"), Path), Type->SteerAxleX, 0.0, 1.0);
		TestTrue(*FString::Printf(TEXT("%s: its mains are measured, aft of that origin"), Path),
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
			"measure against"), Path), LeftWheel != INDEX_NONE))
		{
			const double HubHeight = FAnimationRuntime::GetComponentSpaceTransformRefPose(
				Rig, LeftWheel).GetTranslation().Z;
			TestEqual(*FString::Printf(TEXT("%s: the authored main wheel radius is the "
				"height of the hub the rig carries"), Path),
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
	for (const TCHAR* Path : MeasuredTypes)
	{
		UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
			UAircraftType::StaticClass(), nullptr, Path));
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Path), Type))
		{
			continue;
		}

		// PARSED, NOT TRUSTED - UAircraftType::Code is an FName a human can edit, and a
		// mistyped letter would otherwise have every figure below measured against Code C.
		const TOptional<EIcaoCode> Parsed = IcaoCode::Parse(Type->Code.ToString());
		if (!TestTrue(*FString::Printf(TEXT("%s: its Code '%s' is a recognised ICAO letter"),
			Path, *Type->Code.ToString()), Parsed.IsSet()))
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
				Path, Tail, Letter, IcaoCode::MaxTailAftForLetter(Code)),
			Tail >= -IcaoCode::MaxTailAftForLetter(Code));
		TestTrue(
			*FString::Printf(TEXT("%s: its nose at %.0f is within code %s's %.0f"),
				Path, Nose, Letter, IcaoCode::MaxNoseFwdForLetter(Code)),
			Nose <= IcaoCode::MaxNoseFwdForLetter(Code));
		TestEqual(
			*FString::Printf(TEXT("%s: its measured span of %.0f uu is code %s's, which is the "
				"letter it is authored at"), Path, Type->Footprint.Wingspan, Letter),
			IcaoCode::LetterForWingspan(Type->Footprint.Wingspan), FString(Letter));

		const double WingLine = Type->Footprint.WingX - ToStopMark;
		TestTrue(
			*FString::Printf(TEXT("%s: its wing line at %.0f is inside code %s's %.0f .. %.0f"),
				Path, WingLine, Letter,
				IcaoCode::WingAftForLetter(Code), IcaoCode::WingFwdForLetter(Code)),
			WingLine >= IcaoCode::WingAftForLetter(Code)
				&& WingLine <= IcaoCode::WingFwdForLetter(Code));

		// SAID OUT LOUD WHETHER OR NOT IT PASSES, because the useful question when one of these
		// fails is "by how much", and re-deriving it means loading the asset by hand.
		AddInfo(FString::Printf(
			TEXT("%s is code %s: nose %.0f/%.0f, tail %.0f/%.0f, span %.0f, wing %.0f"),
			Path, Letter, Nose, IcaoCode::MaxNoseFwdForLetter(Code),
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
	struct FExpected
	{
		const TCHAR* Path;
		EPushbackNeed Need;
		const TCHAR* Why;
	};

	const FExpected Expected[] = {
		{ TEXT("/Game/Entities/DA_Aircraft_Plane1"), EPushbackNeed::SelfManoeuvre,
		  TEXT("a 172 is pushed off a stand by one person leaning on the strut - and the "
			   "class default is VehicleTug, so this row is the only thing standing between "
			   "the smallest aeroplane in the game and the Pushback depot") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane7"), EPushbackNeed::SelfManoeuvre,
		  TEXT("the starter aeroplane reverses itself, so a new airport needs no depot - and "
			   "this row is what proves the 2026-09-21 RENAME of DA_Aircraft_Piper carried "
			   "its authored values rather than quietly creating a fresh asset on defaults, "
			   "which would read VehicleTug and gate a Meridian behind the depot") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane2"), EPushbackNeed::SelfManoeuvre,
		  TEXT("a Twin Otter beta-ranges off a stand") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane3"), EPushbackNeed::SelfManoeuvre,
		  TEXT("a Q400 turns out of a regional stand on its own props; the depot is the jets' tax") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane6"), EPushbackNeed::VehicleTug,
		  TEXT("a 350 t 777-300ER is the far end of the same progression - and this row is "
			   "the only thing distinguishing an asset authored VehicleTug from one nobody "
			   "authored at all, since the class default says the same thing") },
		{ TEXT("/Game/Entities/DA_Aircraft_Plane8"), EPushbackNeed::VehicleTug,
		  TEXT("a 575 t A380 is the end of the progression - nothing heavier flies - and the "
			   "row exists for the reason plane6's does: authored VehicleTug and default "
			   "VehicleTug read the same, so only the table can tell them apart") },
		{ TEXT("/Game/Entities/DA_Aircraft_A320"),   EPushbackNeed::VehicleTug,
		  TEXT("an A320 is what forces the Pushback depot") },
		{ TEXT("/Game/Entities/DA_Aircraft_B738"),   EPushbackNeed::VehicleTug,
		  TEXT("and so is a 737") },
	};

	for (const FExpected& Each : Expected)
	{
		UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
			UAircraftType::StaticClass(), nullptr, Each.Path));
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Each.Path), Type))
		{
			continue;
		}

		TestEqual(Each.Why, Type->PushbackNeed, Each.Need);

		// AND IT SURVIVES THE FLATTENING for this particular asset, not just for the
		// hand-built type in AirframeAxles above: the game reads FAirframe, never the DA.
		TestEqual(*FString::Printf(TEXT("%s carries it into the airframe"), Each.Path),
			Type->Airframe().PushbackNeed, Each.Need);
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
