#include "Build/RoadNetworkSolver.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadSolve, Log, All);

namespace
{
	/**
	 * Share of a segment's length one end may consume. Both ends are solved
	 * independently and neither can see the other's radius, so capping each at a little
	 * under half is what guarantees the two cuts never cross.
	 */
	constexpr double MaxCutFraction = 0.45;

	/** How many times to shrink a node's radii before giving up. */
	constexpr int32 MaxClampAttempts = 6;

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
	Out.Input.Position = Node.Position;
	Out.Input.ArcSegments = ArcSegments;
	Out.ArmSegments.Reset();

	// Preferred radii, kept aside so each clamping attempt scales the profile's own
	// value rather than compounding the previous attempt's reduction.
	TArray<double> PreferredRadii;

	/** Longest cut this arm may take before its two cut lines would cross. */
	TArray<double> ArmAllowance;

	for (const FRoadSegmentId SegmentId : Node.Incident)
	{
		const FRoadSegment* Segment = Network.GetSegment(SegmentId);
		if (Segment == nullptr)
		{
			continue;
		}

		const URoadProfile* Profile = Segment->Profile;

		FJunctionArm Arm;
		Arm.Tangent = Network.GetOutgoingTangent(SegmentId, NodeId);
		Arm.HalfWidthLeft  = Profile ? Profile->GetHalfWidthLeft()  : 0.0;
		Arm.HalfWidthRight = Profile ? Profile->GetHalfWidthRight() : 0.0;
		Arm.FilletRadius   = Profile ? Profile->PreferredFilletRadius : 0.0;
		Arm.UserData = SegmentId.Index;
		Out.Input.Arms.Add(Arm);
		Out.ArmSegments.Add(SegmentId);
		PreferredRadii.Add(Arm.FilletRadius);
		ArmAllowance.Add(MaxCutFraction * SegmentChordLength(Network, *Segment));
	}

	if (Out.Input.Arms.Num() == 0)
	{
		return false;
	}

	// Clamp the fillet radius to what the incident segments can physically absorb.
	//
	// Design spec section 5 step 4 is explicit that the solver does NOT do this: the
	// clamp needs a segment length the solver deliberately does not know, so "a caller
	// that must fit a finite segment clamps the radius before calling", and section
	// 4.3 has the radius "further clamped by what geometrically fits". This is that
	// caller. Left unclamped, a corner's cut of reach + |R / tan(Theta/2)| can exceed
	// the segment it is cutting, the two cut lines cross, and the ribbon renders
	// inside-out - a black, back-facing surface rather than a road.
	//
	// Cut distance is affine in the radius, so dividing by the overshoot converges in
	// a couple of passes; the loop re-measures rather than trusting that arithmetic.
	double Scale = 1.0;

	for (int32 Attempt = 0; Attempt < MaxClampAttempts; ++Attempt)
	{
		for (int32 ArmIndex = 0; ArmIndex < Out.Input.Arms.Num(); ++ArmIndex)
		{
			Out.Input.Arms[ArmIndex].FilletRadius = PreferredRadii[ArmIndex] * Scale;
		}

		Out.Result = FJunctionSolver::SolveCuts(Out.Input);
		if (!Out.Result.bValid)
		{
			break;
		}

		double WorstOvershoot = 1.0;
		for (int32 ArmIndex = 0; ArmIndex < Out.Result.Arms.Num(); ++ArmIndex)
		{
			const double Allowance = ArmAllowance[ArmIndex];
			if (Allowance > 0.0)
			{
				WorstOvershoot = FMath::Max(
					WorstOvershoot, Out.Result.Arms[ArmIndex].CutDistance / Allowance);
			}
		}

		if (WorstOvershoot <= 1.0)
		{
			break;
		}

		if (Scale <= 0.0)
		{
			// Already at a zero radius and still overshooting, so the fillet is not
			// what does not fit - the segment is shorter than its own width needs.
			// Nothing this solver can do; say so rather than emitting a folded ribbon.
			UE_LOG(LogRoadSolve, Warning,
				TEXT("Node %d: a segment is too short for its road width - the cut still "
					 "overshoots by %.0f%% at a zero fillet radius. Draw it longer, or use a "
					 "narrower profile."),
				NodeIndex, (WorstOvershoot - 1.0) * 100.0);
			break;
		}

		Scale = FMath::Max(0.0, Scale / WorstOvershoot);
	}

	return true;
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
