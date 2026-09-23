#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ReverseRun.h"
#include "Model/RoadEntity.h"
#include "Model/SpeedProfile.h"
#include "Solve/IcaoCode.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace StandLayoutFixture
{
	/** The letter the shipping stand is built for. One place, so a test cannot name another. */
	static constexpr EIcaoCode Letter = EIcaoCode::C;

	/** Every leg of every bay, named, so a failure says which one. */
	struct FNamedLeg
	{
		FString What;
		const FStandLeg* Leg = nullptr;
		bool bReverse = false;
	};

	inline TArray<FNamedLeg> LegsOf(const UEntityDefinition& Stand)
	{
		TArray<FNamedLeg> Out;
		for (const FServiceBay& Bay : Stand.ServiceBays)
		{
			const FString Who = Bay.AnchorId.ToString();
			Out.Add({ Who + TEXT(" arrive"), &Bay.ArriveLeg, false });
			Out.Add({ Who + TEXT(" serve"), &Bay.ServeLeg, false });
			Out.Add({ Who + TEXT(" reverse"), &Bay.ReverseLeg, true });
			Out.Add({ Who + TEXT(" depart"), &Bay.DepartLeg, false });
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLayoutFitsItsLettersFloorTest,
	"Airside.Entities.StandLayoutFitsItsLettersFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLayoutFitsItsLettersFloorTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// THE TEMPLATE IS BUILT FOR THE FLOOR OF ITS BAND. 53 m to just under 75 is all Code C, and
	// a template authored at a comfortable 60 would fail exactly where a player drew the
	// smallest stand the rules allow. So the assertion is against the letter's MINIMUM, which
	// is what IcaoCode reports.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	const double Width = IcaoCode::StandWidthForLetter(Letter);
	const double Depth = IcaoCode::StandDepthForLetter(Letter);

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
		FString(IcaoCode::ToLetter(Letter)));

	// A BAY PER SERVICE A GROUND VEHICLE DRIVES TO, which is narrower than "not the aeroplane"
	// in two ways that both matter. The TUG is a ground vehicle and still gets none: pushback
	// couples at the nose gear and is FPushbackRun's manoeuvre. The PASSENGER DOOR is not a
	// ground vehicle at all - an air bridge is a structure - so it gets none either, and asking
	// the loose question would have handed it four legs and a road entry nothing would use.
	for (const FEntityAnchor& Anchor : Stand->Anchors)
	{
		const bool bWants = TraversalForRole(Anchor.Role) == ETraversalClass::GroundVehicle
			&& Anchor.Role != EServiceRole::Tug;
		const FServiceBay* Bay = Stand->ServiceBays.FindByPredicate(
			[&Anchor](const FServiceBay& Candidate) { return Candidate.AnchorId == Anchor.Id; });

		TestEqual(
			*FString::Printf(TEXT("anchor '%s' has a bay only if a vehicle services from one"),
				*Anchor.Id.ToString()),
			Bay != nullptr, bWants);
	}

	// EVERY LEG EXISTS AND CARRIES ITS CONTROLS. A chain whose controls do not match its points
	// is one IsSet() refuses, and a refused leg reaches the graph as nothing at all - a bay no
	// vehicle can get to, reported by no warning.
	for (const FNamedLeg& Named : LegsOf(*Stand))
	{
		TestTrue(*FString::Printf(TEXT("%s is a complete chain"), *Named.What),
			Named.Leg->IsSet());
	}

	// AND THE LEGS MEET. A corner that fell BETWEEN two legs would be judged by nothing, which
	// is the exact defect this piece exists to remove: a route of individually-legal edges can
	// still be illegal where two meet. Measured as position, because that is what the graph
	// welds on - the builder hands both legs the same node.
	for (const FServiceBay& Bay : Stand->ServiceBays)
	{
		const FString Who = Bay.AnchorId.ToString();
		if (!Bay.ArriveLeg.IsSet() || !Bay.ServeLeg.IsSet()
			|| !Bay.ReverseLeg.IsSet() || !Bay.DepartLeg.IsSet())
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: arrive ends where serve starts"), *Who),
			Bay.ArriveLeg.Points.Last().Equals(Bay.ServeLeg.Points[0], 0.01));
		TestTrue(*FString::Printf(TEXT("%s: serve ends where reverse starts"), *Who),
			Bay.ServeLeg.Points.Last().Equals(Bay.ReverseLeg.Points[0], 0.01));
		TestTrue(*FString::Printf(TEXT("%s: reverse ends where depart starts"), *Who),
			Bay.ReverseLeg.Points.Last().Equals(Bay.DepartLeg.Points[0], 0.01));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryTemplateLegIsDrivableByEveryVehicleTest,
	"Airside.Entities.EveryTemplateLegIsDrivableByEveryVehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryTemplateLegIsDrivableByEveryVehicleTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// THE PROPERTY THE WHOLE PIECE EXISTS TO GET. Drivability is a property of the TEMPLATE,
	// verified once for every vehicle - not of every placement. Four attempts failed because
	// each derived geometry per stand and so had to re-prove it per stand; placement is now a
	// transform, and a transform preserves curvature.
	//
	// ASKED OF THE AUTHORITIES, never re-derived: forward legs of FSpeedProfile, reverse legs
	// of FReverseRun::Start, which already refuses what it cannot hold. Restating a JUDGEMENT
	// in a test is what let four attempts ship green and crab in PIE.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	TArray<TPair<FString, FChassis>> Fleet;
	Fleet.Emplace(TEXT("default vehicle"), UAirsideSettings::ResolveDefaultVehicle().Chassis);
	const FChassis Largest = UAirsideSettings::ResolveLargestServiceVehicle();
	if (!FMath::IsNearlyEqual(Largest.Wheelbase(), Fleet[0].Value.Wheelbase(), 0.01))
	{
		Fleet.Emplace(TEXT("largest service vehicle"), Largest);
	}

	for (const TPair<FString, FChassis>& Vehicle : Fleet)
	{
		for (const FNamedLeg& Named : LegsOf(*Stand))
		{
			const FRoutePlan Plan = Named.Leg->ToPlan();
			const FString Who = FString::Printf(TEXT("%s: %s"), *Vehicle.Key, *Named.What);

			if (!TestTrue(*FString::Printf(TEXT("%s is a usable plan"), *Who), Plan.IsValid()))
			{
				continue;
			}

			if (Named.bReverse)
			{
				// ASKED OF THE MANOEUVRE ITSELF, not of a profile built here. FReverseRun::Start
				// is what will actually arm it in play, and it refuses a curve it cannot hold
				// AND a sharp vertex, separately. Anything else is a second evaluator.
				FReverseRun Run;
				TestTrue(*FString::Printf(TEXT("%s arms as a reverse"), *Who),
					Run.Start(Plan, Vehicle.Value, /*InReverseSpeed=*/100.0));
				continue;
			}

			FSpeedProfile Profile;
			Profile.Build(Plan.Polyline, Vehicle.Value, EDriveDirection::Forward);
			TestFalse(
				*FString::Printf(TEXT("%s holds the forward limit (tightest %.0f at %.0f)"),
					*Who, Profile.GetTightestRadius(), Profile.GetTightestAt()),
				Profile.WasTighterThanLock());
			TestFalse(
				*FString::Printf(TEXT("%s has no instant turn (sharpest %.0f deg)"),
					*Who, Profile.GetSharpestDegrees()),
				Profile.HasSharpVertex());
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNoTemplateLegPassesUnderTheWingTest,
	"Airside.Entities.NoTemplateLegPassesUnderTheWing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNoTemplateLegPassesUnderTheWingTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// NOTHING DRIVES UNDER A WING. Ruled 2026-09-17, and the keep-out is every wing the letter
	// admits laid over one another - which is what a real apron paints as a no-entry box,
	// because the marking cannot be repainted for each arrival.
	//
	// SEGMENT BY SEGMENT, never point by point. A leg checked at its samples alone steps clean
	// over a corner of the box between two of them and reports itself clear; that is the same
	// class of defect as a per-edge drivability test that cannot see a join, and it is the one
	// this whole piece was written to stop repeating.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	for (const FNamedLeg& Named : LegsOf(*Stand))
	{
		TArray<FVector2D> Sampled;
		Named.Leg->Sample(Sampled);

		for (int32 At = 0; At + 1 < Sampled.Num(); ++At)
		{
			if (IcaoCode::WingKeepOutCrossedBy(Letter, Sampled[At], Sampled[At + 1]))
			{
				AddError(FString::Printf(
					TEXT("%s passes under the wing between (%.0f, %.0f) and (%.0f, %.0f); "
					     "code %s's keep-out runs x %.0f..%.0f"),
					*Named.What, Sampled[At].X, Sampled[At].Y,
					Sampled[At + 1].X, Sampled[At + 1].Y, IcaoCode::ToLetter(Letter),
					IcaoCode::WingAftForLetter(Letter), IcaoCode::WingFwdForLetter(Letter)));
				break;
			}
		}
	}

	// AND NO FIXTURE SITS IN IT EITHER. A service point under a wing is one no vehicle could
	// ever be sent to, so it is an authoring error rather than a routing one - which is why it
	// is caught here, against the definition, and not in a traffic test.
	for (const FEntityAnchor& Anchor : Stand->Anchors)
	{
		TestFalse(
			*FString::Printf(TEXT("'%s' at (%.0f, %.0f) is clear of the wing"),
				*Anchor.Id.ToString(), Anchor.LocalPosition.X, Anchor.LocalPosition.Y),
			IcaoCode::WingKeepOutContains(Letter, Anchor.LocalPosition));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandExtentClearsTheLargestAirframeAdmittedTest,
	"Airside.Entities.StandExtentClearsTheLargestAirframeAdmitted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandExtentClearsTheLargestAirframeAdmittedTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// A LIVE DEFECT THIS FIXES. The old geometry was sized from the A320's tail at -3250, but
	// DA_Aircraft_B738 is authored at -3538 and already parks on the same stand - 0.1 m of tail
	// clearance where 3 was intended. Sizing from the largest airframe the LETTER admits, never
	// from a named one, is the rule the taxiway widths and the service road fillet already
	// follow.
	UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::BuildA320(A320);
	UAircraftType* B738 = NewObject<UAircraftType>(GetTransientPackage());
	UAircraftType::Build737(B738);

	TestTrue(TEXT("the 737-800 really is the longer of the two"),
		B738->Footprint.TailX < A320->Footprint.TailX);
	TestTrue(TEXT("and the letter is sized for it, not for the design aircraft"),
		IcaoCode::MaxTailAftForLetter(Letter) >= -B738->Footprint.TailX);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// Nothing in the layout may sit inside the largest admitted airframe's tail clearance.
	const double Aft = B738->Footprint.TailX;
	for (const FServiceBay& Bay : Stand->ServiceBays)
	{
		for (const FVector2D& Pose : { Bay.ParkLocal, Bay.EntryLocal, Bay.ExitLocal })
		{
			TestTrue(
				*FString::Printf(TEXT("bay '%s' pose (%.0f, %.0f) clears the longest tail at %.0f"),
					*Bay.AnchorId.ToString(), Pose.X, Pose.Y, Aft),
				Pose.X < Aft || FMath::Abs(Pose.Y) > B738->Footprint.Wingspan * 0.5);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryAirframeFitsItsLettersRowTest,
	"Airside.Entities.EveryAirframeFitsItsLettersRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryAirframeFitsItsLettersRowTest::RunTest(const FString& Parameters)
{
	// TWO LISTS THAT MUST AGREE, SO THE CONSUMER CHECKS THEM. IcaoCode's MaxTailAft, MaxNoseFwd
	// and wing band say how large an airframe a letter admits, and the UAircraftType builders
	// say how large each type actually is. Nothing makes those one list, so this is the check -
	// by NAME, and logging both figures, which is CLAUDE.md's rule for a second list UE forces.
	//
	// IT LIVES HERE rather than beside the table because the reason the figures matter is the
	// stand layout: a type longer than its letter's row would have the stand's ground geometry
	// laid inside its own tail clearance, which is the defect this piece exists to fix.
	//
	// THE BUILDERS, NOT THE ASSETS. A type authored only as a DA_Aircraft_* uasset - Plane2 is
	// one - is invisible here, because a test may not load content. That is a real gap and it
	// is named rather than papered over: what this catches is a BUILDER drifting past its row.
	//
	// A FACTORY, NOT A BUILDER, so the Piper case can be TestAirframes::PiperType() itself
	// (issue #194: every other test site that needs a built Piper now goes through it,
	// rather than repeating NewObject<UAircraftType>() plus BuildPiperMeridian by hand).
	// A320 and 737 keep their own NewObject call inside a matching lambda rather than
	// growing a fixture of their own - nothing else in the module needs "a bare A320".
	struct FCase { const TCHAR* What; UAircraftType* (*Make)(); };
	const FCase Cases[] = {
		{ TEXT("A320"), []() { UAircraftType* T = NewObject<UAircraftType>(GetTransientPackage()); UAircraftType::BuildA320(T); return T; } },
		{ TEXT("737-800"), []() { UAircraftType* T = NewObject<UAircraftType>(GetTransientPackage()); UAircraftType::Build737(T); return T; } },
		{ TEXT("Piper Meridian"), &TestAirframes::PiperType },
	};

	for (const FCase& Case : Cases)
	{
		UAircraftType* Type = Case.Make();

		// PARSED, NOT TRUSTED. UAircraftType::Code is an EditAnywhere FName - the same
		// authored field AnchorLink::RadiusForCode reads off a placed stand's DesignAircraft -
		// so this is exactly the site IcaoCode::Parse exists for: a builder that ever
		// mistyped its own Code now fails THIS assertion by name, instead of every figure
		// below silently being measured against Code C.
		const TOptional<EIcaoCode> ParsedCode = IcaoCode::Parse(Type->Code.ToString());
		if (!TestTrue(
			*FString::Printf(TEXT("%s's Code '%s' is a recognised ICAO letter"),
				Case.What, *Type->Code.ToString()),
			ParsedCode.IsSet()))
		{
			continue;
		}
		const EIcaoCode Code = *ParsedCode;
		const TCHAR* Letter = IcaoCode::ToLetter(Code);
		const double TailAft = IcaoCode::MaxTailAftForLetter(Code);
		const double NoseFwd = IcaoCode::MaxNoseFwdForLetter(Code);

		// CONVERTED TO NOSE-GEAR COORDINATES FIRST, because the row is stated about the stop
		// mark and a footprint is stated about whatever origin its type declares. Zero means
		// the origin IS the steered axle (see FChassis::SteerAxleX).
		//
		// THE CONVERSION IS A NO-OP FOR ALL THREE TYPES TODAY and it STAYS, which is the
		// point worth recording. It was here because the Piper declared 237.8 - its main gear
		// - so its nose read 385.1 raw and 147.3 converted; plane7 brought that type onto the
		// nose-gear origin on 2026-09-21 and every row now converts by zero. The first draft
		// of this test skipped the conversion and asserted in a comment that it could not
		// matter. It mattered on the first run, and by 85 uu. Deleting it now because no
		// current type exercises it would be making that same claim a second time, against a
		// field FAirframe still supports and a vehicle still uses.
		const double ToStopMark = Type->SteerAxleX;
		const double Nose = Type->Footprint.NoseX - ToStopMark;
		const double Tail = Type->Footprint.TailX - ToStopMark;

		TestTrue(
			*FString::Printf(TEXT("%s's tail at %.0f (raw %.0f) is within code %s's %.0f"),
				Case.What, Tail, Type->Footprint.TailX, Letter, TailAft),
			Tail >= -TailAft);
		TestTrue(
			*FString::Printf(TEXT("%s's nose at %.0f (raw %.0f) is within code %s's %.0f"),
				Case.What, Nose, Type->Footprint.NoseX, Letter, NoseFwd),
			Nose <= NoseFwd);
		TestTrue(
			*FString::Printf(TEXT("%s's span of %.0f is within code %s"),
				Case.What, Type->Footprint.Wingspan, Letter),
			IcaoCode::LetterForWingspan(Type->Footprint.Wingspan) == Letter);

		// AND ITS WING IS INSIDE ITS LETTER'S KEEP-OUT. The keep-out is authored as the union
		// of every wing the letter admits, and WingX is the only wing datum any type carries -
		// so this is what stops the two drifting. A type whose wing line fell outside the box
		// would be one the painted no-entry marking does not cover.
		const double WingLine = Type->Footprint.WingX - ToStopMark;
		TestTrue(
			*FString::Printf(TEXT("%s's wing line at %.0f is inside code %s's %.0f .. %.0f"),
				Case.What, WingLine, Letter,
				IcaoCode::WingAftForLetter(Code), IcaoCode::WingFwdForLetter(Code)),
			WingLine >= IcaoCode::WingAftForLetter(Code)
				&& WingLine <= IcaoCode::WingFwdForLetter(Code));
	}

	return true;
}

#endif
