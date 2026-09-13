#include "Tool/RoadPlacement.h"

#include "Build/RoadNetworkSolver.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** One arm meeting a corner: exactly what CornerFits needs, nothing about the network. */
	struct FArm
	{
		FVector2D Dir = FVector2D::ZeroVector;   // unit, pointing away from the corner
		double Half = 0.0;                        // road half-width
		double Length = 0.0;                      // this arm's own length, for its far-cut budget
		double FarCut = 0.0;                      // this arm's cut at its OTHER end, already spoken for
	};

	/**
	 * Whether New's corner against Existing fits with no fillet at all, and Existing still has
	 * room for its own far-end cut once New's reach is added. OutReachOnNew is raised (never
	 * lowered) to this corner's floor on New, so a caller checking New against several existing
	 * arms in a loop folds all of them into one running floor with repeated calls.
	 *
	 * This is the arithmetic that was typed three times (RoadPlacement.cpp review, 2026-09):
	 * Validate's per-arm loop at the start/end node, its segment-split variant, and
	 * NodeCornersFit's far-node loop. All three share exactly this shape - a tentative or
	 * moved arm (New) against an arm already in the graph (Existing) that must not be cut past
	 * its own length. NodeCornersFit's OTHER loop, the pairs of arms meeting at the node that
	 * moved, is NOT this shape: neither side has a far end to protect at that step (that is
	 * ReachAtFar, computed separately) and BOTH sides' reach is needed, not one - so it keeps
	 * its own inline arithmetic rather than calling this twice with the roles swapped.
	 */
	bool CornerFits(const FArm& New, const FArm& Existing, double& OutReachOnNew)
	{
		const double Theta = FMath::Acos(FMath::Clamp(FVector2D::DotProduct(New.Dir, Existing.Dir), -1.0, 1.0));
		double AlongNew = 0.0;
		double AlongExisting = 0.0;
		if (!RoadGeom::CornerReachAtZeroRadius(New.Half, Existing.Half, Theta, AlongNew, AlongExisting))
		{
			return false;
		}
		OutReachOnNew = FMath::Max(OutReachOnNew, AlongNew);
		return AlongExisting + Existing.FarCut <= Existing.Length;
	}
}

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
			return Profile != nullptr ? Profile->GetMaxHalfWidth() : 0.0;
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
			const FArm New{ NewDir, Limits.NewRoadHalfWidth, 0.0, 0.0 };
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
				const FArm Existing{ ArmDir, ArmHalfWidth(*Arm), ArmLength(*Arm),
					FRoadNetworkSolver::ZeroRadiusCut(Network, Incident, Network.GetOtherEnd(Incident, AtId)) };
				if (!CornerFits(New, Existing, OutReach))
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
				const FArm New{ Incoming, Limits.NewRoadHalfWidth, 0.0, 0.0 };
				for (int32 Side = 0; Side < 2; ++Side)
				{
					const double HalfLength = FVector2D::Distance(EndNodes[Side]->Position, To.Position);
					const FVector2D Towards = (EndNodes[Side]->Position - To.Position).GetSafeNormal();
					const FArm Existing{ Towards, Half, HalfLength,
						FRoadNetworkSolver::ZeroRadiusCut(Network, To.Segment, Ends[Side]) };
					if (!CornerFits(New, Existing, ReachAtEnd))
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

	// Named apart from the file-local FArm (CornerFits' plain corner-participant struct):
	// this one also carries the network handles and running-max fields NodeCornersFit itself
	// needs across its two passes, which CornerFits has no business knowing about.
	struct FIncidentArm
	{
		FRoadSegmentId Segment;
		FRoadNodeId Far;
		FVector2D Dir = FVector2D::ZeroVector;   // from Position towards the far node
		double Half = 0.0;
		double Length = 0.0;
		double ReachHere = 0.0;                   // the floor at the moved node, max over pairs
		double ReachAtFar = 0.0;                  // the floor at the far node, max over its other arms
	};
	TArray<FIncidentArm> Arms;
	for (const FRoadSegmentId& Incident : Live->Incident)
	{
		const FRoadSegment* Segment = Network.GetSegment(Incident);
		const FRoadNodeId FarId = Segment ? Network.GetOtherEnd(Incident, Node) : FRoadNodeId();
		const FRoadNode* Far = Segment ? Network.GetNode(FarId) : nullptr;
		if (Segment == nullptr || Far == nullptr)
		{
			continue;
		}
		FIncidentArm Arm;
		Arm.Segment = Incident;
		Arm.Far = FarId;
		// Judged where the node WOULD land, not where it is.
		Arm.Length = FVector2D::Distance(Far->Position, Position);
		Arm.Dir = Arm.Length > 0.0 ? (Far->Position - Position) / Arm.Length : FVector2D::ZeroVector;
		const URoadProfile* Profile = Network.ProfileFor(*Segment);
		Arm.Half = Profile != nullptr ? Profile->GetMaxHalfWidth() : 0.0;
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
	// against every other arm there. The other arm must still hold ITS two ends too. This is
	// exactly CornerFits' shape - Arm is the moved (New) side, Other is the unmoved (Existing)
	// one that must not be cut past its own length.
	for (FIncidentArm& Arm : Arms)
	{
		const FRoadNode* Far = Network.GetNode(Arm.Far);
		if (Far == nullptr)
		{
			continue;
		}
		const FArm Moved{ -Arm.Dir, Arm.Half, 0.0, 0.0 };
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
			const FArm Existing{ OtherDir, OtherProfile ? OtherProfile->GetMaxHalfWidth() : 0.0,
				FVector2D::Distance(OtherEnd->Position, Far->Position),
				FRoadNetworkSolver::ZeroRadiusCut(Network, FarIncident, Network.GetOtherEnd(FarIncident, Arm.Far)) };
			if (!CornerFits(Moved, Existing, Arm.ReachAtFar))
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
