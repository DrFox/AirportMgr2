#include "Build/ServiceLoopBuild.h"

#include "AirsideLog.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * Within this of an endpoint, join the endpoint rather than splitting off a stub.
	 *
	 * The same figure FAnchorLink welds at, and for the same reason: a split that leaves a
	 * centimetre of edge behind is a node nothing can usefully be at.
	 */
	constexpr double LaneWeldTolerance = 10.0;

	/**
	 * One lane side, or one anchor spur. Both carry the owner, and neither is ever a target.
	 *
	 * One function rather than two because the two differ in nothing but their endpoints -
	 * and a spur that disagreed with the lane about which class may use it would be a lane a
	 * truck could reach and not leave.
	 */
	FGuidelineEdge MakeServiceEdge(FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D& PositionA, const FVector2D& PositionB, FEntityInstanceId Owner,
		bool bSpur)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;

		// Straight, spelled the way the builder spells it: the control ON the midpoint, which
		// is what GuidelineGeom::IsStraight tests for and what lets Sample short-circuit to
		// two points.
		Edge.Control = (PositionA + PositionB) * 0.5;

		// GroundVehicle, plus Emergency as every derived guideline carries. NOT Aircraft: an
		// aeroplane routed round the lane would be driving round itself.
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = FServiceLoopBuild::LaneWidth;

		// 0 is UNLIMITED - see FProfileGuideline::MaxWingspan. A span limit on a line no wing
		// uses could never bind, and the class has already refused aircraft.
		Edge.MaxWingspan = 0.0;
		Edge.bDerived = true;
		Edge.ServiceLoopOwner = Owner;

		// WHICH OF THE TWO THIS IS, said rather than worked out from the endpoints later -
		// see FGuidelineEdge::bServiceSpur for what reading it off the endpoints cost. The
		// two splits below copy from Original, so a spur cut in half stays a spur.
		Edge.bServiceSpur = bSpur;
		return Edge;
	}
}

