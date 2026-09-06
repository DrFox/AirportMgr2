#include "Build/RoadNetworkSolver.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadSolve, Log, All);

namespace
{
	/** Straight-line distance between a segment's endpoints. */
	double SegmentChordLength(const URoadNetwork& Network, const FRoadSegment& Segment)
	{
		const FRoadNode* A = Network.GetNode(Segment.A);
		const FRoadNode* B = Network.GetNode(Segment.B);
		if (A == nullptr || B == nullptr)
		{
			return 0.0;
		}
		return FVector2D::Distance(A->Position, B->Position);
	}
}

namespace
{
	/**
	 * Each end's share of a segment's slack (its length less both ends' floors). UNDER a
	 * half, so two ends that each take their whole share still leave a ribbon between them:
	 * at exactly a half the cut centres would touch and the ribbon would have no length.
	 */
	constexpr double SlackShare = 0.45;

	/**
	 * One arm per live incident segment, in incidence order, with the profile's own widths
	 * and preferred radius. The ONE place a node's junction input is assembled, so the solve
	 * proper and the zero-radius floor query cannot describe the same node differently.
	 */
	bool BuildNodeInput(const URoadNetwork& Network, int32 NodeIndex, int32 ArcSegments,
		FJunctionInput& OutInput, TArray<FRoadSegmentId>& OutArmSegments)
	{
		const TArray<FRoadNode>& Nodes = Network.GetNodes();
		if (!Nodes.IsValidIndex(NodeIndex))
		{
			return false;
		}
		const FRoadNode& Node = Nodes[NodeIndex];
		if (!Node.bAlive || Node.Incident.Num() == 0)
		{
			return false;
		}
		FRoadNodeId NodeId;
		NodeId.Index = NodeIndex;
		NodeId.Generation = Node.Generation;

		OutInput.Position = Node.Position;
		OutInput.ArcSegments = ArcSegments;
		for (const FRoadSegmentId SegmentId : Node.Incident)
		{
			const FRoadSegment* Segment = Network.GetSegment(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}
			const URoadProfile* Profile = Network.ProfileFor(*Segment);

			FJunctionArm Arm;
			Arm.Tangent = Network.GetOutgoingTangent(SegmentId, NodeId);
			Arm.HalfWidthLeft  = Profile ? Profile->GetHalfWidthLeft()  : 0.0;
			Arm.HalfWidthRight = Profile ? Profile->GetHalfWidthRight() : 0.0;
			Arm.FilletRadius   = Profile ? Profile->PreferredFilletRadius : 0.0;
			// A runway passes through: never trimmed, never filleted. See FJunctionArm.
			Arm.bContinuous    = Profile ? Profile->bContinuousThroughJunctions : false;
			Arm.UserData = SegmentId.Index;
			OutInput.Arms.Add(Arm);
			OutArmSegments.Add(SegmentId);
		}
		return OutInput.Arms.Num() > 0;
	}
}

double FRoadNetworkSolver::ZeroRadiusCut(const URoadNetwork& Network, FRoadSegmentId Segment, FRoadNodeId AtNode)
{
	FJunctionInput Input;
	TArray<FRoadSegmentId> ArmSegments;
	if (!BuildNodeInput(Network, AtNode.Index, 4, Input, ArmSegments) || Input.Arms.Num() == 1)
	{
		return 0.0;   // no node, or a dead end - whose cap shrinks rather than holding a floor
	}
	for (FJunctionArm& Arm : Input.Arms)
	{
		Arm.FilletRadius = 0.0;
	}
	const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
	if (!Result.bValid)
	{
		return 0.0;
	}
	const int32 ArmIndex = ArmSegments.IndexOfByKey(Segment);
	return Result.Arms.IsValidIndex(ArmIndex) ? Result.Arms[ArmIndex].CutDistance : 0.0;
}

