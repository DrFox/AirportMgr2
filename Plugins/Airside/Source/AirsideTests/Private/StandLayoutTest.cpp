#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/StandLayoutBuild.h"
#include "Content/AirsideContent.h"
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
#include "Model/VehicleEnvelope.h"
#include "Model/VehicleFit.h"
#include "Present/RoadNetworkActor.h"
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
	// PER LETTER, BY EVERY VEHICLE THE STAND ADMITS (user 2026-09-26, widened 2026-10-03): each
	// letter's template is laid for its admitted set - its own design vehicle and every smaller
	// letter's (UEntityDefinition::AdmittedVehicles) - and admits whatever is inside that set's
	// FVehicleEnvelope, the bid's own ceiling. So since 2026-10-03 the tow IS asked to drive every
	// C-F stand here: it was refused them (VehicleFit::NoLargerThan the truck, strict on its 575
	// chain) and is admitted now because the lanes carry its settle straight. A TOW is judged
	// through VehicleFit::JudgePlan over the whole bay - arrive, serve, reverse, depart - because
	// a trailer's fold is a property of the chain it arrives with, which no one leg can show.
	const TArray<FVehicle> Fleet = {
		UAirsideSettings::ResolveUtilityTowVehicle(), UAirsideSettings::ResolveDefaultVehicle() };
	URoadNetwork* Network = NewObject<URoadNetwork>();
	TSet<EIcaoCode> TowJudgedOn;

	for (const EIcaoCode StandLetter : AllLetters())
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		const FVehicleEnvelope Admits = FVehicleEnvelope::Of(UAirsideSettings::ResolveStandVehiclesOf(Stand, StandLetter));

		for (const FVehicle& Vehicle : Fleet)
		{
			if (!Admits.Admits(Vehicle))
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
			TowJudgedOn.Add(StandLetter);
			for (const FServiceBay& Bay : Stand->ServiceBays)
			{
				const FVerdictAndPlan Judged = JudgeBay(Bay, Vehicle, *Network);
				TestTrue(*FString::Printf(TEXT("%s: bay '%s' is admitted whole - %s"), *Vehicular,
					*Bay.AnchorId.ToString(), *Judged.Verdict.Describe()), Judged.Verdict.Fits());
			}
		}
	}

	// NOT VACUOUS, AND THE RULING ITSELF: the tow was driven through every letter's bays, C-F
	// included. Until 2026-10-03 this block only MEASURED the tow on C, because it was refused there:
	// every bay "ended 12 uu / 6.9 deg off the end pose (limits 20 uu / 3.0 deg)" of the reverse,
	// with no straight after the reverse corner for the trailer to settle on.
	for (const EIcaoCode StandLetter : AllLetters())
	{
		TestTrue(*FString::Printf(TEXT("Code %s: the stand admits the utility tow, so its bays were judged with it"),
			IcaoCode::ToLetter(StandLetter)), TowJudgedOn.Contains(StandLetter));
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
		const FVehicleEnvelope Admits = FVehicleEnvelope::Of(UAirsideSettings::ResolveStandVehiclesOf(Stand, StandLetter));
		// THE ROAD JOIN'S OWN RADIUS: FAnchorLink fillets a stand's entry for the widest-turning vehicle it
		// admits (FVehicleEnvelope::WidestTurning) - the truck on C-F, so a tow arriving there turns at
		// the truck's radius, not its own. Was the design vehicle's radius, the same figure for every
		// letter today.
		const double Radius = 1.1 * Admits.ForwardRadius;
		for (const FVehicle& Vehicle : Fleet)
		{
			if (Vehicle.Tow.IsEmpty() || !Admits.Admits(Vehicle))
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
	FEveryAdmittedTowSettlesOnItsLanesTest,
	"Airside.Entities.EveryAdmittedTowSettlesOnItsLanes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryAdmittedTowSettlesOnItsLanesTest::RunTest(const FString& Parameters)
{
	using namespace StandLayoutFixture;

	// THE GEOMETRY HALF OF 2026-10-03's RULING, measured off the laid legs rather than read off the
	// log: a stand that admits a tow carries, for the LONGEST towing chain it admits, the two
	// straights UEntityDefinition's TowSettleChains (2.0, user ruling 2026-09-26) asks for - into the
	// service point at the end of the serve leg, and after the reverse corner at the end of the
	// reverse leg. Admission alone (EveryLetterAdmitsEverySmallerLetter) could pass on lanes with
	// neither; the drivability test judges the result; this pins the CAUSE, so a C-F stand laid for
	// the truck alone again goes red here by name, with the length it is short.
	//
	// C-F FOR THE REVERSE STRAIGHT: there it is never cut short. On A and B it is capped at the
	// entrance edge by design (BuildStandTemplate's Cleared - Code A's hydrant keeps 976 of 1150),
	// and the whole-bay judge, not a length, is what admits those.
	constexpr double TowSettleChains = 2.0;
	int32 Measured = 0;
	for (const EIcaoCode StandLetter : AllLetters())
	{
		const UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient(StandLetter);
		const FVehicleEnvelope Admits = FVehicleEnvelope::Of(UAirsideSettings::ResolveStandVehiclesOf(Stand, StandLetter));
		if (Admits.TrailerChain <= 0.0)
		{
			continue;
		}
		const double Wants = TowSettleChains * Admits.TrailerChain;
		for (const FServiceBay& Bay : Stand->ServiceBays)
		{
			// THE LEG'S LAST SPAN, which BuildLeg always lays straight (its control on its midpoint) -
			// asserted, so a curve there cannot be measured as if it were a straight.
			auto LastStraight = [this, &Bay, StandLetter](const FStandLeg& Leg, const TCHAR* What) -> double
			{
				const int32 N = Leg.Points.Num();
				if (!TestTrue(*FString::Printf(TEXT("Code %s '%s' %s has a span"), IcaoCode::ToLetter(StandLetter),
					*Bay.AnchorId.ToString(), What), N >= 2 && Leg.Controls.Num() == N - 1))
				{
					return 0.0;
				}
				const FVector2D Mid = 0.5 * (Leg.Points[N - 2] + Leg.Points[N - 1]);
				TestTrue(*FString::Printf(TEXT("Code %s '%s' %s ends on a straight"), IcaoCode::ToLetter(StandLetter),
					*Bay.AnchorId.ToString(), What), Leg.Controls.Last().Equals(Mid, 0.01));
				return FVector2D::Distance(Leg.Points[N - 2], Leg.Points[N - 1]);
			};
			const double Into = LastStraight(Bay.ServeLeg, TEXT("serve"));
			TestTrue(*FString::Printf(TEXT("Code %s '%s': %.0f uu of straight into the service point, the tow wants %.0f (%.1f chains of %.0f)"),
				IcaoCode::ToLetter(StandLetter), *Bay.AnchorId.ToString(), Into, Wants, TowSettleChains, Admits.TrailerChain),
				Into >= Wants - 0.5);
			if (StandLetter >= EIcaoCode::C)
			{
				const double After = LastStraight(Bay.ReverseLeg, TEXT("reverse"));
				TestTrue(*FString::Printf(TEXT("Code %s '%s': %.0f uu of straight after the reverse corner, the tow wants %.0f"),
					IcaoCode::ToLetter(StandLetter), *Bay.AnchorId.ToString(), After, Wants),
					After >= Wants - 0.5);
			}
			++Measured;
		}
	}
	TestTrue(TEXT("NOT VACUOUS: every letter's bays were measured (6 letters x 3 bays)"), Measured >= 18);
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

namespace RetiredCodeCStand
{
	/** One anchor of the retired asset, as it was saved. */
	struct FAnchorRow { const TCHAR* Id; FVector2D At; double Heading; EServiceRole Role; };

	/** One bay's three poses, as saved. */
	struct FBayPoses
	{
		const TCHAR* AnchorId;
		FVector2D Entry; double EntryHeading;
		FVector2D Exit; double ExitHeading;
		FVector2D Park; double ParkHeading;
	};

	/** One bay's four legs (arrive, serve, reverse, depart), points then controls each. */
	struct FBayLegs { TArrayView<const FVector2D> Points[4]; TArrayView<const FVector2D> Controls[4]; };

	// DUMPED FROM /Game/Entities/DA_Stand_CodeC ON 2026-10-03, at full double precision (Python repr,
	// which round-trips a double exactly), by a read-only commandlet script - the asset as re-authored
	// for the round-1 settle straights, the moment before it was deleted. Its DesignAircraft was
	// /Game/Entities/DA_Aircraft_A320.
	static const FVector2D Extent = FVector2D(5900.0, 6500.0);
	static const FAnchorRow Anchors[] = {
		{ TEXT("HydrantPit"), FVector2D(-800.0, 700.0), -1.5707963267948966, EServiceRole::Fuel },
		{ TEXT("BaggageHold"), FVector2D(-350.0, 700.0), -1.5707963267948966, EServiceRole::Baggage },
		{ TEXT("FixedGPU"), FVector2D(-350.0, -700.0), 1.5707963267948966, EServiceRole::GPU },
		{ TEXT("PassengerDoor"), FVector2D(200.0, -300.0), 1.5707963267948966, EServiceRole::Passenger },
		{ TEXT("TugStand"), FVector2D(350.0, -1400.0), 3.141592653589793, EServiceRole::Tug },
	};
	// bay 0
	static const FBayPoses Bay0 = { TEXT("BaggageHold"), FVector2D(1731.0, 2150.1026286840843), 3.141592653589793, FVector2D(1731.0, 2150.1026286840843), 0.0, FVector2D(1155.9486856579576, 2477.557155952037), 2.356194490192345 };
	static const FVector2D Bay0_arrive_leg_Points[] = { FVector2D(1731.0, 2150.1026286840843), FVector2D(1308.32584578582, 2325.1799958241745), FVector2D(1155.9486856579576, 2477.557155952037) };
	static const FVector2D Bay0_arrive_leg_Controls[] = { FVector2D(1483.4032129259106, 2150.1026286840843), FVector2D(1232.137265721889, 2401.3685758881056) };
	static const FVector2D Bay0_serve_leg_Points[] = { FVector2D(1155.9486856579576, 2477.557155952037), FVector2D(1058.5832087500853, 2574.9226328599098), FVector2D(635.9090545359053, 2750.0), FVector2D(430.9999999999999, 2750.0), FVector2D(-350.0, 1969.0), FVector2D(-350.0, 700.0) };
	static const FVector2D Bay0_serve_leg_Controls[] = { FVector2D(1107.2659472040214, 2526.2398944059732), FVector2D(883.5058416099948, 2750.0), FVector2D(533.4545272679526, 2750.0), FVector2D(-350.0, 2750.0), FVector2D(-350.0, 1334.5) };
	static const FVector2D Bay0_reverse_leg_Points[] = { FVector2D(-350.0, 700.0), FVector2D(-350.0, 2197.7496038933064), FVector2D(-902.2503961066935, 2750.0), FVector2D(-2052.2503961066936, 2750.0) };
	static const FVector2D Bay0_reverse_leg_Controls[] = { FVector2D(-350.0, 1448.8748019466532), FVector2D(-350.0, 2750.0), FVector2D(-1477.2503961066936, 2750.0) };
	static const FVector2D Bay0_depart_leg_Points[] = { FVector2D(-2052.2503961066936, 2750.0), FVector2D(635.9090545359053, 2750.0), FVector2D(1058.5832087500853, 2574.9226328599098), FVector2D(1308.32584578582, 2325.1799958241745), FVector2D(1731.0, 2150.1026286840843) };
	static const FVector2D Bay0_depart_leg_Controls[] = { FVector2D(-708.1706707853941, 2750.0), FVector2D(883.5058416099948, 2750.0), FVector2D(1183.4545272679527, 2450.051314342042), FVector2D(1483.4032129259106, 2150.1026286840843) };
	// bay 1
	static const FBayPoses Bay1 = { TEXT("HydrantPit"), FVector2D(1731.0, 2150.1026286840843), 3.141592653589793, FVector2D(1731.0, 2150.1026286840843), 0.0, FVector2D(660.7551115097785, 2477.557155952037), 2.356194490192345 };
	static const FVector2D Bay1_arrive_leg_Points[] = { FVector2D(1731.0, 2150.1026286840843), FVector2D(1235.806425851821, 2150.1026286840843), FVector2D(813.132271637641, 2325.1799958241745), FVector2D(660.7551115097785, 2477.557155952037) };
	static const FVector2D Bay1_arrive_leg_Controls[] = { FVector2D(1483.4032129259103, 2150.1026286840843), FVector2D(988.2096387777315, 2150.1026286840843), FVector2D(736.9436915737098, 2401.3685758881056) };
	static const FVector2D Bay1_serve_leg_Points[] = { FVector2D(660.7551115097785, 2477.557155952037), FVector2D(563.3896346019061, 2574.9226328599098), FVector2D(140.71548038772622, 2750.0), FVector2D(-19.000000000000114, 2750.0), FVector2D(-800.0, 1969.0), FVector2D(-800.0, 700.0) };
	static const FVector2D Bay1_serve_leg_Controls[] = { FVector2D(612.0723730558423, 2526.2398944059732), FVector2D(388.31226746181574, 2750.0), FVector2D(60.85774019386305, 2750.0), FVector2D(-800.0, 2750.0), FVector2D(-800.0, 1334.5) };
	static const FVector2D Bay1_reverse_leg_Points[] = { FVector2D(-800.0, 700.0), FVector2D(-800.0, 2197.7496038933064), FVector2D(-1352.2503961066936, 2750.0), FVector2D(-2502.2503961066936, 2750.0) };
	static const FVector2D Bay1_reverse_leg_Controls[] = { FVector2D(-800.0, 1448.8748019466532), FVector2D(-800.0, 2750.0), FVector2D(-1927.2503961066936, 2750.0) };
	static const FVector2D Bay1_depart_leg_Points[] = { FVector2D(-2502.2503961066936, 2750.0), FVector2D(635.9090545359053, 2750.0), FVector2D(1058.5832087500853, 2574.9226328599098), FVector2D(1308.32584578582, 2325.1799958241745), FVector2D(1731.0, 2150.1026286840843) };
	static const FVector2D Bay1_depart_leg_Controls[] = { FVector2D(-933.1706707853941, 2750.0), FVector2D(883.5058416099948, 2750.0), FVector2D(1183.4545272679527, 2450.051314342042), FVector2D(1483.4032129259106, 2150.1026286840843) };
	// bay 2
	static const FBayPoses Bay2 = { TEXT("FixedGPU"), FVector2D(1731.0, -2150.1026286840843), 3.141592653589793, FVector2D(1731.0, -2150.1026286840843), 0.0, FVector2D(1155.9486856579576, -2477.557155952037), -2.356194490192345 };
	static const FVector2D Bay2_arrive_leg_Points[] = { FVector2D(1731.0, -2150.1026286840843), FVector2D(1308.32584578582, -2325.1799958241745), FVector2D(1155.9486856579576, -2477.557155952037) };
	static const FVector2D Bay2_arrive_leg_Controls[] = { FVector2D(1483.4032129259106, -2150.1026286840843), FVector2D(1232.137265721889, -2401.3685758881056) };
	static const FVector2D Bay2_serve_leg_Points[] = { FVector2D(1155.9486856579576, -2477.557155952037), FVector2D(1058.5832087500853, -2574.9226328599098), FVector2D(635.9090545359053, -2750.0), FVector2D(430.9999999999999, -2750.0), FVector2D(-350.0, -1969.0), FVector2D(-350.0, -700.0) };
	static const FVector2D Bay2_serve_leg_Controls[] = { FVector2D(1107.2659472040214, -2526.2398944059732), FVector2D(883.5058416099948, -2750.0), FVector2D(533.4545272679526, -2750.0), FVector2D(-350.0, -2750.0), FVector2D(-350.0, -1334.5) };
	static const FVector2D Bay2_reverse_leg_Points[] = { FVector2D(-350.0, -700.0), FVector2D(-350.0, -2197.7496038933064), FVector2D(-902.2503961066935, -2750.0), FVector2D(-2052.2503961066936, -2750.0) };
	static const FVector2D Bay2_reverse_leg_Controls[] = { FVector2D(-350.0, -1448.8748019466532), FVector2D(-350.0, -2750.0), FVector2D(-1477.2503961066936, -2750.0) };
	static const FVector2D Bay2_depart_leg_Points[] = { FVector2D(-2052.2503961066936, -2750.0), FVector2D(635.9090545359053, -2750.0), FVector2D(1058.5832087500853, -2574.9226328599098), FVector2D(1308.32584578582, -2325.1799958241745), FVector2D(1731.0, -2150.1026286840843) };
	static const FVector2D Bay2_depart_leg_Controls[] = { FVector2D(-708.1706707853941, -2750.0), FVector2D(883.5058416099948, -2750.0), FVector2D(1183.4545272679527, -2450.051314342042), FVector2D(1483.4032129259106, -2150.1026286840843) };

	inline FBayLegs LegsOf(int32 Bay)
	{
		FBayLegs Out;
		const TArrayView<const FVector2D> Rows[3][8] = {
		{ MakeArrayView(Bay0_arrive_leg_Points), MakeArrayView(Bay0_arrive_leg_Controls), MakeArrayView(Bay0_serve_leg_Points), MakeArrayView(Bay0_serve_leg_Controls), MakeArrayView(Bay0_reverse_leg_Points), MakeArrayView(Bay0_reverse_leg_Controls), MakeArrayView(Bay0_depart_leg_Points), MakeArrayView(Bay0_depart_leg_Controls) },
		{ MakeArrayView(Bay1_arrive_leg_Points), MakeArrayView(Bay1_arrive_leg_Controls), MakeArrayView(Bay1_serve_leg_Points), MakeArrayView(Bay1_serve_leg_Controls), MakeArrayView(Bay1_reverse_leg_Points), MakeArrayView(Bay1_reverse_leg_Controls), MakeArrayView(Bay1_depart_leg_Points), MakeArrayView(Bay1_depart_leg_Controls) },
		{ MakeArrayView(Bay2_arrive_leg_Points), MakeArrayView(Bay2_arrive_leg_Controls), MakeArrayView(Bay2_serve_leg_Points), MakeArrayView(Bay2_serve_leg_Controls), MakeArrayView(Bay2_reverse_leg_Points), MakeArrayView(Bay2_reverse_leg_Controls), MakeArrayView(Bay2_depart_leg_Points), MakeArrayView(Bay2_depart_leg_Controls) },
		};
		for (int32 Leg = 0; Leg < 4; ++Leg)
		{
			Out.Points[Leg] = Rows[Bay][2 * Leg];
			Out.Controls[Leg] = Rows[Bay][2 * Leg + 1];
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeCodeCStandEqualsTheRetiredAssetTest,
	"Airside.Entities.RuntimeCodeCStandEqualsTheRetiredAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRuntimeCodeCStandEqualsTheRetiredAssetTest::RunTest(const FString& Parameters)
{
	using namespace RetiredCodeCStand;

	// CODE C IS BUILT AT RUNTIME SINCE 2026-10-03 (owner ruling): the saved DA_Stand_CodeC went stale
	// once already - ShippedCodeCStandMatchesTheBuilder, which this replaces, stayed green on it because
	// it compared poses and the stale part was a leg - so a stand layout is derived data now, like the
	// other five letters'. THIS PROVES THE SWITCH MOVED NOTHING: the runtime template, built the way
	// UStandDefinitionCache builds it, against the retired asset's own saved figures.
	//
	// BITWISE, not toleranced: both are the same builder over the same inputs (the asset was authored by
	// BuildCodeCStand, which forwards to the BuildStandFor that MakeStandTransient calls), and the dump
	// round-trips every double exactly. A tolerance here could only hide a difference that should not
	// exist. When the template changes ON PURPOSE this goes red by design - update the rows with the
	// reason, as a box-pinning test would be.
	const UEntityDefinition* Runtime = UEntityDefinition::MakeStandTransient(EIcaoCode::C);
	TestTrue(*FString::Printf(TEXT("extent %s, retired %s"), *Runtime->RequiredExtent.ToString(), *Extent.ToString()),
		Runtime->RequiredExtent == Extent);
	if (TestEqual(TEXT("as many anchors as the retired asset"), Runtime->Anchors.Num(), static_cast<int32>(UE_ARRAY_COUNT(Anchors))))
	{
		for (int32 At = 0; At < Runtime->Anchors.Num(); ++At)
		{
			const FEntityAnchor& Have = Runtime->Anchors[At];
			const FAnchorRow& Want = Anchors[At];
			TestTrue(*FString::Printf(TEXT("anchor %d is '%s' at %s, heading %.17g, role %d - retired '%s' at %s, %.17g, %d"), At,
					*Have.Id.ToString(), *Have.LocalPosition.ToString(), Have.LocalHeading, static_cast<int32>(Have.Role),
					Want.Id, *Want.At.ToString(), Want.Heading, static_cast<int32>(Want.Role)),
				Have.Id == FName(Want.Id) && Have.LocalPosition == Want.At && Have.LocalHeading == Want.Heading && Have.Role == Want.Role);
		}
	}
	// AND EVERY OTHER FIELD THE ASSET CARRIED (review, 2026-10-03). Read off the retired asset (git show
	// b9413f7a:Content/Entities/DA_Stand_CodeC.uasset): AdmittedVehicles and AvailableServices were saved;
	// PoseRole, Trucks, Layout, bTaxiThrough and FootprintExtent have NO tag in its name table, so the asset
	// held the class defaults for them - which is what is asserted. AvailableServices as a SET: the name
	// table proves the members, not their order. The PRICES and aircraft are content now -
	// CodeCStandDrawsTheRetiredAssetsAircraft holds those.
	{
		FString Kinds;
		for (const FVehicle& Vehicle : Runtime->AdmittedVehicles) { Kinds += Vehicle.TypeCode.ToString() + TEXT(" "); }
		TestEqual(TEXT("it admits what the retired asset admitted, by kind and in order"), Kinds,
			UAirsideSettings::ResolveDefaultVehicle().TypeCode.ToString() + TEXT(" ") + UAirsideSettings::ResolveUtilityTowVehicle().TypeCode.ToString() + TEXT(" "));
		const TSet<EServiceRole> Services(Runtime->AvailableServices);
		const TSet<EServiceRole> Retired = { EServiceRole::Aircraft, EServiceRole::Fuel, EServiceRole::Baggage,
			EServiceRole::Tug, EServiceRole::GPU, EServiceRole::Passenger, EServiceRole::Crew };
		TestTrue(TEXT("it provides the retired asset's services, no more and no fewer"),
			Services.Num() == Retired.Num() && Services.Includes(Retired));
		TestEqual(TEXT("pose role, the asset's (class default)"), static_cast<int32>(Runtime->PoseRole), static_cast<int32>(EServiceRole::Aircraft));
		TestEqual(TEXT("trucks, the asset's (class default)"), Runtime->Trucks, 0);
		TestEqual(TEXT("layout, the asset's (class default)"), static_cast<int32>(Runtime->Layout), static_cast<int32>(EPlotLayout::Scatter));
		TestFalse(TEXT("taxi-through, the asset's (class default)"), Runtime->bTaxiThrough);
		TestTrue(TEXT("footprint extent, the asset's (class default)"), Runtime->FootprintExtent == FVector2D::ZeroVector);
	}
	const FBayPoses* Poses[] = { &Bay0, &Bay1, &Bay2 };
	if (!TestEqual(TEXT("as many bays as the retired asset"), Runtime->ServiceBays.Num(), static_cast<int32>(UE_ARRAY_COUNT(Poses))))
	{
		return false;
	}
	const TCHAR* LegNames[] = { TEXT("arrive"), TEXT("serve"), TEXT("reverse"), TEXT("depart") };
	for (int32 Index = 0; Index < Runtime->ServiceBays.Num(); ++Index)
	{
		const FServiceBay& Have = Runtime->ServiceBays[Index];
		const FBayPoses& Want = *Poses[Index];
		const FString Who = Have.AnchorId.ToString();
		TestTrue(*FString::Printf(TEXT("bay %d is '%s', retired '%s'"), Index, *Who, Want.AnchorId), Have.AnchorId == FName(Want.AnchorId));
		TestTrue(*FString::Printf(TEXT("'%s' entry pose is the retired one"), *Who), Have.EntryLocal == Want.Entry && Have.EntryHeading == Want.EntryHeading);
		TestTrue(*FString::Printf(TEXT("'%s' exit pose is the retired one"), *Who), Have.ExitLocal == Want.Exit && Have.ExitHeading == Want.ExitHeading);
		TestTrue(*FString::Printf(TEXT("'%s' park pose is the retired one"), *Who), Have.ParkLocal == Want.Park && Have.ParkHeading == Want.ParkHeading);
		const FBayLegs Legs = LegsOf(Index);
		const FStandLeg* HaveLegs[] = { &Have.ArriveLeg, &Have.ServeLeg, &Have.ReverseLeg, &Have.DepartLeg };
		for (int32 Leg = 0; Leg < 4; ++Leg)
		{
			const FStandLeg& HaveLeg = *HaveLegs[Leg];
			const bool bSameCount = HaveLeg.Points.Num() == Legs.Points[Leg].Num() && HaveLeg.Controls.Num() == Legs.Controls[Leg].Num();
			bool bSame = bSameCount;
			for (int32 K = 0; bSame && K < HaveLeg.Points.Num(); ++K) { bSame = HaveLeg.Points[K] == Legs.Points[Leg][K]; }
			for (int32 K = 0; bSame && K < HaveLeg.Controls.Num(); ++K) { bSame = HaveLeg.Controls[K] == Legs.Controls[Leg][K]; }
			TestTrue(*FString::Printf(TEXT("'%s' %s leg is the retired one, point for point and control for control (%d points, retired %d)"),
				*Who, LegNames[Leg], HaveLeg.Points.Num(), Legs.Points[Leg].Num()), bSame);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCodeCStandDrawsTheRetiredAssetsAircraftTest,
	"Airside.Content.CodeCStandDrawsTheRetiredAssetsAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCodeCStandDrawsTheRetiredAssetsAircraftTest::RunTest(const FString& Parameters)
{
	// THE OTHER HALF OF RETIRING DA_Stand_CodeC (2026-10-03): the asset carried Code C's layout AND its
	// pairing with DA_Aircraft_A320, the aircraft a C stand is drawn with and reads its design span off.
	// The layout is runtime now (RuntimeCodeCStandEqualsTheRetiredAsset); the pairing is content, and
	// moved to the ONE resolver, UAirsideSettings::ResolveLargestAircraftOfLetter, read off the content
	// set's StandLetters row for C - the asset's own reference, dumped from it before it was deleted.
	const UAircraftType* C = UAirsideSettings::ResolveLargestAircraftOfLetter(EIcaoCode::C);
	if (TestNotNull(TEXT("Code C resolves a design aircraft"), C))
	{
		TestEqual(TEXT("and it is the one DA_Stand_CodeC paired with"), C->GetPathName(),
			FString(TEXT("/Game/Entities/DA_Aircraft_A320.DA_Aircraft_A320")));
	}
	// AND THE SUITE'S FIXTURE IS DRAWN WITH THE SAME ONE: zero-arg MakeStandTransient built its own paper A320
	// until 2026-10-03, a second source for C's aircraft beside this row.
	TestTrue(TEXT("MakeStandTransient()'s design aircraft is the content row's, not a second one"),
		C != nullptr && UEntityDefinition::MakeStandTransient()->DesignAircraft.Get() == C);
	for (const EIcaoCode Letter : { EIcaoCode::A, EIcaoCode::B, EIcaoCode::D, EIcaoCode::E, EIcaoCode::F })
	{
		TestNull(*FString::Printf(TEXT("Code %s has no aircraft chosen to draw it with yet"), IcaoCode::ToLetter(Letter)),
			UAirsideSettings::ResolveLargestAircraftOfLetter(Letter));
	}

	// AND ITS PRICES, which the asset carried too (16000 to place, 16 a day, dumped from it before it
	// went): through the ACTOR, the way a placement resolves a stand, so the cache's stamping is what
	// is measured - a template built with no prices would place a Code C stand for nothing.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	const UEntityDefinition* Placed = TestWorld.Actor->ResolveStandDefinition();
	if (TestNotNull(TEXT("the actor resolves a Code C stand"), Placed))
	{
		TestEqual(TEXT("which costs what DA_Stand_CodeC cost to place"), Placed->PlacementCost, 16000.0);
		TestEqual(TEXT("and to keep"), Placed->UpkeepPerDay, 16.0);
		TestTrue(TEXT("and is drawn with its A320"), C != nullptr && Placed->DesignAircraft.Get() == C);
		TestTrue(TEXT("and is the same object a drawn Code C resolves"), Placed == TestWorld.Actor->ResolveStandDefinitionFor(EIcaoCode::C));
	}
	return true;
}

#endif
