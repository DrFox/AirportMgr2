#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "RigTestCourse.h"

#include "Content/AirsideSettings.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
#include "Testing/BendProbe.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Profiles/RoadDesignVehicles.h"

#if WITH_DEV_AUTOMATION_TESTS

// NAMED, NOT ANONYMOUS: the module is a unity build.
namespace RigCourseTest
{
	/** The fixed tick every run here is stepped at, seconds - one figure, read everywhere a tick's length matters. */
	constexpr double TickSeconds = 0.05;

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
	 * SO THOSE GO UNMEASURED: for the first trailer-length of each leg, the part of the tow still
	 * on the previous leg's road is judged by nothing (it was judged, on that leg, until the
	 * handover). A cut-in that only shows in that window would pass here.
	 */
	struct FClearanceProbe
	{
		/** Scales every link's width - 1 always, except in the RED run that proves this measures. */
		double WidthScale = 1.0;

		/** Per vehicle slot. */
		FWorst Swept[2];
		FWorst PerSide[2];
		/** PerSide again, per leg - the loop summary says where each vehicle leaves the tarmac, not only its worst. */
		TMap<int32, double> PerSideByLeg[2];
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
			// A WINDOW OF THE ROUTE round the agent, since a loop is one route (2026-09-25): the
			// course passes some junctions twice a loop (into a stem and out), and a nearest span
			// taken over the whole loop could be the OTHER pass. The tow reaches at most a chain's
			// length behind the steered axle; a little ahead covers the cab's front.
			double ChainBehind = Vehicle.Chassis.Wheelbase() + FMath::Abs(Vehicle.BodyRearX);
			for (const FTowLink& Link : Vehicle.Tow)
			{
				ChainBehind += FMath::Abs(Link.HitchX) + Link.Length + Link.BodyRear + Link.BodyFront;
			}
			const double Lo = Agent.DistanceAlongPlan() - ChainBehind - 500.0;
			const double Hi = Agent.DistanceAlongPlan() + 1000.0;
			SpanLo = 0;
			SpanHi = Plan.Polyline.Num() - 1;
			double Walked = 0.0;
			bool bLoSet = false;
			for (int32 I = 0; I + 1 < Plan.Polyline.Num(); ++I)
			{
				const double Next = Walked + FVector2D::Distance(Plan.Polyline[I], Plan.Polyline[I + 1]);
				if (!bLoSet && Next >= Lo) { SpanLo = I; bLoSet = true; }
				if (Walked > Hi) { SpanHi = I; break; }
				Walked = Next;
			}
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

		/** The spans [SpanLo, SpanHi) of the plan Measure searches - see its window comment. */
		int32 SpanLo = 0;
		int32 SpanHi = 0;

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
			for (int32 I = SpanLo; I < SpanHi && I + 1 < Line.Num(); ++I)
			{
				const FVector2D AB = Line[I + 1] - Line[I];
				const double LenSq = AB.SizeSquared();
				const double T = LenSq > 0.0 ? FVector2D::DotProduct(P - Line[I], AB) / LenSq : 0.0;
				const double D = FVector2D::DistSquared(P, Line[I] + AB * FMath::Clamp(T, 0.0, 1.0));
				if (D < Best) { Best = D; Span = I; SpanT = T; }
			}
			// Behind the window's first span or past its last: road this search does not name.
			if (Best == TNumericLimits<double>::Max() || (Span == SpanLo && SpanT < 0.0)
				|| (Span == FMath::Min(SpanHi, Line.Num() - 1) - 1 && SpanT > 1.0))
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

				double& LegWorst = PerSideByLeg[Slot].FindOrAdd(Leg, -TNumericLimits<double>::Max());
				LegWorst = FMath::Max(LegWorst, Reach >= 0.0 ? Reach - ClearIn : -Reach - ClearOut);
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
	 * THE CHAIN, CONTINUOUS FOR THE WHOLE RUN (one route per loop, 2026-09-25): on every tick
	 * the same agent drives, no tow axle may move further than the cab's own fixed axle moved
	 * plus one tow sub-step (VehicleSweep::TraceStep). A chain re-laid straight would jump by a
	 * trailer's worth of swing; RelayWouldMove, taken as each waypoint is passed, says how far.
	 *
	 * AND THE WAYPOINTS ARE NOT STOPS: on the tick a runner's position moves on, the agent's
	 * speed is recorded against its own speed profile's limit there.
	 */
	struct FContinuity
	{
		struct FPass
		{
			double Speed = 0.0;
			/** The profile's limit where the agent is, and where it was a tick ago (the follower's speed is last tick's). */
			double Limit = 0.0;
			double LimitBehind = 0.0;
			double Cap = 0.0;
			/**
			 * THE FORWARD PASS the profile does not make: the fastest the vehicle can be here,
			 * accelerating out of every profile minimum within an acceleration distance behind (and
			 * from rest at the route's start, while that is within reach). The only excuse for being
			 * below the limit that is not traffic.
			 */
			double Reach = 0.0;
			/** Held short by traffic arbitration: a stop point within the distance it takes to brake. */
			bool bHeld = false;
			FString Where;
		};

		int32 PrevAgent[2] = { 0, 0 };
		int32 PrevPosition[2] = { 0, 0 };
		int32 PrevLoops[2] = { 0, 0 };
		TArray<FVector2D> PrevAxles[2];
		FVector2D PrevAt[2] = { FVector2D::ZeroVector, FVector2D::ZeroVector };
		double PrevHeading[2] = { 0.0, 0.0 };
		int32 Handovers[2] = { 0, 0 };
		FWorst Worst[2];
		FWorst RelayWouldMove[2];
		TArray<FPass> Passes[2];

		/** The tick the slot's first agent was seen, and the tick it first passed a waypoint. */
		int32 FirstSeen[2] = { INDEX_NONE, INDEX_NONE };
		int32 FirstPass[2] = { INDEX_NONE, INDEX_NONE };

		void Observe(const ARoadNetworkActor& Actor, const ARigTestCourse& Course, int32 Slot, int32 Tick)
		{
			const FRigCourseRunner& Runner = Course.GetRunnerForTest(Slot);
			const FRoadAgent* Agent = Runner.AgentId != 0 ? Actor.GetGroundTraffic()->FindAgent(Runner.AgentId) : nullptr;
			if (Agent == nullptr)
			{
				PrevAgent[Slot] = 0;
				return;
			}
			if (FirstSeen[Slot] == INDEX_NONE)
			{
				FirstSeen[Slot] = Tick;
			}
			const FVehicle& Vehicle = Course.GetVehicles()[Slot];
			const TArray<FVector2D>& Axles = Agent->TowAxles;
			const bool bPosed = Agent->LastMotion.Tow.Num() == Axles.Num();
			if (Runner.AgentId == PrevAgent[Slot] && PrevAxles[Slot].Num() == Axles.Num())
			{
				const bool bPassed = Runner.Position != PrevPosition[Slot] || Runner.LoopsCompleted != PrevLoops[Slot];
				if (bPassed)
				{
					++Handovers[Slot];
					if (FirstPass[Slot] == INDEX_NONE)
					{
						FirstPass[Slot] = Tick;
					}
					FPass& Pass = Passes[Slot].AddDefaulted_GetRef();
					Pass.Speed = Agent->Follower.Speed;
					Pass.Limit = Agent->Follower.Profile.LimitAt(Agent->Follower.Travelled);
					Pass.LimitBehind = Agent->Follower.Profile.LimitAt(
						FMath::Max(0.0, Agent->Follower.Travelled - Agent->Follower.Speed * TickSeconds));
					const FGroundRegime& Taxi = Vehicle.Chassis.Ground.Taxi;
					const double Here = Agent->Follower.Travelled;
					const double Window = Taxi.SpeedCap * Taxi.SpeedCap / (2.0 * FMath::Max(Taxi.Accel, 1.0));
					Pass.Reach = Taxi.SpeedCap;
					// EVERY UU BEHIND, not every 25: a profile minimum can be one point wide - the course's
					// R=138 crab dip is 201 uu/s at a single sample, 1000 either side - and a 25 uu grid
					// that lands beside it starts the forward pass from a limit tens of uu/s higher. After
					// the #279 merge moved the course by 66 uu the grid missed it by enough to read the rig
					// 13 uu/s short at stop 7 (455 vs 468) while it was accelerating flat out from the dip
					// (205 -> 455 at 5 uu/s a tick, measured 2026-09-25). 5000 steps a pass, ~40 passes.
					for (double Back = 0.0; Back <= Window && Here - Back >= 0.0; Back += 1.0)
					{
						const double From = Agent->Follower.Profile.LimitAt(Here - Back);
						Pass.Reach = FMath::Min(Pass.Reach, FMath::Sqrt(From * From + 2.0 * Taxi.Accel * Back));
					}
					// FROM REST at the route's start, only while the route still starts where it was
					// dispatched (no extension has trimmed it).
					if (Runner.Extensions == 0 && Here < Window)
					{
						Pass.Reach = FMath::Min(Pass.Reach, FMath::Sqrt(2.0 * Taxi.Accel * Here));
					}
					const double Braking = Agent->Follower.Speed * Agent->Follower.Speed / (2.0 * FMath::Max(Taxi.Decel, 1.0));
					Pass.bHeld = Agent->GetStopWithin() < Braking + 100.0;
					Pass.Cap = Vehicle.Chassis.Ground.Taxi.SpeedCap;
					Pass.Where = FString::Printf(TEXT("tick %d, loop %d, stop %d"), Tick, Runner.LoopsCompleted + 1, Runner.Position);

					// The control: the same chain laid straight behind the cab where it was.
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
				if (bPosed)
				{
					// How far the axles moved beyond what the cab moved: the chain's own jump.
					const double CabMoved = FVector2D::Distance(Agent->LastMotion.Position, PrevAt[Slot]);
					double Moved = 0.0;
					for (int32 I = 0; I < Axles.Num(); ++I)
					{
						Moved = FMath::Max(Moved, FVector2D::Distance(Axles[I], PrevAxles[Slot][I]));
					}
					Worst[Slot].Offer(Moved - CabMoved, FString::Printf(TEXT("tick %d, position %d"), Tick, Runner.Position));
				}
			}
			PrevAgent[Slot] = Runner.AgentId;
			PrevPosition[Slot] = Runner.Position;
			PrevLoops[Slot] = Runner.LoopsCompleted;
			PrevAxles[Slot] = Axles;
			// The pose is only real once Advance has run: a restart leaves a bare fallback.
			if (bPosed)
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
					Tick * TickSeconds, At[0].X, At[0].Y, RigLeg, *W[(RigLeg + 1) % W.Num()].Label,
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
		constexpr float Step = static_cast<float>(TickSeconds);
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

	/** 10 000 s of sim time at TickSeconds: ~15x a healthy loop. */
	constexpr int32 MaxTicks = 200000;

	const TCHAR* const Names[2] = { TEXT("rig"), TEXT("utility") };

	/**
	 * A waypoint's lane end in world XY and HALF ITS LANE's width - one lane of the two-way
	 * road it is arrived along. Resolved here from the network, independently of the course.
	 */
	bool LaneEndOf(const URoadNetwork& Net, const FRigCourseWaypoint& W, FVector2D& OutAt, double& OutHalfLane)
	{
		const FGuidelineNodeId Id = ARigTestCourse::ResolveWaypoint(Net, W);
		const FGuidelineNode* Node = Id.IsSet() ? Net.GetGuidelineNode(Id) : nullptr;
		const FRoadNode* RoadNode = Net.GetNode(W.Node);
		if (Node == nullptr || RoadNode == nullptr) { return false; }
		OutAt = Node->Position;
		OutHalfLane = 0.0;
		for (const FRoadSegmentId& SegId : RoadNode->Incident)
		{
			const FRoadSegment* Seg = Net.GetSegment(SegId);
			const URoadProfile* Profile = Seg != nullptr ? Net.ProfileFor(*Seg) : nullptr;
			if (Profile != nullptr && (Seg->A == W.From || Seg->B == W.From))
			{
				OutHalfLane = 0.25 * Profile->GetTotalWidth();
			}
		}
		return OutHalfLane > 0.0;
	}

	/**
	 * THE MARKERS ARE AT THEIR WAYPOINTS (2026-09-25). A marker is a distance along the route,
	 * so a route that stops going where the marker was planned still "arrives" at every one:
	 * the utility's log said each dead-end leg arrived while it drove past all three stems. So
	 * for every DRIVEN leg of Slot's last loop - every waypoint not refused or routed past - both
	 * the vehicle's steered axle as the marker fired and the live route's own point at the
	 * marker's distance must be within the lane's half-width, plus the tick's overshoot and
	 * 20 uu, of THAT waypoint's lane end.
	 */
	void CheckMarkersAtWaypoints(FAutomationTestBase& Test, const ARigTestCourse& Course, const URoadNetwork& Net, int32 V)
	{
		const TArray<FRigCourseWaypoint>& Waypoints = Course.GetWaypoints();
		const int32 Legs = Waypoints.Num();
		const TArray<FRigLegResult>& Results = Course.LastLoopResultsForTest(V);
		int32 Checked = 0;
		for (int32 L = 0; L < Legs && L < Results.Num(); ++L)
		{
			const FRigLegResult& R = Results[L];
			if (R.Outcome != ERigLegOutcome::Driven) { continue; }
			// Forwards, leg L ends at waypoint L + 1; reversed, the same node pair ends at L.
			const FRigCourseWaypoint Arrived = V == 0 ? Waypoints[(L + 1) % Legs] : Reversed(Waypoints[L]);
			FVector2D LaneEnd;
			double HalfLane = 0.0;
			const FString What = FString::Printf(TEXT("%s leg %d (%s%s)"), Names[V], L, *Waypoints[(L + 1) % Legs].Label,
				V == 1 ? TEXT(", reversed") : TEXT(""));
			if (!Test.TestTrue(What + TEXT(": its waypoint names a lane end"), LaneEndOf(Net, Arrived, LaneEnd, HalfLane))) { continue; }
			const double Allowed = HalfLane + R.PassSpeed * TickSeconds + 20.0;
			const double Vehicle = FVector2D::Distance(R.SteeredPosition, LaneEnd);
			const double Route = FVector2D::Distance(R.RoutePosition, LaneEnd);
			Test.TestTrue(FString::Printf(TEXT("%s: the steered axle was AT the waypoint's lane end when its marker fired (%.0f uu off, %.0f allowed; axle (%.0f, %.0f), lane end (%.0f, %.0f))"),
				*What, Vehicle, Allowed, R.SteeredPosition.X, R.SteeredPosition.Y, LaneEnd.X, LaneEnd.Y), Vehicle <= Allowed);
			Test.TestTrue(FString::Printf(TEXT("%s: the live route passes through the waypoint's lane end at the marker (%.0f uu off, %.0f allowed; route (%.0f, %.0f))"),
				*What, Route, Allowed, R.RoutePosition.X, R.RoutePosition.Y), Route <= Allowed);
			++Checked;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: markers were checked against their waypoints - not vacuous (%d)"), Names[V], Checked), Checked >= 10);
	}
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

	// WHAT EACH LEG'S OUTCOME SHOULD BE, from the router alone: the look-ahead rule stated once
	// here over Fits, not read back from the course. A vehicle drives a leg that fits into a stop
	// it can leave; otherwise the leg is Refused (it does not fit) or Bypassed (it fits, but into
	// a trap), and the vehicle routes from where it stands to the next stop it can reach AND
	// leave, the legs in between judged on their own plans.
	TArray<ERigLegOutcome> Outcome[2];
	for (int32 V = 0; V < 2; ++V)
	{
		auto StopOf = [&Waypoints, Legs, V](int32 P)
		{
			P = ((P % Legs) + Legs) % Legs;
			return V == 0 ? Waypoints[P] : Reversed(Waypoints[(Legs - P) % Legs]);
		};
		auto LegOf = [Legs, V](int32 P) { P = ((P % Legs) + Legs) % Legs; return V == 0 ? P : Legs - 1 - P; };
		auto Onward = [&](int32 Stop)
		{
			for (int32 Later = Stop + 1; Later < Stop + Legs; ++Later)
			{
				if (Fits(Net, StopOf(Stop), StopOf(Later), Vehicles[V])) { return true; }
			}
			return false;
		};
		auto Own = [&](int32 P) { return Fits(Net, StopOf(P), StopOf(P + 1), Vehicles[V]); };
		Outcome[V].Init(ERigLegOutcome::NotRun, Legs);
		for (int32 P = 0; P < Legs;)
		{
			if (Own(P) && Onward(P + 1))
			{
				Outcome[V][LegOf(P)] = ERigLegOutcome::Driven;
				++P;
				continue;
			}
			int32 Target = INDEX_NONE;
			for (int32 Stop = P + 2; Stop <= Legs && Target == INDEX_NONE; ++Stop)
			{
				Target = (Fits(Net, StopOf(P), StopOf(Stop), Vehicles[V]) && Onward(Stop)) ? Stop : INDEX_NONE;
			}
			if (!TestTrue(FString::Printf(TEXT("%s: the router leaves a way on from stop %d - no stranding is expected"), Names[V], P),
				Target != INDEX_NONE)) { return false; }
			for (int32 M = P; M < Target; ++M)
			{
				Outcome[V][LegOf(M)] = Own(M) ? ERigLegOutcome::Bypassed : ERigLegOutcome::Refused;
			}
			P = Target;
		}
		TArray<FString> Bypassed;
		for (int32 L = 0; L < Legs; ++L)
		{
			if (Outcome[V][L] == ERigLegOutcome::Bypassed) { Bypassed.Add(FString::FromInt(L)); }
		}
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %s expected bypassed legs: %s"), Names[V],
			Bypassed.Num() > 0 ? *FString::Join(Bypassed, TEXT(", ")) : TEXT("none"));
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
	RouteSearch::ResetTowCheckCountForTest();
	FRoadNetworkSolver::ResetWideningTraceCountForTest();
	const int32 Ticks = RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedByAllForTest() >= 1; }, Watch);
	// THE WHOLE-ROUTE TOW CHECK'S COST, counted: the course plans per loop and per look-ahead, never per frame.
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %d route plan(s), worst %.1f ms, %.1f ms in all"),
		Course->GetPlanCallsForTest(), Course->GetWorstPlanMsForTest(), Course->GetTotalPlanMsForTest());
	// THE ROUTE PLANS DRIVE NO BEND (review of 75d3cbc0): the widening is traced on the course's
	// Topology rebuild, before the loop, and the first cold plan is route search and its tow check.
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %d bend widening trace(s) during the loop"),
		FRoadNetworkSolver::WideningTraceCountForTest);
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %d whole-route tow check(s) over the loop, %.1f ms in all"),
		RouteSearch::TowCheckCountForTest(), RouteSearch::TowCheckSecondsForTest() * 1000.0);
	GLog->RemoveOutputDevice(&Spy);
	Probe.Finish();
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: both loops took %d ticks (%.0f s sim); rig %d loop(s), utility %d; %d turn / %d lane tow point(s) probed"),
		Ticks, Ticks * TickSeconds, Course->LoopsCompletedForTest(0), Course->LoopsCompletedForTest(1), Probe.TurnPoints, Probe.LanePoints);
	for (int32 V = 0; V < 2; ++V)
	{
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicle %d tow - swept worst %.0f uu over (%s); per-side worst %.0f uu (%s); lane worst %.0f uu (%s)"),
			V, Probe.Swept[V].Excess, *Probe.Swept[V].Where, Probe.PerSide[V].Excess, *Probe.PerSide[V].Where,
			Probe.Lane[V].Excess, *Probe.Lane[V].Where);
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicle %d chain - %d waypoint(s) passed, %d agent(s) dispatched, %d route extension(s), %d restart(s) from rest; worst axle move beyond the cab's in one tick %.2f uu (%s); a re-lay would have moved it up to %.1f uu (%s)"),
			V, Continuity.Handovers[V], Course->GetRunnerForTest(V).Dispatches, Course->GetRunnerForTest(V).Extensions,
			Course->GetRunnerForTest(V).Redirects, Continuity.Worst[V].Excess, *Continuity.Worst[V].Where,
			Continuity.RelayWouldMove[V].Excess, *Continuity.RelayWouldMove[V].Where);
	}
	// THE LOOP SUMMARY PER VEHICLE, per leg: how far past its turn clearance each vehicle's tow
	// reached (negative: that much to spare), with the leg's feature - which tier and which corner.
	for (int32 V = 0; V < 2; ++V)
	{
		TArray<int32> LegsSeen;
		Probe.PerSideByLeg[V].GetKeys(LegsSeen);
		LegsSeen.Sort();
		TArray<FString> Parts;
		for (const int32 L : LegsSeen)
		{
			Parts.Add(FString::Printf(TEXT("%d (%s) %.0f"), L, *Waypoints[(L + 1) % Legs].Label, Probe.PerSideByLeg[V][L]));
		}
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %s per-side by leg, uu past the turn clearance: %s"), Names[V], *FString::Join(Parts, TEXT("; ")));
	}
	UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: vehicles overlapped on %d tick(s) in %d episode(s); worst penetration %.0f uu (%s)"),
		Overlap.TicksOverlapping, Overlap.Episodes, Overlap.TicksOverlapping > 0 ? Overlap.Worst.Excess : 0.0, *Overlap.Worst.Where);
	if (!TestTrue(TEXT("both vehicles completed a loop within the tick bound - no hang, no deadlock between them"),
		Course->LoopsCompletedByAllForTest() >= 1)) { return false; }

	TestEqual(TEXT("no tow jack-knifed - driving forwards within the lock never reaches the guard"),
		Spy.Containing(TEXT("jack-knifed")), 0);
	TestEqual(TEXT("no leg entered Reversing - the course has no reverse legs"), Spy.Containing(TEXT("Reversing")), 0);
	TestEqual(TEXT("no leg timed out stuck"), Spy.Containing(TEXT("stuck")), 0);

	// THE CHAIN IS CONTINUOUS: one agent per vehicle for the whole run, one route per loop joined
	// onto the next before it runs out, its axles never jumping beyond what the cab moved. The
	// look-ahead keeps the rig out of the dead ends it cannot turn in, so nothing is stranded.
	for (int32 V = 0; V < 2; ++V)
	{
		TestEqual(FString::Printf(TEXT("%s: never stranded - the look-ahead routes past every trap"), Names[V]),
			Spy.Containing(*FString::Printf(TEXT("RigCourse: %s loop"), Names[V]), TEXT("stranded")), 0);
		TestEqual(FString::Printf(TEXT("%s: ONE agent drove the whole run - never one per leg, never a respawn"), Names[V]),
			Course->GetRunnerForTest(V).Dispatches, 1);
		int32 DrivenLegs = 0;
		for (const ERigLegOutcome O : Outcome[V]) { DrivenLegs += O == ERigLegOutcome::Driven ? 1 : 0; }
		TestTrue(FString::Printf(TEXT("%s: waypoints were passed - the continuity check is not vacuous (%d)"), Names[V], Continuity.Handovers[V]),
			Continuity.Handovers[V] >= DrivenLegs - 1);
		TestTrue(FString::Printf(TEXT("%s: no tow axle ever moved more than one sub-step (%.0f uu) beyond the cab in a tick (worst %.2f uu: %s) - the chain was never re-laid"),
			Names[V], VehicleSweep::TraceStep, Continuity.Worst[V].Excess, *Continuity.Worst[V].Where),
			Continuity.Worst[V].Excess <= VehicleSweep::TraceStep);
		// ONE ROUTE, JOINED LOOP TO LOOP: exactly one splice per loop boundary crossed or about
		// to be, and never a restart from rest.
		const FRigCourseRunner& Runner = Course->GetRunnerForTest(V);
		TestTrue(FString::Printf(TEXT("%s: one route extension per loop boundary (%d extension(s), %d loop(s) done)"), Names[V], Runner.Extensions, Runner.LoopsCompleted),
			Runner.Extensions >= Runner.LoopsCompleted && Runner.Extensions <= Runner.LoopsCompleted + 1);
		TestEqual(FString::Printf(TEXT("%s: never restarted from rest"), Names[V]), Runner.Redirects, 0);
		// NO SHARP JOIN ANYWHERE. Until 2026-09-25 the width step's lane-offset jog started at the
		// step node's lane end, where two legs are welded, and showed at that join (90 deg). The
		// builder now tapers the width on an S (Airside.Build.WidthTaper.*), so a sharp join
		// anywhere is the route being rougher than its legs.
		TArray<FString> Where;
		for (const int32 IntoLeg : Runner.SharpJoinLegs)
		{
			Where.Add(FString::FromInt(IntoLeg));
		}
		TestEqual(FString::Printf(TEXT("%s: no sharp vertex at any join - the width step included (joins into legs: %s)"), Names[V],
			Where.Num() > 0 ? *FString::Join(Where, TEXT(", ")) : TEXT("none")), Runner.SharpJoinLegs.Num(), 0);
		UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %s sharp joins into legs: %s"), Names[V],
			Where.Num() > 0 ? *FString::Join(Where, TEXT(", ")) : TEXT("none"));
	}

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
	// INNER pavement edge on near-side (right) turns, while its swept width fits - VehicleFit
	// admits by width because a real driver swings wide, and the agent does not swing: it holds
	// its lane line. The utility's short trailer stays on. A decision for the spec, not this
	// course; when the agent learns to swing wide (or VehicleFit asks per side) the rig's line
	// here goes red and is to be flipped.
	// BEND LANES (2026-09-25): a bend's lanes are now arcs about its inner fillet and a Wide bend's
	// inside is widened to the rig's sweep - where its arms can hold it. On this course they cannot
	// everywhere: the Wide lane's corners have a 30 m arm already cut to its allowance, so the
	// widening is capped there (traced 231 -> 188 uu, AirportMgr.RigCourse.BendCensus), and the
	// rig is not the design vehicle of Narrow or Standard at all. The worst is still a Narrow T.
	TestTrue(FString::Printf(TEXT("the utility's tow stays on the tarmac per side (worst %.0f uu: %s)"),
		Probe.PerSide[1].Excess, *Probe.PerSide[1].Where), Probe.PerSide[1].Excess <= 10.0);
	TestTrue(FString::Printf(TEXT("KNOWN GAP: the rig's trailer cuts in past the inner edge on near-side turns (worst %.0f uu: %s)"),
		Probe.PerSide[0].Excess, *Probe.PerSide[0].Where), Probe.PerSide[0].Excess > 10.0);
	// AND NO WORSE THAN MEASURED: the floor above only says the gap is still there, so a
	// regression that put the trailer further off the pavement would pass it. The ceiling is the
	// measurement plus a little room: worst 355 uu at tier 0's T junction, out of the stem (leg 4),
	// after the bend-lane arcs and the Wide widening (2026-09-25; 342-358 before them).
	TestTrue(FString::Printf(TEXT("KNOWN GAP, bounded: the rig's inner cut-in is no worse than the 355 uu measured after the bend widening, 2026-09-25 (worst %.0f uu: %s)"),
		Probe.PerSide[0].Excess, *Probe.PerSide[0].Where), Probe.PerSide[0].Excess <= 375.0);
	// The same cut-in shows on the LANE just before a near-side corner (measured 164 uu at the
	// east link's stub when every leg began with the chain laid straight; 261 uu at tier 1's
	// entry stub once the chain carried across waypoints, 2026-09-25): the trailer leaves the
	// road before the junction pavement begins. Bound it by the turn's own overrun - it is the
	// approach to that cut, never a worse one.
	TestTrue(FString::Printf(TEXT("KNOWN GAP: the rig's lane overrun is the approach to that same cut-in, no worse (lane %.0f uu: %s)"),
		Probe.Lane[0].Excess, *Probe.Lane[0].Where), Probe.Lane[0].Excess <= Probe.PerSide[0].Excess + 10.0);

	for (int32 V = 0; V < Vehicles.Num(); ++V)
	{
		// THE TAPER IS IN THE LEG THAT LEAVES THE STEP NODE. Forwards that is WidthStepLeg itself;
		// in reverse the runner arrives at the step node BEFORE the taper, and the leg leaving it is
		// the node pair of the forward leg one earlier - a reversed leg turns at its other end.
		const int32 TaperLeg = V == 0 ? WidthStepLeg : (WidthStepLeg + Legs - 1) % Legs;
		const TArray<FRigLegResult>& Results = Course->LastLoopResultsForTest(V);
		if (!TestEqual(TEXT("a result per leg"), Results.Num(), Legs)) { return false; }
		int32 Refusals = 0;
		for (int32 L = 0; L < Legs; ++L)
		{
			const FRigLegResult& R = Results[L];
			const FString What = FString::Printf(TEXT("%s leg %d (%s%s)"), Names[V], L, *Waypoints[(L + 1) % Legs].Label,
				V == 1 ? TEXT(", reversed") : TEXT(""));
			TestTrue(What + TEXT(" was driven, refused or bypassed"),
				R.Outcome == ERigLegOutcome::Driven || R.Outcome == ERigLegOutcome::Refused || R.Outcome == ERigLegOutcome::Bypassed);
			TestEqual(What + TEXT(": the outcome the router's verdicts and the look-ahead rule give"),
				static_cast<int32>(R.Outcome), static_cast<int32>(Outcome[V][L]));
			TestEqual(What + TEXT(": refused exactly when the router refuses it with this body, in this direction"),
				R.Outcome == ERigLegOutcome::Refused, !Expected[V][L]);
			TestFalse(What + TEXT(" never reversed"), R.bReversed);
			if (R.Outcome == ERigLegOutcome::Driven)
			{
				// The steered axle walks the line, so arrival is measured along it; the chassis
				// origin then sits one wheelbase back from the lane end, and no further.
				const double Wheelbase = Vehicles[V].Chassis.Wheelbase();
				// THE MARKER FIRES ON THE TICK IT IS PASSED: at most one tick's travel past it (the
				// speed it passed at, times the tick), never before it, never a tick late.
				TestTrue(FString::Printf(TEXT("%s: its marker fired within one tick's travel of the lane end (%.1f uu past, %.1f allowed)"),
					*What, -R.DistanceLeft, R.PassSpeed * TickSeconds + 1.0),
					R.DistanceLeft <= 1.0 && -R.DistanceLeft <= R.PassSpeed * TickSeconds + 1.0);
				// AND THE BODY IS WHERE THAT SAYS: the steered axle is -DistanceLeft past the lane
				// end, so the fixed axle is a wheelbase less that from it - to a fixed 20 uu, not a
				// tolerance that grows with the overshoot.
				const double Behind = FVector2D::Distance(R.EndPosition, R.GoalPosition);
				TestTrue(FString::Printf(TEXT("%s: and its fixed axle was a wheelbase behind the lane end, less the overshoot (%.1f vs %.1f uu)"),
					*What, Behind, Wheelbase + R.DistanceLeft), FMath::Abs(Behind - (Wheelbase + R.DistanceLeft)) < 20.0);

				// THE WIDTH STEP, FLIPPED (was pinned by controller ruling 5 as the builder defect:
				// a 90 degree jog FSpeedProfile crawled; this line asserted it was there). The
				// builder tapers it on an S since 2026-09-25, so NO leg has a sharp vertex, the
				// width-step leg included, in either direction.
				TestEqual(What + (L == TaperLeg ? TEXT(": the width step tapers on an S - no sharp vertex") : TEXT(" has no sharp vertex")),
					R.SharpVertexCount, 0);
				// AND NEITHER VEHICLE CRAWLS THROUGH IT. The two legs either side of the step node
				// (one ends at it, one leaves it) were passed at 39-44 uu/s on the jog - steering
				// speed; on the S they pass at road speed. A quarter of the cap is far below any
				// healthy pass on this course and far above a crawl.
				if (L == WidthStepLeg || L == (WidthStepLeg + Legs - 1) % Legs)
				{
					const double Cap = Vehicles[V].Chassis.Ground.Taxi.SpeedCap;
					UE_LOG(LogTemp, Display, TEXT("RigCourse.OneLoopHeadless: %s passed at %.0f uu/s after %.1f s"), *What, R.PassSpeed, R.Elapsed);
					TestTrue(FString::Printf(TEXT("%s: passed at road speed, not a crawl (%.0f uu/s, floor %.0f)"), *What, R.PassSpeed, 0.25 * Cap),
						R.PassSpeed >= 0.25 * Cap);
				}
			}
			Refusals += R.Outcome == ERigLegOutcome::Refused ? 1 : 0;
			// THE REASON IS THE TRAILER, NOT THE LOCK (2026-09-25): the dead ends were reshaped
			// within the bowser footprint and every piece now clears the rig's lock; what refuses
			// it is the whole-route tow check - the trailer folds going round. Said in the refusal.
			if (V == 0 && R.Outcome == ERigLegOutcome::Refused)
			{
				TestTrue(FString::Printf(TEXT("%s: refused because its trailer folds (%s)"), *What, *R.Reason),
					R.Reason.Contains(TEXT("trailer folds at guideline node")));
			}
		}
		// The width step's own expected outcome: DRIVEN, on the S, within the 3x allowance.
		TestEqual(FString::Printf(TEXT("%s drives the Narrow->Wide mid-straight step"), Names[V]),
			static_cast<int32>(Results[TaperLeg].Outcome), static_cast<int32>(ERigLegOutcome::Driven));
		// ONCE PER LOOP: counted in the loop these results are from, by the loop number in the line.
		const int32 Loop = Course->LoopsCompletedForTest(V);
		TestEqual(FString::Printf(TEXT("%s: each refusal was logged once in loop %d"), Names[V], Loop),
			Spy.Containing(*FString::Printf(TEXT("RigCourse: %s loop %d leg "), Names[V], Loop), TEXT("refused:")), Refusals);
		// AND EACH BYPASS: the route-on branch's own line, once per bypassed leg per loop.
		int32 Bypasses = 0;
		for (const FRigLegResult& R : Results) { Bypasses += R.Outcome == ERigLegOutcome::Bypassed ? 1 : 0; }
		TestEqual(FString::Printf(TEXT("%s: each bypassed leg was logged once in loop %d"), Names[V], Loop),
			Spy.Containing(*FString::Printf(TEXT("RigCourse: %s loop %d leg "), Names[V], Loop), TEXT("bypassed:")), Bypasses);
		CheckMarkersAtWaypoints(*this, *Course, Net, V);
	}
	return true;
}

