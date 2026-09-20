#include "Tool/StandPlaceTool.h"

#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Solve/GuideArbiter.h"
#include "Tool/StandPreview.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/**
	 * The dashed line to each thing the stand is lined up with, and its label.
	 *
	 * PREFIXED for the unity build, like FRoadDrawTool's RoadGuidedSnap and
	 * FOutlineDrawTool's OutlineDrawGuide - "DrawGuide" is the name a fourth tool would also
	 * pick, and two of them collide only once they share a blob.
	 *
	 * IT LIVES IN THE TOOL because nothing else draws a guide: FBuildSession resolves one onto
	 * the context and stops there, so a tool that describes an anchor and never emits is a
	 * feature that computes correctly and shows the player nothing. See
	 * IBuildTool::WantsFreeStartGuides.
	 */
	void StandDrawGuide(const FToolContext& Context, const FVector2D& Moving,
		IToolPreviewSink& Sink)
	{
		if (!Context.Guide.bActive)
		{
			return;
		}

		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			Sink.Line(Moving, Winner.ReferenceAt, EPreviewStyle::Guide);

			// At the line's MIDPOINT, like every other tool: two labels at the moving point
			// overprint, and the plugin has no camera to offset them by readable pixels.
			Sink.Label((Moving + Winner.ReferenceAt) * 0.5, Winner.Description,
				EPreviewStyle::Guide);
		}
	}
}

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

bool FStandPlaceTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// THE WAY THIS STAND WILL FACE, which a click with no drag takes from the last one placed.
	// Named as the reference so FPointAlignGuideSource has a direction to run its lines along -
	// without it that source declines and the Stand column proposes nothing at all. See this
	// function's declaration.
	Out.Reference = FVector2D(FMath::Cos(LastHeading), FMath::Sin(LastHeading));
	Out.ReferenceName = TEXT("the way it faces");

	// NO ReferenceAt, AND THAT IS NOT AN OMISSION. It is the point FExtendingGuideSource draws
	// its dashed line to, and a heading is not a thing on the map to point at - there is no
	// honest answer. It is never reached: Extending's two candidates are EFit::Angular, this
	// tool only ever anchors on a FREE START, and a free start puts the cursor exactly on the
	// origin, so SnapGuide::Arbitrate has no direction to measure and drops every angular
	// candidate before any of them can be drawn. Naming the reference buys PointAlign its
	// direction and costs nothing, precisely because of that.

	// EVERY STAND IN REACH IS SOMETHING TO BE LEVEL WITH. Reach measured from the ENTITY to the
	// cursor cannot be asked here - this function is handed no cursor (see
	// IBuildTool::DescribeGuideAnchor) - so every live entity is offered and
	// FPointAlignGuideSource's own tolerance decides which the player is actually near. There
	// are tens of entities on a field, not thousands; the road tool's node loop bounds itself
	// by SearchRadiusUu because it HAS an origin to measure from, and this does not.
	if (Network != nullptr)
	{
		const TArray<FEntityInstance>& Entities = Network->GetEntities();
		for (const FEntityInstance& Entity : Entities)
		{
			if (!Entity.bAlive)
			{
				continue;
			}

			// SPELT OUT, not braced: a third member arrived on FGuidePoint in 2026-09-20 and a
			// braced initialiser would have taken the default for it in silence.
			FGuidePoint Point;
			Point.At = Entity.Position;
			Point.Name = EntityNaming::Describe(Entity);

			// THE STAND COLUMN'S, so the Stand button switches these off - and so they are not
			// mistaken for the gesture's own points, which have no button at all.
			Point.Reference = SnapGuide::EReference::Stand;
			Out.AlignTo.Add(Point);
		}
	}

	// A FREE START OR NOTHING. Everything above is the anchor's furniture; the base decides
	// whether there is an anchor at all, which for this tool means "not mid-aim". Delegating
	// rather than answering here is what opts the tool in - see IBuildTool::DescribeGuideAnchor.
	return IBuildTool::DescribeGuideAnchor(Network, Target, Out);
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
	//
	// GUIDED, because this press IS the free start - the tool is still idle on the frame the
	// button goes down, so the anchor and the guide are live and the player is looking at a
	// dashed line. Pinning the raw cursor here would put the stand a few metres off the line
	// they were shown, which is the whole of "a guide drawn and then not obeyed".
	PressedAt = Context.GuidedCursor();
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
	//
	// AT THE GUIDED POINT, not the raw cursor: this is where the free start pays, and the row
	// of stands the guide was argued for is a row only if the click lands on the line.
	Context.Target->PlaceEntity(Context.GuidedCursor(), LastHeading, Kind);
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
		if (Under != INDEX_NONE && Context.Network() != nullptr)
		{
			const TArray<FEntityInstance>& Entities = Context.Network()->GetEntities();
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
					const int32 Holder = Entities[Under].PoseNode.IsSet()
						? Traffic->HolderOfNode(Entities[Under].PoseNode) : 0;
					if (Holder != 0)
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
	// follows the GUIDED cursor at the heading a click would use - which is where the click
	// puts it, so the ghost and the placement cannot disagree.
	const FVector2D At = bAiming ? PressedAt : Context.GuidedCursor();
	const double Heading = bAiming ? AimedHeading(Context) : LastHeading;

	PreviewPose(Context, At, Heading, Sink);

	// AND WHAT IT IS LINED UP WITH. Not while aiming: the position is already pinned, so a
	// dashed line to something the cursor is passing would describe a constraint that is no
	// longer being applied - the same rule the outline tool keeps while closing. In practice
	// the anchor declines mid-aim too, so this is belt and braces on one frame's ordering.
	if (!bAiming)
	{
		StandDrawGuide(Context, At, Sink);
	}
}

#undef LOCTEXT_NAMESPACE
