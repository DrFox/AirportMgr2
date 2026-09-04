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

		/** Sweep radius for this stand's painted line - see RadiusForCode. */
		double Radius = 2500.0;
	};

	/**
	 * Minimum centreline curve radius, in uu, for an ICAO aerodrome code letter.
	 *
	 * A taxi line is PAINTED, and the pilot follows it with the nose wheel - so its radius
	 * belongs to the STAND, sized for the largest aircraft that stand admits, and never to
	 * whichever aircraft happens to be taxiing. One line, one curve, every type follows it.
	 *
	 * PROVENANCE, stated plainly as UAircraftType does for its door stations: these are
	 * standard aerodrome design values by code letter, not figures lifted from a specific
	 * Annex 14 edition. They are what to check first if a real layout looks wrong - but the
	 * SHAPE of the rule, radius by code letter, is how aerodromes are actually dimensioned.
	 */
	double RadiusForCode(FName Code)
	{
		const FString Letter = Code.ToString().ToUpper();

		if (Letter == TEXT("A")) { return 1500.0; }
		if (Letter == TEXT("B")) { return 2000.0; }
		if (Letter == TEXT("C")) { return 2500.0; }
		if (Letter == TEXT("D")) { return 4000.0; }
		if (Letter == TEXT("E")) { return 5000.0; }
		if (Letter == TEXT("F")) { return 6000.0; }

		// No code, or one nobody recognises. Code C is the commonest stand in the world, and
		// erring to Code F instead would put a 60 m curve on a light-aircraft apron.
		return 2500.0;
	}

	/**
	 * The curve parameter Offset of ARC LENGTH away from Param, walked on the sampled
	 * polyline. Negative walks backwards. Clamped to the curve's own ends.
	 *
	 * Walked on the SAMPLES rather than integrated in closed form, because the samples are
	 * what every other consumer of this graph measures - the search costs them, the overlay
	 * draws them, a follower walks them. An exact arc length here would be more accurate
	 * and would disagree with all three.
	 */
	double ParamAtArcOffset(const TArray<FVector2D>& Points, double Param, double Offset)
	{
		if (Points.Num() < 2)
		{
			return FMath::Clamp(Param, 0.0, 1.0);
		}

		const int32 Spans = Points.Num() - 1;
		const double Scaled = FMath::Clamp(Param, 0.0, 1.0) * Spans;
		int32 Index = FMath::Clamp(static_cast<int32>(Scaled), 0, Spans - 1);
		double Fraction = Scaled - Index;

		double Remaining = FMath::Abs(Offset);
		const bool bForward = Offset >= 0.0;

		while (Remaining > 0.0)
		{
			const double SpanLength = FVector2D::Distance(Points[Index], Points[Index + 1]);
			const double Available = bForward ? SpanLength * (1.0 - Fraction) : SpanLength * Fraction;

			if (SpanLength <= 0.0 || Available >= Remaining)
			{
				Fraction += (bForward ? 1.0 : -1.0) * (SpanLength > 0.0 ? Remaining / SpanLength : 0.0);
				break;
			}

			Remaining -= Available;
			if (bForward)
			{
				if (Index + 1 >= Spans) { Fraction = 1.0; break; }
				++Index;
				Fraction = 0.0;
			}
			else
			{
				if (Index == 0) { Fraction = 0.0; break; }
				--Index;
				Fraction = 1.0;
			}
		}

		return GuidelineGeom::ParamAtSample(Index, FMath::Clamp(Fraction, 0.0, 1.0), Points.Num());
	}

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

		// The same design aircraft decides how wide the painted line sweeps. Its CODE
		// LETTER, not its span: aerodromes are dimensioned by code letter, and the line is
		// laid once for the largest aircraft the stand admits.
		const double StandRadius = RadiusForCode(
			Instance.Definition->DesignAircraft != nullptr
				? Instance.Definition->DesignAircraft->Code
				: FName());

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
			Link.Radius = StandRadius;
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
			Link.Radius = StandRadius;
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

		// WHERE THE LEAD-IN RAY STRIKES IS THE CORNER, NOT THE JOIN.
		//
		// Joining here is what produced a hard turn: the ray meets the taxiway at whatever
		// angle the stand happens to face, and a stand square to the taxiway makes it 90
		// degrees. A curve cannot fix that in place - a quadratic's end tangents both point
		// at its control, so being tangent to the lead-in AND to the taxiway would put the
		// control exactly here, which is the straight line again. The join has to MOVE.
		const FVector2D Corner =
			GuidelineGeom::Eval(PositionA, Original.Control, PositionB, BestParam);
		const FVector2D TaxiDir =
			GuidelineGeom::Tangent(PositionA, Original.Control, PositionB, BestParam);

		TArray<FVector2D> Curve;
		GuidelineGeom::Sample(PositionA, Original.Control, PositionB, Curve);

		// How far back along each line the tangent points sit, for a fillet of this radius.
		// The lead-in makes two corners with the taxiway - theta one way, 180 - theta the
		// other - and the SHARPER one sizes the offset, so it gets at least its radius and
		// the shallower one simply sweeps more gently.
		const double Cosine = FMath::Clamp(
			FVector2D::DotProduct(-Link.Dir, TaxiDir), -1.0, 1.0);
		const double Theta = FMath::Acos(Cosine);
		const double Sharper = FMath::Min(Theta, UE_DOUBLE_PI - Theta);

		double Offset = 0.0;
		if (Sharper > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			Offset = Link.Radius / FMath::Tan(Sharper * 0.5);
		}

		// It has to fit: on the lead-in, and on the taxiway BOTH ways, with a weld
		// tolerance of room left over so no split produces a stub.
		const double LeadRoom = FVector2D::Distance(Link.At, Corner) - LeadInWeldTolerance;
		const double TotalLength = GuidelineGeom::PolylineLength(Curve);
		const double Behind = TotalLength * BestParam - LeadInWeldTolerance;
		const double Ahead = TotalLength * (1.0 - BestParam) - LeadInWeldTolerance;

		Offset = FMath::Min(Offset, FMath::Min(LeadRoom, FMath::Min(Behind, Ahead)));

		FGuidelineNodeId LeadEnd;
		TArray<FGuidelineNodeId> SweepEnds;

		if (Offset <= LeadInWeldTolerance)
		{
			// No room to sweep - a stand crammed against a taxiway end, or one whose
			// lead-in is barely longer than the weld tolerance. Fall back to the hard join
			// rather than emit folded geometry: an ugly corner is recoverable, an inverted
			// arc is not. The junction solver clamps its fillets for the same reason.
			FGuidelineNodeId Join;
			if (FVector2D::Distance(Corner, PositionA) <= LeadInWeldTolerance)
			{
				Join = Original.A;
			}
			else if (FVector2D::Distance(Corner, PositionB) <= LeadInWeldTolerance)
			{
				Join = Original.B;
			}
			else
			{
				FVector2D Mid, ControlLeft, ControlRight;
				GuidelineGeom::Split(PositionA, Original.Control, PositionB, BestParam,
					Mid, ControlLeft, ControlRight);

				Join = Network.AddGuidelineNode(Mid, /*bDerived=*/true);

				FGuidelineEdge Left = Original;
				Left.B = Join;
				Left.Control = ControlLeft;

				FGuidelineEdge Right = Original;
				Right.A = Join;
				Right.Control = ControlRight;

				Network.RemoveGuidelineEdge(BestEdge);
				Network.AddGuidelineEdge(MoveTemp(Left));
				Network.AddGuidelineEdge(MoveTemp(Right));
			}

			LeadEnd = Join;
		}
		else
		{
			// Cut the taxiway at BOTH tangent points and keep the piece between them: an
			// aircraft taxiing PAST the stand still needs a way through.
			const double ParamBack = ParamAtArcOffset(Curve, BestParam, -Offset);
			const double ParamFwd  = ParamAtArcOffset(Curve, BestParam, +Offset);

			FVector2D BackAt, ControlToBack, ControlFromBack;
			GuidelineGeom::Split(PositionA, Original.Control, PositionB, ParamBack,
				BackAt, ControlToBack, ControlFromBack);

			// The forward cut expressed in the REMAINING piece's own parameter, because
			// that is the curve it is now being taken from.
			const double ParamFwdInRest = (ParamFwd - ParamBack) / FMath::Max(1.0 - ParamBack, UE_DOUBLE_SMALL_NUMBER);

			FVector2D FwdAt, ControlMiddle, ControlTail;
			GuidelineGeom::Split(BackAt, ControlFromBack, PositionB, ParamFwdInRest,
				FwdAt, ControlMiddle, ControlTail);

			const FGuidelineNodeId BackNode = Network.AddGuidelineNode(BackAt, /*bDerived=*/true);
			const FGuidelineNodeId FwdNode = Network.AddGuidelineNode(FwdAt, /*bDerived=*/true);

			FGuidelineEdge Head = Original;
			Head.B = BackNode;
			Head.Control = ControlToBack;

			FGuidelineEdge Middle = Original;
			Middle.A = BackNode;
			Middle.B = FwdNode;
			Middle.Control = ControlMiddle;

			FGuidelineEdge Tail = Original;
			Tail.A = FwdNode;
			Tail.Control = ControlTail;

			Network.RemoveGuidelineEdge(BestEdge);
			Network.AddGuidelineEdge(MoveTemp(Head));
			Network.AddGuidelineEdge(MoveTemp(Middle));
			Network.AddGuidelineEdge(MoveTemp(Tail));

			// The straight lead-in now stops short of the corner; the sweeps take over.
			const FVector2D LeadEndAt = Corner - Link.Dir * Offset;
			LeadEnd = Network.AddGuidelineNode(LeadEndAt, /*bDerived=*/true);

			SweepEnds.Add(BackNode);
			SweepEnds.Add(FwdNode);
		}

		// The painted lead-in itself: still straight, because it is. Only the ENTRY sweeps.
		FGuidelineEdge Lead;
		Lead.A = Link.Node;
		Lead.B = LeadEnd;
		Lead.Control = (Link.At + Network.GetGuidelineNode(LeadEnd)->Position) * 0.5;

		Lead.AllowedTraffic = FTrafficMask::Only(Link.Class);
		Lead.AllowedTraffic.Add(ETraversalClass::Emergency);
		Lead.Direction = EGuidelineDir::Bidirectional;
		Lead.Width = Original.Width;
		Lead.MaxWingspan = Link.MaxWingspan;
		Lead.bDerived = true;
		Network.AddGuidelineEdge(MoveTemp(Lead));

		// One sweep to each side, so the stand is reachable whichever way an aircraft
		// arrives. With only one, an approach from the other side is routed round a hairpin
		// at the tangent point - the same corner, moved a few metres rather than removed.
		//
		// Control AT THE CORNER, which is what makes each arc tangent to the lead-in at one
		// end and to the taxiway at the other: a quadratic's end tangents point at its
		// control, and both tangent points sit the same distance from it. Exactly the
		// construction RoadGuidelineBuilder uses for a junction turn path.
		for (const FGuidelineNodeId SweepEnd : SweepEnds)
		{
			FGuidelineEdge Sweep;
			Sweep.A = LeadEnd;
			Sweep.B = SweepEnd;
			Sweep.Control = Corner;
			Sweep.AllowedTraffic = FTrafficMask::Only(Link.Class);
			Sweep.AllowedTraffic.Add(ETraversalClass::Emergency);
			Sweep.Direction = EGuidelineDir::Bidirectional;
			Sweep.Width = Original.Width;
			Sweep.MaxWingspan = Link.MaxWingspan;
			Sweep.bDerived = true;
			Network.AddGuidelineEdge(MoveTemp(Sweep));
		}

		++Joined;
	}

	return Joined;
}