// A REBUILD MID-LOOP KEEPS THE COURSE (2026-09-25). The user drew a road in PIE while both
// vehicles were out; the utility then "cut across" and skipped every dead end in reverse,
// while its log still reported each dead-end leg arrived. The log showed why: the graph
// rebuild re-resolved the utility's spliced loop route, a step failed, and the replan went
// straight to the route's END - dropping the stems, which are via points no search to the
// end would choose. This places a lone node exactly where the user did, at the moment the
// user did (the utility on the return road, ahead of tier 2's stem), and holds every marker
// to its waypoint for the loop that follows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseRebuildKeepsTheCourseTest,
	"AirportMgr.RigCourse.RebuildKeepsTheCourse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseRebuildKeepsTheCourseTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);

	// The utility past the width step and on the return road north (its stop 2) - where the
	// PIE log had it when the node went down.
	RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->GetRunnerForTest(1).Position >= 2; });
	if (!TestEqual(TEXT("the utility is on the return road, ahead of every stem"), Course->GetRunnerForTest(1).Position, 2)) { return false; }
	FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	const int32 Placed = Actor->PlaceNode(FVector2D(-9186.0, 24654.0));
	TestTrue(TEXT("the lone node was placed - the rebuild happened"), Placed != INDEX_NONE);

	RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedByAllForTest() >= 1; });
	GLog->RemoveOutputDevice(&Spy);
	// THE BOUNDARY ITSELF: nothing but the course may change the route its markers measure.
	TestEqual(TEXT("neither vehicle's route was replanned under its markers by the rebuild"),
		Spy.Containing(TEXT("replanned outside the course")), 0);
	if (!TestTrue(TEXT("both vehicles completed a loop"), Course->LoopsCompletedByAllForTest() >= 1)) { return false; }
	const URoadNetwork& Net = *Actor->Network;
	for (int32 V = 0; V < 2; ++V)
	{
		CheckMarkersAtWaypoints(*this, *Course, Net, V);
	}
	return true;
}

