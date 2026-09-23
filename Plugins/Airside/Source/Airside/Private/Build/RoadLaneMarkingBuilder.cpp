#include "Build/RoadLaneMarkingBuilder.h"

#include "Build/MarkingQuads.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/** A lane each way: the profiles FRoadLaneMarkingBuilder paints. A taxiway's one bidirectional line is not. */
	bool IsLaneMarkedProfile(const URoadProfile& Profile)
	{
		bool bForward = false;
		bool bBack = false;
		for (const FProfileGuideline& Line : Profile.Guidelines)
		{
			bForward |= Line.Direction == EGuidelineDir::AToB;
			bBack |= Line.Direction == EGuidelineDir::BToA;
		}
		return bForward && bBack;
	}
}

int32 FRoadLaneMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out, int32* OutSegments)
{
	int32 Dashes = 0;
	int32 Segments = 0;
	for (const FRoadSegment& Segment : Network.GetSegments())
	{
		if (!Segment.bAlive || !Segment.bSolvedA || !Segment.bSolvedB)
		{
			continue;
		}
		const URoadProfile* Profile = Network.ProfileFor(Segment);
		const FRoadNode* NodeA = Network.GetNode(Segment.A);
		const FRoadNode* NodeB = Network.GetNode(Segment.B);
		if (Profile == nullptr || NodeA == nullptr || NodeB == nullptr || !IsLaneMarkedProfile(*Profile))
		{
			continue;
		}

		// STRAIGHT, because production lays only straight segments (URoadNetwork::
		// AddStraightSegment is the one caller that makes one). A curved road would need the
		// dashes laid along its sample array instead.
		const FVector2D Along = NodeB->Position - NodeA->Position;
		const double Length = Along.Size();
		const double First = Segment.TrimA;
		const double Last = Length - Segment.TrimB;
		if (Length <= 0.0 || Last <= First)
		{
			continue;
		}
		const FVector2D Dir = Along / Length;
		const FVector2D Across(-Dir.Y, Dir.X);

		// Phase from the A cut, so no dash enters the junction polygon at either end: the last
		// one is clipped to the B cut rather than running past it.
		int32 Here = 0;
		for (double At = First; At < Last; At += DashLength + DashGap)
		{
			MarkingQuads::AddRect(Out, Z, NodeA->Position, Dir, Across,
				At, FMath::Min(At + DashLength, Last), -LineWidth * 0.5, LineWidth * 0.5);
			++Here;
		}
		Dashes += Here;
		Segments += Here > 0 ? 1 : 0;
	}
	if (OutSegments != nullptr)
	{
		*OutSegments = Segments;
	}
	return Dashes;
}
