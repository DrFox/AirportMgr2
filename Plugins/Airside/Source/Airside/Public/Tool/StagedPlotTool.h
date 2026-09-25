#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Tool/PlotGesture.h"
#include "Tool/RoadBuildTool.h"

/**
 * Template Method for a staged corner-by-corner plot gesture: anchor on a road, drag out a
 * quadrilateral one click at a time, lock on the last click, Build commits it.
 *
 * ISSUE #302: FStandPlotTool was FPlotPlaceTool's skeleton copied by hand - the Remove branch,
 * the Idle anchor search, the cancel step-back, OnCommit's return-honouring and OnDeactivate
 * were bit-for-bit duplicates (StandPlotTool.cpp:136-141/241-277/365-367/429-458 against
 * PlotPlaceTool.cpp:299-459/499-655), and the copy is what let the newer tool ship without
 * #180's per-outline memo - WhyStandRefused and StandBox::LetterOf each ran two or three
 * times a frame because nothing here reminded the second author that FPlotPlaceTool had
 * already paid for that lesson once. FStandPlotTool.h's own comment called this "not a
 * subclass of FPlotPlaceTool: everything past the anchor differs" - true of the SHAPE (a
 * rectangle from three clicks, not a free quad from four) but not of the SKELETON around it,
 * which is what this class now owns.
 *
 * A HOOK PER GENUINE DIFFERENCE, nothing more: which road the gesture anchors on (Filter),
 * which entity kind Remove takes (IsMine) and what it calls it (RemoveLabel, RoadNoun), how
 * the pinned corners and the cursor become this frame's quad (Shape), how a locked shape is
 * placed (Place), and what each tool draws or reports once the shape means something
 * (Describe, DescribeReadout). Neither derived class overrides OnClick, OnCancel, OnCommit,
 * OnDeactivate, BuildPreview or BuildReadout at all - if a future difference needs one to,
 * that is this base's contract to widen, not a reason to fork the class again.
 *
 * PINNED, NOT A STAGE ENUM, IS THE SINGLE SOURCE - a deliberate inversion of what
 * FPlotPlaceTool::PinnedCount and FStandPlotTool::PinnedCount each argued for on their own
 * terms ("read off the stage so the two cannot disagree"). Two sibling stage enums naming the
 * SAME three-phase shape (Idle -> per-corner clicks -> Confirm) under different labels was
 * itself a list that had to agree with nothing, which is how the copy drifted. CLAUDE.md's "a
 * phase is an enum, never a set of bools" is about illegal state, not about counters - Pinned
 * is a monotonic count with no illegal value, and GetStage() on each derived tool is a one-line
 * cast back to the UENUM every test and the readout already expect, so nothing downstream can
 * tell the direction of derivation changed.
 */
class AIRSIDE_API FStagedPlotTool : public IBuildTool
{
public:
	virtual void OnClick(const FToolContext& Context) override final;
	virtual void OnCancel(const FToolContext& Context) override final;
	virtual void OnCommit(const FToolContext& Context) override final;
	virtual void OnDeactivate(const FToolContext& Context) override final;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override final;
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const override final;
	virtual bool IsIdle() const override final { return Pinned == 0; }

	/**
	 * How many of MaxPinned corners the player has placed. What the readout reports as "N/M",
	 * and what each derived tool's GetStage() casts back to its own UENUM - see this class's
	 * own comment on why an int is the source rather than a second enum.
	 */
	int32 PinnedCount() const { return Pinned; }

protected:
	explicit FStagedPlotTool(int32 InMaxPinned) : MaxPinned(InMaxPinned) {}

	/** MaxPinned corners pinned: the shape is locked and Build is the only way on. */
	bool IsConfirmed() const { return Pinned == MaxPinned; }

	// --- Hooks: the one place each derived tool answers a genuine difference -------------

	/** Which road this gesture may anchor on - PlotGesture::IsServiceRoad or IsTaxiway. */
	virtual bool Filter(const URoadNetwork& Network, FRoadSegmentId Id) const = 0;