bool FRoadNetworkSolver::SolveNodeCuts(const URoadNetwork& Network, int32 NodeIndex,
	int32 ArcSegments, FRoadNodeCuts& Out)
{
	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return false;
	}

	const FRoadNode& Node = Nodes[NodeIndex];
	if (!Node.bAlive || Node.Incident.Num() == 0)
	{
		return false;
	}

	FRoadNodeId NodeId;
	NodeId.Index = NodeIndex;
	NodeId.Generation = Node.Generation;

	// Incident is maintained sorted by CCW bearing, which is exactly what
	// FJunctionSolver requires. Do not re-sort here.
	Out.Input = FJunctionInput();
	Out.ArmSegments.Reset();
	if (!BuildNodeInput(Network, NodeIndex, ArcSegments, Out.Input, Out.ArmSegments))
	{
		return false;
	}

	TArray<double> PreferredRadii;
	for (const FJunctionArm& Arm : Out.Input.Arms)
	{
		PreferredRadii.Add(Arm.FilletRadius);
	}

	// THE ALLOWANCE IS SET BY BOTH ENDS. A segment holds its two cuts only if their sum is
	// under its length. Each end is solved on its own and cannot see the other's fillet, so
	// each is given its own zero-radius floor (the inner corner, or a dead end's cap) plus
	// HALF the slack the segment has left once both floors are paid for. Two ends that each
	// stay inside that can never cross. A flat fraction of the length was tried first and
	// refused honest roads: a 2300-wide L bend with 2500 arms needs 1150 at the corner and
	// 1150 at the cap, which fits, and a 45% cap said it did not.
	FJunctionInput ZeroInput = Out.Input;
	for (FJunctionArm& Arm : ZeroInput.Arms)
	{
		Arm.FilletRadius = 0.0;
	}
	const FJunctionResult ZeroHere = FJunctionSolver::SolveCuts(ZeroInput);
	if (!ZeroHere.bValid)
	{
		// Nothing fits at any radius; the fit below reports the same and fails the node.
		Out.Result = ZeroHere;
		return true;
	}

	TArray<double> ArmAllowance;
	for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
	{
		const FRoadSegmentId SegmentId = Out.ArmSegments[ArmIndex];
		const FRoadSegment* Segment = Network.GetSegment(SegmentId);
		const double Length = Segment ? SegmentChordLength(Network, *Segment) : 0.0;
		// A dead end's floor is ZERO: its cap has no corner to respect and is drawn shorter
		// when the segment is short (FJunctionArm::MaxCutDistance). Only a corner has a floor.
		const double MinHere = Out.Input.Arms.Num() == 1 ? 0.0 : ZeroHere.Arms[ArmIndex].CutDistance;
		const double MinFar = Segment ? ZeroRadiusCut(Network, SegmentId, Network.GetOtherEnd(SegmentId, NodeId)) : 0.0;
		const double Slack = Length - MinHere - MinFar;
		if (Slack < 0.0)
		{
			// Even with no fillet at either end the two cuts cross: the segment is shorter
			// than its own width and corners need. The node FAILS - its segments are then
			// not drawn from this end rather than drawn folded and facing down (the road
			// that vanished, 2026-09-06). RoadPlacement refuses this before it exists, so
			// this line means a load, a heal or a drag got past that rule.
			UE_LOG(LogRoadSolve, Warning,
				TEXT("Node %d: segment %d is %.0f uu long but its two corners need %.0f + %.0f even ")
				TEXT("with no fillet. Draw it longer, widen the angle, or use a narrower profile."),
				NodeIndex, SegmentId.Index, Length, MinHere, MinFar);
			Out.Result.bValid = false;
			return true;
		}
		ArmAllowance.Add(MinHere + SlackShare * Slack);
	}
	for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
	{
		Out.Input.Arms[ArmIndex].MaxCutDistance = ArmAllowance[ArmIndex];
	}

	// FIT THE RADII EXACTLY, in at most three solves. A cut distance is Reach + R * cot(Theta/2):
	// a constant part (where the inner edges meet) plus a part proportional to the radius.
	// The old loop divided the radii by the overshoot ratio and hoped, which is right only
	// when the constant part is zero; for a tight corner it ran out of attempts still
	// overshooting and said nothing, and at radius zero it warned and then carried on - both
	// emitted a folded ribbon, which is the road that vanished (2026-09-06). Now: solve at
	// the preferred radii; if any arm overshoots, solve at zero radius to learn the constant
	// part; if even that overshoots, the node FAILS - nothing this solver can do - otherwise
	// interpolate the scale that lands every arm inside its allowance and solve once more.
	auto SolveAtScale = [&](double Scale)
	{
		for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
		{
			Out.Input.Arms[ArmIndex].FilletRadius = PreferredRadii[ArmIndex] * Scale;
		}
		Out.Result = FJunctionSolver::SolveCuts(Out.Input);
	};
	auto WorstOvershoot = [&]()
	{
		double Worst = 0.0;
		for (int32 ArmIndex = 0; ArmIndex < Out.Result.Arms.Num(); ++ArmIndex)
		{
			if (ArmAllowance[ArmIndex] > 0.0)
			{
				Worst = FMath::Max(Worst, Out.Result.Arms[ArmIndex].CutDistance / ArmAllowance[ArmIndex]);
			}
		}
		return Worst;
	};

	SolveAtScale(1.0);
	if (!Out.Result.bValid || WorstOvershoot() <= 1.0)
	{
		return true;
	}
	TArray<double> CutAtFull;
	for (const FJunctionArmResult& Arm : Out.Result.Arms) { CutAtFull.Add(Arm.CutDistance); }

	SolveAtScale(0.0);
	if (!Out.Result.bValid)
	{
		return true;
	}
	if (WorstOvershoot() > 1.0)
	{
		// Unreachable by construction - every allowance is at least this node's own
		// zero-radius cut - and kept as the last line of defence, saying so.
		UE_LOG(LogRoadSolve, Warning,
			TEXT("Node %d: zero-radius cut overshoots its allowance by %.0f%%; node failed"),
			NodeIndex, (WorstOvershoot() - 1.0) * 100.0);
		Out.Result.bValid = false;
		return true;
	}

	// Cut(s) is convex piecewise-linear in the scale (each arm takes the max over its two
	// corners, each linear), so the chord between Cut(0) and Cut(1) bounds it from above and
	// the chord's crossing of the allowance is a scale that fits, with a hair to spare.
	double Scale = 1.0;
	for (int32 ArmIndex = 0; ArmIndex < Out.Result.Arms.Num(); ++ArmIndex)
	{
		const double Allowance = ArmAllowance[ArmIndex];
		const double CutZero = Out.Result.Arms[ArmIndex].CutDistance;
		const double Rise = CutAtFull[ArmIndex] - CutZero;
		if (Allowance > 0.0 && Rise > 1e-9 && CutAtFull[ArmIndex] > Allowance)
		{
			Scale = FMath::Min(Scale, (Allowance - CutZero) / Rise);
		}
	}
	SolveAtScale(FMath::Clamp(Scale * 0.999, 0.0, 1.0));
	if (Out.Result.bValid && WorstOvershoot() > 1.0 + 1e-6)
	{
		// The bound above should make this unreachable; if it is ever reached, say so rather
		// than let a folded ribbon out.
		UE_LOG(LogRoadSolve, Warning, TEXT("Node %d: radius fit did not converge (overshoot %.3f); node failed"),
			NodeIndex, WorstOvershoot());
		Out.Result.bValid = false;
	}

	return true;
}

