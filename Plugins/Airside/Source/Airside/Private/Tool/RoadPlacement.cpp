#include "Tool/RoadPlacement.h"

#include "Build/RoadNetworkSolver.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"

ERoadPlacement RoadPlacement::Validate(const URoadNetwork& Network, FRoadNodeId From,
	const FRoadSnapResult& To, const FRoadPlacementLimits& Limits)
{
	const FRoadNode* Start = Network.GetNode(From);
	if (Start == nullptr)
	{
		return ERoadPlacement::NoStart;
	}

	// Only an existing node can already be joined to something. A Free or Segment snap
	// resolves to a node that does not exist yet, so neither can collide with an arm the
	// start node already has.
	if (To.Kind == ERoadSnapKind::Node)
	{
		if (To.Node == From)
		{
			return ERoadPlacement::SameNode;
		}

		for (const FRoadSegmentId& Incident : Start->Incident)
		{
			if (Network.GetOtherEnd(Incident, From) == To.Node)
			{
				return ERoadPlacement::AlreadyJoined;
			}
		}
	}

	const double Length = FVector2D::Distance(Start->Position, To.Position);
	if (Length < Limits.MinSegmentLength)
	{
		return ERoadPlacement::TooShort;
	}

	// A corner is measured between two tangents that both point AWAY from the node they
	// meet at, so the angle between them IS the corner - small means a hairpin, 180 means
	// straight through.
	const auto IsTooSharp = [&Limits](const FVector2D& A, const FVector2D& B)
	{
		if (A.IsNearlyZero() || B.IsNearlyZero())
		{
			return false;
		}
		const double Cosine = FMath::Clamp(FVector2D::DotProduct(A, B), -1.0, 1.0);
		return FMath::RadiansToDegrees(FMath::Acos(Cosine)) < Limits.MinTurnDegrees;
	};

	const FVector2D Outgoing = (To.Position - Start->Position) / Length;

	// The corner at the START node.
	for (const FRoadSegmentId& Incident : Start->Incident)
	{
		if (IsTooSharp(Outgoing, Network.GetOutgoingTangent(Incident, From)))
		{
			return ERoadPlacement::TooSharp;
		}
	}

	// The corner at the FAR end, which used to go unmeasured entirely.
	//
	// The previous comment here reasoned that the far end "is checked when a segment is
	// drawn FROM there". It is not: that would be a DIFFERENT segment. This one makes a
	// corner at its destination the moment it is built, and nothing else will ever look at
	// it - so a hairpin was reachable simply by drawing towards a junction instead of away
	// from one. Both ends make corners; both ends are measured.
	const FVector2D Incoming = -Outgoing;

	if (To.Kind == ERoadSnapKind::Node)
	{
		const FRoadNode* End = Network.GetNode(To.Node);
		if (End != nullptr)
		{
			for (const FRoadSegmentId& Incident : End->Incident)
			{
				if (IsTooSharp(Incoming, Network.GetOutgoingTangent(Incident, To.Node)))
				{
					return ERoadPlacement::TooSharpAtEnd;
				}
			}
		}
	}
	else if (To.Kind == ERoadSnapKind::Segment)
	{
		// Splitting hands the new node BOTH halves of the segment as arms, pointing at the
		// two endpoints. Drawing back almost along the road just split is exactly as sharp
		// as any other hairpin, and the node does not exist yet to be asked.
		const FRoadSegment* Split = Network.GetSegment(To.Segment);
		const FRoadNode* EndA = Split ? Network.GetNode(Split->A) : nullptr;
		const FRoadNode* EndB = Split ? Network.GetNode(Split->B) : nullptr;

		if (EndA != nullptr && EndB != nullptr)
		{
			const FVector2D TowardsA = (EndA->Position - To.Position).GetSafeNormal();
			const FVector2D TowardsB = (EndB->Position - To.Position).GetSafeNormal();

			if (IsTooSharp(Incoming, TowardsA) || IsTooSharp(Incoming, TowardsB))
			{
				return ERoadPlacement::TooSharpAtEnd;
			}
		}
	}

	// A Free snap lands on a node that does not exist yet and will carry only this arm, so
	// there is no corner at the far end to measure.

	// THE CORNER MUST FIT ITS ARMS. A segment holds its two cuts only if their sum is under
	// its length, and with no fillet at all each cut is at least the inner corner's reach
	// (RoadGeom::CornerReachAtZeroRadius) or a dead end's cap. If that floor is already too
	// much, the solver could only fail the node and draw nothing from it, so it is refused
	// here, where the ghost can say why. Skipped when the caller gave no half-width for the
	// new road (0): every caller from before the rule.
	if (Limits.NewRoadHalfWidth > 0.0)
	{
		const auto ArmHalfWidth = [&Network](const FRoadSegment& Segment)
		{
			const URoadProfile* Profile = Network.ProfileFor(Segment);
			return Profile != nullptr ? FMath::Max(Profile->GetHalfWidthLeft(), Profile->GetHalfWidthRight()) : 0.0;
		};
		const auto ArmLength = [&Network](const FRoadSegment& Segment)
		{
			const FRoadNode* A = Network.GetNode(Segment.A);
			const FRoadNode* B = Network.GetNode(Segment.B);
			return (A != nullptr && B != nullptr) ? FVector2D::Distance(A->Position, B->Position) : 0.0;
		};
		// The new road's floor at one of its ends, given the existing arms there, and a
		// refusal if any of those arms can no longer hold its own two ends.
		const auto NewReachAt = [&](const FVector2D& NewDir, const FRoadNode& At, FRoadNodeId AtId, double& OutReach)
		{
			OutReach = 0.0;   // a dead end has no floor: its cap shrinks to fit
			for (const FRoadSegmentId& Incident : At.Incident)
			{
				const FRoadSegment* Arm = Network.GetSegment(Incident);
				if (Arm == nullptr)
				{
					continue;
				}
				const FVector2D ArmDir = Network.GetOutgoingTangent(Incident, AtId);
				if (ArmDir.IsNearlyZero())
				{
					continue;
				}
				const double Theta = FMath::Acos(FMath::Clamp(FVector2D::DotProduct(NewDir, ArmDir), -1.0, 1.0));
				double AlongNew = 0.0;
				double AlongArm = 0.0;
				if (!RoadGeom::CornerReachAtZeroRadius(Limits.NewRoadHalfWidth, ArmHalfWidth(*Arm), Theta, AlongNew, AlongArm))
				{
					return false;
				}
				OutReach = FMath::Max(OutReach, AlongNew);
				const double ArmFar = FRoadNetworkSolver::ZeroRadiusCut(Network, Incident, Network.GetOtherEnd(Incident, AtId));
				if (AlongArm + ArmFar > ArmLength(*Arm))
				{
					return false;
				}
			}
			return true;
		};

		double ReachAtStart = 0.0;
		if (!NewReachAt(Outgoing, *Start, From, ReachAtStart))
		{
			return ERoadPlacement::TooShortForCorner;
		}

		double ReachAtEnd = 0.0;
		if (To.Kind == ERoadSnapKind::Node)
		{
			const FRoadNode* End = Network.GetNode(To.Node);
			if (End != nullptr && !NewReachAt(Incoming, *End, To.Node, ReachAtEnd))
			{
				return ERoadPlacement::TooShortForCorner;
			}
		}
		else if (To.Kind == ERoadSnapKind::Segment)
		{
			// The split makes two arms at the new node, each as long as its own remainder
			// and each keeping the far end the whole segment had.
			const FRoadSegment* Split = Network.GetSegment(To.Segment);
			const FRoadNode* EndA = Split ? Network.GetNode(Split->A) : nullptr;
			const FRoadNode* EndB = Split ? Network.GetNode(Split->B) : nullptr;
			if (Split != nullptr && EndA != nullptr && EndB != nullptr)
			{
				const double Half = ArmHalfWidth(*Split);
				const FRoadNodeId Ends[2] = { Split->A, Split->B };
				const FRoadNode* EndNodes[2] = { EndA, EndB };
				for (int32 Side = 0; Side < 2; ++Side)
				{
					const double HalfLength = FVector2D::Distance(EndNodes[Side]->Position, To.Position);
					const FVector2D Towards = (EndNodes[Side]->Position - To.Position).GetSafeNormal();
					const double Theta = FMath::Acos(FMath::Clamp(FVector2D::DotProduct(Incoming, Towards), -1.0, 1.0));
					double AlongNew = 0.0;
					double AlongHalf = 0.0;
					if (!RoadGeom::CornerReachAtZeroRadius(Limits.NewRoadHalfWidth, Half, Theta, AlongNew, AlongHalf))
					{
						return ERoadPlacement::TooShortForCorner;
					}
					ReachAtEnd = FMath::Max(ReachAtEnd, AlongNew);
					if (AlongHalf + FRoadNetworkSolver::ZeroRadiusCut(Network, To.Segment, Ends[Side]) > HalfLength)
					{
						return ERoadPlacement::TooShortForCorner;
					}
				}
			}
		}

		if (ReachAtStart + ReachAtEnd > Length)
		{
			return ERoadPlacement::TooShortForCorner;
		}
	}

	return ERoadPlacement::Valid;
}

