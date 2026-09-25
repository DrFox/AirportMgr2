#include "Model/VehicleFit.h"

#include "AirsideLog.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/VehicleSweep.h"

VehicleSweep::FBody VehicleFit::BodyOf(const FVehicle& Vehicle)
{
	VehicleSweep::FBody Body;
	Body.Wheelbase = Vehicle.Chassis.Wheelbase();
	Body.Width = Vehicle.BodyWidth;
	Body.FrontX = Vehicle.BodyFrontX;
	Body.RearX = Vehicle.BodyRearX;
	// Link for link, field for field: FTowLink is FLink with UPROPERTYs on. An empty Tow maps to
	// an empty chain - rigid - as an unset FTrailer used to map to KingpinToAxle 0.
	for (const FTowLink& Link : Vehicle.Tow)
	{
		VehicleSweep::FLink& Out = Body.Tow.AddDefaulted_GetRef();
		Out.HitchX = Link.HitchX;
		Out.Length = Link.Length;
		Out.BodyFront = Link.BodyFront;
		Out.BodyRear = Link.BodyRear;
		Out.Width = Link.Width;
	}
	return Body;
}

FString FFitVerdict::Describe() const
{
	switch (Refusal)
	{
	case EFitRefusal::LaneTooNarrow:
		return FString::Printf(TEXT("body %.2f m with margins vs lane %.2f m"), Needed / 100.0, Available / 100.0);
	case EFitRefusal::TighterThanLock:
		return FString::Printf(TEXT("lock radius %.1f m vs curve %.1f m"), Needed / 100.0, Available / 100.0);
	case EFitRefusal::Jackknife:
		return TEXT("the tow jack-knifes on the curve");
	case EFitRefusal::SweptOverTarmac:
		return bWholeRoute
			? FString::Printf(TEXT("swept %.1f m vs tarmac %.1f m at sample %d of the edge leaving guideline node %d, on the whole route"),
				Needed / 100.0, Available / 100.0, Sample, Node.Index)
			: FString::Printf(TEXT("swept %.1f m vs tarmac %.1f m at sample %d"), Needed / 100.0, Available / 100.0, Sample);
	case EFitRefusal::TrailerFolds:
		return FString::Printf(TEXT("trailer folds at guideline node %d / (%.0f, %.0f), link %d, angle %.0f deg, %.1f m along the route"),
			Node.Index, At.X, At.Y, Link, FMath::RadiansToDegrees(Radians), Along / 100.0);
	default:
		return FString();
	}
}

FFitVerdict VehicleFit::Judge(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network)
{
	FFitVerdict Verdict;
	const double Widest = Vehicle.WidestBody();
	if (Widest > 0.0 && Edge.Width > 0.0 && Widest + 2.0 * WidthMargin > Edge.Width)
	{
		Verdict.Refusal = EFitRefusal::LaneTooNarrow;
		Verdict.Needed = Widest + 2.0 * WidthMargin;
		Verdict.Available = Edge.Width;
		return Verdict;
	}
	if (Edge.MinRadius <= 0.0)
	{
		return Verdict;
	}

	const double Lock = Vehicle.Chassis.TightestFollowableRadius();
	if (Lock > Edge.MinRadius)
	{
		Verdict.Refusal = EFitRefusal::TighterThanLock;
		Verdict.Needed = Lock;
		Verdict.Available = Edge.MinRadius;
		return Verdict;
	}
	if (Widest <= 0.0 || Edge.ClearInnerAt.Num() == 0)
	{
		return Verdict;
	}

	const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
	const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
	if (A == nullptr || B == nullptr)
	{
		return Verdict;
	}
	// The samples the builder measured the clearances on, so they line up with the simulated
	// path index for index.
	// ENFORCED BY: Airside.Build.MeasuredOnFollowerSamples (these are the follower's points too)
	TArray<FVector2D> Path;
	GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Path);
	if (Path.Num() != Edge.ClearInnerAt.Num() || Path.Num() != Edge.ClearOuterAt.Num())
	{
		return Verdict;   // measured against a different sampling: say nothing rather than guess
	}

	TArray<double> Inner, Outer;
	if (!VehicleSweep::Trace(BodyOf(Vehicle), Path, Inner, Outer))
	{
		Verdict.Refusal = EFitRefusal::Jackknife;
		return Verdict;
	}
	// THE WORST SAMPLE, not the first: the verdict is the same either way (any excess refuses),
	// and the worst is the figure that says how much wider the road must be.
	double WorstExcess = 0.0;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		const double Swept = Inner[Index] + Outer[Index];
		const double Tarmac = Edge.ClearInnerAt[Index] + Edge.ClearOuterAt[Index];
		if (Swept > Tarmac && Swept - Tarmac > WorstExcess)
		{
			WorstExcess = Swept - Tarmac;
			Verdict.Refusal = EFitRefusal::SweptOverTarmac;
			Verdict.Needed = Swept;
			Verdict.Available = Tarmac;
			Verdict.Sample = Index;
		}
	}
	return Verdict;
}