	/** Is this entity one THIS tool places - what Remove and the idle preview act on. */
	virtual bool IsMine(const FEntityInstance& Entity) const = 0;

	/** The Remove preview's label, e.g. "remove fuel depot". */
	virtual FString RemoveLabel() const = 0;

	/** The Remove readout's warning, e.g. "Click a fuel depot to remove it". */
	virtual FString RemoveWarning() const = 0;

	/** What the Idle stage is hunting for, e.g. "a service road" - used in both directions'
	 *  wording ("move near %s" in the preview, "Move near %s" in the readout). */
	virtual FString RoadNoun() const = 0;

	/** The first readout fact's label, e.g. "Plot Points" or "Stand Points". */
	virtual FString PointsFactLabel() const = 0;

	/**
	 * This frame's shape from the pinned Corners (below) and the cursor in Context - the body
	 * FPlotPlaceTool::Quad and FStandPlotTool::Rect each kept under their own name, since tests
	 * call them directly. ONE DERIVATION, EVERY CALLER inside this base: OnClick pins from what
	 * this drew, BuildPreview draws it, BuildReadout measures it and OnCommit builds from it -
	 * the invariant both tools' headers documented separately is enforced once, here.
	 */
	virtual void Shape(const FToolContext& Context, TArray<FVector2D>& OutShape) const = 0;

	/**
	 * Whether the corner that would complete the shape (the LAST click, MaxPinned - 1) may be
	 * pinned. Every earlier click is unconditional - only the closing corner can fold the
	 * polygon or, for a rectangle, flatten it to zero depth.
	 */
	virtual bool CanCloseShape(TConstArrayView<FVector2D> Shown) const = 0;

	/** Try the commit against the model; INDEX_NONE on refusal - PlaceEntityInPlot or
	 *  PlaceStandInPlot, the two mutators this gesture may end in. */
	virtual int32 Place(const FToolContext& Context, const TArray<FVector2D>& Outline) const = 0;

	/**
	 * Extra preview content once all four corners of Shown exist: the rest of the boundary's
	 * styling (which edge is Pinned versus Provisional differs by tool - see each override),
	 * any guide line, and whatever is drawn INSIDE the shape (module footprints, the letter and
	 * its keep-out).
	 */
	virtual void Describe(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolPreviewSink& Sink) const = 0;

	/**
	 * The rest of the readout once Shown has at least two corners to measure: every fact past
	 * "N/M Points", every warning, and the Committable call - which predicate lights the Build
	 * button differs by tool (a kit total for the depot, WhyStandRefused for the stand), so
	 * there is no shared tail to factor out beyond the one fact BuildReadout already emits.
	 */
	virtual void DescribeReadout(const FToolContext& Context, TConstArrayView<FVector2D> Shown,
		IToolReadoutSink& Sink) const = 0;

	/**
	 * Extra Remove-preview content past the Doomed outline and RemoveLabel - the stand tool's
	 * "in use by aircraft N" (stand-occupancy spec §6). Silent by default, so the depot tool
	 * changes nothing by not overriding it.
	 */
	virtual void DescribeRemoveExtra(const FToolContext& Context, int32 Doomed,
		const FEntityInstance& Entity, IToolPreviewSink& Sink) const {}

	// --- Shared state -----------------------------------------------------------------------

	/** How many clicks complete the shape - 4 for a free quad, 3 for a rectangle whose fourth
	 *  corner is derived. Set once, at construction, by the derived tool. */
	const int32 MaxPinned;

	/** How many corners are pinned right now, 0 to MaxPinned. THE single source - see this
	 *  class's own comment on why this is an int rather than a second stage enum. */
	int32 Pinned = 0;

	/** Unit vector along the road at the anchor. */
	FVector2D Along = FVector2D(1.0, 0.0);

	/** Unit vector away from the road, on the side the cursor was when it anchored. */
	FVector2D Inward = FVector2D(0.0, 1.0);

