#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Solve/IcaoCode.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLayoutFitsItsLettersFloorTest,
	"Airside.Entities.StandLayoutFitsItsLettersFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLayoutFitsItsLettersFloorTest::RunTest(const FString& Parameters)
{
	// THE TEMPLATE IS BUILT FOR THE FLOOR OF ITS BAND. 45 m to just under 67 is all Code C, and
	// a template authored at a comfortable 55 would fail exactly where a player drew the
	// smallest stand the rules allow. So the assertion is against the letter's MINIMUM, which
	// is what IcaoCode reports.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	const double Width = IcaoCode::StandWidthForLetter(TEXT("C"));
	const double Depth = IcaoCode::StandDepthForLetter(TEXT("C"));

	AddInfo(FString::Printf(TEXT("Code C floor %.0f x %.0f uu; template needs %.0f x %.0f"),
		Width, Depth, Stand->RequiredExtent.X, Stand->RequiredExtent.Y));

	TestTrue(
		*FString::Printf(TEXT("the layout fits the width floor (%.0f needed, %.0f available)"),
			Stand->RequiredExtent.X, Width),
		Stand->RequiredExtent.X <= Width);
	TestTrue(
		*FString::Printf(TEXT("the layout fits the depth floor (%.0f needed, %.0f available)"),
			Stand->RequiredExtent.Y, Depth),
		Stand->RequiredExtent.Y <= Depth);

	// AND IT REPORTS ITS OWN LETTER. RequiredExtent is width-then-depth precisely so it can be
	// asked this, and a layout that quietly grew past its band would answer D here while still
	// passing the two bounds above against a letter nobody re-derived.
	TestEqual(TEXT("the layout's own extent still reads as a Code C stand"),
		IcaoCode::LetterForStandSize(Stand->RequiredExtent.X, Stand->RequiredExtent.Y),
		FString(TEXT("C")));

	// A BAY PER SERVICE THAT A VEHICLE DRIVES TO. An anchor a truck visits with no bay to park
	// in is the pile-up this design exists to prevent.
	for (const FEntityAnchor& Anchor : Stand->Anchors)
	{
		if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
		{
			continue;
		}
		const FServiceBay* Bay = Stand->ServiceBays.FindByPredicate(
			[&Anchor](const FServiceBay& Candidate) { return Candidate.AnchorId == Anchor.Id; });
		TestNotNull(
			*FString::Printf(TEXT("anchor '%s' has a bay"), *Anchor.Id.ToString()), Bay);
	}

	// THE STAGING RANK HOLDS MORE THAN ONE. A single pose queues arriving trucks on the road
	// outside and blocks it, which is what "so the vehicles don't pile up" rules out.
	TestTrue(
		*FString::Printf(TEXT("staging is a rank, not a point (holds %d)"), Stand->StagingCapacity),
		Stand->StagingCapacity > 1);

	// AND THE ENTRY IS ON THE BACK EDGE, which is the one thing placement will VALIDATE rather
	// than solve. An entry that drifted inboard would be an entry no road could ever meet, and
	// every stand would be refused with a reason that read as a content bug.
	const double BackX =
		IcaoCode::MaxNoseFwdForLetter(TEXT("C")) - IcaoCode::StandDepthForLetter(TEXT("C"));
	TestEqual(
		*FString::Printf(TEXT("the entry is on the stand's aft edge (%.0f)"), Stand->EntryLocal.X),
		Stand->EntryLocal.X, BackX, 0.5);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandExtentClearsTheLargestAirframeAdmittedTest,
	"Airside.Entities.StandExtentClearsTheLargestAirframeAdmitted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandExtentClearsTheLargestAirframeAdmittedTest::RunTest(const FString& Parameters)
{
	// A LIVE DEFECT THIS FIXES. The old geometry was sized from the A320's tail at -3250, but
	// DA_Aircraft_B738 is authored at -3430 and already parks on the same stand - 1.2 m of tail
	// clearance where 3 was intended. Sizing from the largest airframe the LETTER admits, never
	// from a named one, is the rule the taxiway widths and the service road fillet already
	// follow.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);
	UAircraftType* B738 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::Build737(B738);

	TestTrue(TEXT("the 737-800 really is the longer of the two"),
		B738->Footprint.TailX < A320->Footprint.TailX);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// Nothing in the layout may sit inside the largest admitted airframe's tail clearance.
	const double Aft = B738->Footprint.TailX;
	for (const FServiceBay& Bay : Stand->ServiceBays)
	{
		TestTrue(
			*FString::Printf(TEXT("bay '%s' at (%.0f, %.0f) is clear of the longest tail at %.0f"),
				*Bay.AnchorId.ToString(), Bay.Local.X, Bay.Local.Y, Aft),
			Bay.Local.X > Aft || FMath::Abs(Bay.Local.Y) > B738->Footprint.Wingspan * 0.5);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryAirframeFitsItsLettersRowTest,
	"Airside.Entities.EveryAirframeFitsItsLettersRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryAirframeFitsItsLettersRowTest::RunTest(const FString& Parameters)
{
	// TWO LISTS THAT MUST AGREE, SO THE CONSUMER CHECKS THEM. IcaoCode's MaxTailAft and
	// MaxNoseFwd say how long an airframe a letter admits, and the UAircraftType builders say
	// how long each type actually is. Nothing makes those one list, so this is the check - by
	// NAME, and logging both figures, which is CLAUDE.md's rule for a second list UE forces.
	//
	// IT LIVES HERE rather than beside the table because the reason the figures matter is the
	// stand layout: a type longer than its letter's row would have the stand's ground geometry
	// laid inside its own tail clearance, which is the defect this piece exists to fix.
	//
	// THE BUILDERS, NOT THE ASSETS. A type authored only as a DA_Aircraft_* uasset - Plane2 is
	// one - is invisible here, because a test may not load content. That is a real gap and it
	// is named rather than papered over: what this catches is a BUILDER drifting past its row.
	struct FCase { const TCHAR* What; void (*Build)(UAircraftType*); };
	const FCase Cases[] = {
		{ TEXT("A320"), &UAircraftType::BuildA320 },
		{ TEXT("737-800"), &UAircraftType::Build737 },
		{ TEXT("Piper Meridian"), &UAircraftType::BuildPiperMeridian },
	};

	for (const FCase& Case : Cases)
	{
		UAircraftType* Type = NewObject<UAircraftType>(GetTransientPackage());
		Case.Build(Type);

		// Code is an FName on UAircraftType; IcaoCode speaks FString, as every other caller
		// of it does.
		const FString Letter = Type->Code.ToString();
		const double TailAft = IcaoCode::MaxTailAftForLetter(Letter);
		const double NoseFwd = IcaoCode::MaxNoseFwdForLetter(Letter);

		// CONVERTED TO NOSE-GEAR COORDINATES FIRST, because the row is stated about the stop
		// mark and a footprint is stated about whatever origin its type declares. Zero means
		// the origin IS the steered axle (see FAirframe::SteerAxleX); the Piper declares
		// 237.8, its main gear, so its nose reads 385.1 raw and 147.3 converted. The first
		// draft of this test skipped the conversion and asserted in a comment that it could
		// not matter - it mattered on the first run, and by 85 uu.
		const double ToStopMark = Type->SteerAxleX;
		const double Nose = Type->Footprint.NoseX - ToStopMark;
		const double Tail = Type->Footprint.TailX - ToStopMark;

		TestTrue(
			*FString::Printf(TEXT("%s's tail at %.0f (raw %.0f) is within code %s's %.0f"),
				Case.What, Tail, Type->Footprint.TailX, *Letter, TailAft),
			Tail >= -TailAft);
		TestTrue(
			*FString::Printf(TEXT("%s's nose at %.0f (raw %.0f) is within code %s's %.0f"),
				Case.What, Nose, Type->Footprint.NoseX, *Letter, NoseFwd),
			Nose <= NoseFwd);
		TestTrue(
			*FString::Printf(TEXT("%s's span of %.0f is within code %s"),
				Case.What, Type->Footprint.Wingspan, *Letter),
			IcaoCode::LetterForWingspan(Type->Footprint.Wingspan) == Letter);

		// AND ITS WING IS INSIDE ITS LETTER'S KEEP-OUT. The keep-out is authored as the union
		// of every wing the letter admits, and WingX is the only wing datum any type carries -
		// so this is what stops the two drifting. A type whose wing line fell outside the box
		// would be one the painted no-entry marking does not cover.
		const double WingLine = Type->Footprint.WingX - ToStopMark;
		TestTrue(
			*FString::Printf(TEXT("%s's wing line at %.0f is inside code %s's %.0f .. %.0f"),
				Case.What, WingLine, *Letter,
				IcaoCode::WingAftForLetter(Letter), IcaoCode::WingFwdForLetter(Letter)),
			WingLine >= IcaoCode::WingAftForLetter(Letter)
				&& WingLine <= IcaoCode::WingFwdForLetter(Letter));
	}

	return true;
}

#endif
