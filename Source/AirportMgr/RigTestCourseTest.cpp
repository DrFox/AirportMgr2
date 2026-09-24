#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RigTestCourse.h"

#include "Content/AirsideSettings.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS: the module is a unity build.
namespace RigCourseTest
{
	/**
	 * Every WARNING line in the categories the course and the tow guard log to. Unbuffered
	 * (CanBeUsedOnMultipleThreads), or the log thread delivers lines after the spy has gone
	 * (memory: log spies must be unbuffered).
	 */
	struct FWarningSpy : public FOutputDevice
	{
		TArray<FString> Lines;

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity == ELogVerbosity::Warning
				&& (Category == FName(TEXT("LogRoadBuild")) || Category == FName(TEXT("LogAirside"))))
			{
				Lines.Add(FString(V));
			}
		}

		int32 Containing(const TCHAR* Text, const TCHAR* AlsoText = nullptr) const
		{
			int32 Count = 0;
			for (const FString& Line : Lines)
			{
				Count += (Line.Contains(Text) && (AlsoText == nullptr || Line.Contains(AlsoText))) ? 1 : 0;
			}
			return Count;
		}
	};

	/** Plans a leg exactly as a route search would with the vehicle's body - independently of the course. */
	bool Fits(const URoadNetwork& Net, const FRigCourseWaypoint& From, const FRigCourseWaypoint& To, const FVehicle& Vehicle)
	{
		const FGuidelineNodeId Start = ARigTestCourse::ResolveWaypoint(Net, From);
		const FGuidelineNodeId Goal = ARigTestCourse::ResolveWaypoint(Net, To);
		if (!Start.IsSet() || !Goal.IsSet()) { return false; }
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::PlayerIssued, Start, Goal, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Vehicle);
		return RouteSearch::Find(Net, Query).IsValid();
	}

	/** The worst of one measure, and where it happened. */
	struct FWorst
	{
		double Excess = -TNumericLimits<double>::Max();
		FString Where;

		void Offer(double InExcess, const FString& InWhere)
		{
			if (InExcess > Excess) { Excess = InExcess; Where = InWhere; }
		}
	};

	/**
	 * THE TOW ON THE TARMAC, measured on the agent as it drives (spec §Tests: "the trailer stays
	 * inside the tarmac clearance at every sample"). Each body-carrying link's outline - its
	 * corners and points every 50 uu down both sides, from the model's own FTowPose - is put
	 * against the nearest vertex of the plan it is driving. Three measures:
	 *
	 *   SWEPT (turn edges): per (attempt, edge, sample), the deepest reach toward the turn's
	 *   centre plus the deepest reach away from it, over the whole traversal, against that
	 *   sample's ClearInnerAt + ClearOuterAt. VehicleFit's own rule, measured on the agent that
	 *   actually drove rather than on the router's Trace.
	 *   PER SIDE (turn edges): each point's reach against its own side's clearance - the
	 *   physical "is it on the tarmac", which the SWEPT rule deliberately does not ask (a real
	 *   rig swings wide; see VehicleFit.h). The simulated agent does NOT swing wide - it holds
	 *   its lane line - so this is the measure that finds out what that costs.
	 *   LANE: on a lane edge, distance from the ROAD's centreline against half the road's total
	 *   width, for points that project within the segment (beyond it they are over a junction).
	 *
	 * Unmeasured edges (the dead-end balloon, ruled over grass) are skipped, as VehicleFit skips
	 * them. Points behind the plan's start or past its end are skipped: the tow is laid straight
	 * back over road the plan does not name.
	 */
	struct FClearanceProbe
	{
		/** Scales every link's width - 1 always, except in the RED run that proves this measures. */
		double WidthScale = 1.0;

		/** Per vehicle slot. */
		FWorst Swept[2];
		FWorst PerSide[2];
		FWorst Lane[2];
		int32 TurnPoints = 0;
		int32 LanePoints = 0;

		struct FSampleReach
		{
			double In = 0.0;
			double Out = 0.0;
			double Tarmac = 0.0;
			int32 Slot = 0;
			FString Where;
		};
		TMap<FString, FSampleReach> Reaches;

		void Measure(const URoadNetwork& Net, const FRoadAgent& Agent, const FVehicle& Vehicle, int32 Leg, int32 Slot, const FString& What)
		{
			const FRoutePlan& Plan = Agent.PlanInProgress();
			if (Plan.Polyline.Num() < 2 || Plan.Steps.Num() == 0) { return; }
			const TArray<FTowPose, TInlineAllocator<2>>& Poses = Agent.LastMotion.Tow;
			for (int32 LinkIndex = 0; LinkIndex < Vehicle.Tow.Num() && LinkIndex < Poses.Num(); ++LinkIndex)
			{
				const FTowLink& Link = Vehicle.Tow[LinkIndex];
				if (Link.IsBar()) { continue; }
				const FTowPose& Pose = Poses[LinkIndex];
				const FVector2D H(FMath::Cos(Pose.Heading), FMath::Sin(Pose.Heading));
				const FVector2D N(-H.Y, H.X);
				const FVector2D Front = Pose.Hitch + H * Link.BodyFront;
				const FVector2D Rear = Pose.Axle - H * Link.BodyRear;
				const double Half = 0.5 * Link.Width * WidthScale;
				const double Length = FVector2D::Distance(Front, Rear);
				const int32 Count = FMath::Max(1, FMath::CeilToInt32(Length / 50.0));
				for (int32 I = 0; I <= Count; ++I)
				{
					const FVector2D Along = FMath::Lerp(Rear, Front, static_cast<double>(I) / Count);
					MeasurePoint(Net, Plan, Along + N * Half, Leg, Slot, What, LinkIndex);
					MeasurePoint(Net, Plan, Along - N * Half, Leg, Slot, What, LinkIndex);
				}
			}
		}

		void MeasurePoint(const URoadNetwork& Net, const FRoutePlan& Plan, const FVector2D& P,
			int32 Leg, int32 Slot, const FString& What, int32 LinkIndex)
		{
			const TArray<FVector2D>& Line = Plan.Polyline;
			int32 V = 0;
			double Best = TNumericLimits<double>::Max();
			for (int32 I = 0; I < Line.Num(); ++I)
			{
				const double D = FVector2D::DistSquared(P, Line[I]);
				if (D < Best) { Best = D; V = I; }
			}
			const int32 Last = Line.Num() - 1;
			if ((V == 0 && FVector2D::DotProduct(P - Line[0], Line[1] - Line[0]) < 0.0)
				|| (V == Last && FVector2D::DotProduct(P - Line[Last], Line[Last] - Line[Last - 1]) > 0.0))
			{
				return;
			}
			int32 StepIndex = 0;
			while (StepIndex < Plan.Steps.Num() - 1 && Plan.Steps[StepIndex].EndVertex < V) { ++StepIndex; }
			const FRouteStep& Step = Plan.Steps[StepIndex];
			const int32 StartV = StepIndex == 0 ? 0 : Plan.Steps[StepIndex - 1].EndVertex;
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Step.Edge);
			if (Edge == nullptr) { return; }
			const FString Where = FString::Printf(TEXT("%s, link %d, at (%.0f, %.0f) on %s edge %d"), *What, LinkIndex,
				P.X, P.Y, Edge->ClearInnerAt.Num() > 0 ? TEXT("turn") : TEXT("lane"), Step.Edge.Index);

			if (Edge->ClearInnerAt.Num() > 0)
			{
				const FGuidelineNode* A = Net.GetGuidelineNode(Edge->A);
				const FGuidelineNode* B = Net.GetGuidelineNode(Edge->B);
				if (A == nullptr || B == nullptr) { return; }
				TArray<FVector2D> Points;
				GuidelineGeom::Sample(A->Position, Edge->Control, B->Position, Points);
				if (Points.Num() != Edge->ClearInnerAt.Num() || Step.EndVertex - StartV + 1 != Points.Num()) { return; }
				int32 Local = V - StartV;
				if (Step.bReversed) { Local = Points.Num() - 1 - Local; }
				// Inward exactly as FRoadGuidelineBuilder's MeasureTurn takes it, so the reach is
				// measured along the same normal the clearance was.
				const double Cross = FVector2D::CrossProduct(Edge->Control - A->Position, B->Position - Edge->Control);
				if (FMath::IsNearlyZero(Cross)) { return; }
				const FVector2D Tangent = (Points[FMath::Min(Local + 1, Points.Num() - 1)]
					- Points[FMath::Max(Local - 1, 0)]).GetSafeNormal();
				const FVector2D Inward = FVector2D(-Tangent.Y, Tangent.X) * (Cross > 0.0 ? 1.0 : -1.0);
				const double Reach = FVector2D::DotProduct(P - Points[Local], Inward);
				const double ClearIn = Edge->ClearInnerAt[Local];
				const double ClearOut = Edge->ClearOuterAt[Local];
				++TurnPoints;

				PerSide[Slot].Offer(Reach >= 0.0 ? Reach - ClearIn : -Reach - ClearOut,
					Where + FString::Printf(TEXT(", sample %d: reach %.0f %s vs clear in %.0f / out %.0f"),
						Local, FMath::Abs(Reach), Reach >= 0.0 ? TEXT("inward") : TEXT("outward"), ClearIn, ClearOut));

				FSampleReach& Sample = Reaches.FindOrAdd(FString::Printf(TEXT("%d|%d|%d|%d"), Leg, Slot, Step.Edge.Index, Local));
				Sample.In = FMath::Max(Sample.In, Reach);
				Sample.Out = FMath::Max(Sample.Out, -Reach);
				Sample.Tarmac = ClearIn + ClearOut;
				Sample.Slot = Slot;
				Sample.Where = FString::Printf(TEXT("%s, edge %d sample %d"), *What, Step.Edge.Index, Local);
			}
			else if (Edge->DerivedFrom.IsSet())
			{
				const FRoadSegment* Seg = Net.GetSegment(Edge->DerivedFrom);
				const FRoadNode* SA = Seg != nullptr ? Net.GetNode(Seg->A) : nullptr;
				const FRoadNode* SB = Seg != nullptr ? Net.GetNode(Seg->B) : nullptr;
				const URoadProfile* Profile = Seg != nullptr ? Net.ProfileFor(*Seg) : nullptr;
				if (SA == nullptr || SB == nullptr || Profile == nullptr) { return; }
				const FVector2D Axis = SB->Position - SA->Position;
				const double Len = Axis.Size();
				if (Len <= 0.0) { return; }
				const FVector2D Dir = Axis / Len;
				const double T = FVector2D::DotProduct(P - SA->Position, Dir);
				if (T < 0.0 || T > Len) { return; }
				++LanePoints;
				Lane[Slot].Offer(FMath::Abs(FVector2D::CrossProduct(Dir, P - SA->Position)) - 0.5 * Profile->GetTotalWidth(), Where);
			}
		}

		/** Folds the per-sample reaches into Swept. Call once, after the loop. */
		void Finish()
		{
			for (const TPair<FString, FSampleReach>& Pair : Reaches)
			{
				const FSampleReach& R = Pair.Value;
				Swept[R.Slot].Offer(R.In + R.Out - R.Tarmac, FString::Printf(TEXT("%s: swept %.0f (in %.0f + out %.0f) vs tarmac %.0f"),
					*R.Where, R.In + R.Out, R.In, R.Out, R.Tarmac));
			}
		}
	};

	/**
	 * Ticks the network and the course at a fixed step until one loop is done or MaxTicks pass,
	 * probing the tow every other tick when a probe is given. Returns the ticks taken.
	 */
	int32 RunLoop(ARoadNetworkActor& Actor, ARigTestCourse& Course, int32 MaxTicks, FClearanceProbe* Probe = nullptr)
	{
		constexpr float Step = 0.05f;
		int32 Ticks = 0;
		for (; Ticks < MaxTicks && Course.LoopsCompletedForTest() < 1; ++Ticks)
		{
			Actor.Tick(Step);
			Course.Tick(Step);
			const int32 Id = Course.GetActiveAgentIdForTest();
			if (Probe != nullptr && Id != 0 && (Ticks % 2) == 0)
			{
				const FRoadAgent* Agent = Actor.GetGroundTraffic()->FindAgent(Id);
				const int32 Leg = Course.GetActiveLegForTest();
				const int32 Slot = Course.GetActiveSlotForTest();
				if (Agent != nullptr)
				{
					const TArray<FRigCourseWaypoint>& W = Course.GetWaypoints();
					Probe->Measure(*Actor.Network, *Agent, Course.GetVehicles()[Slot], Leg, Slot,
						FString::Printf(TEXT("leg %d (%s) vehicle %d"), Leg, *W[(Leg + 1) % W.Num()].Label, Slot));
				}
			}
		}
		return Ticks;
	}

	/** 10 000 s of sim time at 0.05 s: ~15x a healthy loop. */
	constexpr int32 MaxTicks = 200000;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseOneLoopHeadlessTest,
	"AirportMgr.RigCourse.OneLoopHeadless",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseOneLoopHeadlessTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }

	Course->BuildCourseForTest(*Actor);
	const URoadNetwork& Net = *Actor->Network;

	// THE LAYOUT: every feature at every tier, so one loop says which tier each vehicle fits.
	TestEqual(TEXT("every road the course asked for was laid - a refused segment would change the course silently"),
		Course->GetRefusedConnectsForTest(), 0);
	TestEqual(TEXT("3 tiers x 5 features (straight, right 90, left 90, T both ways, dead end)"),
		Course->FeatureCountForTest(), 15);
	const TArray<FRigCourseWaypoint>& Waypoints = Course->GetWaypoints();
	const int32 Legs = Course->LegCountForTest();
	TestEqual(TEXT("one leg per waypoint, the last wrapping to the first"), Legs, Waypoints.Num());
	int32 WidthStepLeg = INDEX_NONE;
	for (int32 I = 0; I < Waypoints.Num(); ++I)
	{
		const FRoadNode* Node = Net.GetNode(Waypoints[I].Node);
		TestTrue(FString::Printf(TEXT("waypoint %d (%s) is a live node"), I, *Waypoints[I].Label),
			Node != nullptr && Node->bAlive);
		TestTrue(FString::Printf(TEXT("and names a lane end in the derived graph (%s)"), *Waypoints[I].Label),
			ARigTestCourse::ResolveWaypoint(Net, Waypoints[I]).IsSet());
		if (Waypoints[I].Feature == ERigCourseFeature::WidthStep)
		{
			TestEqual(TEXT("exactly one width-step feature"), WidthStepLeg, static_cast<int32>(INDEX_NONE));
			WidthStepLeg = (I + Legs - 1) % Legs;   // the leg that ENDS at this waypoint
		}
	}
	if (!TestTrue(TEXT("the Narrow->Wide mid-straight step is on the course"), WidthStepLeg != INDEX_NONE)) { return false; }

	// WHAT EACH VEHICLE SHOULD FIT, asked of the router independently of the course's driver.
	const TArray<FVehicle>& Vehicles = Course->GetVehicles();
	if (!TestEqual(TEXT("two vehicles: the rig, then the utility + trailer"), Vehicles.Num(), 2)) { return false; }
	TestTrue(TEXT("both vehicles tow something - the course is for chains"),
		Vehicles[0].HasTrailer() && Vehicles[1].HasTrailer());
	TArray<bool> Expected;
	for (int32 L = 0; L < Legs; ++L)
	{
		for (int32 V = 0; V < Vehicles.Num(); ++V)
		{
			Expected.Add(Fits(Net, Waypoints[L], Waypoints[(L + 1) % Legs], Vehicles[V]));
		}
	}

	// ONE LOOP of both vehicles at a fixed step, bounded so a hang fails rather than spins.
	FWarningSpy Spy;
	FClearanceProbe Probe;
	GLog->AddOutputDevice(&Spy);
	const int32 Ticks = RunLoop(*Actor, *Course, MaxTicks, &Probe);
	GLog->RemoveOutputDevice(&Spy);
	Probe.Finish();
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: loop took %d ticks (%.0f s sim); %d turn / %d lane tow point(s) probed"),
		Ticks, Ticks * 0.05, Probe.TurnPoints, Probe.LanePoints);
	for (int32 V = 0; V < 2; ++V)
	{
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicle %d tow - swept worst %.0f uu over (%s); per-side worst %.0f uu (%s); lane worst %.0f uu (%s)"),
			V, Probe.Swept[V].Excess, *Probe.Swept[V].Where, Probe.PerSide[V].Excess, *Probe.PerSide[V].Where,
			Probe.Lane[V].Excess, *Probe.Lane[V].Where);
	}
	if (!TestEqual(TEXT("one loop completed within the tick bound"), Course->LoopsCompletedForTest(), 1)) { return false; }

	TestEqual(TEXT("no tow jack-knifed - driving forwards within the lock never reaches the guard"),
		Spy.Containing(TEXT("jack-knifed")), 0);
	TestEqual(TEXT("no leg entered Reversing - the course has no reverse legs"), Spy.Containing(TEXT("Reversing")), 0);
	TestEqual(TEXT("no leg timed out stuck"), Spy.Containing(TEXT("stuck")), 0);

	// THE TOW STAYED ON THE TARMAC. 10 uu is the builder's clearance march step (ClearanceStep):
	// the measured clearance is short of the real edge by up to that much.
	TestTrue(TEXT("the probe measured turn samples and lane samples - it is not vacuous"),
		Probe.TurnPoints > 0 && Probe.LanePoints > 0);
	for (int32 V = 0; V < 2; ++V)
	{
		TestTrue(FString::Printf(TEXT("vehicle %d: the driven tow's swept width fits the tarmac at every turn sample - VehicleFit's rule, held on the agent (worst %.0f uu: %s)"),
			V, Probe.Swept[V].Excess, *Probe.Swept[V].Where), Probe.Swept[V].Excess <= 10.0);
	}
	TestTrue(FString::Printf(TEXT("the utility's tow stays within the road's width on every lane (worst %.0f uu: %s)"),
		Probe.Lane[1].Excess, *Probe.Lane[1].Where), Probe.Lane[1].Excess <= 10.0);
	// PER SIDE, PINNED, NOT PASSED (measured 2026-09-25): the rig's trailer cuts 2-3.4 m past the
	// INNER pavement edge on every near-side (right) turn, while its swept width fits - VehicleFit
	// admits by width because a real driver swings wide, and the agent does not swing: it holds
	// its lane line. The utility's short trailer stays on. A decision for the spec, not this
	// course; when the agent learns to swing wide (or VehicleFit asks per side) the rig's line
	// here goes red and is to be flipped.
	TestTrue(FString::Printf(TEXT("the utility's tow stays on the tarmac per side (worst %.0f uu: %s)"),
		Probe.PerSide[1].Excess, *Probe.PerSide[1].Where), Probe.PerSide[1].Excess <= 10.0);
	TestTrue(FString::Printf(TEXT("KNOWN GAP: the rig's trailer cuts in past the inner edge on near-side turns (worst %.0f uu: %s)"),
		Probe.PerSide[0].Excess, *Probe.PerSide[0].Where), Probe.PerSide[0].Excess > 10.0);
	// AND NO WORSE THAN MEASURED: the floor above only says the gap is still there, so a
	// regression that put the trailer further off the pavement would pass it. The ceiling is the
	// measurement - worst 342 uu at the east link's corner, 2026-09-25 - plus a little room.
	TestTrue(FString::Printf(TEXT("KNOWN GAP, bounded: the rig's inner cut-in is no worse than the 3.4 m measured on 2026-09-25 (worst %.0f uu: %s)"),
		Probe.PerSide[0].Excess, *Probe.PerSide[0].Where), Probe.PerSide[0].Excess <= 400.0);
	// The same cut-in shows on the LANE just before a near-side corner (measured 164 uu at the
	// east link's stub): the trailer leaves the road before the junction pavement begins. Bound
	// it by the turn's own overrun - it is the approach to that cut, never a worse one.
	TestTrue(FString::Printf(TEXT("KNOWN GAP: the rig's lane overrun is the approach to that same cut-in, no worse (lane %.0f uu: %s)"),
		Probe.Lane[0].Excess, *Probe.Lane[0].Where), Probe.Lane[0].Excess <= Probe.PerSide[0].Excess + 10.0);

	const TArray<FRigLegResult>& Results = Course->LastLoopResultsForTest();
	if (!TestEqual(TEXT("a result per leg per vehicle"), Results.Num(), Legs * Vehicles.Num())) { return false; }
	int32 Refusals = 0;
	for (int32 L = 0; L < Legs; ++L)
	{
		for (int32 V = 0; V < Vehicles.Num(); ++V)
		{
			const FRigLegResult& R = Results[L * Vehicles.Num() + V];
			const FString What = FString::Printf(TEXT("leg %d (%s), vehicle %d"), L, *Waypoints[(L + 1) % Legs].Label, V);
			TestTrue(What + TEXT(" was driven or refused"),
				R.Outcome == ERigLegOutcome::Driven || R.Outcome == ERigLegOutcome::Refused);
			TestEqual(What + TEXT(": refused exactly when the router refuses it with this body"),
				R.Outcome == ERigLegOutcome::Refused, !Expected[L * Vehicles.Num() + V]);
			TestFalse(What + TEXT(" never reversed"), R.bReversed);
			if (R.Outcome == ERigLegOutcome::Driven)
			{
				// The steered axle walks the line, so arrival is measured along it; the chassis
				// origin then sits one wheelbase back from the lane end, and no further.
				const double Wheelbase = Vehicles[V].Chassis.Wheelbase();
				TestTrue(What + TEXT(" drove its whole line to the waypoint's lane end"), R.DistanceLeft < 50.0);
				TestTrue(What + TEXT(" and stopped with its fixed axle a wheelbase behind that lane end"),
					FMath::Abs(FVector2D::Distance(R.EndPosition, R.GoalPosition) - Wheelbase) < 50.0);

				// THE WIDTH-STEP DEFECT, PINNED (controller ruling 5): FSpeedProfile reports a
				// sharp vertex on the one leg that crosses a mid-straight width change, and on no
				// other. When the builder blends the lane offset this goes red, on purpose.
				if (L == WidthStepLeg)
				{
					TestTrue(What + TEXT(": the width step's jog is a sharp vertex FSpeedProfile crawls (known builder defect)"),
						R.SharpVertexCount > 0);
				}
				else
				{
					TestEqual(What + TEXT(" has no sharp vertex - only the width step does"), R.SharpVertexCount, 0);
				}
			}
			Refusals += R.Outcome == ERigLegOutcome::Refused ? 1 : 0;
		}
	}
	// The width step's own expected outcome: DRIVEN, crawling, within the 3x allowance.
	for (int32 V = 0; V < Vehicles.Num(); ++V)
	{
		TestEqual(FString::Printf(TEXT("vehicle %d drives the Narrow->Wide mid-straight step (crawling, not timed out)"), V),
			static_cast<int32>(Results[WidthStepLeg * Vehicles.Num() + V].Outcome), static_cast<int32>(ERigLegOutcome::Driven));
	}
	TestEqual(TEXT("each refusal was logged once in the loop"), Spy.Containing(TEXT("refused:")), Refusals);
	return true;
}

