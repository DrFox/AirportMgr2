#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/StandLayoutBuild.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ReverseRun.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/SpeedProfile.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/IcaoCode.h"
#include "Solve/StandBox.h"
#include "StandFixture.h"

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

	/** A whole-route verdict, beside the plan it was reached on. */
	struct FVerdictAndPlan
	{
		FFitVerdict Verdict;
		FRoutePlan Plan;
	};

	/**
	 * One bay's four legs, chained into the plan a tow drives through it (arrive, serve,
	 * reverse flagged, depart) and judged by VehicleFit::JudgePlan - the router's own whole-route
	 * check, which solves the reverse from the chain the serve leg left.
	 */
	inline FVerdictAndPlan JudgeBay(const FServiceBay& Bay, const FVehicle& Vehicle, const URoadNetwork& Network)
	{
		TArray<TestPlans::FRun> Runs;
		const TPair<const FStandLeg*, bool> Legs[] = {
			{ &Bay.ArriveLeg, false }, { &Bay.ServeLeg, false }, { &Bay.ReverseLeg, true }, { &Bay.DepartLeg, false } };
		for (const TPair<const FStandLeg*, bool>& Leg : Legs)
		{
			TestPlans::FRun& Run = Runs.AddDefaulted_GetRef();
			Leg.Key->Sample(Run.Points);
			Run.bReverse = Leg.Value;
		}
		FVerdictAndPlan Out;
		Out.Plan = TestPlans::Chain(Runs);
		Out.Verdict = VehicleFit::JudgePlan(Out.Plan, Vehicle, Network);
		return Out;
	}

	/**
	 * JudgeBay with a LEAD-IN in front of the bay: a road straight along the far edge, then a
	 * square turn at Radius onto the entry's own heading, ending on the entry. bFromPort picks
	 * which way along the road it comes, since the turn's hand decides which way the trailer
	 * swings.
	 *
	 * WHY: JudgeBay alone starts the tow STRAIGHT at the entry, and no tow arrives that way - it
	 * has just turned off the road. Measured on the utility tow reaching a Code A entry off a
	 * far-edge road (AirportOps.Fuel.TowServesCodeA, 2026-09-27, since deleted - see the note in FuelServiceTest.cpp): turntable 35.7 deg in the
	 * corner and still 12.9 at the entry. A square turn at the design vehicle's own template
	 * radius (LegSlack times its tightest) is the tightest a road join lays for it, so it bends
	 * the chain at least as hard as a real lead-in does.
	 */
	inline FVerdictAndPlan JudgeBayArrivingBent(const FServiceBay& Bay, const FVehicle& Vehicle,
		const URoadNetwork& Network, double Radius, bool bFromPort)
	{
		const FVector2D Entry = Bay.EntryLocal;
		const FVector2D Heading(FMath::Cos(Bay.EntryHeading), FMath::Sin(Bay.EntryHeading));
		// THE TURN'S CENTRE, on the inside: left of the final heading for a turn that ends
		// turning left (arriving from port), right of it for the other hand.
		const FVector2D Left(-Heading.Y, Heading.X);
		const double Hand = bFromPort ? 1.0 : -1.0;
		const FVector2D Centre = Entry + Left * Hand * Radius;
		TArray<TestPlans::FRun> Runs;
		TestPlans::FRun& Road = Runs.AddDefaulted_GetRef();
		// Back along the entry heading by R from the centre is the arc's start; the road runs
		// into it along the final heading's perpendicular.
		const FVector2D ArcStart = Centre - Heading * Radius;
		const FVector2D Along = -Left * Hand;   // the direction of travel on the road
		for (double D = 3000.0; D > 0.0; D -= 100.0)
		{
			Road.Points.Add(ArcStart - Along * D);
		}
		TestPlans::FRun& Turn = Road;
		constexpr int32 Samples = 36;
		const FVector2D From = ArcStart - Centre;
		for (int32 K = 0; K <= Samples; ++K)
		{
			const double A = Hand * UE_DOUBLE_HALF_PI * K / Samples;
			const FVector2D R(From.X * FMath::Cos(A) - From.Y * FMath::Sin(A), From.X * FMath::Sin(A) + From.Y * FMath::Cos(A));
			Turn.Points.Add(Centre + R);
		}
		const TPair<const FStandLeg*, bool> Legs[] = {
			{ &Bay.ArriveLeg, false }, { &Bay.ServeLeg, false }, { &Bay.ReverseLeg, true }, { &Bay.DepartLeg, false } };
		for (const TPair<const FStandLeg*, bool>& Leg : Legs)
		{
			TestPlans::FRun& Run = Runs.AddDefaulted_GetRef();
			Leg.Key->Sample(Run.Points);
			Run.bReverse = Leg.Value;
		}
		// THE ROAD AND THE BAY'S ARRIVE LEG ARE ONE FORWARD RUN in play (no reverse between
		// them), so they are one run here: the chain carries its bend straight into the arrive leg.
		Runs[0].Points.Pop();
		Runs[0].Points.Append(Runs[1].Points);
		Runs.RemoveAt(1);
		FVerdictAndPlan Out;
		Out.Plan = TestPlans::Chain(Runs);
		Out.Verdict = VehicleFit::JudgePlan(Out.Plan, Vehicle, Network);
		return Out;
	}

	/** Every letter the drawn-stand tool offers, A to F, in declaration order. */
	inline TArray<EIcaoCode> AllLetters()
	{
		TArray<EIcaoCode> Out;
		for (int32 Index = 0; Index <= static_cast<int32>(EIcaoCode::F); ++Index)
		{
			Out.Add(static_cast<EIcaoCode>(Index));
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
	//
	// PER LETTER, BY ITS DESIGN VEHICLE AND EVERY VEHICLE NO LARGER (user 2026-09-26): each
	// letter's template is laid for UAirsideSettings::ResolveStandDesignVehicle(Letter), and a
	// smaller vehicle may serve it too. "Smaller" is VehicleFit::NoLargerThan, strict on all four
	// axes - so the tow (chain 575) is NOT smaller than the truck (355) and is not asked to drive
	// a C-F stand here. A TOW is judged through VehicleFit::JudgePlan over the whole bay - arrive,
	// serve, reverse, depart - because a trailer's fold is a property of the chain it arrives
	// with, which no one leg on its own can show.
	const TArray<FVehicle> Fleet = {
		UAirsideSettings::ResolveUtilityTowVehicle(), UAirsideSettings::ResolveDefaultVehicle() };
	URoadNetwork* Network = NewObject<URoadNetwork>();

	for (const EIcaoCode StandLetter : AllLetters())
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		const FVehicle Design = UAirsideSettings::ResolveStandDesignVehicle(StandLetter);

		for (const FVehicle& Vehicle : Fleet)
		{
			if (!VehicleFit::NoLargerThan(Vehicle, Design))
			{
				continue;
			}
			const FString Vehicular = FString::Printf(TEXT("Code %s, %s"),
				IcaoCode::ToLetter(StandLetter), *Vehicle.TypeCode.ToString());
			const bool bTow = !Vehicle.Tow.IsEmpty();

			for (const FNamedLeg& Named : LegsOf(*Stand))
			{
				const FRoutePlan Plan = Named.Leg->ToPlan();
				const FString Who = FString::Printf(TEXT("%s: %s"), *Vehicular, *Named.What);

				if (!TestTrue(*FString::Printf(TEXT("%s is a usable plan"), *Who), Plan.IsValid()))
				{
					continue;
				}

				if (Named.bReverse)
				{
					// ASKED OF THE MANOEUVRE ITSELF, not of a profile built here. FReverseRun::Start
					// is what will actually arm it in play, and it refuses a curve it cannot hold
					// AND a sharp vertex, separately. Anything else is a second evaluator. A tow's
					// reverse is FTowReverseRun's instead, judged below with the rest of its bay.
					if (!bTow)
					{
						FReverseRun Run;
						TestTrue(*FString::Printf(TEXT("%s arms as a reverse"), *Who),
							Run.Start(Plan, Vehicle.Chassis, /*InReverseSpeed=*/100.0));
					}
					continue;
				}

				FSpeedProfile Profile;
				Profile.Build(Plan.Polyline, Vehicle.Chassis, EDriveDirection::Forward);
				TestFalse(
					*FString::Printf(TEXT("%s holds the forward limit (tightest %.0f at %.0f)"),
						*Who, Profile.GetTightestRadius(), Profile.GetTightestAt()),
					Profile.WasTighterThanLock());
				TestFalse(
					*FString::Printf(TEXT("%s has no instant turn (sharpest %.0f deg)"),
						*Who, Profile.GetSharpestDegrees()),
					Profile.HasSharpVertex());
			}

			if (!bTow)
			{
				continue;
			}
			for (const FServiceBay& Bay : Stand->ServiceBays)
			{
				const FVerdictAndPlan Judged = JudgeBay(Bay, Vehicle, *Network);
				TestTrue(*FString::Printf(TEXT("%s: bay '%s' is admitted whole - %s"), *Vehicular,
					*Bay.AnchorId.ToString(), *Judged.Verdict.Describe()), Judged.Verdict.Fits());
			}
		}

		// A MEASUREMENT, NOT AN ASSERTION (controller ruling 2026-09-26): whether the tow could
		// serve a C stand anyway. NoLargerThan says it may not be sent; this says whether the
		// ground would have held it, for the PR to report.
		if (StandLetter == EIcaoCode::C)
		{
			for (const FServiceBay& Bay : Stand->ServiceBays)
			{
				const FFitVerdict Measured = JudgeBay(Bay, Fleet[0], *Network).Verdict;
				AddInfo(FString::Printf(TEXT("MEASURED tow on Code C bay '%s': fits %d - %s"),
					*Bay.AnchorId.ToString(), Measured.Fits() ? 1 : 0, *Measured.Describe()));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryBayHoldsATowArrivingBentTest,
	"Airside.Entities.EveryBayHoldsATowArrivingBent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryBayHoldsATowArrivingBentTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// EveryTemplateLegIsDrivableByEveryVehicle's whole-bay check, from a REALISTIC arrival: the
	// tow has just turned off the far-edge road, both ways round, so its chain reaches the entry
	// bent (see JudgeBayArrivingBent for the figures that justify the lead-in). The bay's settle
	// straight must still bring the turntable inside TowReverse's 3 degree lock before the
	// reverse, or the route home off the service point strands the tow - found 2026-09-27.
	const TArray<FVehicle> Fleet = {
		UAirsideSettings::ResolveUtilityTowVehicle(), UAirsideSettings::ResolveDefaultVehicle() };
	URoadNetwork* Network = NewObject<URoadNetwork>();
	int32 Judged = 0;
	double WorstLeadIn = 0.0;
	for (const EIcaoCode StandLetter : AllLetters())
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		const FVehicle Design = UAirsideSettings::ResolveStandDesignVehicle(StandLetter);
		const double Radius = 1.1 * Design.Chassis.TightestFollowableRadius();
		for (const FVehicle& Vehicle : Fleet)
		{
			if (Vehicle.Tow.IsEmpty() || !VehicleFit::NoLargerThan(Vehicle, Design))
			{
				continue;
			}
			for (const FServiceBay& Bay : Stand->ServiceBays)
			{
				for (const bool bFromPort : { true, false })
				{
					const FVerdictAndPlan Out = JudgeBayArrivingBent(Bay, Vehicle, *Network, Radius, bFromPort);
					++Judged;
					// THE LEAD-IN ON ITS OWN - the road's 30 points and the turn's 36 before the entry, the plan's
					// first 66 - for the worst hitch it leaves: what makes this more than JudgeBay's
					// straight start.
					TArray<TestPlans::FRun> LeadIn;
					LeadIn.AddDefaulted_GetRef().Points.Append(Out.Plan.Polyline.GetData(), 66);
					WorstLeadIn = FMath::Max(WorstLeadIn,
						VehicleFit::JudgePlan(TestPlans::Chain(LeadIn), Vehicle, *Network).Radians);
					TestTrue(*FString::Printf(TEXT("Code %s, %s: bay '%s' arriving off the road from %s is admitted whole - %s (worst hitch %.1f deg)"),
						IcaoCode::ToLetter(StandLetter), *Vehicle.TypeCode.ToString(), *Bay.AnchorId.ToString(),
						bFromPort ? TEXT("port") : TEXT("starboard"), *Out.Verdict.Describe(),
						FMath::RadiansToDegrees(Out.Verdict.Radians)), Out.Verdict.Fits());
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("MEASURED: %d tow bay arrival(s) judged; the lead-in bent the chain up to %.1f deg"),
		Judged, FMath::RadiansToDegrees(WorstLeadIn)));
	TestTrue(TEXT("NOT VACUOUS: some tow bay was judged"), Judged > 0);
	TestTrue(*FString::Printf(TEXT("NOT VACUOUS: the lead-in really bends the chain (worst %.1f deg before the bay)"),
		FMath::RadiansToDegrees(WorstLeadIn)), FMath::RadiansToDegrees(WorstLeadIn) > 10.0);
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
		IcaoCode::FloorEnvelopeForLetter(Letter).MaxTailAft >= -B738->Footprint.TailX);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// Nothing in the layout may sit inside the largest admitted airframe - its [tail..nose] x
	// [+-half-span] box. RE-DERIVED 2026-09-26: this asked only "behind the tail, or outboard",
	// which was the whole question while every pose sat on the aft edge; with the poses on the
	// far edge that clause could no longer fail, and the nose is the end they are near.
	const double Aft = B738->Footprint.TailX;
	const double Fwd = B738->Footprint.NoseX;
	const double HalfSpan = B738->Footprint.Wingspan * 0.5;
	for (const FServiceBay& Bay : Stand->ServiceBays)
	{
		for (const FVector2D& Pose : { Bay.ParkLocal, Bay.EntryLocal, Bay.ExitLocal })
		{
			TestTrue(
				*FString::Printf(TEXT("bay '%s' pose (%.0f, %.0f) is outside the longest airframe's box "
					"x %.0f..%.0f, y +-%.0f"),
					*Bay.AnchorId.ToString(), Pose.X, Pose.Y, Aft, Fwd, HalfSpan),
				Pose.X < Aft || Pose.X > Fwd || FMath::Abs(Pose.Y) > HalfSpan);
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
		const FLetterEnvelope Envelope = IcaoCode::FloorEnvelopeForLetter(Code);
		const double TailAft = Envelope.MaxTailAft;
		const double NoseFwd = Envelope.MaxNoseFwd;

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLayoutEveryLetterBuildsTest,
	"Airside.Entities.StandLayoutEveryLetterBuilds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLayoutEveryLetterBuildsTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// EVERY LETTER BUILDS (user 2026-09-26), where a report test first only recorded which did (it
	// compared FitsItsLetter with a copy of its own body, so it could not fail, and went in #462). A/B were refused because their bays were laid for the truck; each letter is now
	// laid for its own design vehicle. A failure carries both figures, so the log is the evidence
	// a floor is too small - the floors are the user's to change, not this test's.
	//
	// NOT AllLetters() - CODE A DROPPED (2026-09-27 merge): FitsItsLetter(_, A) can never be true
	// any more (LetterForStandSize never answers "A" - see IcaoCode.h's StandLetterFor), so A's
	// own case here would be a permanent, expected failure rather than a floor to fix. B's case
	// already covers the geometry A's template now shares.
	for (const EIcaoCode StandLetter : { EIcaoCode::B, EIcaoCode::C, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		if (!TestNotNull(TEXT("a template"), Stand))
		{
			return false;
		}
		if (!UEntityDefinition::FitsItsLetter(*Stand, StandLetter))
		{
			AddError(FString::Printf(TEXT("Code %s's template needs %.0f x %.0f against its floor %.0f x %.0f"),
				IcaoCode::ToLetter(StandLetter), Stand->RequiredExtent.X, Stand->RequiredExtent.Y,
				IcaoCode::StandWidthForLetter(StandLetter), IcaoCode::StandDepthForLetter(StandLetter)));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryBayContactIsOnTheFarEdgeTest,
	"Airside.Entities.EveryBayContactIsOnTheFarEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryBayContactIsOnTheFarEdgeTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// SERVICE VEHICLES ENTER ONLY BY THE EDGE OPPOSITE THE TAXIWAY (user 2026-09-26). The box
	// runs from the entrance - EntranceSetback behind the stop mark, on the taxiway - to the far
	// edge Depth beyond it; every contact sits Square inside the FAR edge, facing aft, so the
	// service road is drawn beyond the nose and never between the stand and the taxiway.
	//
	// Square IS RESTATED HERE, as a position and not a judgement: the template's LegSlack (1.1)
	// times the design vehicle's right-angle corner run. A contact that drifted from it is one
	// FAnchorLink's lead-in corner no longer has its run for.
	for (const EIcaoCode StandLetter : AllLetters())
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		const FLetterEnvelope& Envelope = UAirsideSettings::ResolveLetterEnvelope(StandLetter);
		const FVehicle Design = UAirsideSettings::ResolveStandDesignVehicle(StandLetter);
		const double BackX = -StandBox::EntranceSetback(StandLetter, Envelope);
		const double FrontX = BackX + IcaoCode::StandDepthForLetter(StandLetter);
		const double Square = 1.1 * GuidelineGeom::CornerRunFor(
			Design.Chassis.TightestFollowableRadius(), UE_DOUBLE_HALF_PI);

		if (!TestTrue(*FString::Printf(TEXT("Code %s has bays"), IcaoCode::ToLetter(StandLetter)),
			Stand->ServiceBays.Num() > 0))
		{
			continue;
		}
		for (const FServiceBay& Bay : Stand->ServiceBays)
		{
			const FString Who = FString::Printf(TEXT("Code %s bay '%s' entry (%.0f, %.0f)"),
				IcaoCode::ToLetter(StandLetter), *Bay.AnchorId.ToString(), Bay.EntryLocal.X, Bay.EntryLocal.Y);
			TestTrue(*FString::Printf(TEXT("%s sits Square inside the far edge at %.0f - service vehicles "
				"enter only by the edge opposite the taxiway (user 2026-09-26)"), *Who, FrontX - Square),
				FMath::IsNearlyEqual(Bay.EntryLocal.X, FrontX - Square, 1.0));
			TestTrue(*FString::Printf(TEXT("%s faces aft, into the stand from the far edge"), *Who),
				FMath::IsNearlyEqual(Bay.EntryHeading, UE_DOUBLE_PI, 1e-6));
			TestTrue(*FString::Printf(TEXT("%s leaves facing the far edge"), *Who),
				FMath::IsNearlyEqual(Bay.ExitHeading, 0.0, 1e-6));
			TestTrue(*FString::Printf(TEXT("%s is also its exit - one contact per side"), *Who),
				Bay.ExitLocal.Equals(Bay.EntryLocal, 0.01));
			TestTrue(*FString::Printf(TEXT("%s is more than Square from the taxiway edge at %.0f"), *Who, BackX),
				Bay.EntryLocal.X - BackX > Square);
		}

		// AND NOTHING THEY DRIVE CROSSES BACK OVER THE ENTRANCE. The ground aft of BackX is the
		// taxiway's (the entrance edge sits on its pavement edge), so a leg that dips behind it -
		// Code A's hydrant reverse did, by 174 uu, when its settle straight ran out of stand -
		// puts a service vehicle where a taxiing wing sweeps. Every SAMPLED point, not just the
		// vertices, since a curve can bulge past a line its corners respect.
		for (const FNamedLeg& Named : LegsOf(*Stand))
		{
			TArray<FVector2D> Sampled;
			Named.Leg->Sample(Sampled);
			double MinX = TNumericLimits<double>::Max();
			for (const FVector2D& At : Sampled)
			{
				MinX = FMath::Min(MinX, At.X);
			}
			TestTrue(*FString::Printf(TEXT("Code %s %s stays inside the entrance edge (aft-most x %.0f, "
				"edge %.0f) - service vehicles never use the taxiway-side ground"),
				IcaoCode::ToLetter(StandLetter), *Named.What, MinX, BackX),
				MinX >= BackX - 0.5);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryBayEntryReachesItsServicePointTest,
	"Airside.Entities.EveryBayEntryReachesItsServicePoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryBayEntryReachesItsServicePointTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;
	using namespace ServiceLinkFixture;

	// THE EQUIVALENCE InspectFacts::DescribeStand's bServiceable RELIES ON (review, 2026-09-26).
	// It walks Network.IsServiceNodeConnected from each bay's SERVICE-POINT node (the resolved
	// anchor - HydrantPit, BaggageHold, FixedGPU...), never from FServiceBay::EntryLocal's own
	// node, because nothing in Model/ may read EntryLocal off Definition->ServiceBays (the
	// Entities layer). That is correct only because StandLayoutBuild::LayLeg lays every bay as
	// ONE CONTINUOUS stand-owned chain - Entry -> Park -> Service -> Cleared -> Exit - so a walk
	// from either end reaches the other without ever crossing an unowned (road) edge. This pins
	// that chain directly, over stand-owned edges only, for every letter and every bay: if a
	// future layout ever forked into two chains that merely TOUCHED at the service point rather
	// than one running through it, this goes red and names which bay broke.
	for (const EIcaoCode StandLetter : AllLetters())
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		if (!TestNotNull(TEXT("a template"), Stand))
		{
			continue;
		}

		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FStandLayoutBuild::Build(*Net);

		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (!TestNotNull(*FString::Printf(TEXT("Code %s placed"), IcaoCode::ToLetter(StandLetter)), Instance))
		{
			continue;
		}

		for (const FServiceBay& Bay : Stand->ServiceBays)
		{
			const FString Who = FString::Printf(
				TEXT("Code %s bay '%s'"), IcaoCode::ToLetter(StandLetter), *Bay.AnchorId.ToString());

			FGuidelineNodeId ServiceNode;
			for (const FResolvedAnchor& Resolved : Instance->ResolvedAnchors)
			{
				if (Resolved.Id == Bay.AnchorId) { ServiceNode = Resolved.Node; break; }
			}
			if (!TestTrue(*FString::Printf(TEXT("%s resolved its service point"), *Who), ServiceNode.IsSet()))
			{
				continue;
			}

			// THE ENTRY NODE, FOUND BY POSITION rather than handed back by name: nothing public
			// returns a per-bay node (FStandLayoutBuild::FResult::Entries is one deduplicated
			// array per STAND, since two bays share one side's contact). PlaceStand's heading
			// and position are both zero here, so a bay's world position is its local one -
			// the same trick StandLayoutBuild's own idempotent recovery path (NodeNear) uses.
			FGuidelineNodeId EntryNode;
			const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				const FGuidelineNodeId Id = Net->GuidelineNodeIdAt(Index);
				if (Id.IsSet() && FVector2D::Distance(Nodes[Index].Position, Bay.EntryLocal) <= 1.0)
				{
					EntryNode = Id;
					break;
				}
			}
			if (!TestTrue(*FString::Printf(TEXT("%s's entry node exists in the graph"), *Who), EntryNode.IsSet()))
			{
				continue;
			}

			// BFS OVER STAND-OWNED EDGES ONLY, from the service point - deliberately NOT
			// IsServiceNodeConnected, which stops at the FIRST unowned edge and answers a
			// different question (does the lane reach a road at all, not does it reach its
			// own entry).
			TSet<FGuidelineNodeId> Seen;
			TArray<FGuidelineNodeId> Frontier;
			Seen.Add(ServiceNode);
			Frontier.Add(ServiceNode);
			bool bReachedEntry = ServiceNode == EntryNode;
			while (Frontier.Num() > 0 && !bReachedEntry)
			{
				const FGuidelineNodeId At = Frontier.Pop();
				const FGuidelineNode* Found = Net->GetGuidelineNode(At);
				if (Found == nullptr)
				{
					continue;
				}
				for (const FGuidelineEdgeId Id : Found->Incident)
				{
					const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
					if (Edge == nullptr || !Edge->bAlive || Edge->StandGeometryOwner != Placed)
					{
						continue;
					}
					const FGuidelineNodeId Other = Edge->A == At ? Edge->B : Edge->A;
					if (Other == EntryNode)
					{
						bReachedEntry = true;
						break;
					}
					if (!Seen.Contains(Other))
					{
						Seen.Add(Other);
						Frontier.Add(Other);
					}
				}
			}

			TestTrue(*FString::Printf(TEXT("%s: entry and service-point nodes share one stand-owned "
				"chain, which is the fact InspectFacts::DescribeStand's bServiceable relies on"), *Who),
				bReachedEntry);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShippedCodeCStandMatchesTheBuilderTest,
	"Airside.Entities.ShippedCodeCStandMatchesTheBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FShippedCodeCStandMatchesTheBuilderTest::RunTest(const FString& Parameters)
{
	// THE GAME PLOPS THE ASSET; EVERY OTHER C TEST MEASURES THE TRANSIENT (final review,
	// 2026-09-27). DA_Stand_CodeC is a SAVED copy of what BuildCodeCStand laid the day it was
	// authored, and every change to the template since - the far-side bays of 2026-09-26 among
	// them - reaches a player only when the asset is re-authored. So the shipped bays are held
	// against the builder as it is now: an asset left behind by a template change goes red here,
	// not as a stand in PIE whose bays sit where the tests say they do not.
	const UEntityDefinition* Shipped =
		LoadObject<UEntityDefinition>(nullptr, TEXT("/Game/Entities/DA_Stand_CodeC.DA_Stand_CodeC"));
	if (!TestNotNull(TEXT("DA_Stand_CodeC loads"), Shipped))
	{
		return false;
	}

	// THE SAME AIRCRAFT the asset carries, so the one thing compared is the layout itself.
	UEntityDefinition* Built = NewObject<UEntityDefinition>(GetTransientPackage());
	UEntityDefinition::BuildCodeCStand(Built, Shipped->DesignAircraft);

	TestEqual(TEXT("the shipped stand's design vehicle is the one the builder lays C for"),
		Shipped->DesignVehicle.TypeCode, Built->DesignVehicle.TypeCode);
	TestTrue(*FString::Printf(TEXT("the shipped extent (%.0f x %.0f) is the builder's (%.0f x %.0f)"),
			Shipped->RequiredExtent.X, Shipped->RequiredExtent.Y, Built->RequiredExtent.X, Built->RequiredExtent.Y),
		Shipped->RequiredExtent.Equals(Built->RequiredExtent, 0.5));
	if (!TestEqual(TEXT("as many bays as the builder lays"), Shipped->ServiceBays.Num(), Built->ServiceBays.Num()))
	{
		return false;
	}

	// BY ANCHOR, never by index - see FResolvedAnchor for why position in an array is not identity.
	for (const FServiceBay& Want : Built->ServiceBays)
	{
		const FServiceBay* Have = Shipped->ServiceBays.FindByPredicate(
			[&Want](const FServiceBay& Bay) { return Bay.AnchorId == Want.AnchorId; });
		if (!TestNotNull(*FString::Printf(TEXT("the shipped stand has a bay for '%s'"), *Want.AnchorId.ToString()), Have))
		{
			continue;
		}
		const FString Who = Want.AnchorId.ToString();
		TestTrue(*FString::Printf(TEXT("'%s' entry at (%.0f, %.0f), builder (%.0f, %.0f)"), *Who,
				Have->EntryLocal.X, Have->EntryLocal.Y, Want.EntryLocal.X, Want.EntryLocal.Y),
			Have->EntryLocal.Equals(Want.EntryLocal, 0.5));
		TestTrue(*FString::Printf(TEXT("'%s' exit at (%.0f, %.0f), builder (%.0f, %.0f)"), *Who,
				Have->ExitLocal.X, Have->ExitLocal.Y, Want.ExitLocal.X, Want.ExitLocal.Y),
			Have->ExitLocal.Equals(Want.ExitLocal, 0.5));
		TestTrue(*FString::Printf(TEXT("'%s' park at (%.0f, %.0f), builder (%.0f, %.0f)"), *Who,
				Have->ParkLocal.X, Have->ParkLocal.Y, Want.ParkLocal.X, Want.ParkLocal.Y),
			Have->ParkLocal.Equals(Want.ParkLocal, 0.5));
		TestEqual(*FString::Printf(TEXT("'%s' entry heading"), *Who), Have->EntryHeading, Want.EntryHeading, 1.0e-6);
		TestEqual(*FString::Printf(TEXT("'%s' exit heading"), *Who), Have->ExitHeading, Want.ExitHeading, 1.0e-6);
		TestEqual(*FString::Printf(TEXT("'%s' park heading"), *Who), Have->ParkHeading, Want.ParkHeading, 1.0e-6);
	}
	return true;
}

#endif