bool FRoadNetworkSolver::NodeClaims(const URoadNetwork& Network, FRoadNodeId Node, const FVector2D& Point, double Factor)
{
	if (Factor <= 0.0)
	{
		return false;
	}
	const FRoadNode* Live = Network.GetNode(Node);
	if (Live == nullptr)
	{
		return false;
	}

	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, Node.Index, 4, Cuts) || !Cuts.Result.bValid)
	{
		return false;
	}
	FJunctionSolver::SolveBoundary(Cuts.Input, Cuts.Result);

	// The rim is every boundary point but the trailing apex; fewer than three is a dead
	// end (two cut vertices) with no polygon of its own.
	const int32 RimCount = Cuts.Result.Boundary.Num() - 1;
	if (RimCount < 3)
	{
		double HalfWidth = 0.0;
		for (const FJunctionArm& Arm : Cuts.Input.Arms)
		{
			HalfWidth = FMath::Max(HalfWidth, FMath::Max(Arm.HalfWidthLeft, Arm.HalfWidthRight));
		}
		const double Reach = HalfWidth * Factor;
		return FVector2D::DistSquared(Live->Position, Point) <= Reach * Reach;
	}

	TArray<FVector2D> Rim;
	Rim.Reserve(RimCount);
	for (int32 Slot = 0; Slot < RimCount; ++Slot)
	{
		Rim.Add(Live->Position + (Cuts.Result.Boundary[Slot] - Live->Position) * Factor);
	}
	if (RoadGeom::PointInPolygon(Rim, Point))
	{
		return true;
	}

	// The rim ITSELF is pavement: the arm's derived guideline node sits exactly on the cut
	// line, and a point-in-polygon test is undefined on its own edge. One uu of tolerance
	// is far below anything a cursor can express and far above double noise.
	constexpr double EdgeTolerance = 1.0;
	for (int32 Slot = 0; Slot < RimCount; ++Slot)
	{
		const FVector2D& A = Rim[Slot];
		const FVector2D& B = Rim[(Slot + 1) % RimCount];
		const FVector2D AB = B - A;
		const double LengthSquared = AB.SizeSquared();
		const double T = LengthSquared > 0.0 ? FMath::Clamp(FVector2D::DotProduct(Point - A, AB) / LengthSquared, 0.0, 1.0) : 0.0;
		if (FVector2D::Distance(Point, A + AB * T) <= EdgeTolerance)
		{
			return true;
		}
	}
	return false;
}

