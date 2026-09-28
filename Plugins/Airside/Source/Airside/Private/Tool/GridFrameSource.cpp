#include "Tool/GridFrameSource.h"

namespace GridFrameSource
{
	namespace
	{
		bool IsAThing(SnapGuide::EReference Reference)
		{
			switch (Reference)
			{
			case SnapGuide::EReference::Taxiway:
			case SnapGuide::EReference::ServiceRoad:
			case SnapGuide::EReference::Runway:
			case SnapGuide::EReference::Apron:
			case SnapGuide::EReference::Stand:
				return true;
			case SnapGuide::EReference::ThisGesture:
			case SnapGuide::EReference::World:
				return false;
			}
			return false;
		}
	}

	bool LineOfWinner(const SnapGuide::FCandidate& Winner, FVector2D& OutThrough, FVector2D& OutDirection)
	{
		if (!IsAThing(Winner.Reference))
		{
			return false;
		}

		switch (Winner.Relation)
		{
		case SnapGuide::ERelation::Collinear:
			// THE THING'S OWN LINE - Through is the segment's end, Direction along it.
			OutThrough = Winner.Through;
			OutDirection = Winner.Direction;
			return true;

		case SnapGuide::ERelation::Parallel:
			if (Winner.Label.Kind == SnapGuide::ELabelKind::Along)
			{
				OutThrough = Winner.ReferenceAt;
				OutDirection = Winner.Direction;
				return true;
			}
			if (Winner.Label.Kind == SnapGuide::ELabelKind::SquareTo)
			{
				OutThrough = Winner.ReferenceAt;
				OutDirection = FVector2D(-Winner.Direction.Y, Winner.Direction.X);
				return true;
			}
			return false;

		case SnapGuide::ERelation::MatchingGap:
			// THE REFERENCE ROAD, not the line one gap over: the new road will lie on a grid line
			// anyway only if the gap is whole steps, and the reference is the thing named.
			OutThrough = Winner.ReferenceAt;
			OutDirection = Winner.Direction;
			return true;

		case SnapGuide::ERelation::Extending:
		case SnapGuide::ERelation::LevelWith:
		case SnapGuide::ERelation::AngledFrom:
			return false;
		}
		return false;
	}

	EGridFrameSource Resolve(const FGridFrameInputs& In, GridSnap::FGridFrame& InOutHeld, GridSnap::FGridFrame& Out)
	{
		if (In.StepUu <= 0.0 || In.Orientation == EGridOrientation::World)
		{
			Out = GridSnap::FGridFrame::World(In.StepUu);
			return EGridFrameSource::World;
		}

		FVector2D Through = FVector2D::ZeroVector;
		FVector2D Direction = FVector2D::ZeroVector;
		EGridFrameSource Source = EGridFrameSource::Held;

		// WINNERS IN THEIR OWN ORDER - at most two, one per fit; the first that is about a thing.
		if (In.Guide != nullptr && In.Guide->bActive)
		{
			for (const SnapGuide::FCandidate& Winner : In.Guide->Winners)
			{
				if (LineOfWinner(Winner, Through, Direction))
				{
					Source = EGridFrameSource::Winner;
					break;
				}
			}
		}
		if (Source == EGridFrameSource::Held && In.bToolLine && !In.ToolDirection.IsNearlyZero())
		{
			Through = In.ToolThrough;
			Direction = In.ToolDirection;
			Source = EGridFrameSource::ToolLine;
		}
		if (Source == EGridFrameSource::Held && In.bAnchor && !In.AnchorReference.IsNearlyZero())
		{
			Through = In.AnchorOrigin;
			Direction = In.AnchorReference;
			Source = EGridFrameSource::Anchor;
		}
		if (Source == EGridFrameSource::Held && In.bRoadSnap && !(In.RoadB - In.RoadA).IsNearlyZero())
		{
			Through = In.RoadA;
			Direction = In.RoadB - In.RoadA;
			Source = EGridFrameSource::RoadSnap;
		}

		if (Source != EGridFrameSource::Held)
		{
			InOutHeld = GridSnap::FGridFrame::Along(Through, Direction, In.StepUu);
		}
		// THE STEP IS NEVER HELD: a step changed while the frame is held keeps the direction and
		// takes the new pitch, which is what the Grid button promises.
		InOutHeld.StepUu = In.StepUu;
		Out = InOutHeld;
		return Source;
	}

	const TCHAR* Name(EGridFrameSource Source)
	{
		switch (Source)
		{
		case EGridFrameSource::World:    return TEXT("world");
		case EGridFrameSource::Winner:   return TEXT("winner");
		case EGridFrameSource::ToolLine: return TEXT("tool line");
		case EGridFrameSource::Anchor:   return TEXT("anchor");
		case EGridFrameSource::RoadSnap: return TEXT("road snap");
		case EGridFrameSource::Held:     return TEXT("held");
		}
		return TEXT("?");
	}
}
