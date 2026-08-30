#include "Tool/StandPlaceTool.h"

#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"

#define LOCTEXT_NAMESPACE "RoadNet"

FText FStandPlaceTool::GetDisplayName() const
{
	return LOCTEXT("StandTool", "Stand");
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

	const double Heading = AimedHeading(Context);
	if (Context.Target->PlaceStand(PressedAt, Heading) != INDEX_NONE)
	{
		LastHeading = Heading;
		Context.Target->RebuildMesh();
	}
}

void FStandPlaceTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		const int32 Under = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
		if (Under != INDEX_NONE && Context.Target->DeleteEntity(Under))
		{
			Context.Target->RebuildMesh();
		}
		return;
	}

	// A press that never travelled. It still places a stand - facing the way the last one
	// did - because refusing would make the tool feel broken for the common case of a row
	// of identically-oriented stands.
	if (Context.Target->PlaceStand(Context.Cursor, LastHeading) != INDEX_NONE)
	{
		Context.Target->RebuildMesh();
	}
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
	const UEntityDefinition* Definition =
		Context.Target != nullptr ? Context.Target->StandDefinition.Get() : nullptr;
	if (Definition == nullptr)
	{
		Sink.Marker(At, EPreviewStyle::Refused);
		Sink.Label(At, TEXT("no stand definition"), EPreviewStyle::Refused);
		return;
	}

	const double Cos = FMath::Cos(Heading);
	const double Sin = FMath::Sin(Heading);

	// The same local-to-world transform PlaceEntity uses. Restated here rather than shared
	// because the preview must work on a pose that does not exist in the model yet - there
	// is no entity to ask.
	auto ToWorld = [&At, Cos, Sin](const FVector2D& Local)
	{
		return FVector2D(
			At.X + Local.X * Cos - Local.Y * Sin,
			At.Y + Local.X * Sin + Local.Y * Cos);
	};

	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		const FVector2D World = ToWorld(Anchor.LocalPosition);

		// The aircraft's own stop position reads differently from the vehicles that serve
		// it: it is the thing being positioned, the rest are consequences of where it sits.
		Sink.Marker(World, Anchor.Role == EServiceRole::Aircraft
			? EPreviewStyle::Pending : EPreviewStyle::Snap);
		Sink.Line(At, World, EPreviewStyle::Heal);
	}

	// Which way it faces, drawn from the stop mark forward. Without it a stand aimed 180
	// degrees out looks identical to one aimed correctly until vehicles try to use it.
	Sink.Line(At, ToWorld(FVector2D(2500.0, 0.0)), EPreviewStyle::Pending);
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
		if (Under != INDEX_NONE && Context.Target->Network != nullptr)
		{
			const TArray<FEntityInstance>& Entities = Context.Target->Network->GetEntities();
			if (Entities.IsValidIndex(Under))
			{
				Sink.Marker(Entities[Under].Position, EPreviewStyle::Doomed);
				Sink.Label(Entities[Under].Position, TEXT("remove stand"), EPreviewStyle::Doomed);
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