// WAYPOINTS ARE NOT STOPS (spec §3 REVISED "one route per loop", 2026-09-25). The user saw the
// rig brake to a halt at every waypoint: each leg was its own route, and FSpeedProfile brakes to
// zero at a route's end. A job in the game drives one plan to a real destination; so does the
// course now, and this is what says so.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseWaypointsAreNotStopsTest,
	"AirportMgr.RigCourse.WaypointsAreNotStops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseWaypointsAreNotStopsTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	const URoadNetwork& Net = *Actor->Network;
	const TArray<FRigCourseWaypoint>& Waypoints = Course->GetWaypoints();
	const FVehicle& Rig = Course->GetVehicles()[0];

	FContinuity Continuity;
	FObservers Watch;
	Watch.Continuity = &Continuity;
	RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedByAllForTest() >= 1; }, Watch);
	if (!TestTrue(TEXT("both vehicles completed a loop"), Course->LoopsCompletedByAllForTest() >= 1)) { return false; }

	// EVERY WAYPOINT PASSED ON THE MOVE, AND AT THE PROFILE'S SPEED. Never above the profile's
	// limit (as it stood a tick ago, the follower's speed being last tick's); and at EVERY
	// waypoint no slower than that limit or the forward pass (FPass::Reach - what it can have
	// accelerated to since the last slowdown), give or take a tick - unless traffic arbitration
	// holds it within its braking distance. A vehicle braking for a route end it should have
	// been joined past fails this; so did every waypoint of the per-leg course.
	for (int32 V = 0; V < 2; ++V)
	{
		const FGroundRegime& Taxi = Course->GetVehicles()[V].Chassis.Ground.Taxi;
		const double Tolerance = FMath::Max(Taxi.Accel, Taxi.Decel) * TickSeconds + 1.0;
		TestTrue(FString::Printf(TEXT("%s: waypoints were passed (%d)"), Names[V], Continuity.Passes[V].Num()),
			Continuity.Passes[V].Num() >= 10);
		for (const FContinuity::FPass& Pass : Continuity.Passes[V])
		{
			UE_LOG(LogTemp, Display, TEXT("RigCourse.WaypointsAreNotStops: %s at %s: %.0f uu/s, profile limit %.0f (a tick back %.0f), forward pass %.0f, cap %.0f%s"),
				Names[V], *Pass.Where, Pass.Speed, Pass.Limit, Pass.LimitBehind, Pass.Reach, Pass.Cap,
				Pass.bHeld ? TEXT(", held by traffic") : TEXT(""));
			TestTrue(FString::Printf(TEXT("%s at %s: moving through the waypoint (%.0f uu/s), not stopped at it"), Names[V], *Pass.Where, Pass.Speed),
				Pass.Speed > 1.0);
			const double Governing = FMath::Max(Pass.Limit, Pass.LimitBehind);
			TestTrue(FString::Printf(TEXT("%s at %s: never above the profile's limit (%.0f vs %.0f uu/s)"), Names[V], *Pass.Where, Pass.Speed, Governing),
				Pass.Speed <= Governing + Tolerance);
			if (!Pass.bHeld)
			{
				// The LOWER of the two limits: just past a crawl (a slow corner's apex) the limit a tick
				// ahead is already rising and no vehicle has that acceleration in one tick.
				const double Expected = FMath::Min3(Pass.Limit, Pass.LimitBehind, Pass.Reach);
				TestTrue(FString::Printf(TEXT("%s at %s: at the profile's speed (%.0f vs %.0f uu/s: limit %.0f, forward pass %.0f)"),
					Names[V], *Pass.Where, Pass.Speed, Expected, FMath::Min(Pass.Limit, Pass.LimitBehind), Pass.Reach),
					Pass.Speed >= Expected - Tolerance);
			}
		}
	}

	// LEG 0, THE STRAIGHT (80 m node to node, 68 m of lane between its junctions' cut-backs), AT
	// THE NO-STOP TIME: from rest to the cap and on at it, derived
	// from the rig's own figures - not rest-to-rest, which is what a leg that is its own route
	// costs (measured 14.3 s for 68 m before this change).
	FRoutePlan Leg0;
	{
		const FGuidelineNodeId Start = ARigTestCourse::ResolveWaypoint(Net, Waypoints[0]);
		const FGuidelineNodeId Goal = ARigTestCourse::ResolveWaypoint(Net, Waypoints[1]);
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::PlayerIssued, Start, Goal, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Rig);
		Leg0 = RouteSearch::Find(Net, Query);
	}
	if (!TestTrue(TEXT("leg 0 plans for the rig"), Leg0.IsValid())) { return false; }
	const FGroundRegime& Taxi = Rig.Chassis.Ground.Taxi;
	const double Cap = Taxi.SpeedCap;
	const double UpTo = Cap * Cap / (2.0 * Taxi.Accel);
	const double Down = Cap * Cap / (2.0 * Taxi.Decel);
	const double NoStop = Leg0.Length >= UpTo ? Cap / Taxi.Accel + (Leg0.Length - UpTo) / Cap : FMath::Sqrt(2.0 * Leg0.Length / Taxi.Accel);
	// Rest to rest: a trapezium when the leg is long enough to reach the cap, else a triangle
	// peaking at sqrt(2 L a d / (a + d)).
	const double Peak = FMath::Sqrt(2.0 * Leg0.Length * Taxi.Accel * Taxi.Decel / (Taxi.Accel + Taxi.Decel));
	const double RestToRest = Leg0.Length >= UpTo + Down
		? Cap / Taxi.Accel + Cap / Taxi.Decel + (Leg0.Length - UpTo - Down) / Cap
		: Peak / Taxi.Accel + Peak / Taxi.Decel;
	const double Took = (Continuity.FirstPass[0] - Continuity.FirstSeen[0]) * TickSeconds;
	UE_LOG(LogTemp, Display, TEXT("RigCourse.WaypointsAreNotStops: rig leg 0, %.0f m, took %.2f s; no-stop %.2f s, rest-to-rest %.2f s"),
		Leg0.Length / 100.0, Took, NoStop, RestToRest);
	TestTrue(FString::Printf(TEXT("the rig's leg 0 took about the no-stop time (%.2f s vs %.2f s ideal) - not rest-to-rest (%.2f s)"), Took, NoStop, RestToRest),
		Took <= NoStop * 1.1 + 0.1 && Took >= NoStop * 0.9);
	TestTrue(FString::Printf(TEXT("and clearly under rest-to-rest (%.2f s vs %.2f s)"), Took, RestToRest), Took < RestToRest - 1.0);
	return true;
}

