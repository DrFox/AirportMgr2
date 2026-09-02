#include "Build/AnchorLink.h"

#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/** Within this of an endpoint, join the endpoint rather than splitting off a stub. */
	constexpr double LeadInWeldTolerance = 10.0;

	/** 2D cross product. Positive when B is counter-clockwise of A. */
	double Cross(const FVector2D& A, const FVector2D& B)
	{
		return A.X * B.Y - A.Y * B.X;
	}

	/**
	 * Ray against one segment. OutAlongRay is in uu; OutAlongSegment is a 0..1 fraction.
	 *
	 * A ray, not a line: a lead-in points one way, and a guideline BEHIND the stand is
	 * behind the aircraft's tail. Testing the infinite line would happily join a stand to
	 * the taxiway it is facing away from.
	 */
	bool RayHitsSegment(
		const FVector2D& Origin, const FVector2D& Dir,
		const FVector2D& P, const FVector2D& Q,
		double& OutAlongRay, double& OutAlongSegment)
	{
		const FVector2D Edge = Q - P;
		const double Denominator = Cross(Dir, Edge);
		if (FMath::IsNearlyZero(Denominator, UE_DOUBLE_SMALL_NUMBER))
		{
			// Parallel. A collinear ray running along the guideline is deliberately not a
			// hit: there is no single point to join, and picking one would be arbitrary.
			return false;
		}

		const FVector2D ToP = P - Origin;
		OutAlongRay = Cross(ToP, Edge) / Denominator;
		OutAlongSegment = Cross(ToP, Dir) / Denominator;

		return OutAlongSegment >= 0.0 && OutAlongSegment <= 1.0;
	}

	/** One anchor waiting to be joined, gathered before the graph is mutated. */
	struct FPendingLink
	{
		FGuidelineNodeId Node;
		FVector2D At = FVector2D::ZeroVector;
		FVector2D Dir = FVector2D(1.0, 0.0);
		ETraversalClass Class = ETraversalClass::GroundVehicle;
		double MaxWingspan = 0.0;
	};
}

