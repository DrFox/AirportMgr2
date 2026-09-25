#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RigTestCourse.h"

#include "Content/AirsideSettings.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/AgentMotion.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/VehicleSweep.h"
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
	 * them. Points behind the plan's start or past its end are skipped: the tow trails back over
	 * road the plan does not name - the previous leg's, since the chain carries across waypoints.
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
			// THE NEAREST SPAN, not the nearest vertex: a lane edge is one span however long, so
			// half-way up the 340 m return road the nearest VERTEX is the corner's last sample,
			// 160 m away - which charged the utility (running that road northwards since the
			// reverse course, 2026-09-25) with a 6.5 m "reach" off a turn it had left long before.
			const TArray<FVector2D>& Line = Plan.Polyline;
			int32 Span = 0;
			double SpanT = 0.0;
			double Best = TNumericLimits<double>::Max();
			for (int32 I = 0; I + 1 < Line.Num(); ++I)
			{
				const FVector2D AB = Line[I + 1] - Line[I];
				const double LenSq = AB.SizeSquared();
				const double T = LenSq > 0.0 ? FVector2D::DotProduct(P - Line[I], AB) / LenSq : 0.0;
				const double D = FVector2D::DistSquared(P, Line[I] + AB * FMath::Clamp(T, 0.0, 1.0));
				if (D < Best) { Best = D; Span = I; SpanT = T; }
			}
			const int32 Last = Line.Num() - 1;
			if ((Span == 0 && SpanT < 0.0) || (Span == Last - 1 && SpanT > 1.0))
			{
				return;
			}
			// The span's nearer end is the sample measured against; the step is the one the SPAN is in.
			const int32 V = SpanT < 0.5 ? Span : Span + 1;
			int32 StepIndex = 0;
			while (StepIndex < Plan.Steps.Num() - 1 && Plan.Steps[StepIndex].EndVertex < Span + 1) { ++StepIndex; }
			const FRouteStep& Step = Plan.Steps[StepIndex];
			const int32 StartV = StepIndex == 0 ? 0 : Plan.Steps[StepIndex - 1].EndVertex;
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Step.Edge);
			if (Edge == nullptr) { return; }
			const FGuidelineNode* EdgeA = Net.GetGuidelineNode(Edge->A);
			const FGuidelineNode* EdgeB = Net.GetGuidelineNode(Edge->B);
			const FString Where = FString::Printf(TEXT("%s, link %d, at (%.0f, %.0f) on %s edge %d (%.0f, %.0f)->(%.0f, %.0f), plan vertex %d"),
				*What, LinkIndex, P.X, P.Y, Edge->ClearInnerAt.Num() > 0 ? TEXT("turn") : TEXT("lane"), Step.Edge.Index,
				EdgeA != nullptr ? EdgeA->Position.X : 0.0, EdgeA != nullptr ? EdgeA->Position.Y : 0.0,
				EdgeB != nullptr ? EdgeB->Position.X : 0.0, EdgeB != nullptr ? EdgeB->Position.Y : 0.0, V);

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

	/** A waypoint arrived at the other way - what the reverse runner stops at, built here from Next. */
	FRigCourseWaypoint Reversed(const FRigCourseWaypoint& W)
	{
		FRigCourseWaypoint Out = W;
		Out.From = W.Next;
		return Out;
	}

	/**
	 * THE CHAIN ACROSS A WAYPOINT. A handover is a tick on which a runner's position moved on
	 * while its agent stayed the same - the redirect. Over that tick and the next, no tow axle
	 * may move further than one tow sub-step's travel (VehicleSweep::TraceStep): the vehicle is
	 * at rest either side of it, so a larger jump is the chain being re-laid, not driven.
	 * RelayWouldMove is the control: how far LayChainStraight would have moved the axles at the
	 * same moment, so a pass is known to be able to fail.
	 */
	struct FContinuity
	{
		int32 PrevAgent[2] = { 0, 0 };
		int32 PrevPosition[2] = { 0, 0 };
		int32 PrevLoops[2] = { 0, 0 };
		TArray<FVector2D> PrevAxles[2];
		FVector2D PrevAt[2] = { FVector2D::ZeroVector, FVector2D::ZeroVector };
		double PrevHeading[2] = { 0.0, 0.0 };
		int32 Window[2] = { 0, 0 };
		int32 Handovers[2] = { 0, 0 };
		FWorst Worst[2];
		FWorst RelayWouldMove[2];

		void Observe(const ARoadNetworkActor& Actor, const ARigTestCourse& Course, int32 Slot, int32 Tick)
		{
			const FRigCourseRunner& Runner = Course.GetRunnerForTest(Slot);
			const FRoadAgent* Agent = Runner.AgentId != 0 ? Actor.GetGroundTraffic()->FindAgent(Runner.AgentId) : nullptr;
			if (Agent == nullptr)
			{
				PrevAgent[Slot] = 0;
				return;
			}
			const TArray<FVector2D>& Axles = Agent->TowAxles;
			if (Runner.AgentId == PrevAgent[Slot] && PrevAxles[Slot].Num() == Axles.Num())
			{
				const bool bHandover = Runner.Position != PrevPosition[Slot] || Runner.LoopsCompleted != PrevLoops[Slot];
				if (bHandover)
				{
					++Handovers[Slot];
					Window[Slot] = 2;
					// The control: the same chain laid straight behind the cab where it stopped.
					const FVehicle& Vehicle = Course.GetVehicles()[Slot];
					const FVector2D Forward(FMath::Cos(PrevHeading[Slot]), FMath::Sin(PrevHeading[Slot]));
					TArray<FVector2D> Straight;
					VehicleSweep::LayChainStraight(VehicleFit::BodyOf(Vehicle),
						PrevAt[Slot] + Forward * Vehicle.Chassis.FixedAxleX, Forward, Straight);
					double Relay = 0.0;
					for (int32 I = 0; I < Straight.Num() && I < Axles.Num(); ++I)
					{
						Relay = FMath::Max(Relay, FVector2D::Distance(Straight[I], PrevAxles[Slot][I]));
					}
					RelayWouldMove[Slot].Offer(Relay, FString::Printf(TEXT("tick %d, position %d"), Tick, Runner.Position));
				}
				if (Window[Slot] > 0)
				{
					--Window[Slot];
					double Moved = 0.0;
					for (int32 I = 0; I < Axles.Num(); ++I)
					{
						Moved = FMath::Max(Moved, FVector2D::Distance(Axles[I], PrevAxles[Slot][I]));
					}
					Worst[Slot].Offer(Moved, FString::Printf(TEXT("tick %d, position %d"), Tick, Runner.Position));
				}
			}
			PrevAgent[Slot] = Runner.AgentId;
			PrevPosition[Slot] = Runner.Position;
			PrevLoops[Slot] = Runner.LoopsCompleted;
			PrevAxles[Slot] = Axles;
			// The pose is only real once Advance has run: RestartTaxi leaves a bare fallback.
			if (Agent->LastMotion.Tow.Num() == Axles.Num())
			{
				PrevAt[Slot] = Agent->LastMotion.Position;
				PrevHeading[Slot] = Agent->LastMotion.Heading;
			}
		}
	};

	/** An oriented rectangle down a centreline, for the overlap measure. */
	struct FBox2
	{
		FVector2D Rear;
		FVector2D Front;
		double Half = 0.0;

		void Corners(FVector2D Out[4]) const
		{
			const FVector2D H = (Front - Rear).GetSafeNormal();
			const FVector2D N(-H.Y, H.X);
			Out[0] = Rear + N * Half;
			Out[1] = Front + N * Half;
			Out[2] = Front - N * Half;
			Out[3] = Rear - N * Half;
		}
	};

	/** Separating-axis penetration of two boxes, uu: 0 when apart, else the least push that parts them. */
	double Penetration(const FBox2& A, const FBox2& B)
	{
		FVector2D CA[4], CB[4];
		A.Corners(CA);
		B.Corners(CB);
		double Least = TNumericLimits<double>::Max();
		for (const FVector2D* Poly : { CA, CB })
		{
			for (int32 E = 0; E < 2; ++E)
			{
				const FVector2D Axis = (Poly[E + 1] - Poly[E]).GetSafeNormal();
				double MinA = TNumericLimits<double>::Max(), MaxA = -MinA, MinB = MinA, MaxB = -MinA;
				for (int32 I = 0; I < 4; ++I)
				{
					const double PA = FVector2D::DotProduct(CA[I], Axis);
					const double PB = FVector2D::DotProduct(CB[I], Axis);
					MinA = FMath::Min(MinA, PA); MaxA = FMath::Max(MaxA, PA);
					MinB = FMath::Min(MinB, PB); MaxB = FMath::Max(MaxB, PB);
				}
				const double Overlap = FMath::Min(MaxA, MaxB) - FMath::Max(MinA, MinB);
				if (Overlap <= 0.0)
				{
					return 0.0;
				}
				Least = FMath::Min(Least, Overlap);
			}
		}
		return Least;
	}

	/** Every body of an agent - the cab and each body-carrying link - as boxes. */
	void BodiesOf(const FRoadAgent& Agent, const FVehicle& Vehicle, TArray<FBox2>& Out)
	{
		const FAgentMotion& M = Agent.LastMotion;
		const FVector2D H(FMath::Cos(M.Heading), FMath::Sin(M.Heading));
		const FVector2D Fixed = M.Position + H * Vehicle.Chassis.FixedAxleX;
		Out.Add({ Fixed + H * Vehicle.BodyRearX, Fixed + H * Vehicle.BodyFrontX, 0.5 * Vehicle.BodyWidth });
		for (int32 I = 0; I < Vehicle.Tow.Num() && I < M.Tow.Num(); ++I)
		{
			const FTowLink& Link = Vehicle.Tow[I];
			if (Link.IsBar()) { continue; }
			const FTowPose& Pose = M.Tow[I];
			const FVector2D LH(FMath::Cos(Pose.Heading), FMath::Sin(Pose.Heading));
			Out.Add({ Pose.Axle - LH * Link.BodyRear, Pose.Hitch + LH * Link.BodyFront, 0.5 * Link.Width });
		}
	}

	/**
	 * THE TWO VEHICLES AGAINST EACH OTHER. Measured and reported, NOT asserted: claims do not
	 * reserve the oncoming lane while the rig swings across it (spec "Out of this step"), so an
	 * overlap on a corner is the known gap, and this says how big it is.
	 */
	struct FOverlap
	{
		FWorst Worst;
		int32 TicksOverlapping = 0;
		/** Runs of consecutive overlapping ticks, each logged where it began - so a spawn and a corner are told apart. */
		int32 Episodes = 0;
		bool bWasOverlapping = false;

		void Observe(const ARoadNetworkActor& Actor, const ARigTestCourse& Course, int32 Tick)
		{
			TArray<FBox2> Bodies[2];
			FVector2D At[2];
			for (int32 Slot = 0; Slot < 2; ++Slot)
			{
				const FRigCourseRunner& Runner = Course.GetRunnerForTest(Slot);
				const FRoadAgent* Agent = Runner.AgentId != 0 ? Actor.GetGroundTraffic()->FindAgent(Runner.AgentId) : nullptr;
				const FVehicle& Vehicle = Course.GetVehicles()[Slot];
				if (Agent == nullptr || Agent->LastMotion.Tow.Num() != Vehicle.Tow.Num()) { return; }
				BodiesOf(*Agent, Vehicle, Bodies[Slot]);
				At[Slot] = Agent->LastMotion.Position;
			}
			double Deepest = 0.0;
			for (const FBox2& A : Bodies[0])
			{
				for (const FBox2& B : Bodies[1])
				{
					Deepest = FMath::Max(Deepest, Penetration(A, B));
				}
			}
			if (Deepest > 0.0)
			{
				++TicksOverlapping;
				const TArray<FRigCourseWaypoint>& W = Course.GetWaypoints();
				const int32 RigLeg = Course.LegAt(false, Course.GetRunnerForTest(0).Target - 1);
				const int32 UtilityLeg = Course.LegAt(true, Course.GetRunnerForTest(1).Target - 1);
				const FString Where = FString::Printf(TEXT("t %.2f s: rig at (%.0f, %.0f) on leg %d (%s), utility at (%.0f, %.0f) on leg %d (%s, reversed)"),
					Tick * 0.05, At[0].X, At[0].Y, RigLeg, *W[(RigLeg + 1) % W.Num()].Label,
					At[1].X, At[1].Y, UtilityLeg, *W[(UtilityLeg + 1) % W.Num()].Label);
				if (!bWasOverlapping)
				{
					++Episodes;
					UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: overlap episode %d begins, %.0f uu, %s"), Episodes, Deepest, *Where);
				}
				Worst.Offer(Deepest, Where);
			}
			bWasOverlapping = Deepest > 0.0;
		}
	};

	/** What a run watches, each optional. */
	struct FObservers
	{
		FClearanceProbe* Probe = nullptr;
		FContinuity* Continuity = nullptr;
		FOverlap* Overlap = nullptr;
	};

	/**
	 * Ticks the network and the course at a fixed step until Done or MaxTicks pass, observing
	 * both vehicles: the tow probe every other tick, continuity and overlap every tick. Returns
	 * the ticks taken.
	 */
	int32 RunUntil(ARoadNetworkActor& Actor, ARigTestCourse& Course, int32 MaxTicks, TFunctionRef<bool()> Done,
		const FObservers& Watch = FObservers())
	{
		constexpr float Step = 0.05f;
		int32 Ticks = 0;
		for (; Ticks < MaxTicks && !Done(); ++Ticks)
		{
			Actor.Tick(Step);
			Course.Tick(Step);
			for (int32 Slot = 0; Slot < Course.GetVehicles().Num(); ++Slot)
			{
				if (Watch.Continuity != nullptr)
				{
					Watch.Continuity->Observe(Actor, Course, Slot, Ticks);
				}
				const FRigCourseRunner& Runner = Course.GetRunnerForTest(Slot);
				if (Watch.Probe == nullptr || Runner.AgentId == 0 || (Ticks % 2) != 0) { continue; }
				const FRoadAgent* Agent = Actor.GetGroundTraffic()->FindAgent(Runner.AgentId);
				if (Agent != nullptr)
				{
					const TArray<FRigCourseWaypoint>& W = Course.GetWaypoints();
					const int32 Leg = Course.LegAt(Runner.bReverse, Runner.Target - 1);
					Watch.Probe->Measure(*Actor.Network, *Agent, Course.GetVehicles()[Slot], Leg, Slot,
						FString::Printf(TEXT("leg %d (%s%s) vehicle %d"), Leg, *W[(Leg + 1) % W.Num()].Label,
							Runner.bReverse ? TEXT(", reversed") : TEXT(""), Slot));
				}
			}
			if (Watch.Overlap != nullptr)
			{
				Watch.Overlap->Observe(Actor, Course, Ticks);
			}
		}
		return Ticks;
	}

	/** 10 000 s of sim time at 0.05 s: ~15x a healthy loop. */
	constexpr int32 MaxTicks = 200000;

	const TCHAR* const Names[2] = { TEXT("rig"), TEXT("utility") };
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
		// The reverse runner's stop: the same node along the road the forward leg leaves by.
		TestTrue(FString::Printf(TEXT("and, arrived the other way along Next, names a lane end too (%s)"), *Waypoints[I].Label),
			ARigTestCourse::ResolveWaypoint(Net, Reversed(Waypoints[I])).IsSet());
		if (Waypoints[I].Feature == ERigCourseFeature::WidthStep)
		{
			TestEqual(TEXT("exactly one width-step feature"), WidthStepLeg, static_cast<int32>(INDEX_NONE));
			WidthStepLeg = (I + Legs - 1) % Legs;   // the leg that ENDS at this waypoint
		}
	}
	if (!TestTrue(TEXT("the Narrow->Wide mid-straight step is on the course"), WidthStepLeg != INDEX_NONE)) { return false; }

	// WHAT EACH VEHICLE SHOULD FIT, asked of the router independently of the course's driver,
	// each in its OWN direction: forward leg L for the rig, and for the utility the same node
	// pair driven the other way, from waypoint L + 1 to waypoint L.
	const TArray<FVehicle>& Vehicles = Course->GetVehicles();
	if (!TestEqual(TEXT("two vehicles: the rig, then the utility + trailer"), Vehicles.Num(), 2)) { return false; }
	TestTrue(TEXT("both vehicles tow something - the course is for chains"),
		Vehicles[0].HasTrailer() && Vehicles[1].HasTrailer());
	TestFalse(TEXT("the rig runs the course forwards"), Course->GetRunnerForTest(0).bReverse);
	TestTrue(TEXT("and the utility in reverse"), Course->GetRunnerForTest(1).bReverse);
	TArray<bool> Expected[2];
	for (int32 L = 0; L < Legs; ++L)
	{
		const FRigCourseWaypoint& A = Waypoints[L];
		const FRigCourseWaypoint& B = Waypoints[(L + 1) % Legs];
		Expected[0].Add(Fits(Net, A, B, Vehicles[0]));
		Expected[1].Add(Fits(Net, Reversed(B), Reversed(A), Vehicles[1]));
	}

	// BOTH VEHICLES AT ONCE until each has done one loop, at a fixed step, bounded so a hang -
	// or a traffic deadlock between the two - fails rather than spins.
	FWarningSpy Spy;
	FClearanceProbe Probe;
	FContinuity Continuity;
	FOverlap Overlap;
	FObservers Watch;
	Watch.Probe = &Probe;
	Watch.Continuity = &Continuity;
	Watch.Overlap = &Overlap;
	GLog->AddOutputDevice(&Spy);
	const int32 Ticks = RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedByAllForTest() >= 1; }, Watch);
	GLog->RemoveOutputDevice(&Spy);
	Probe.Finish();
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: both loops took %d ticks (%.0f s sim); rig %d loop(s), utility %d; %d turn / %d lane tow point(s) probed"),
		Ticks, Ticks * 0.05, Course->LoopsCompletedForTest(0), Course->LoopsCompletedForTest(1), Probe.TurnPoints, Probe.LanePoints);
	for (int32 V = 0; V < 2; ++V)
	{
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicle %d tow - swept worst %.0f uu over (%s); per-side worst %.0f uu (%s); lane worst %.0f uu (%s)"),
			V, Probe.Swept[V].Excess, *Probe.Swept[V].Where, Probe.PerSide[V].Excess, *Probe.PerSide[V].Where,
			Probe.Lane[V].Excess, *Probe.Lane[V].Where);
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicle %d chain - %d handover(s), %d agent(s) dispatched; worst axle move across a handover %.2f uu (%s); a re-lay would have moved it up to %.1f uu (%s)"),
			V, Continuity.Handovers[V], Course->GetRunnerForTest(V).Dispatches, Continuity.Worst[V].Excess, *Continuity.Worst[V].Where,
			Continuity.RelayWouldMove[V].Excess, *Continuity.RelayWouldMove[V].Where);
	}
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicles overlapped on %d tick(s) in %d episode(s); worst penetration %.0f uu (%s)"),
		Overlap.TicksOverlapping, Overlap.Episodes, Overlap.TicksOverlapping > 0 ? Overlap.Worst.Excess : 0.0, *Overlap.Worst.Where);
	if (!TestTrue(TEXT("both vehicles completed a loop within the tick bound - no hang, no deadlock between them"),
		Course->LoopsCompletedByAllForTest() >= 1)) { return false; }

	TestEqual(TEXT("no tow jack-knifed - driving forwards within the lock never reaches the guard"),
		Spy.Containing(TEXT("jack-knifed")), 0);
	TestEqual(TEXT("no leg entered Reversing - the course has no reverse legs"), Spy.Containing(TEXT("Reversing")), 0);
	TestEqual(TEXT("no leg timed out stuck"), Spy.Containing(TEXT("stuck")), 0);

	// THE CHAIN IS CONTINUOUS ACROSS WAYPOINTS: one agent per vehicle, redirected at each one,
	// its axles moving no more than a sub-step over the handover - except where the course had
	// to dispatch fresh (a stranding: the rig at a dead end it cannot U-turn in).
	for (int32 V = 0; V < 2; ++V)
	{
		const int32 Strandings = Spy.Containing(*FString::Printf(TEXT("RigCourse: %s loop"), Names[V]), TEXT("stranded"));
		TestEqual(FString::Printf(TEXT("%s: one agent drove every leg, plus one fresh agent per stranding (%d) - never one per leg"), Names[V], Strandings),
			Course->GetRunnerForTest(V).Dispatches, 1 + Strandings);
		TestTrue(FString::Printf(TEXT("%s: handovers were observed - the continuity check is not vacuous (%d)"), Names[V], Continuity.Handovers[V]),
			Continuity.Handovers[V] >= Legs - 1 - 2 * Strandings);
		TestTrue(FString::Printf(TEXT("%s: no tow axle moved more than one sub-step (%.0f uu) across a handover (worst %.2f uu: %s) - the chain was not re-laid"),
			Names[V], VehicleSweep::TraceStep, Continuity.Worst[V].Excess, *Continuity.Worst[V].Where),
			Continuity.Worst[V].Excess <= VehicleSweep::TraceStep);
	}
	TestEqual(TEXT("the utility is never stranded - it fits every leg in its direction"),
		Spy.Containing(TEXT("RigCourse: utility loop"), TEXT("stranded")), 0);

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
	// east link's stub when every leg began with the chain laid straight; 261 uu at tier 1's
	// entry stub once the chain carried across waypoints, 2026-09-25): the trailer leaves the
	// road before the junction pavement begins. Bound it by the turn's own overrun - it is the
	// approach to that cut, never a worse one.
	TestTrue(FString::Printf(TEXT("KNOWN GAP: the rig's lane overrun is the approach to that same cut-in, no worse (lane %.0f uu: %s)"),
		Probe.Lane[0].Excess, *Probe.Lane[0].Where), Probe.Lane[0].Excess <= Probe.PerSide[0].Excess + 10.0);

	for (int32 V = 0; V < Vehicles.Num(); ++V)
	{
		// THE JOG IS IN THE LEG THAT LEAVES THE STEP NODE. Forwards that is WidthStepLeg itself;
		// in reverse the runner arrives at the step node BEFORE the jog, and the leg leaving it is
		// the node pair of the forward leg one earlier - a reversed leg turns at its other end.
		const int32 JogLeg = V == 0 ? WidthStepLeg : (WidthStepLeg + Legs - 1) % Legs;
		const TArray<FRigLegResult>& Results = Course->LastLoopResultsForTest(V);
		if (!TestEqual(TEXT("a result per leg"), Results.Num(), Legs)) { return false; }
		int32 Refusals = 0;
		for (int32 L = 0; L < Legs; ++L)
		{
			const FRigLegResult& R = Results[L];
			const FString What = FString::Printf(TEXT("%s leg %d (%s%s)"), Names[V], L, *Waypoints[(L + 1) % Legs].Label,
				V == 1 ? TEXT(", reversed") : TEXT(""));
			TestTrue(What + TEXT(" was driven or refused"),
				R.Outcome == ERigLegOutcome::Driven || R.Outcome == ERigLegOutcome::Refused);
			TestEqual(What + TEXT(": refused exactly when the router refuses it with this body, in this direction"),
				R.Outcome == ERigLegOutcome::Refused, !Expected[V][L]);
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
				// other - in either direction. When the builder blends the lane offset this goes
				// red, on purpose.
				if (L == JogLeg)
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
		// The width step's own expected outcome: DRIVEN, crawling, within the 3x allowance.
		TestEqual(FString::Printf(TEXT("%s drives the Narrow->Wide mid-straight step (crawling, not timed out)"), Names[V]),
			static_cast<int32>(Results[JogLeg].Outcome), static_cast<int32>(ERigLegOutcome::Driven));
		// ONCE PER LOOP: counted in the loop these results are from, by the loop number in the line.
		const int32 Loop = Course->LoopsCompletedForTest(V);
		TestEqual(FString::Printf(TEXT("%s: each refusal was logged once in loop %d"), Names[V], Loop),
			Spy.Containing(*FString::Printf(TEXT("RigCourse: %s loop %d leg "), Names[V], Loop), TEXT("refused:")), Refusals);
	}
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
	RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedByAllForTest() >= 1; });
	GLog->RemoveOutputDevice(&Spy);

	if (!TestTrue(TEXT("both loops complete within the tick bound although legs time out"),
		Course->LoopsCompletedByAllForTest() >= 1)) { return false; }
	for (int32 V = 0; V < 2; ++V)
	{
		int32 Stuck = 0;
		for (const FRigLegResult& R : Course->LastLoopResultsForTest(V))
		{
			if (R.Outcome != ERigLegOutcome::Stuck) { continue; }
			++Stuck;
			// Retired, not left holding the road: the persistent agent is given up on a stuck leg.
			TestTrue(FString::Printf(TEXT("%s: the stuck leg's agent had an id"), Names[V]), R.AgentId != 0);
			TestNull(FString::Printf(TEXT("%s: and was retired - the model no longer holds it"), Names[V]),
				Actor->GetGroundTraffic()->FindAgent(R.AgentId));
		}
		TestTrue(FString::Printf(TEXT("%s: at least one leg timed out"), Names[V]), Stuck >= 1);
		const int32 Loop = Course->LoopsCompletedForTest(V);
		TestEqual(FString::Printf(TEXT("%s: each timed-out leg was logged stuck exactly once in loop %d"), Names[V], Loop),
			Spy.Containing(*FString::Printf(TEXT("RigCourse: %s loop %d leg "), Names[V], Loop), TEXT("stuck")), Stuck);
	}
	return true;
}

