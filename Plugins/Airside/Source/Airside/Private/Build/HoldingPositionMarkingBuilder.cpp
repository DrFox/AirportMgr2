#include "Build/HoldingPositionMarkingBuilder.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/** A taxiway with no profile to ask still gets a bar this wide. The standard taxiway. */
	constexpr double MarkingFallbackWidth = 2300.0;

	/**
	 * One quad, corners in either rotational order, wound counter-clockwise as seen from +Z
	 * by measurement - the maths convention every builder here works in - and emitted with
	 * the SAME flip FRoadMeshBuilder::AddTriangle applies: Unreal is left-handed, so a
	 * counter-clockwise triangle faces DOWN and is culled from above. The flip lives in one
	 * place per builder, never at a call site.
	 */
	void MarkingAddQuad(FRoadMeshBuffers& Out, double Z,
		const FVector2D& P0, const FVector2D& P1, const FVector2D& P2, const FVector2D& P3)
	{
		const int32 Base = Out.Positions.Num();
		FVector2D Corners[4] = { P0, P1, P2, P3 };
		// MEASURED, not trusted. The first cut of this builder handed its corners over
		// "counter-clockwise" by inspection and every one of them was clockwise: 112 of 112
		// vertex normals pointed down in Airside.Build.HoldingPositionMarking. A bar's corner
		// order depends on which way Toward and Across happen to point, so the signed area
		// decides here and the caller's order is a hint at most.
		double TwiceArea = 0.0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FVector2D& A = Corners[Index];
			const FVector2D& B = Corners[(Index + 1) % 4];
			TwiceArea += A.X * B.Y - B.X * A.Y;
		}
		if (TwiceArea < 0.0)
		{
			Swap(Corners[1], Corners[3]);
		}
		const FVector2f UV0s[4] = { FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f) };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Out.Positions.Add(FVector3d(Corners[Index].X, Corners[Index].Y, Z));
			Out.UV0.Add(UV0s[Index]);
			// Lateral 0 everywhere: the whole quad is centreline as far as the material can
			// tell, which is what paints it MarkingColor - see the header.
			Out.UV1.Add(FVector2f(0.f, 0.f));
			Out.UV2.Add(FVector2f(0.f, 0.f));
		}
		// (0,1,2) and (0,2,3) counter-clockwise, flipped to (0,2,1) and (0,3,2).
		Out.Indices.Append({ Base + 0, Base + 2, Base + 1 });
		Out.MaterialIDs.Add(0);
		Out.Indices.Append({ Base + 0, Base + 3, Base + 2 });
		Out.MaterialIDs.Add(0);
	}

	/**
	 * One bar across the taxiway: solid, or dashed from one edge. Toward is the unit
	 * direction from the node INTO the junction (the side the pattern sits on), Across its
	 * left-hand perpendicular. Near and Far are distances from the node along Toward.
	 */
	void MarkingAddBar(FRoadMeshBuffers& Out, double Z, const FVector2D& Node,
		const FVector2D& Toward, const FVector2D& Across, double HalfWidth,
		double Near, double Far, bool bDashed)
	{
		const FVector2D NearLine = Node + Toward * Near;
		const FVector2D FarLine = Node + Toward * Far;
		if (!bDashed)
		{
			MarkingAddQuad(Out, Z,
				NearLine - Across * HalfWidth, NearLine + Across * HalfWidth,
				FarLine + Across * HalfWidth, FarLine - Across * HalfWidth);
			return;
		}
		const double Period = FHoldingPositionMarkingBuilder::DashLength + FHoldingPositionMarkingBuilder::DashGap;
		for (double Start = -HalfWidth; Start < HalfWidth; Start += Period)
		{
			const double End = FMath::Min(Start + FHoldingPositionMarkingBuilder::DashLength, HalfWidth);
			MarkingAddQuad(Out, Z,
				NearLine + Across * Start, NearLine + Across * End,
				FarLine + Across * End, FarLine + Across * Start);
		}
	}
}

int32 FHoldingPositionMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out)
{
	int32 Painted = 0;
	const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = Nodes[Index];
		if (!Node.bAlive || Node.HoldingPosition == EHoldingPositionKind::None)
		{
			continue;
		}

		// THE DIRECTION INTO THE JUNCTION, and the taxiway's width. Origin names the road
		// this node was derived FOR - the taxiway the aircraft is travelling along - and
		// GetOutgoingTangent points from that road's node OUT along the segment, so its
		// negation points from the node into the junction. Incident[0] is the fallback for
		// a node with no Origin, exactly as GuidelineOverlay falls back for the overlay bar.
		FVector2D Toward = FVector2D::ZeroVector;
		double HalfWidth = MarkingFallbackWidth * 0.5;
		if (Node.Origin.IsSet())
		{
			if (const FRoadSegment* Segment = Network.GetSegment(Node.Origin.Segment))
			{
				const FRoadNodeId RoadNode = Node.Origin.bEndA ? Segment->A : Segment->B;
				Toward = -Network.GetOutgoingTangent(Node.Origin.Segment, RoadNode).GetSafeNormal();
				if (const URoadProfile* Profile = Network.ProfileFor(*Segment))
				{
					HalfWidth = FMath::Max(Profile->GetTotalWidth() * 0.5, LineWidth);
				}
			}
		}
		if (Toward.IsNearlyZero())
		{
			for (const FGuidelineEdgeId& EdgeId : Node.Incident)
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
				if (Edge == nullptr) { continue; }
				const FGuidelineNodeId Self = Network.GuidelineNodeIdAt(Index);
				const FGuidelineNodeId Other = (Edge->A == Self) ? Edge->B : Edge->A;
				const FGuidelineNode* OtherNode = Network.GetGuidelineNode(Other);
				if (OtherNode == nullptr) { continue; }
				const FVector2D Away = (OtherNode->Position - Node.Position).GetSafeNormal();
				if (!Away.IsNearlyZero())
				{
					Toward = -Away;
					if (Edge->Width > 0.0) { HalfWidth = FMath::Max(Edge->Width * 0.5, LineWidth); }
					break;
				}
			}
		}
		if (Toward.IsNearlyZero())
		{
			continue;   // an isolated node has no bar to be across
		}
		const FVector2D Across(-Toward.Y, Toward.X);

		if (Node.HoldingPosition == EHoldingPositionKind::Runway)
		{
			// Nearest the aircraft first: two solid, then two dashed, each LineWidth deep
			// with LineGap between. The nose stopped at the node is on the first line's edge.
			double At = 0.0;
			for (int32 Line = 0; Line < 4; ++Line)
			{
				MarkingAddBar(Out, Z, Node.Position, Toward, Across, HalfWidth, At, At + LineWidth, /*bDashed=*/Line >= 2);
				At += LineWidth + LineGap;
			}
		}
		else
		{
			MarkingAddBar(Out, Z, Node.Position, Toward, Across, HalfWidth, 0.0, LineWidth, /*bDashed=*/true);
		}
		++Painted;
	}
	return Painted;
}