int32 FAnchorLink::Build(URoadNetwork& Network, double MaxLeadIn)
{
	// Gathered up front, because joining one anchor adds and removes edges and an
	// iteration over the graph must not be holding pointers into it while that happens.
	TArray<FPendingLink> Pending;
	TSet<FGuidelineNodeId> AnchorNodes;

	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];
		if (!Instance.bAlive || Instance.Definition == nullptr)
		{
			continue;
		}

		FEntityInstanceId EntityId;
		EntityId.Index = Index;
		EntityId.Generation = Instance.Generation;

		// The stand's design aircraft is what its lead-in can take. A Code C stand's link
		// then refuses a widebody by the ordinary wingspan rule rather than by a special
		// case, and the search reports TooWide instead of a bare "no route".
		const double StandWingspan = Instance.Definition->DesignAircraft != nullptr
			? Instance.Definition->DesignAircraft->Footprint.Wingspan
			: 0.0;

		// The stop position first. It is not an anchor - see FEntityInstance::PoseNode -
		// but it needs a lead-in for exactly the same reason, and it is the one an
		// AIRCRAFT is routed to.
		//
		// The ray leaves along the entity's heading PLUS 180: +X faces the terminal, so a
		// stand's lead-in runs back out of it to the movement area. Cast the other way and
		// every stand would try to join a guideline inside the building.
		AnchorNodes.Add(Instance.PoseNode);
		if (const FGuidelineNode* Pose = Network.GetGuidelineNode(Instance.PoseNode);
			Pose != nullptr && Pose->Incident.Num() == 0)
		{
			const double Out = Instance.Heading + UE_DOUBLE_PI;

			FPendingLink Link;
			Link.Node = Instance.PoseNode;
			Link.At = Pose->Position;
			Link.Dir = FVector2D(FMath::Cos(Out), FMath::Sin(Out));
			Link.Class = ETraversalClass::Aircraft;
			Link.MaxWingspan = StandWingspan;
			Pending.Add(Link);
		}

		for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
		{
			AnchorNodes.Add(Resolved.Node);

			const FGuidelineNode* Node = Network.GetGuidelineNode(Resolved.Node);
			if (Node == nullptr || Node->Incident.Num() > 0)
			{
				// Already joined - by a previous pass that survived, or by hand. Either
				// way, adding a second lead-in would leave two lines into one nose-stop.
				continue;
			}

			double Heading = 0.0;
			if (!Network.GetAnchorWorldHeading(EntityId, Resolved.Id, Heading))
			{
				continue;
			}

			// The role lives on the definition, addressed by id - never by position in the
			// array, which is the invariant FResolvedAnchor exists to remove.
			const FEntityAnchor* Declared = Instance.Definition->Anchors.FindByPredicate(
				[&Resolved](const FEntityAnchor& Candidate) { return Candidate.Id == Resolved.Id; });
			if (Declared == nullptr)
			{
				continue;
			}

			FPendingLink Link;
			Link.Node = Resolved.Node;
			Link.At = Node->Position;
			Link.Dir = FVector2D(FMath::Cos(Heading), FMath::Sin(Heading));
			Link.Class = TraversalForRole(Declared->Role);
			Link.MaxWingspan = Link.Class == ETraversalClass::Aircraft ? StandWingspan : 0.0;
			Pending.Add(Link);
		}
	}

	int32 Joined = 0;

	for (const FPendingLink& Link : Pending)
	{
		FGuidelineEdgeId BestEdge;
		double BestDistance = MaxLeadIn;
		double BestParam = 0.0;

		// Re-read each time: a previous anchor may have split the very guideline this one
		// is about to hit, and it must see the halves rather than the edge that is gone.
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || !Edge.bDerived || Edge.A == Edge.B)
			{
				continue;
			}

			// Never target another lead-in. Every one has an anchor node at an end, so
			// excluding those is enough and needs no separate mark on the edge.
			if (AnchorNodes.Contains(Edge.A) || AnchorNodes.Contains(Edge.B))
			{
				continue;
			}

			if (!Edge.AllowedTraffic.Allows(Link.Class))
			{
				continue;
			}

			const FGuidelineNode* EndA = Network.GetGuidelineNode(Edge.A);
			const FGuidelineNode* EndB = Network.GetGuidelineNode(Edge.B);
			if (EndA == nullptr || EndB == nullptr)
			{
				continue;
			}

			TArray<FVector2D> Points;
			GuidelineGeom::Sample(EndA->Position, Edge.Control, EndB->Position, Points);

			for (int32 At = 1; At < Points.Num(); ++At)
			{
				double AlongRay = 0.0;
				double AlongSegment = 0.0;
				if (!RayHitsSegment(Link.At, Link.Dir, Points[At - 1], Points[At], AlongRay, AlongSegment))
				{
					continue;
				}

				// Strictly ahead, and no further than the cap.
				if (AlongRay <= LeadInWeldTolerance || AlongRay >= BestDistance)
				{
					continue;
				}

				BestDistance = AlongRay;
				BestParam = GuidelineGeom::ParamAtSample(At - 1, AlongSegment, Points.Num());

				FGuidelineEdgeId Id;
				Id.Index = Index;
				Id.Generation = Edge.Generation;
				BestEdge = Id;
			}
		}

		if (!BestEdge.IsSet())
		{
			continue;
		}

		const FGuidelineEdge* Found = Network.GetGuidelineEdge(BestEdge);
		const FGuidelineNode* EndA = Found != nullptr ? Network.GetGuidelineNode(Found->A) : nullptr;
		const FGuidelineNode* EndB = Found != nullptr ? Network.GetGuidelineNode(Found->B) : nullptr;
		if (Found == nullptr || EndA == nullptr || EndB == nullptr)
		{
			continue;
		}

		// Copied before anything is removed: Found points into the slot array, and adding
		// the halves can reallocate it.
		const FGuidelineEdge Original = *Found;
		const FVector2D PositionA = EndA->Position;
		const FVector2D PositionB = EndB->Position;

		FVector2D Mid;
		FVector2D ControlLeft;
		FVector2D ControlRight;
		GuidelineGeom::Split(PositionA, Original.Control, PositionB, BestParam, Mid, ControlLeft, ControlRight);

		FGuidelineNodeId Join;
		if (FVector2D::Distance(Mid, PositionA) <= LeadInWeldTolerance)
		{
			// The lead-in met the guideline at its own end. Splitting here would leave a
			// stub edge shorter than the weld tolerance, which is geometry nobody wants and
			// a zero-length cost the search would have to tolerate.
			Join = Original.A;
		}
		else if (FVector2D::Distance(Mid, PositionB) <= LeadInWeldTolerance)
		{
			Join = Original.B;
		}
		else
		{
			Join = Network.AddGuidelineNode(Mid, /*bDerived=*/true);

			FGuidelineEdge Left = Original;
			Left.B = Join;
			Left.Control = ControlLeft;

			FGuidelineEdge Right = Original;
			Right.A = Join;
			Right.Control = ControlRight;

			// Removed before the halves are added, so the graph is never momentarily
			// carrying the whole guideline and both of its parts.
			Network.RemoveGuidelineEdge(BestEdge);
			Network.AddGuidelineEdge(MoveTemp(Left));
			Network.AddGuidelineEdge(MoveTemp(Right));
		}

		FGuidelineEdge Lead;
		Lead.A = Link.Node;
		Lead.B = Join;

		// Straight: the painted lead-in line is straight, and the control point is the
		// midpoint because that is how this graph spells "no bend".
		Lead.Control = (Link.At + Mid) * 0.5;

		Lead.AllowedTraffic = FTrafficMask::Only(Link.Class);
		Lead.AllowedTraffic.Add(ETraversalClass::Emergency);
		Lead.Direction = EGuidelineDir::Bidirectional;
		Lead.Width = Original.Width;
		Lead.MaxWingspan = Link.MaxWingspan;
		Lead.bDerived = true;

		Network.AddGuidelineEdge(MoveTemp(Lead));
		++Joined;
	}

	return Joined;
}