// THE JACK-KNIFE EXIT: a folded agent holds for ever, so the course must retire it and go on.
// The rig is handed Airside.Model.Tow.JackknifeStops' hairpin - a 700 uu circle the cab can
// steer and the 10.3 m trailer cannot follow - in place of its first leg, ONCE.
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
	// ONE-SHOT: the rig reaches leg 0 again at the start of its second loop, and a second fold
	// there would be a second jack-knife line this test does not mean to count.
	TSharedRef<bool> bUsed = MakeShared<bool>(false);
	Course->PlanOverrideForTest = [bUsed](int32 Leg, int32 Slot, FRoutePlan& OutPlan)
	{
		if (Leg != 0 || Slot != 0 || *bUsed) { return false; }
		*bUsed = true;
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
	RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedForTest(0) >= 1; });
	GLog->RemoveOutputDevice(&Spy);

	if (!TestEqual(TEXT("the rig's loop completes although it folded on leg 0"), Course->LoopsCompletedForTest(0), 1)) { return false; }
	const FRigLegResult& Folded = Course->LastLoopResultsForTest(0)[0];
	TestEqual(TEXT("leg 0's rig attempt ended Jackknifed"),
		static_cast<int32>(Folded.Outcome), static_cast<int32>(ERigLegOutcome::Jackknifed));
	TestEqual(TEXT("the course's jack-knife line fired exactly once"),
		Spy.Containing(TEXT("RigCourse:"), TEXT("jack-knifed at link")), 1);
	TestTrue(TEXT("the folded agent had an id"), Folded.AgentId != 0);
	TestNull(TEXT("and was retired through the normal path - the model no longer holds it"),
		Actor->GetGroundTraffic()->FindAgent(Folded.AgentId));
	TestTrue(TEXT("and a fresh agent took the rig on from the next waypoint - the display continues"),
		Course->GetRunnerForTest(0).Dispatches >= 2);
	TestEqual(TEXT("leg 1 was driven by that fresh agent"),
		static_cast<int32>(Course->LastLoopResultsForTest(0)[1].Outcome), static_cast<int32>(ERigLegOutcome::Driven));
	TestEqual(TEXT("no leg was left to time out instead"), Spy.Containing(TEXT("stuck")), 0);
	return true;
}

#endif