// THE ROUTE STAYS BOUNDED (review of ed81410c): each loop is spliced onto the live route, and
// without trimming the driven history a course left running grows its plan - and every
// per-tick walk over it - for ever. Three loops in, the route is no longer than two loops plus
// the history kept behind the vehicle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseRouteStaysBoundedTest,
	"AirportMgr.RigCourse.RouteStaysBounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseRouteStaysBoundedTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);

	double Longest = 0.0;
	double FirstLoop = 0.0;
	for (int32 Tick = 0; Tick < MaxTicks && Course->LoopsCompletedForTest(0) < 3; ++Tick)
	{
		Actor->Tick(static_cast<float>(TickSeconds));
		Course->Tick(static_cast<float>(TickSeconds));
		const FRigCourseRunner& Rig = Course->GetRunnerForTest(0);
		const FRoadAgent* Agent = Rig.AgentId != 0 ? Actor->GetGroundTraffic()->FindAgent(Rig.AgentId) : nullptr;
		if (Agent != nullptr)
		{
			Longest = FMath::Max(Longest, Agent->PlanInProgress().Length);
			if (Rig.Extensions == 0)
			{
				FirstLoop = Agent->PlanInProgress().Length;
			}
		}
	}
	if (!TestEqual(TEXT("the rig drove three loops"), Course->LoopsCompletedForTest(0), 3)) { return false; }
	const FRigCourseRunner& Rig = Course->GetRunnerForTest(0);
	TestTrue(FString::Printf(TEXT("it was extended at every boundary (%d)"), Rig.Extensions), Rig.Extensions >= 3);
	UE_LOG(LogTemp, Display, TEXT("RigCourse.RouteStaysBounded: first loop's route %.0f m; longest route over three loops %.0f m"),
		FirstLoop / 100.0, Longest / 100.0);
	TestTrue(FString::Printf(TEXT("the live route never grew past two loops plus the history kept (%.0f m vs %.0f m)"),
		Longest / 100.0, (2.0 * FirstLoop + 20000.0) / 100.0), Longest <= 2.0 * FirstLoop + 20000.0);
	return true;
}