const TCHAR* RoadPlacement::Describe(ERoadPlacement Result)
{
	switch (Result)
	{
	case ERoadPlacement::Valid:         return TEXT("");
	case ERoadPlacement::NoStart:       return TEXT("no start node");
	case ERoadPlacement::SameNode:      return TEXT("same node");
	case ERoadPlacement::AlreadyJoined: return TEXT("already joined");
	case ERoadPlacement::TooShort:      return TEXT("too short");
	case ERoadPlacement::TooSharp:      return TEXT("turn too sharp");
	case ERoadPlacement::TooSharpAtEnd: return TEXT("turn too sharp at the far end");
	case ERoadPlacement::TooShortForCorner: return TEXT("too short to hold the corner");
	default:                            return TEXT("refused");
	}
}

bool RoadPlacement::NodeCornersFit(const URoadNetwork& Network, FRoadNodeId Node, const FVector2D& Position)
{
	const FRoadNode* Live = Network.GetNode(Node);
	if (Live == nullptr)
	{
		return true;
	}

	struct FArm
	{
		FRoadSegmentId Segment;
		FRoadNodeId Far;
		FVector2D Dir = FVector2D::ZeroVector;   // from Position towards the far node
		double Half = 0.0;
		double Length = 0.0;
		double ReachHere = 0.0;                   // the floor at the moved node, max over pairs
		double ReachAtFar = 0.0;                  // the floor at the far node, max over its other arms
	};
	TArray<FArm> Arms;
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		const FRoadSegment* Segment = Network.GetSegment(Incident);
		const FRoadNodeId FarId = Segment ? Network.GetOtherEnd(Incident, Node) : FRoadNodeId();
		const FRoadNode* Far = Segment ? Network.GetNode(FarId) : nullptr;
		if (Segment == nullptr || Far == nullptr)
		{
			continue;
		}
		FArm Arm;
		Arm.Segment = Incident;
		Arm.Far = FarId;
		// Judged where the node WOULD land, not where it is.
		Arm.Length = FVector2D::Distance(Far->Position, Position);
		Arm.Dir = Arm.Length > 0.0 ? (Far->Position - Position) / Arm.Length : FVector2D::ZeroVector;
		const URoadProfile* Profile = Network.ProfileFor(*Segment);
		Arm.Half = Profile != nullptr ? FMath::Max(Profile->GetHalfWidthLeft(), Profile->GetHalfWidthRight()) : 0.0;
		Arm.ReachHere = 0.0;    // a dead end has no floor; corners raise these below
		Arm.ReachAtFar = 0.0;
		Arms.Add(Arm);
	}

	// Corners at the moved node: every pair. Adjacent-in-bearing pairs are the tight ones,
	// and all pairs cost nothing at these counts while never depending on a sort.
	for (int32 I = 0; I < Arms.Num(); ++I)
	{
		for (int32 J = I + 1; J < Arms.Num(); ++J)
		{
			if (Arms[I].Dir.IsNearlyZero() || Arms[J].Dir.IsNearlyZero())
			{
				return false;
			}
			const double Theta = FMath::Acos(FMath::Clamp(FVector2D::DotProduct(Arms[I].Dir, Arms[J].Dir), -1.0, 1.0));
			double AlongI = 0.0;
			double AlongJ = 0.0;
			if (!RoadGeom::CornerReachAtZeroRadius(Arms[I].Half, Arms[J].Half, Theta, AlongI, AlongJ))
			{
				return false;
			}
			Arms[I].ReachHere = FMath::Max(Arms[I].ReachHere, AlongI);
			Arms[J].ReachHere = FMath::Max(Arms[J].ReachHere, AlongJ);
		}
	}

	// Corners at each far node that the moved arm takes part in: the arm's new direction
	// against every other arm there. The other arm must still hold ITS two ends too.
	for (FArm& Arm : Arms)
	{
		const FRoadNode* Far = Network.GetNode(Arm.Far);
		if (Far == nullptr)
		{
			continue;
		}
		const FVector2D MovedDirAtFar = -Arm.Dir;
		for (const FRoadSegmentId& FarIncident : Far->Incident)
		{
			if (FarIncident == Arm.Segment)
			{
				continue;
			}
			const FRoadSegment* Other = Network.GetSegment(FarIncident);
			const FRoadNode* OtherEnd = Other ? Network.GetNode(Network.GetOtherEnd(FarIncident, Arm.Far)) : nullptr;
			if (Other == nullptr || OtherEnd == nullptr)
			{
				continue;
			}
			const FVector2D OtherDir = (OtherEnd->Position - Far->Position).GetSafeNormal();
			const URoadProfile* OtherProfile = Network.ProfileFor(*Other);
			const double OtherHalf = OtherProfile ? FMath::Max(OtherProfile->GetHalfWidthLeft(), OtherProfile->GetHalfWidthRight()) : 0.0;
			const double Theta = FMath::Acos(FMath::Clamp(FVector2D::DotProduct(MovedDirAtFar, OtherDir), -1.0, 1.0));
			double AlongMoved = 0.0;
			double AlongOther = 0.0;
			if (!RoadGeom::CornerReachAtZeroRadius(Arm.Half, OtherHalf, Theta, AlongMoved, AlongOther))
			{
				return false;
			}
			Arm.ReachAtFar = FMath::Max(Arm.ReachAtFar, AlongMoved);
			const double OtherLength = FVector2D::Distance(OtherEnd->Position, Far->Position);
			const double OtherFar = FRoadNetworkSolver::ZeroRadiusCut(Network, FarIncident, Network.GetOtherEnd(FarIncident, Arm.Far));
			if (AlongOther + OtherFar > OtherLength)
			{
				return false;
			}
		}
		if (Arm.ReachHere + Arm.ReachAtFar > Arm.Length)
		{
			return false;
		}
	}
	return true;
}