bool VehicleFit::Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network)
{
	return Judge(Edge, Vehicle, Network).Fits();
}

double VehicleFit::TowSubStepSeconds(const FChassis& Chassis)
{
	// SIZED BY TIME at the cap, not by distance: a sub-step at the cap covers exactly TraceStep,
	// and any slower one less. The expression FRoadAgent's FollowAndTow used inline until
	// 2026-09-25, moved here unchanged so its sub-steps stay bitwise what they were.
	const double SpeedCap = FMath::Max(Chassis.Ground.Taxi.SpeedCap, UE_KINDA_SMALL_NUMBER);
	return VehicleSweep::TraceStep / SpeedCap;
}

FVector2D VehicleFit::FixedAxleAt(const FChassis& Chassis, const FVector2D& Origin, double Heading,
	FVector2D& OutForward)
{
	OutForward = FVector2D(FMath::Cos(Heading), FMath::Sin(Heading));
	return Origin + OutForward * Chassis.FixedAxleX;
}

void VehicleFit::LayTow(const FVehicle& Vehicle, const FVector2D& Steered, const FVector2D& Forward,
	TArray<FVector2D>& OutAxles)
{
	OutAxles.Reset();
	if (!Vehicle.HasTrailer())
	{
		return;
	}
	// StartDrive's own arithmetic, moved: the fixed axle is (SteerAxleX - FixedAxleX) behind the
	// steered axle on the cab's line, and the chain lies straight behind that.
	const FVector2D Fixed = Steered - Forward * (Vehicle.Chassis.SteerAxleX - Vehicle.Chassis.FixedAxleX);
	VehicleSweep::LayChainStraight(BodyOf(Vehicle), Fixed, Forward, OutAxles);
}

bool VehicleFit::StepTow(const VehicleSweep::FBody& Body, const FChassis& Chassis, const FVector2D& Origin,
	double Heading, TArrayView<FVector2D> InOutAxles, int32& OutFoldedLink, double& OutFoldRadians)
{
	FVector2D Forward;
	const FVector2D Fixed = FixedAxleAt(Chassis, Origin, Heading, Forward);
	return VehicleSweep::StepChain(Body, Fixed, Forward, InOutAxles, OutFoldedLink, OutFoldRadians);
}

namespace
{
	/** The plan step whose span holds route distance Along: the first to end at or past it. */
	int32 PlanStepAtDistance(const FRoutePlan& Plan, double Along)
	{
		for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
		{
			if (Plan.Steps[Index].EndDistance >= Along)
			{
				return Index;
			}
		}
		return Plan.Steps.Num() - 1;
	}

	/** Names a plan step for a verdict: its edge, the node it leaves, and where that node is. */
	void NamePlanStep(const FRoutePlan& Plan, int32 Step, const URoadNetwork& Network, FFitVerdict& Out)
	{
		if (!Plan.Steps.IsValidIndex(Step))
		{
			return;
		}
		Out.Edge = Plan.Steps[Step].Edge;
		Out.Node = Step == 0 ? Plan.Start : Plan.Steps[Step - 1].To;
		if (const FGuidelineNode* Node = Network.GetGuidelineNode(Out.Node))
		{
			Out.NodeAt = Node->Position;
		}
	}

	/**
	 * A tow's SAFETY CAP on the whole-route drive, s of simulated time: an hour. A follower with
	 * a valid plan arrives (the profile brakes it onto the end), so this only ends a drive that
	 * something broke - and a check that never returned would hang the search that asked.
	 */
	constexpr double MaxJudgedPlanSeconds = 3600.0;
}