// THE FALLBACK KEEPS THE CHAIN TOO: with every extension refused, each route runs out at its
// loop's end and the next starts from rest through RedirectAgent (ContinueRoute) - the same
// agent, its trailer where it was, never re-laid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseRanOutRestartsWithTheChainTest,
	"AirportMgr.RigCourse.RanOutRestartsWithTheChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseRanOutRestartsWithTheChainTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	Course->bRefuseExtensionsForTest = true;

	// THE FIRST RESTART, TICK BY TICK: the axles on the tick before the redirect and the tick
	// after it, against the cab's own move over the same two ticks. A fallback that re-laid the
	// chain straight moves a trailer's worth of swing here; one that kept it moves a sub-step.
	// Bounded at about one and a half loops, so a fallback that never restarts fails, not hangs.
	{
		const int32 Cap = 6500;
		int32 RedirectTick = INDEX_NONE;
		TArray<FVector2D> AxlesBefore, AxlesAfter;
		FVector2D CabBefore = FVector2D::ZeroVector, CabAfter = FVector2D::ZeroVector;
		TArray<FVector2D> PrevAxles;
		FVector2D PrevCab = FVector2D::ZeroVector;
		for (int32 Tick = 0; Tick < Cap && (RedirectTick == INDEX_NONE || Tick <= RedirectTick + 5); ++Tick)
		{
			const int32 RedirectsBefore = Course->GetRunnerForTest(0).Redirects;
			Actor->Tick(static_cast<float>(TickSeconds));
			Course->Tick(static_cast<float>(TickSeconds));
			const FRigCourseRunner& Rig = Course->GetRunnerForTest(0);
			const FRoadAgent* Agent = Rig.AgentId != 0 ? Actor->GetGroundTraffic()->FindAgent(Rig.AgentId) : nullptr;
			if (Agent == nullptr) { continue; }
			if (RedirectTick == INDEX_NONE && Rig.Redirects > RedirectsBefore)
			{
				RedirectTick = Tick;
				AxlesBefore = PrevAxles;
				CabBefore = PrevCab;
			}
			if (RedirectTick != INDEX_NONE && Tick == RedirectTick + 1)
			{
				AxlesAfter = Agent->TowAxles;
				CabAfter = Agent->LastMotion.Position;
			}
			PrevAxles = Agent->TowAxles;
			PrevCab = Agent->LastMotion.Position;
		}
		if (!TestTrue(TEXT("the route ran out and the rig was restarted from rest within one and a half loops"), RedirectTick != INDEX_NONE)) { return false; }
		if (!TestTrue(TEXT("the chain was seen either side of the restart"), AxlesBefore.Num() > 0 && AxlesBefore.Num() == AxlesAfter.Num())) { return false; }
		const double CabMoved = FVector2D::Distance(CabBefore, CabAfter);
		for (int32 I = 0; I < AxlesBefore.Num(); ++I)
		{
			const double Moved = FVector2D::Distance(AxlesBefore[I], AxlesAfter[I]);
			TestTrue(FString::Printf(TEXT("across the first restart, link %d's axle moved %.2f uu - no more than the cab's %.2f plus a sub-step: the chain was not re-laid"),
				I, Moved, CabMoved), Moved <= CabMoved + VehicleSweep::TraceStep);
		}
	}

	FContinuity Continuity;
	FObservers Watch;
	Watch.Continuity = &Continuity;
	RunUntil(*Actor, *Course, MaxTicks, [Course]() { return Course->LoopsCompletedForTest(0) >= 2; }, Watch);
	if (!TestTrue(TEXT("the rig drove two loops"), Course->LoopsCompletedForTest(0) >= 2)) { return false; }
	const FRigCourseRunner& Rig = Course->GetRunnerForTest(0);
	TestEqual(TEXT("no extension was made - the test refused them all"), Rig.Extensions, 0);
	// Each boundary CROSSED: the last loop counts as done on passing its end marker, a tick
	// before its restart.
	TestTrue(FString::Printf(TEXT("so each loop boundary crossed was a restart from rest (%d, %d loop(s))"), Rig.Redirects, Rig.LoopsCompleted),
		Rig.Redirects >= FMath::Max(1, Rig.LoopsCompleted - 1));
	TestEqual(TEXT("by the SAME agent - one dispatch for the whole run"), Rig.Dispatches, 1);
	TestTrue(FString::Printf(TEXT("and its chain never jumped beyond the cab's own move (worst %.2f uu: %s)"),
		Continuity.Worst[0].Excess, *Continuity.Worst[0].Where), Continuity.Worst[0].Excess <= VehicleSweep::TraceStep);
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
	// SPENT ONLY BY A DISPATCH: the course also asks for leg 0's plan to JUDGE it (the look-ahead),
	// and a one-shot spent there would hand the dispatch the router's plan instead.
	TSharedRef<bool> bUsed = MakeShared<bool>(false);
	Course->PlanOverrideForTest = [bUsed](int32 Leg, int32 Slot, bool bDispatching, FRoutePlan& OutPlan)
	{
		if (Leg != 0 || Slot != 0 || *bUsed) { return false; }
		*bUsed = bDispatching;
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

// THE COURSE'S BENDS, MEASURED (bend lanes, 2026-09-25): every two-arm corner's fillets, each lane
// turn's radius, and how far each vehicle leaves the tarmac driving it - traced by the router's
// own pursuit (VehicleSweep::Trace) against the pavement polygons. A census for the report and for
// the pins in OneLoopHeadless; it asserts only that it measured something.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseBendCensusTest,
	"AirportMgr.RigCourse.BendCensus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseBendCensusTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	const URoadNetwork& Net = *Actor->Network;
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	// Re-solved as the presenter solves it (same arc count, same vehicles): the cuts it writes back
	// are the ones already there.
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Actor->Network, 12, &Designs);
	const TArray<FVehicle>& Vehicles = Course->GetVehicles();
	const BendProbe::FPavement Pavement = BendProbe::PavementAll(Net, Solved);
	int32 Bends = 0;
	for (int32 Index = 0; Index < Net.GetNodes().Num(); ++Index)
	{
		const FRoadNode& Node = Net.GetNodes()[Index];
		if (!Node.bAlive || Node.Incident.Num() != 2) { continue; }
		const FRoadNodeId NodeId = Net.NodeIdAt(Index);
		RoadGeom::FFillet Inner, Outer;
		if (!BendProbe::Fillets(Solved, NodeId, Inner, Outer)) { continue; }
		++Bends;
		const FJunctionResult& Junction = Solved.NodeResults[Index];
		double Widths[2] = { 0.0, 0.0 };
		for (int32 Arm = 0; Arm < 2; ++Arm)
		{
			const FRoadSegment* Seg = Net.GetSegment(Node.Incident[Arm]);
			const URoadProfile* Profile = Seg != nullptr ? Net.ProfileFor(*Seg) : nullptr;
			Widths[Arm] = Profile != nullptr ? Profile->GetTotalWidth() : 0.0;
		}
		UE_LOG(LogTemp, Display, TEXT("CourseBend node %d at (%.0f, %.0f): widths %.0f / %.0f; inner fillet R %.0f, outer fillet R %.0f; cuts %.0f / %.0f"),
			Index, Node.Position.X, Node.Position.Y, Widths[0], Widths[1], Inner.Radius, Outer.Radius,
			Junction.Arms[0].CutDistance, Junction.Arms[1].CutDistance);
		for (const BendProbe::FTurnChain& Turn : BendProbe::TurnsAt(Net, NodeId))
		{
			const FVector2D Mid = Turn.Path[Turn.Path.Num() / 2];
			const bool bLeft = FVector2D::CrossProduct(Mid - Turn.Path[0], Turn.Path.Last() - Mid) > 0.0;
			double AboutMin = TNumericLimits<double>::Max(), AboutMax = 0.0;
			BendProbe::RadiusAbout(Turn, Inner.Centre, AboutMin, AboutMax);
			FString Line = FString::Printf(TEXT("CourseBend node %d %s turn: %d piece(s), MinRadius %.0f, about the inner fillet's centre %.0f..%.0f"),
				Index, bLeft ? TEXT("left") : TEXT("right"), Turn.Pieces.Num(), Turn.MinRadius, AboutMin, AboutMax);
			for (int32 V = 0; V < Vehicles.Num(); ++V)
			{
				const BendProbe::FOverrun Off = BendProbe::Overrun(Turn, Vehicles[V], Pavement);
				Line += FString::Printf(TEXT("; vehicle %d off inner %.0f outer %.0f"), V, Off.Inner, Off.Outer);
			}
			UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		}
	}
	TestTrue(FString::Printf(TEXT("the course has bends to measure (%d)"), Bends), Bends > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseJoinThatFoldsIsCutTest,
	"AirportMgr.RigCourse.JoinThatFoldsIsCut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseJoinThatFoldsIsCutTest::RunTest(const FString& Parameters)
{
	using namespace RigCourseTest;
	// A JOIN THAT FOLDS IS CUT, NEVER HANDED ON (re-review of aa90eec2). No two legs of the course
	// fold when joined, so the rig's first two legs are overridden with a fixture that does: leg 0
	// two same-hand quarters (a U, which holds), leg 1 a third quarter (which holds on its own).
	// Joined they are three quarters, which fold - PlanLoopRoute must end the route at leg 0's
	// end (waypoint 1, mid-loop), not hand on the folding route.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	URoadNetwork& Net = *Actor->Network;

	// Hand-drawn, far off the course, authored so no rebuild sweeps them.
	const double Leg = 830.0;
	const FVector2D Origin(-200000.0, -200000.0);
	auto Node = [&Net](const FVector2D& At) { return Net.AddGuidelineNode(At, /*bDerived=*/false); };
	auto Join = [&Net](FGuidelineNodeId A, FGuidelineNodeId B, const FVector2D& Control)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = Control;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::AToB;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	};
	const FGuidelineNodeId Start = Node(Origin + FVector2D(-4000.0, 0.0));
	FGuidelineNodeId From = Node(Origin);
	Join(Start, From, Origin + FVector2D(-2000.0, 0.0));
	FVector2D At = Origin;
	FVector2D Heading(1.0, 0.0);
	TArray<FGuidelineNodeId> Ends;
	for (int32 Quarter = 0; Quarter < 3; ++Quarter)
	{
		const FVector2D Side(-Heading.Y, Heading.X);
		const FVector2D Control = At + Heading * Leg;
		At = Control + Side * Leg;
		Heading = Side;
		const FGuidelineNodeId To = Node(At);
		Join(From, To, Control);
		Ends.Add(To);
		From = To;
	}
	const FGuidelineNodeId Goal = Node(At + Heading * 4000.0);
	Join(From, Goal, At + Heading * 2000.0);
	auto Plan = [&Net](FGuidelineNodeId A, FGuidelineNodeId B)
	{
		return RouteSearch::Find(Net, FRouteQuery::For(ERouteErrand::GraphProbe, A, B, 0.0, ETraversalClass::GroundVehicle));
	};
	const FRoutePlan Leg0 = Plan(Start, Ends[1]);
	const FRoutePlan Leg1 = Plan(Ends[1], Goal);
	const FVehicle Rig = Course->GetVehicles()[0];
	if (!TestTrue(TEXT("both fixture legs plan"), Leg0.IsValid() && Leg1.IsValid())) { return false; }
	TestTrue(TEXT("leg 0, a U, holds the rig on its own"), VehicleFit::JudgePlan(Leg0, Rig, Net).Fits());
	TestTrue(TEXT("leg 1, one quarter, holds the rig on its own"), VehicleFit::JudgePlan(Leg1, Rig, Net).Fits());

	Course->PlanOverrideForTest = [&](int32 LegIndex, int32 Slot, bool, FRoutePlan& OutPlan)
	{
		if (Slot != 0 || LegIndex > 1) { return false; }
		OutPlan = LegIndex == 0 ? Leg0 : Leg1;
		return true;
	};
	FRoutePlan Route;
	TArray<FRigLegMarker> Markers;
	int32 EndStop = INDEX_NONE;
	FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	const bool bPlanned = Course->PlanLoopRouteForTest(0, 0, Route, Markers, EndStop);
	GLog->RemoveOutputDevice(&Spy);
	Course->PlanOverrideForTest = nullptr;

	TestTrue(TEXT("a route is planned"), bPlanned);
	TestEqual(TEXT("it ends at leg 0's end - waypoint 1, mid-loop"), EndStop, 1);
	TestEqual(TEXT("with one leg's marker"), Markers.Num(), 1);
	TestEqual(TEXT("and exactly leg 0's road, the folding join cut off"), Route.Length, Leg0.Length, 0.01);
	TestTrue(TEXT("the kept route holds the rig"), VehicleFit::JudgePlan(Route, Rig, Net).Fits());
	TestTrue(TEXT("and the cut is said, with the fold"),
		Spy.Containing(TEXT("the joined route folds the tow"), TEXT("trailer folds at guideline node")) == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCoursePlanCacheKnowsItsVehicleTest,
	"AirportMgr.RigCourse.PlanCacheKnowsItsVehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCoursePlanCacheKnowsItsVehicleTest::RunTest(const FString& Parameters)
{
	// A CACHED PLAN IS A FACT ABOUT A BODY, not a slot (re-review of aa90eec2): asked again, the
	// same leg is answered from the cache; asked for a different vehicle in the same slot, it is
	// planned afresh.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);

	FRoutePlan Plan;
	FString Reason;
	const int32 Before = Course->GetPlanFindsForTest();
	Course->PlanBetweenForTest(0, 1, 0, Plan, Reason);
	TestEqual(TEXT("the first ask runs a Find"), Course->GetPlanFindsForTest(), Before + 1);
	Course->PlanBetweenForTest(0, 1, 0, Plan, Reason);
	TestEqual(TEXT("the same ask is answered from the cache"), Course->GetPlanFindsForTest(), Before + 1);

	FVehicle Longer = Course->GetVehicles()[0];
	Longer.Tow[0].Length += 100.0;
	Course->SetVehicleForTest(0, Longer);
	Course->PlanBetweenForTest(0, 1, 0, Plan, Reason);
	TestEqual(TEXT("a different trailer in the same slot is planned afresh"), Course->GetPlanFindsForTest(), Before + 2);
	TestNotEqual(TEXT("because the key is the vehicle's figures"),
		ARigTestCourse::VehicleIdentity(Longer), ARigTestCourse::VehicleIdentity(UAirsideSettings::ResolveRigVehicle()));
	return true;
}

namespace RigCourseFoldFixture
{
	/**
	 * Hand-drawn, far off the course and authored so no rebuild sweeps them: straight in, Quarters
	 * same-hand quarters of 830 uu legs (a U folds nothing; three fold the rig), 40 m out to End,
	 * and 40 m straight on to Beyond. Ends[i] is quarter i's end.
	 */
	struct FFold
	{
		FGuidelineNodeId Start;
		TArray<FGuidelineNodeId> Ends;
		FGuidelineNodeId End;
		FGuidelineNodeId Beyond;
	};

	FFold Lay(URoadNetwork& Net, int32 Quarters)
	{
		const double Leg = 830.0;
		const FVector2D Origin(-200000.0, -200000.0);
		auto Node = [&Net](const FVector2D& At) { return Net.AddGuidelineNode(At, /*bDerived=*/false); };
		auto Join = [&Net](FGuidelineNodeId A, FGuidelineNodeId B, const FVector2D& Control)
		{
			FGuidelineEdge Edge;
			Edge.A = A;
			Edge.B = B;
			Edge.Control = Control;
			Edge.AllowedTraffic = FTrafficMask::All();
			Edge.Direction = EGuidelineDir::AToB;
			Edge.bDerived = false;
			Net.AddGuidelineEdge(MoveTemp(Edge));
		};
		FFold Out;
		Out.Start = Node(Origin + FVector2D(-4000.0, 0.0));
		FGuidelineNodeId From = Node(Origin);
		Join(Out.Start, From, Origin + FVector2D(-2000.0, 0.0));
		FVector2D At = Origin;
		FVector2D Heading(1.0, 0.0);
		for (int32 Quarter = 0; Quarter < Quarters; ++Quarter)
		{
			const FVector2D Side(-Heading.Y, Heading.X);
			const FVector2D Control = At + Heading * Leg;
			At = Control + Side * Leg;
			Heading = Side;
			const FGuidelineNodeId To = Node(At);
			Join(From, To, Control);
			Out.Ends.Add(To);
			From = To;
		}
		Out.End = Node(At + Heading * 4000.0);
		Join(From, Out.End, At + Heading * 2000.0);
		Out.Beyond = Node(At + Heading * 8000.0);
		Join(Out.End, Out.Beyond, At + Heading * 6000.0);
		return Out;
	}

	FRoutePlan Plan(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		return RouteSearch::Find(Net, FRouteQuery::For(ERouteErrand::GraphProbe, A, B, 0.0, ETraversalClass::GroundVehicle));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseFirstLegFoldIsRefusedTest,
	"AirportMgr.RigCourse.FirstLegFoldIsRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseFirstLegFoldIsRefusedTest::RunTest(const FString& Parameters)
{
	// NOTHING BEFORE THE FOLD TO KEEP (re-review of 8de90a45): the rig's first leg is overridden
	// with three same-hand quarters, which fold it on their own, and its second with a straight on.
	// The joined route folds on leg 0, so no cut leaves a leg that holds - PlanLoopRoute must say
	// so and refuse the section, handing nothing on to be dispatched.
	using namespace RigCourseTest;
	using namespace RigCourseFoldFixture;
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	URoadNetwork& Net = *Actor->Network;
	const FFold Fold = Lay(Net, 3);
	const FRoutePlan Leg0 = Plan(Net, Fold.Start, Fold.End);
	const FRoutePlan Leg1 = Plan(Net, Fold.End, Fold.Beyond);
	const FVehicle Rig = Course->GetVehicles()[0];
	if (!TestTrue(TEXT("both fixture legs plan"), Leg0.IsValid() && Leg1.IsValid())) { return false; }
	TestEqual(TEXT("leg 0 folds the rig on its own"),
		static_cast<int32>(VehicleFit::JudgePlan(Leg0, Rig, Net).Refusal), static_cast<int32>(EFitRefusal::TrailerFolds));

	Course->PlanOverrideForTest = [&](int32 LegIndex, int32 Slot, bool, FRoutePlan& OutPlan)
	{
		if (Slot != 0 || LegIndex > 1) { return false; }
		OutPlan = LegIndex == 0 ? Leg0 : Leg1;
		return true;
	};
	FRoutePlan Route;
	TArray<FRigLegMarker> Markers;
	int32 EndStop = INDEX_NONE;
	FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	const bool bPlanned = Course->PlanLoopRouteForTest(0, 0, Route, Markers, EndStop);
	GLog->RemoveOutputDevice(&Spy);
	Course->PlanOverrideForTest = nullptr;

	TestFalse(TEXT("the section is refused"), bPlanned);
	TestFalse(TEXT("no route is handed on to dispatch"), Route.IsValid());
	TestEqual(TEXT("and no leg marker"), Markers.Num(), 0);
	TestEqual(TEXT("the route ends where it would have started"), EndStop, 0);
	TestEqual(TEXT("said once, with the fold"),
		Spy.Containing(TEXT("folds the tow on its first leg"), TEXT("trailer folds at guideline node")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseFitCacheDropsOnRebuildTest,
	"AirportMgr.RigCourse.FitCacheDropsOnRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseFitCacheDropsOnRebuildTest::RunTest(const FString& Parameters)
{
	// A STALE "FITS" IS NEVER SERVED (re-review of 8de90a45). FRouteQuery::FitCache trusts its
	// owner to key it on the graph; the course does, by GuidelineRevision. The cache is filled,
	// then a course node is moved through the editor's own path - its roads rebuild, their
	// curves' radii and clearances change - and every entry the cache holds afterwards names a
	// LIVE edge and says what VehicleFit says of it now.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	const FVehicle Rig = Course->GetVehicles()[0];

	FRoutePlan Plan;
	FString Reason;
	Course->PlanBetweenForTest(0, 1, 0, Plan, Reason);
	Course->PlanBetweenForTest(1, 2, 0, Plan, Reason);
	int32 Filled = 0;
	Course->ForEachFitCacheEntryForTest([&Filled](FGuidelineEdgeId, bool) { ++Filled; });
	if (!TestTrue(FString::Printf(TEXT("the cache is filled (%d edges)"), Filled), Filled > 0)) { return false; }

	// Waypoint 1's corner, moved 3 m: every lane and turn at it is derived again.
	const URoadNetwork& Net = *Actor->Network;
	const uint32 RevisionBefore = Net.GetGuidelineRevision();
	const FRoadNodeId Corner = Course->GetWaypoints()[1].Node;
	const FRoadNode* CornerNode = Net.GetNode(Corner);
	if (!TestNotNull(TEXT("the corner"), CornerNode)) { return false; }
	TestTrue(TEXT("the corner moves through the editor's path"),
		Actor->MoveNode(Corner.Index, CornerNode->Position + FVector2D(300.0, 0.0)));
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		Actor->Tick(1.0f / 30.0f);
	}
	if (!TestNotEqual(TEXT("the rebuild bumped the guideline revision"), Net.GetGuidelineRevision(), RevisionBefore)) { return false; }

	const int32 FindsBefore = Course->GetPlanFindsForTest();
	Course->PlanBetweenForTest(0, 1, 0, Plan, Reason);
	TestEqual(TEXT("the same leg is planned afresh on the new graph"), Course->GetPlanFindsForTest(), FindsBefore + 1);
	int32 Stale = 0;
	int32 Wrong = 0;
	int32 Checked = 0;
	Course->ForEachFitCacheEntryForTest([&](FGuidelineEdgeId Id, bool bFits)
	{
		++Checked;
		const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
		if (Edge == nullptr) { ++Stale; return; }
		Wrong += VehicleFit::Fits(*Edge, Rig, Net) != bFits ? 1 : 0;
	});
	AddInfo(FString::Printf(TEXT("after the move: %d cached edge(s), %d dead, %d wrong"), Checked, Stale, Wrong));
	TestTrue(TEXT("the cache was refilled on the new graph"), Checked > 0);
	TestEqual(TEXT("no cached edge is from the old graph"), Stale, 0);
	TestEqual(TEXT("and every cached verdict is VehicleFit's now"), Wrong, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseEveryBendIsConcentricTest,
	"AirportMgr.RigCourse.EveryBendIsConcentric",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseEveryBendIsConcentricTest::RunTest(const FString& Parameters)
{
	// ONE SHAPE (re-review of d487f0da: the user saw three on the course). Every two-arm road bend
	// the course lays gets the outer edge concentric with its lanes - width steps included - and
	// paves by the fan with its cut vertices bitwise on the rim, so the ribbons weld as before.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	URoadNetwork& Net = *Actor->Network;
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net, 12, &Designs, EWideningTrace::Trace);

	int32 Applied = 0;
	for (const FBendOuter& Bend : Solved.BendOuters)
	{
		const FCappedWidening* Capped = Solved.CappedWidenings.FindByPredicate(
			[&Bend](const FCappedWidening& C) { return C.NodeIndex == Bend.NodeIndex; });
		const FJunctionResult* Row = Solved.NodeResults.Find(Bend.NodeIndex);
		const bool bWidened = Row != nullptr && Row->Arms.Num() == 2 && Row->Corners.Num() == 2;
		AddInfo(FString::Printf(TEXT("BENDROW (%.0f, %.0f) | %s | %s | inner %.0f | outer %.0f | widths %.0f / %.0f | cuts %.0f / %.0f | widening %s"),
			Bend.Position.X, Bend.Position.Y, *Bend.Tiers, Bend.bApplied ? TEXT("applied") : *(TEXT("skipped: ") + Bend.Reason),
			Bend.InnerRadius, Bend.OuterRadius, Bend.Widths[0], Bend.Widths[1],
			bWidened ? Row->Arms[0].CutDistance : 0.0, bWidened ? Row->Arms[1].CutDistance : 0.0,
			Capped != nullptr ? *FString::Printf(TEXT("CAPPED by segment %d (%.0f uu missing)"), Capped->Segment.Index, Capped->Missing) : TEXT("full or none")));
		TestTrue(FString::Printf(TEXT("bend at (%.0f, %.0f) [%s] has the concentric outer edge (%s)"),
			Bend.Position.X, Bend.Position.Y, *Bend.Tiers, *Bend.Reason), Bend.bApplied);
		if (!Bend.bApplied) { continue; }
		++Applied;
		const FJunctionResult* Junction = Solved.NodeResults.Find(Bend.NodeIndex);
		if (!TestNotNull(TEXT("its junction solved"), Junction)) { continue; }
		TestTrue(FString::Printf(TEXT("bend at (%.0f, %.0f) is paved by the fan, not ear-clipped"), Bend.Position.X, Bend.Position.Y),
			Junction->Triangles.Num() > 0);
		for (const FJunctionArmResult& Arm : Junction->Arms)
		{
			for (const FVector2D& Cut : { Arm.LeftCut, Arm.RightCut })
			{
				bool bOnRim = false;
				for (const FVector2D& P : Junction->Boundary) { bOnRim |= (P.X == Cut.X && P.Y == Cut.Y); }
				TestTrue(FString::Printf(TEXT("bend at (%.0f, %.0f): cut vertex (%.1f, %.1f) is on the rim bitwise - the weld"),
					Bend.Position.X, Bend.Position.Y, Cut.X, Cut.Y), bOnRim);
			}
		}
	}
	TestTrue(FString::Printf(TEXT("the course has bends to judge (%d)"), Solved.BendOuters.Num()), Applied >= 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseBendsAreSmoothTest,
	"AirportMgr.RigCourse.BendsAreSmooth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseBendsAreSmoothTest::RunTest(const FString& Parameters)
{
	// ONE SMOOTH SHAPE (user, PIE on 33d6f49b: "three variants"). At every two-arm road bend the
	// inner edge, the outer edge and every lane are tangent-continuous from arm to arm - no kink
	// over BendProbe::KinkThreshold, no edge tighter than TightestFraction of its inner arc - and
	// every lane through a bend is laid by one density rule (GuidelineGeom::BendPieceLength).
	// The per-tier fixture judges the same way: Airside.Build.BendLanes.EveryTierIsSmooth.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	ARigTestCourse* Course = TestWorld.World->SpawnActor<ARigTestCourse>();
	if (!TestNotNull(TEXT("the course actor"), Course)) { return false; }
	Course->BuildCourseForTest(*Actor);
	URoadNetwork& Net = *Actor->Network;
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net, 12, &Designs, EWideningTrace::Trace);
	FRoadGuidelineBuilder::Build(Net, Solved, Designs);

	int32 Bends = 0;
	for (const FBendOuter& Bend : Solved.BendOuters)
	{
		const FJunctionResult* Junction = Solved.NodeResults.Find(Bend.NodeIndex);
		if (!TestNotNull(TEXT("the bend's junction"), Junction) || Junction->Corners.Num() != 2) { continue; }
		++Bends;
		const BendProbe::FSmoothness S = BendProbe::MeasureSmoothness(Net, *Junction, Net.NodeIdAt(Bend.NodeIndex));
		AddInfo(FString::Printf(TEXT("SMOOTHROW (%.0f, %.0f) | %s | inner kink %.1f at (%.0f, %.0f) | outer kink %.1f at (%.0f, %.0f) | tightest %.0f of arc %.0f | ramps %.0f / %.0f | lanes: %s"),
			Bend.Position.X, Bend.Position.Y, *Bend.Tiers, S.InnerKink, S.InnerAt.X, S.InnerAt.Y, S.OuterKink, S.OuterAt.X, S.OuterAt.Y,
			S.Tightest, Bend.InnerArcRadius, Bend.Ramp[0], Bend.Ramp[1], *S.LaneText));
		TestTrue(FString::Printf(TEXT("bend (%.0f, %.0f) was laid as the smooth shape (%s)"), Bend.Position.X, Bend.Position.Y, *Bend.Reason),
			Bend.bApplied);
		// THE FIVE SHARED SHAPE RULES, held once in BendProbe::JudgeBend since #301 - the per-tier
		// fixture judges the same way (Airside.Build.BendLanes.EveryTierIsSmooth) and used to
		// repeat these five TestTrue calls verbatim. MinLanes 1: the course's tiers see a variable
		// lane count, unlike the fixture's fixed two-arm bend.
		BendProbe::JudgeBend(*this, FString::Printf(TEXT("bend (%.0f, %.0f)"), Bend.Position.X, Bend.Position.Y),
			S, Bend.InnerArcRadius, /*MinLanes=*/1);
	}
	TestTrue(FString::Printf(TEXT("the course's bends were judged (%d)"), Bends), Bends >= 14);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigCourseLayoutJsonWrittenTest,
	"AirportMgr.RigCourse.LayoutJsonWritten",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRigCourseLayoutJsonWrittenTest::RunTest(const FString& Parameters)
{
	// #301: Tools/Python/build_rig_test_level.py used to retype FRigCourseLayout's constants and
	// the course's own bounding box by hand off a code comment ("Recompute this block if that
	// file's layout constants move"). THE SEAM this test pins: Lay()'s own JSON export, written
	// to Saved/RigCourseLayout.json for the Python script to read instead. Goes red if ToJson
	// stops naming a figure the script needs, or if nothing ever writes the file for it to read.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the network actor"), Actor)) { return false; }
	const FRigCourseLayoutResult Result = FRigCourseLayout::Lay(*Actor);
	if (!TestTrue(TEXT("the layout placed waypoints"), Result.Waypoints.Num() > 0)) { return false; }
	if (!TestTrue(TEXT("the bounding box is real, not the untouched sentinel"),
		Result.BoxMax.X > Result.BoxMin.X && Result.BoxMax.Y > Result.BoxMin.Y)) { return false; }

	const FString Json = FRigCourseLayout::ToJson(Result);
	// EVERY FIGURE THE PYTHON SCRIPT NAMES (build_rig_test_level.py's COURSE_MIN_X etc.): if one
	// goes missing from the export, the script would silently keep reading whatever figure it
	// last had, which is exactly the drift #301 found.
	for (const TCHAR* Key : { TEXT("\"LaneStraight\""), TEXT("\"TierPitch\""), TEXT("\"BoxMinX\""),
		TEXT("\"BoxMaxX\""), TEXT("\"BoxMinY\""), TEXT("\"BoxMaxY\""), TEXT("\"TierCount\"") })
	{
		TestTrue(FString::Printf(TEXT("the JSON names %s"), Key), Json.Contains(Key));
	}

	const FString Path = FPaths::ProjectSavedDir() / TEXT("RigCourseLayout.json");
	if (!TestTrue(TEXT("the JSON writes to Saved/ - the Python script's own read path"),
		FFileHelper::SaveStringToFile(Json, *Path))) { return false; }

	FString ReadBack;
	if (!TestTrue(TEXT("it reads back"), FFileHelper::LoadFileToString(ReadBack, *Path))) { return false; }
	TestEqual(TEXT("bitwise the same JSON that was written"), ReadBack, Json);
	return true;
}

#endif
