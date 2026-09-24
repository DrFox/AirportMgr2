#include "Tool/SnapGuideLabel.h"

#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Tool/RoadNaming.h"
#include "Tool/SnapGuideChain.h"

namespace
{
	/** See SnapGuide::DescribeCallCountForTest. A free variable behind the namespace functions
	 *  that read and reset it, matching RouteSearch::GNodeVisitCountForTest's own reasoning:
	 *  Describe is a free function, not a member, so there is no object to hang a static on. */
	int32 GDescribeCallCountForTest = 0;

	/**
	 * <Name> FOR A LABEL - the one place every ELabelSubject is resolved, so Describe's switch on
	 * ELabelKind below can read Name once rather than re-deriving it per template.
	 *
	 * SEGMENT'S HANDLE COMES FROM Network.SegmentIdAt, NEVER hand-assembled from the label's bare
	 * index and a carried Generation: see FGuideLabel::SegmentIndex's own comment on why that is
	 * the one same rule RoadSlot::HandleAt enforces everywhere else, not an exception to it.
	 */
	FString NameFor(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const SnapGuide::FGuideLabel& Label)
	{
		switch (Label.Subject)
		{
		case SnapGuide::ELabelSubject::Segment:
			return RoadNaming::Describe(Network, Network.SegmentIdAt(Label.SegmentIndex));

		case SnapGuide::ELabelSubject::GestureReference:
			return Anchor.ReferenceName;

		case SnapGuide::ELabelSubject::GesturePoint:
			// HONOURED, NOT ASSUMED: an index a stale AlignTo array no longer has is a defect in
			// whichever caller built the label, and "" says so on screen instead of reading a
			// neighbour's name or crashing.
			return Anchor.AlignTo.IsValidIndex(Label.SubjectIndex)
				? Anchor.AlignTo[Label.SubjectIndex].Name
				: FString();

		case SnapGuide::ELabelSubject::Entity:
			return Network.GetEntities().IsValidIndex(Label.SubjectIndex)
				? EntityNaming::Describe(Network.GetEntities()[Label.SubjectIndex])
				: FString();

		case SnapGuide::ELabelSubject::ApronEdge:
			// NO NAME OF ITS OWN - FApronSurface carries a material slot and nothing a player
			// would read, exactly as FApronGuideSource's own header says.
			return TEXT("the apron edge");

		case SnapGuide::ELabelSubject::ApronCorner:
			return TEXT("the apron corner");

		case SnapGuide::ELabelSubject::EntityEdge:
		case SnapGuide::ELabelSubject::EntityCorner:
		{
			// HONOURED, NOT ASSUMED, like GesturePoint above: a stale index says so with "".
			if (!Network.GetEntities().IsValidIndex(Label.SubjectIndex))
			{
				return FString();
			}
			const FEntityInstance& Entity = Network.GetEntities()[Label.SubjectIndex];
			const bool bCorner = Label.Subject == SnapGuide::ELabelSubject::EntityCorner;

			// BY KIND, which the instance captured at placement - a Model/ fact, no definition
			// read. The last word is EntityNaming's own for an unnamed thing, and is unreachable
			// while ForEachOutlineEdge walks only stands and depots.
			const TCHAR* Kind = Entity.IsDepot() ? TEXT("fuel depot")
				: Entity.IsStand() ? TEXT("stand")
				: TEXT("installation");
			return FString::Printf(TEXT("the %s's %s"), Kind, bCorner ? TEXT("corner") : TEXT("edge"));
		}

		default:
			return FString();
		}
	}
}

FString SnapGuide::Describe(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	const FGuideLabel& Label)
{
	// COUNTED FIRST, unconditionally - see DescribeCallCountForTest's own comment. The #183
	// regression is that this must run at most twice per Resolve(), however many candidates
	// Propose gathered, so an early-out below must not hide a call that happened anyway.
	++GDescribeCallCountForTest;

	if (Label.Kind == ELabelKind::Literal)
	{
		return Label.Text != nullptr ? FString(Label.Text) : FString();
	}

	const FString Name = NameFor(Network, Anchor, Label);

	switch (Label.Kind)
	{
	case ELabelKind::Along:
		// "%s %s" - AddDirections' 0-degree row and FExtendingGuideSource's Parallel candidate,
		// which is the same shape with Verb fixed to "along".
		return FString::Printf(TEXT("%s %s"), Label.Verb, *Name);

	case ELabelKind::SquareTo:
		return FString::Printf(TEXT("square to %s"), *Name);

	case ELabelKind::DegreesTo:
		// "0 degrees to %s" AT Degrees == 0 is the SAME STRING FPointAlignGuideSource's Level
		// used to Printf directly - see ELabelKind's own comment on why this is one member.
		return FString::Printf(TEXT("%d degrees to %s"), Label.Degrees, *Name);

	case ELabelKind::AngledFromEnd:
		return FString::Printf(TEXT("%d degrees from the end of %s"), Label.Degrees, *Name);

	case ELabelKind::InLineWith:
		return FString::Printf(TEXT("in line with %s"), *Name);

	case ELabelKind::EdgeFlushWith:
		return FString::Printf(TEXT("edge flush with %s"), *Name);

	case ELabelKind::LevelWith:
		return FString::Printf(TEXT("level with %s"), *Name);

	case ELabelKind::MatchingGap:
		// /100.0: GapUu is uu and 100 uu is 1 m, the same conversion FOffsetGuideSource's Printf
		// used to do inline.
		return FString::Printf(TEXT("%.0f m, matching %s"), Label.GapUu / 100.0, *Name);

	default:
		// UNREACHABLE - ELabelKind::Literal returned above, and every other member is a case
		// here. A future member landing without a case would fall through to this rather than
		// silently mislabel a candidate.
		return FString();
	}
}

int32 SnapGuide::DescribeCallCountForTest() { return GDescribeCallCountForTest; }
void SnapGuide::ResetDescribeCallCountForTest() { GDescribeCallCountForTest = 0; }