FFitVerdict VehicleFit::JudgePlan(const FRoutePlan& InPlan, const FVehicle& Vehicle, const URoadNetwork& Network,
	const FTowSeed* Seed)
{
	FFitVerdict Verdict;
	Verdict.bWholeRoute = true;
	if (!Vehicle.HasTrailer() || !InPlan.IsValid() || InPlan.Polyline.Num() < 2 || InPlan.Steps.Num() == 0)
	{
		return Verdict;
	}

	// UP TO THE FIRST REVERSE LEG: the agent hands those to FReverseRun, which moves no chain.
	const int32 FirstReverse = InPlan.Steps.IndexOfByPredicate([](const FRouteStep& Step) { return Step.bReverseLeg; });
	if (FirstReverse == 0)
	{
		return Verdict;
	}
	FRoutePlan Cut;
	if (FirstReverse != INDEX_NONE)
	{
		Cut = RouteSearch::Section(InPlan, 0, FirstReverse - 1);
	}
	const FRoutePlan& Plan = FirstReverse != INDEX_NONE ? Cut : InPlan;

	const FChassis& Chassis = Vehicle.Chassis;
	const VehicleSweep::FBody Body = BodyOf(Vehicle);

	// DISPATCHED AS StartDrive DISPATCHES: the follower from rest on the plan's first point,
	// facing its line, the chain laid straight behind - once, here, and never again. OR, SEEDED,
	// as RestartTaxi restarts a vehicle already out: part-way along (InitialTravelled, which a
	// rejoin passes), at its own heading and speed, its chain exactly where it is.
	FRouteFollower Follower;
	TArray<FVector2D> Axles;
	if (Seed != nullptr && Seed->Axles.Num() == Vehicle.Tow.Num())
	{
		Follower.Start(Plan, Chassis, Seed->Speed, Seed->Heading, Seed->Travelled);
		Axles.Append(Seed->Axles.GetData(), Seed->Axles.Num());
	}
	else
	{
		Follower.Start(Plan, Chassis);
		FVector2D Steered = Plan.Polyline[0];
		double LineHeading = 0.0;
		GuidelineGeom::PointAtDistance(Plan.Polyline, Follower.Travelled, Steered, LineHeading);
		LayTow(Vehicle, Steered, FVector2D(FMath::Cos(Follower.Heading), FMath::Sin(Follower.Heading)), Axles);
	}

	// THE CLEARANCE HALF, only where an edge on the plan has per-sample data to judge against:
	// a route of straights and balloons pays for the fold walk and nothing else.
	bool bAnyClearance = false;
	for (const FRouteStep& Step : Plan.Steps)
	{
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
		bAnyClearance |= Edge != nullptr && Edge->ClearInnerAt.Num() > 0 && Vehicle.WidestBody() > 0.0;
	}
	// Per polyline vertex, the body's furthest reach either side of the line - Trace's Inner and
	// Outer, named by side rather than by the turn, because a route turns both ways.
	TArray<double> Left, Right, VertexAlong;
	// How far along the route a corner may be credited to, either side of the steered axle:
	// the whole train and a margin. NOT the whole polyline, which would credit a trailer in a
	// balloon to the lane coming the other way past it.
	double Reach = FMath::Abs(Body.FrontX) + FMath::Abs(Body.RearX) + Body.Wheelbase + 200.0;
	for (const VehicleSweep::FLink& Link : Body.Tow)
	{
		Reach += FMath::Abs(Link.HitchX) + Link.Length + Link.BodyFront + Link.BodyRear;
	}
	// WHICH VERTICES CAN BE JUDGED AT ALL - those of a step whose edge measured per-sample
	// clearances on this sampling - as a running count, so a sub-step whose window holds none of
	// them skips the corner projection outright (re-review of aa90eec2: most of a course route is
	// straight lane, and projecting the body there only fed samples the tarmac loop below then
	// ignores - the same verdict, measured cheaper). MeasuredBefore[V] counts measured vertices < V.
	TArray<int32> MeasuredBefore;
	if (bAnyClearance)
	{
		Left.Init(0.0, Plan.Polyline.Num());
		Right.Init(0.0, Plan.Polyline.Num());
		TArray<bool> Measured;
		Measured.Init(false, Plan.Polyline.Num());
		for (int32 StepIndex = 0; StepIndex < Plan.Steps.Num(); ++StepIndex)
		{
			const FRouteStep& Step = Plan.Steps[StepIndex];
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
			const int32 StartVertex = StepIndex == 0 ? 0 : Plan.Steps[StepIndex - 1].EndVertex;
			const int32 Count = Step.EndVertex - StartVertex + 1;
			if (Edge != nullptr && Edge->ClearInnerAt.Num() == Count && Edge->ClearOuterAt.Num() == Count
				&& Step.EndVertex < Plan.Polyline.Num())
			{
				for (int32 K = 0; K < Count; ++K)
				{
					Measured[StartVertex + K] = true;
				}
			}
		}
		MeasuredBefore.Init(0, Plan.Polyline.Num() + 1);
		for (int32 Index = 0; Index < Plan.Polyline.Num(); ++Index)
		{
			MeasuredBefore[Index + 1] = MeasuredBefore[Index] + (Measured[Index] ? 1 : 0);
		}
	}
	VertexAlong.Init(0.0, Plan.Polyline.Num());
	for (int32 Index = 1; Index < Plan.Polyline.Num(); ++Index)
	{
		VertexAlong[Index] = VertexAlong[Index - 1] + FVector2D::Distance(Plan.Polyline[Index - 1], Plan.Polyline[Index]);
	}

	const double Dt = TowSubStepSeconds(Chassis);
	const int32 MaxSubSteps = FMath::CeilToInt32(MaxJudgedPlanSeconds / Dt);
	int32 Lo = 0;
	int32 Hi = 0;
	VehicleSweep::FCorners Corners;
	int32 SubStep = 0;
	double WorstRadians = 0.0;
	for (; SubStep < MaxSubSteps && !Follower.HasArrived(); ++SubStep)
	{
		FVector2D At;
		double Heading = 0.0;
		if (!Follower.Advance(Dt, Chassis, TNumericLimits<double>::Max(), At, Heading))
		{
			break;
		}
		int32 FoldedLink = INDEX_NONE;
		double Radians = 0.0;
		if (!StepTow(Body, Chassis, At, Heading, Axles, FoldedLink, Radians))
		{
			Verdict.Refusal = EFitRefusal::TrailerFolds;
			Verdict.Link = FoldedLink;
			Verdict.Radians = Radians;
			Verdict.At = Axles[FoldedLink];
			Verdict.Along = Follower.Travelled;

			// NAMED BY WHERE THE CAB IS - the steered axle's edge at the first failing sample, the
			// edge being driven when the trailer went. Two alternatives were tried on the three-
			// quarter loop (2026-09-25) and rejected: the folded axle's PROJECTION onto the plan
			// named the loop's first quarter (a folded trailer has cut deep inside the turn), and
			// the axle's place a train-length back along the route named the second - both edges
			// the working way round shares (Airside.Model.Tow.WholeRouteRetriesRoundAFold).
			NamePlanStep(Plan, PlanStepAtDistance(Plan, Follower.Travelled), Network, Verdict);
			return Verdict;
		}
		WorstRadians = FMath::Max(WorstRadians, Radians);
		if (!bAnyClearance)
		{
			continue;
		}
		// THE WINDOW: vertices within Reach of the steered axle along the route. Travelled only
		// grows, so both ends only move forward - one pass over the polyline for the whole drive.
		while (Lo + 1 < Plan.Polyline.Num() && VertexAlong[Lo + 1] < Follower.Travelled - Reach)
		{
			++Lo;
		}
		while (Hi + 1 < Plan.Polyline.Num() && VertexAlong[Hi] < Follower.Travelled + Reach)
		{
			++Hi;
		}
		if (Hi <= Lo || MeasuredBefore[Hi + 1] == MeasuredBefore[Lo])
		{
			continue;
		}
		FVector2D Forward;
		const FVector2D Fixed = FixedAxleAt(Chassis, At, Heading, Forward);
		VehicleSweep::BodyCorners(Body, Fixed, Forward, Axles, Corners);
		for (const FVector2D& Corner : Corners)
		{
			// Trace's projection, on the plan's polyline: the nearest span, its foot, and the
			// side. A point beyond the route's own ends is in no lane this check can judge.
			double Best = TNumericLimits<double>::Max();
			int32 BestSpan = Lo;
			double BestT = 0.0;
			for (int32 Span = Lo; Span < Hi; ++Span)
			{
				const FVector2D AB = Plan.Polyline[Span + 1] - Plan.Polyline[Span];
				const double T = FMath::Clamp(FVector2D::DotProduct(Corner - Plan.Polyline[Span], AB)
					/ FMath::Max(AB.SizeSquared(), UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
				const double Distance = FVector2D::Distance(Corner, Plan.Polyline[Span] + AB * T);
				if (Distance < Best)
				{
					Best = Distance;
					BestSpan = Span;
					BestT = T;
				}
			}
			if ((BestSpan == 0 && BestT <= 0.0) || (BestSpan == Plan.Polyline.Num() - 2 && BestT >= 1.0))
			{
				continue;
			}
			const FVector2D AB = Plan.Polyline[BestSpan + 1] - Plan.Polyline[BestSpan];
			const FVector2D Foot = Plan.Polyline[BestSpan] + AB * BestT;
			const FVector2D Normal = FVector2D(-AB.Y, AB.X).GetSafeNormal();
			const double Lateral = FVector2D::DotProduct(Corner - Foot, Normal);
			const int32 Sample = BestT < 0.5 ? BestSpan : BestSpan + 1;
			if (Lateral > 0.0)
			{
				Left[Sample] = FMath::Max(Left[Sample], Lateral);
			}
			else
			{
				Right[Sample] = FMath::Max(Right[Sample], -Lateral);
			}
		}
	}
	if (SubStep >= MaxSubSteps)
	{
		// Said, not swallowed: a check that gave up has judged nothing, and passing the plan
		// on that is the permissive answer - the log is how anyone finds out it happened.
		UE_LOG(LogAirside, Warning, TEXT("VehicleFit::JudgePlan: %s did not arrive in %.0f s of simulated driving; the route was not judged."),
			*Vehicle.TypeCode.ToString(), MaxJudgedPlanSeconds);
		return Verdict;
	}

	// HOW CLOSE IT CAME, on a pass: the worst hitch angle of the whole drive, for the log and the
	// tests that say how much margin a route had (a clearance refusal below keeps it too).
	Verdict.Radians = WorstRadians;

	// THE TARMAC, per edge that measured it: the plan's vertices of a step ARE that edge's
	// samples (BuildPlanFromArrival welds them in order, reversed with the step), so vertex
	// StartVertex + K is the edge's sample K, or Count - 1 - K when walked B to A.
	double WorstExcess = 0.0;
	for (int32 StepIndex = 0; bAnyClearance && StepIndex < Plan.Steps.Num(); ++StepIndex)
	{
		const FRouteStep& Step = Plan.Steps[StepIndex];
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
		const int32 StartVertex = StepIndex == 0 ? 0 : Plan.Steps[StepIndex - 1].EndVertex;
		const int32 Count = Step.EndVertex - StartVertex + 1;
		if (Edge == nullptr || Edge->ClearInnerAt.Num() == 0 || Count != Edge->ClearInnerAt.Num()
			|| Count != Edge->ClearOuterAt.Num() || Step.EndVertex >= Plan.Polyline.Num())
		{
			continue;   // unmeasured, or measured on a different sampling: say nothing, as Judge does
		}
		for (int32 K = 0; K < Count; ++K)
		{
			const int32 EdgeSample = Step.bReversed ? Count - 1 - K : K;
			const double Swept = Left[StartVertex + K] + Right[StartVertex + K];
			const double Tarmac = Edge->ClearInnerAt[EdgeSample] + Edge->ClearOuterAt[EdgeSample];
			if (Swept > Tarmac && Swept - Tarmac > WorstExcess)
			{
				WorstExcess = Swept - Tarmac;
				Verdict.Refusal = EFitRefusal::SweptOverTarmac;
				Verdict.Needed = Swept;
				Verdict.Available = Tarmac;
				Verdict.Sample = EdgeSample;
				Verdict.Along = VertexAlong[StartVertex + K];
				NamePlanStep(Plan, StepIndex, Network, Verdict);
			}
		}
	}
	return Verdict;
}