FServiceLoopBuild::FResult FServiceLoopBuild::Build(URoadNetwork& Network)
{
	FResult Result;

	// WHAT IS ALREADY THERE, gathered before anything is added so the idempotence check
	// cannot see this pass's own output. An owner whose entity has since died leaves its
	// edges here until the next sweep: they are still excluded as targets, which is right,
	// and are never adopted by anything, which is also right.
	{
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || !Edge.ServiceLoopOwner.IsSet())
			{
				continue;
			}

			FGuidelineEdgeId Id;
			Id.Index = Index;
			Id.Generation = Edge.Generation;

			Result.Lanes.FindOrAdd(Edge.ServiceLoopOwner).Add(Id);
			Result.Nodes.Add(Edge.A);
			Result.Nodes.Add(Edge.B);
		}
	}

	// By index, like FAnchorLink::Build: nothing here adds or removes an ENTITY, so holding
	// this reference across the mutations below is safe, and the handle still has to be built
	// by hand from the slot - the array elements have no stable handle of their own.
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr
			|| Instance.Definition->ServiceLoop.Num() < 3)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		if (Result.Lanes.Contains(EntityId))
		{
			// Already laid, by a pass whose output nothing swept. See the header.
			continue;
		}

		const TArray<FVector2D>& Local = Instance.Definition->ServiceLoop;
		const double Cosine = FMath::Cos(Instance.Heading);
		const double Sine = FMath::Sin(Instance.Heading);
		auto ToWorld = [&Instance, Cosine, Sine](const FVector2D& Point)
		{
			return Instance.Position
				+ FVector2D(Point.X * Cosine - Point.Y * Sine, Point.X * Sine + Point.Y * Cosine);
		};

		TArray<FGuidelineNodeId> Corners;
		Corners.Reserve(Local.Num());
		for (const FVector2D& Point : Local)
		{
			Corners.Add(Network.AddGuidelineNode(ToWorld(Point), /*bDerived=*/true));
		}

		TArray<FGuidelineEdgeId>& Lane = Result.Lanes.FindOrAdd(EntityId);
		for (int32 At = 0; At < Corners.Num(); ++At)
		{
			// CLOSED IMPLICITLY: the last corner joins the first, and the definition's array
			// does not repeat it - see UEntityDefinition::ServiceLoop for why storing the
			// repeat would be a value that has to agree with another value beside it.
			const FGuidelineNodeId A = Corners[At];
			const FGuidelineNodeId B = Corners[(At + 1) % Corners.Num()];

			// Read fresh each time: adding a node or an edge can reallocate the array a
			// pointer taken before it was pointing into.
			const FVector2D PositionA = Network.GetGuidelineNode(A)->Position;
			const FVector2D PositionB = Network.GetGuidelineNode(B)->Position;

			Lane.Add(Network.AddGuidelineEdge(MakeServiceEdge(A, B, PositionA, PositionB, EntityId, /*bSpur=*/false)));
			Result.Nodes.Add(A);
			Result.Nodes.Add(B);
		}
		++Result.LoopsBuilt;

		// SPURS. Each service anchor to its nearest point on the lane. On a Code C stand every
		// one is under fourteen metres and none crosses the aeroplane - measured, on the
		// definition, by Airside.Entities.ServiceLoopClearsTheAircraft rather than trusted
		// here.
		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			if (TraversalForRole(Resolved.Role) == ETraversalClass::Aircraft)
			{
				// An aircraft anchor is not something a truck drives to. There are none on a
				// Code C stand today, and a definition that grew one would want a painted
				// lead-in of its own, not a spur onto the service lane.
				continue;
			}

			const FGuidelineNode* AnchorNode = Network.GetGuidelineNode(Resolved.Node);
			if (AnchorNode == nullptr || AnchorNode->Incident.Num() > 0)
			{
				// Already joined - by hand, or by a pass that survived. A second spur would
				// leave two lines into one painted box.
				continue;
			}
			const FVector2D At = AnchorNode->Position;

			// Across the lane's CURRENT edges, re-read every time: an earlier spur may have
			// split the very side this one is about to meet, and it must see the halves
			// rather than the edge that is gone.
			FGuidelineEdgeId BestEdge;
			double BestDistance = TNumericLimits<double>::Max();
			double BestParam = 0.0;
			FVector2D BestPoint = FVector2D::ZeroVector;

			for (const FGuidelineEdgeId& EdgeId : Lane)
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
				const FGuidelineNode* EndA = Edge != nullptr ? Network.GetGuidelineNode(Edge->A) : nullptr;
				const FGuidelineNode* EndB = Edge != nullptr ? Network.GetGuidelineNode(Edge->B) : nullptr;
				if (EndA == nullptr || EndB == nullptr)
				{
					continue;
				}

				TArray<FVector2D> Points;
				GuidelineGeom::Sample(EndA->Position, Edge->Control, EndB->Position, Points);

				int32 Span = 0;
				double Fraction = 0.0;
				const double Distance = GuidelineGeom::NearestOnPolyline(Points, At, Span, Fraction);
				if (Distance >= BestDistance)
				{
					continue;
				}

				BestDistance = Distance;
				BestEdge = EdgeId;
				BestParam = GuidelineGeom::ParamAtSample(Span, Fraction, Points.Num());
				BestPoint = FMath::Lerp(Points[Span], Points[Span + 1], Fraction);
			}

			if (!BestEdge.IsSet())
			{
				continue;
			}

			FGuidelineNodeId Join;
			FGuidelineEdgeId Head, Tail;
			if (!Network.SplitGuidelineEdge(BestEdge, BestParam, LaneWeldTolerance, Join, Head, Tail))
			{
				continue;
			}

			if (Head.IsSet() && Tail.IsSet())
			{
				// An actual split, not a weld to an existing endpoint: the halves inherit the
				// owner from the original edge (SplitGuidelineEdge copies every field but the
				// endpoint and control that moved), which is what keeps the whole lane
				// recognisable as one entity's after any number of splits.
				Lane.Remove(BestEdge);
				Lane.Add(Head);
				Lane.Add(Tail);
				Result.Nodes.Add(Join);
			}

			const FVector2D JoinAt = Network.GetGuidelineNode(Join)->Position;
			Lane.Add(Network.AddGuidelineEdge(
				MakeServiceEdge(Resolved.Node, Join, At, JoinAt, EntityId, /*bSpur=*/true)));
			++Result.SpursBuilt;
		}
	}

	if (Result.LoopsBuilt > 0)
	{
		// One census line, beside the guideline builder's and FAnchorLink's. Zero lanes is
		// the common idle rebuild and stays quiet.
		UE_LOG(LogAirside, Log, TEXT("Service loops: %d lane(s) laid, %d anchor spur(s)"),
			Result.LoopsBuilt, Result.SpursBuilt);
	}

	return Result;
}
