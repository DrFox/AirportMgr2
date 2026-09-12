#include "Tool/RunwayTool.h"

#include "AirsideLog.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FRunwayTool::GetDisplayName() const
{
	return LOCTEXT("RunwayTool", "Runway");
}

URoadProfile* FRunwayTool::ProfileForWidth() const
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (Content == nullptr || Content->RunwayProfiles.Num() == 0)
	{
		return nullptr;
	}

	// Clamped rather than checked: the list is content, so it can be shorter than an index
	// left over from a longer one. Wrapping would silently lay a different width from the one
	// the HUD is showing.
	const int32 Index = FMath::Clamp(WidthIndex, 0, Content->RunwayProfiles.Num() - 1);
	return Content->RunwayProfiles[Index].LoadSynchronous();
}

void FRunwayTool::NextWidth(const FToolContext& Context)
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	const int32 Count = Content != nullptr ? Content->RunwayProfiles.Num() : 0;
	if (Count <= 0)
	{
		// SAID OUT LOUD. This used to return in silence, which is indistinguishable from a
		// key that never arrived: the player presses the tool's key again, the width does
		// not change, and nothing anywhere says why. A content set with no runway profiles
		// is a real state - it is what a fresh project has - and it deserves a line.
		UE_LOG(LogAirside, Warning,
			TEXT("Runway width unchanged: the content set declares no runway profiles, so "
				 "there is nothing to cycle through"));
		return;
	}

	WidthIndex = (WidthIndex + 1) % Count;

	// The width is otherwise visible ONLY in the drag preview, so a player who has not
	// started a drag has no way to tell whether the key did anything. One line per press,
	// naming the width in metres, is what makes "the key does nothing" answerable.
	const URoadProfile* Profile = ProfileForWidth();
	UE_LOG(LogAirside, Log, TEXT("Runway width -> %d of %d, %.0f m"),
		WidthIndex + 1, Count, Profile != nullptr ? Profile->GetTotalWidth() / 100.0 : 0.0);
}

void FRunwayTool::NextSurface()
{
	Surface = static_cast<ERunwaySurface>((static_cast<uint8>(Surface) + 1) % 4);
}

void FRunwayTool::NextApproach()
{
	Approach = static_cast<ERunwayApproach>((static_cast<uint8>(Approach) + 1) % 3);
}

FRunwayFacts FRunwayTool::Facts() const
{
	FRunwayFacts Out;
	Out.Surface = Surface;
	Out.Approach = Approach;
	return Out;
}

void FRunwayTool::OnReselect(const FToolContext& Context)
{
	// AT THE BOUNDARY, because "pressing the key again does nothing" has two causes that
	// look identical from the outside: the reselect never reached the tool, or it reached it
	// and cycled something else. This line distinguishes them before the next repro.
	UE_LOG(LogAirside, Log, TEXT("Runway tool reselected (remove=%d insert=%d)"),
		Context.bRemoveModifier ? 1 : 0, Context.bInsertModifier ? 1 : 0);

	// One choice per press, and the modifiers decide which. Remove wins over insert when
	// both are held only because something must; neither is a gesture anyone makes.
	if (Context.bRemoveModifier)
	{
		NextApproach();
	}
	else if (Context.bInsertModifier)
	{
		NextSurface();
	}
	else
	{
		NextWidth(Context);
	}
}

void FRunwayTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return;
	}

	// The RAW cursor, not the snap. A runway threshold is a place on the ground, not a point
	// on the road graph - snapping it to an existing node would drag a threshold onto a
	// taxiway junction, which is the one place a runway must never start. See FToolContext,
	// where the same distinction is drawn for the stand and route tools.
	if (!bHasThreshold)
	{
		Threshold = Context.Cursor;
		bHasThreshold = true;
		return;
	}

	// Cleared BEFORE the placement, so a refusal - too short, no profile - leaves the tool
	// idle rather than holding a threshold the player can no longer see the preview for.
	const FVector2D Far = Context.Cursor;
	bHasThreshold = false;

	Context.Target->PlaceRunway(Threshold, Far, ProfileForWidth(), Facts());
}

void FRunwayTool::OnCancel(const FToolContext& Context)
{
	bHasThreshold = false;
}

void FRunwayTool::OnDeactivate(const FToolContext& Context)
{
	bHasThreshold = false;
}

void FRunwayTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadProfile* Profile = ProfileForWidth();

	if (!bHasThreshold)
	{
		// Nothing placed yet: show where the threshold would go, and which width is armed, so
		// the choice is visible before it is committed rather than after.
		Sink.Marker(Context.Cursor, EPreviewStyle::Pending);
		if (Profile != nullptr)
		{
			// All three choices, so what the next click commits to is readable before it is
			// committed - "45 m, concrete, precision".
			Sink.Label(Context.Cursor,
				FString::Printf(TEXT("%.0f m, %s, %s"), Profile->GetTotalWidth() / 100.0,
					RunwaySurfaceName(Surface), RunwayApproachName(Approach)),
				EPreviewStyle::Pending);
		}
		else
		{
			Sink.Label(Context.Cursor, TEXT("no runway profile"), EPreviewStyle::Refused);
		}
		return;
	}

	const FVector2D Far = Context.Cursor;
	const FVector2D Along = Far - Threshold;
	const double Length = Along.Size();

	const double Minimum = Context.Target != nullptr
		? Context.Target->GetMinimumRunwayLength() : 0.0;
	const bool bLongEnough = Length >= Minimum;
	const EPreviewStyle Style = bLongEnough ? EPreviewStyle::Pending : EPreviewStyle::Refused;

	Sink.Marker(Threshold, Style);
	Sink.Marker(Far, Style);
	Sink.Line(Threshold, Far, Style);

	if (Profile != nullptr && Length > 0.0)
	{
		// Both edges, so the width being laid is visible rather than inferred from a label.
		const FVector2D Unit = Along / Length;
		const FVector2D Side = FVector2D(-Unit.Y, Unit.X) * Profile->GetHalfWidthLeft();
		Sink.Line(Threshold + Side, Far + Side, Style);
		Sink.Line(Threshold - Side, Far - Side, Style);
	}

	// A cross mark AT each threshold, along the runway - the piano keys go here, and it is
	// where the designator is painted.
	Sink.CrossMark(Threshold, Along, Style);
	Sink.CrossMark(Far, Along, Style);

	if (!bLongEnough)
	{
		Sink.Label(Far, FString::Printf(TEXT("too short: %.0f m, needs %.0f m"),
			Length / 100.0, Minimum / 100.0), EPreviewStyle::Refused);
		return;
	}

	// THE NUMBER AT EACH END, not the pair, because each threshold carries its own. Reading
	// "27" while standing at the far end is how you check the strip points where you meant.
	Sink.Label(Threshold, RunwayDesignator::ToText(RunwayDesignator::Designate(-Along)), Style);
	Sink.Label(Far, RunwayDesignator::ToText(RunwayDesignator::Designate(Along)), Style);

	if (Profile != nullptr)
	{
		Sink.Label((Threshold + Far) * 0.5,
			FString::Printf(TEXT("%s  %.0f x %.0f m, %s, %s"),
				*RunwayDesignator::ToPairText(Along),
				Length / 100.0, Profile->GetTotalWidth() / 100.0,
				RunwaySurfaceName(Surface), RunwayApproachName(Approach)),
			Style);
	}
}

#undef LOCTEXT_NAMESPACE
