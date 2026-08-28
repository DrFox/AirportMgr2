#include "Build/RoadNetworkSolver.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

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

		// Incident is maintained sorted by CCW bearing, which is exactly what
		// FJunctionSolver requires. Do not re-sort here.
		FJunctionInput Input;
		Input.Position = Node.Position;
		Input.ArcSegments = ArcSegments;

		// Kept index-aligned with Input.Arms even when a segment is skipped below, so
		// the write-back loop never has to assume Node.Incident lines up with Arms.
		TArray<FRoadSegmentId> ArmSegments;

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
			Input.Arms.Add(Arm);
			ArmSegments.Add(SegmentId);
		}

		if (Input.Arms.Num() == 0)
		{
			continue;
		}

		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);

		if (!Result.bValid)
		{
			++Out.FailedNodes;
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
			}
			else
			{
				Segment->TrimB = ArmResult.CutDistance;
				Segment->LeftCutB = ArmResult.LeftCut;
				Segment->RightCutB = ArmResult.RightCut;
			}
			Segment->bSolved = true;
		}

		Out.NodeResults.Add(NodeIndex, MoveTemp(Result));
		++Out.SolvedNodes;
	}

	return Out;
}