double FRoadNetworkSolver::ArmCutDistance(const URoadNetwork& Network, FRoadSegmentId Segment, FRoadNodeId AtNode)
{
	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, AtNode.Index, 4, Cuts) || !Cuts.Result.bValid)
	{
		return 0.0;
	}
	const int32 ArmIndex = Cuts.ArmSegments.IndexOfByKey(Segment);
	return Cuts.Result.Arms.IsValidIndex(ArmIndex) ? Cuts.Result.Arms[ArmIndex].CutDistance : 0.0;
}

double FRoadNetworkSolver::NodeReach(const URoadNetwork& Network, FRoadNodeId Node,
	int32 ArcSegments)
{
	FRoadNodeCuts Cuts;
	if (!SolveNodeCuts(Network, Node.Index, ArcSegments, Cuts) || !Cuts.Result.bValid)
	{
		// No arms, or a solve that declined. Either way this node paves nothing, so it
		// claims nothing - a bare node must not grow a snap radius around itself.
		return 0.0;
	}

	double Reach = 0.0;
	for (int32 ArmIndex = 0; ArmIndex < Cuts.Result.Arms.Num(); ++ArmIndex)
	{
		if (!Cuts.Input.Arms.IsValidIndex(ArmIndex))
		{
			continue;
		}

		const FJunctionArm& Arm = Cuts.Input.Arms[ArmIndex];
		const double HalfWidth = FMath::Max(Arm.HalfWidthLeft, Arm.HalfWidthRight);
		Reach = FMath::Max(Reach, Cuts.Result.Arms[ArmIndex].CutDistance + HalfWidth);
	}

	return Reach;
}

FRoadSolveResult FRoadNetworkSolver::SolveAll(URoadNetwork& Network, int32 ArcSegments)
{
	FRoadSolveResult Out;

	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const FRoadNode& Node = Nodes[NodeIndex];
		if (!Node.bAlive || Node.Incident.Num() == 0)
		{
			continue;
		}

		FRoadNodeId NodeId;
		NodeId.Index = NodeIndex;
		NodeId.Generation = Node.Generation;

		// The arm gathering, the skip rule and the fillet clamp all live in SolveNodeCuts,
		// so a tool asking how far this junction reaches gets the answer from the same
		// code that decides where the pavement actually stops.
		FRoadNodeCuts Cuts;
		if (!SolveNodeCuts(Network, NodeIndex, ArcSegments, Cuts))
		{
			continue;
		}

		FJunctionInput& Input = Cuts.Input;
		FJunctionResult& Result = Cuts.Result;
		const TArray<FRoadSegmentId>& ArmSegments = Cuts.ArmSegments;

		FJunctionSolver::SolveBoundary(Input, Result);

		if (!Result.bValid)
		{
			++Out.FailedNodes;

			// A failed solve must not leave a previous solve's vertices stranded looking
			// valid. Clear only the end this node owns on every incident segment - the
			// other end (at the segment's other node) is untouched and keeps its own flag.
			for (const FRoadSegmentId SegmentId : Node.Incident)
			{
				FRoadSegment* Segment = Network.GetSegmentMutable(SegmentId);
				if (Segment == nullptr)
				{
					continue;
				}
				if (Segment->A == NodeId)
				{
					Segment->bSolvedA = false;
				}
				else
				{
					Segment->bSolvedB = false;
				}
			}
			continue;
		}

		// Write the solve back into the model. ArmSegments is index-aligned with
		// Result.Arms (both built in lockstep above), so ArmSegments[ArmIndex] is the
		// segment each arm belongs to, regardless of anything skipped while building Arms.
		for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
		{
			const FRoadSegmentId SegmentId = ArmSegments[ArmIndex];
			FRoadSegment* Segment = Network.GetSegmentMutable(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}

			const FJunctionArmResult& ArmResult = Result.Arms[ArmIndex];
			const bool bIsEndA = (Segment->A == NodeId);

			if (bIsEndA)
			{
				Segment->TrimA = ArmResult.CutDistance;
				Segment->LeftCutA = ArmResult.LeftCut;
				Segment->RightCutA = ArmResult.RightCut;
				Segment->bSolvedA = true;
			}
			else
			{
				Segment->TrimB = ArmResult.CutDistance;
				Segment->LeftCutB = ArmResult.LeftCut;
				Segment->RightCutB = ArmResult.RightCut;
				Segment->bSolvedB = true;
			}
		}

		// Copied BEFORE Result is moved from, and keyed on the same NodeIndex.
		Out.NodeArmSegments.Add(NodeIndex, ArmSegments);
		Out.NodeResults.Add(NodeIndex, MoveTemp(Result));
		++Out.SolvedNodes;
	}

	return Out;
}
