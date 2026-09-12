#include "Tool/StandPlaceTool.h"

#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Tool/StandPreview.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FStandPlaceTool::GetDisplayName() const
{
	// TWO NAMES FOR ONE TOOL, and each must match the registry's own Name for its entry -
	// Airside.Tool.BuildSession asserts the two cannot drift, which is exactly the class of
	// bug the registry exists to make impossible elsewhere.
	return Kind == EPlaceableEntity::FuelDepot
		? LOCTEXT("FuelDepotTool", "Fuel depot")
		: LOCTEXT("StandTool", "Stand");
}

double FStandPlaceTool::AimedHeading(const FToolContext& Context) const
{
	const FVector2D Along = Context.Cursor - PressedAt;

	// Below the threshold the direction is noise: two points a few uu apart give a heading
	// that swings wildly with the cursor. Falling back to the last one used means a stand
	// dropped without aiming faces the same way as the one before it, which on a pier is
	// almost always what was wanted.
	if (Along.SizeSquared() < FMath::Square(Context.SnapRadius))
	{
		return LastHeading;
	}

	return FMath::Atan2(Along.Y, Along.X);
}

void FStandPlaceTool::OnDragBegin(const FToolContext& Context)
{
	if (Context.Target == nullptr || Context.bRemoveModifier)
	{
		return;
	}

	// The press point, not the current cursor: the stand goes where the gesture STARTED and
	// the drag only says which way it faces. Aiming would otherwise drag the stand along
	// with it and there would be no way to place one facing anywhere but at your cursor.
	PressedAt = Context.Cursor;
	bAiming = true;
}

void FStandPlaceTool::OnDrag(const FToolContext& Context)
{
	// Nothing to do: the preview reads the cursor directly, and nothing is committed until
	// the button comes up. A stand that appeared on drag-begin and then span would be an
	// edit the player never asked for, present in the undo stack whatever they did next.
}

void FStandPlaceTool::OnDragEnd(const FToolContext& Context)
{
	if (!bAiming || Context.Target == nullptr)
	{
		bAiming = false;
		return;
	}

	bAiming = false;

	// Releasing ENDS THE AIM. It does not place: a mouse-up that finishes a rotation is
	// not a decision to commit, and treating it as one meant every attempt to re-aim left
	// a stand behind. Placing is its own click, which OnClick does with this heading.
	LastHeading = AimedHeading(Context);
}

void FStandPlaceTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		// No RebuildMesh() on success any more - DeleteEntity notifies on commit (issue #77).
		const int32 Under = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Under != INDEX_NONE)
		{
			Context.Target->DeleteEntity(Under);
		}
		return;
	}

	// A press that never travelled. It still places one - facing the way the last one did -
	// because refusing would make the tool feel broken for the common case of a row of
	// identically-oriented stands. No RebuildMesh() here any more - PlaceEntity notifies on
	// commit (issue #77).
	Context.Target->PlaceEntity(Context.Cursor, LastHeading, Kind);
}

void FStandPlaceTool::OnCancel(const FToolContext& Context)
{
	// Nothing is ever part-placed, so there is nothing to back out of. Right-clicking
	// mid-drag abandons the aim by ending it without committing.
	bAiming = false;
}

void FStandPlaceTool::OnDeactivate(const FToolContext& Context)
{
	bAiming = false;
}

void FStandPlaceTool::PreviewPose(const FToolContext& Context, const FVector2D& At,
	double Heading, IToolPreviewSink& Sink) const
{
	// Shared with the editor's view of an already-placed stand, so the same object cannot
	// be drawn two different ways depending on which code path found it.
	const UEntityDefinition* Definition =
		Context.Target != nullptr ? Context.Target->GetEntityDefinition(Kind) : nullptr;

	StandPreview::Describe(Definition, At, Heading, Sink);
}

void FStandPlaceTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr)
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		const int32 Under = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Under != INDEX_NONE && Context.Target->GetNetwork() != nullptr)
		{
			const TArray<FEntityInstance>& Entities = Context.Target->GetNetwork()->GetEntities();
			if (Entities.IsValidIndex(Under))
			{
				Sink.Marker(Entities[Under].Position, EPreviewStyle::Doomed);
				Sink.Label(Entities[Under].Position,
					Kind == EPlaceableEntity::FuelDepot ? TEXT("remove fuel depot") : TEXT("remove stand"),
					EPreviewStyle::Doomed);

				// IN USE. The claim holder, read from the traffic table through the edit target
				// (Model/, so a tool may see it). The click still deletes - the player owns the
				// infrastructure - but not without being told who is about to lose a stand.
				if (const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic())
				{
					int32 Holder = 0;
					if (Entities[Under].PoseNode.IsSet()
						&& Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Entities[Under].PoseNode), 0, &Holder))
					{
						Sink.Label(Entities[Under].Position + FVector2D(0.0, 600.0),
							FString::Printf(TEXT("in use by aircraft %d"), Holder), EPreviewStyle::Refused);
					}
				}
			}
		}
		return;
	}

	// While aiming, the stand stays where the press landed and only turns. Otherwise it
	// follows the cursor at the heading a click would use.
	const FVector2D At = bAiming ? PressedAt : Context.Cursor;
	const double Heading = bAiming ? AimedHeading(Context) : LastHeading;

	PreviewPose(Context, At, Heading, Sink);
}

#undef LOCTEXT_NAMESPACE