// THE STUCK EXIT: a loop must never wait on an arrival that cannot come. A tiny allowance makes
// healthy legs time out; each must be logged, retired and skipped, and the loop still end.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseStuckIsSkippedTest,
	"AirportMgr.RigCourse.StuckIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseStuckIsSkippedTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	Course->SetLegTimeoutFactorForTest(0.05);

	FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	RunLoop(*Actor, *Course, MaxTicks);
	GLog->RemoveOutputDevice(&Spy);

	if (!TestEqual(TEXT("the loop completes within the tick bound although legs time out"),
		Course->LoopsCompletedForTest(), 1)) { return false; }
	int32 Stuck = 0;
	for (const FRigLegResult& R : Course->LastLoopResultsForTest())
	{
		Stuck += R.Outcome == ERigLegOutcome::Stuck ? 1 : 0;
	}
	TestTrue(TEXT("at least one leg timed out"), Stuck >= 1);
	TestEqual(TEXT("each timed-out leg was logged stuck exactly once"), Spy.Containing(TEXT("stuck")), Stuck);
	TestEqual(TEXT("and every stuck agent was retired - none is left on the road"), Actor->GetAgentCount(), 0);
	return true;
}

// THE JACK-KNIFE EXIT: a folded agent holds for ever, so the course must retire it and go on.
// The rig is handed Airside.Model.Tow.JackknifeStops' hairpin - a 700 uu circle the cab can
// steer and the 10.3 m trailer cannot follow - in place of its first leg.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseJackknifeIsRetiredTest,
	"AirportMgr.RigCourse.JackknifeIsRetired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseJackknifeIsRetiredTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	Course->PlanOverrideForTest = [](int32 Leg, int32 Slot, FRoutePlan& OutPlan)
	{
		if (Leg != 0 || Slot != 0) { return false; }
		OutPlan = FRoutePlan();
		OutPlan.Result = ERouteResult::Found;
		OutPlan.Polyline.Add(FVector2D(0.0, -4000.0));
		const double R = 700.0;
		for (int32 Deg = 0; Deg <= 330; Deg += 3)
		{
			const double A = FMath::DegreesToRadians(static_cast<double>(Deg));
			OutPlan.Polyline.Add(FVector2D(-R + R * FMath::Cos(A), R * FMath::Sin(A)));
		}
		OutPlan.Length = GuidelineGeom::PolylineLength(OutPlan.Polyline);
		return true;
	};

	FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	RunLoop(*Actor, *Course, MaxTicks);
	GLog->RemoveOutputDevice(&Spy);

	if (!TestEqual(TEXT("the loop completes although the rig folded on leg 0"), Course->LoopsCompletedForTest(), 1)) { return false; }
	const FRigLegResult& Folded = Course->LastLoopResultsForTest()[0];
	TestEqual(TEXT("leg 0's rig attempt ended Jackknifed"),
		static_cast<int32>(Folded.Outcome), static_cast<int32>(ERigLegOutcome::Jackknifed));
	TestEqual(TEXT("the course's jack-knife line fired exactly once"),
		Spy.Containing(TEXT("RigCourse:"), TEXT("jack-knifed at link")), 1);
	TestTrue(TEXT("the folded agent had an id"), Folded.AgentId != 0);
	TestNull(TEXT("and was retired through the normal path - the model no longer holds it"),
		Actor->GetGroundTraffic()->FindAgent(Folded.AgentId));
	TestEqual(TEXT("no leg was left to time out instead"), Spy.Containing(TEXT("stuck")), 0);
	return true;
}

#endif