	/**
	 * The corners, in the order they are pinned. Corners[0] IS the anchor, quantised onto the
	 * road's own step and stood off the kerb at the first click - see OnClick.
	 *
	 * ENTRIES PAST Pinned ARE STALE and must not be read outside Shape() overrides, which
	 * rebuild the moving one from the cursor every frame rather than trusting what is here -
	 * drawing a stale corner is how a ghost shows the PREVIOUS gesture's geometry, a bug both
	 * tools' own histories record having shipped once.
	 *
	 * SIZED FOR THE LARGER GESTURE (four) even though FStandPlotTool pins only three - its own
	 * fourth corner is derived by Shape(), never clicked, so Corners[3] there is simply unused
	 * rather than a smaller array the base would need a second size to describe.
	 */
	FVector2D Corners[4] = { FVector2D::ZeroVector, FVector2D::ZeroVector,
		FVector2D::ZeroVector, FVector2D::ZeroVector };

	/**
	 * OnCommit's own Place() call was refused for a shape DescribeReadout's Committable already
	 * agreed could not be placed - issue #182, kept as a safety net rather than the ordinary
	 * path for the reason each tool's own field used to give: Committable is computed from the
	 * SAME evaluator Place's mutator judges the commit against, so a refusal here should already
	 * have greyed the Build button. CLEARED ON EVERY GESTURE BOUNDARY (a fresh anchor, a cancel,
	 * a deactivate) so a stale refusal from one shape cannot bleed its warning onto the next.
	 */
	bool bLastCommitRefused = false;

	/**
	 * An outline-keyed memo, shared by whichever per-frame computation a derived tool needs to
	 * pay for at most once per distinct shape.
	 *
	 * MOVED UP FROM FPlotPlaceTool::FReservationMemo (issue #180) once the stand tool needed
	 * the identical SHAPE for a different payload: WhyStandRefused and StandBox::LetterOf,
	 * asked once by BuildPreview and again by BuildReadout for the SAME Shown outline, exactly
	 * the "solve done twice a frame" #180 already named for the depot's own packer call. Two
	 * structs copying {bValid, Outline[4]} into two sibling caches for two different payloads
	 * is the shape CLAUDE.md's "one struct per thing" rule means to catch - so the fields both
	 * tools needed are here once, and only the payload (a PlotYard::FReservation for one, a
	 * refusal string for the other) is theirs to add.
	 *
	 * EXACT EQUALITY, not a tolerance - Shape() either reproduces a pinned corner bit for bit or
	 * derives the moving one from the same cursor value MakeToolContext resolved this frame, so
	 * two calls describing the same frame's shape compare equal without help; see
	 * FPlotPlaceTool::ReservationFor's own comment, unchanged, for why a memo keyed any looser
	 * would risk answering last frame's question.
	 */
	template <typename PayloadT>
	struct TOutlineMemo
	{
		bool bValid = false;
		FVector2D Outline[4] = { FVector2D::ZeroVector, FVector2D::ZeroVector,
			FVector2D::ZeroVector, FVector2D::ZeroVector };
		PayloadT Payload{};

		/** Whether Candidate is the same four corners this memo already holds. */
		bool Matches(TConstArrayView<FVector2D> Candidate) const
		{
			return bValid && Candidate.Num() == 4
				&& Outline[0] == Candidate[0] && Outline[1] == Candidate[1]
				&& Outline[2] == Candidate[2] && Outline[3] == Candidate[3];
		}

		/** Replace the memo with Candidate's shape and InPayload. Candidate must have four
		 *  corners - the caller's own outline-completeness check, not this one's to repeat. */
		void Store(TConstArrayView<FVector2D> Candidate, PayloadT InPayload)
		{
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Outline[Index] = Candidate[Index];
			}
			Payload = MoveTemp(InPayload);
			bValid = true;
		}
	};

private:
	/** The entity under the cursor this tool would act on for Remove, or INDEX_NONE - the body
	 *  FPlotPlaceTool::PlotUnder and FStandPlotTool::StandUnder shared but for IsMine. */
	int32 EntityUnder(const FToolContext& Context) const;
};
