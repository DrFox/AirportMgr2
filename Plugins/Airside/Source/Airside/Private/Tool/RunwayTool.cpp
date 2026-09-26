#include "Tool/RunwayTool.h"

#include "AirsideLog.h"
#include "Model/BuildPurse.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FRunwayTool::GetDisplayName() const
{
	return LOCTEXT("RunwayTool", "Runway");
}

URoadProfile* FRunwayTool::ProfileForWidth(const FToolContext& Context) const
{
	if (Context.Target == nullptr || Context.Target->GetRunwayProfileCount() <= 0)
	{
		return nullptr;
	}

	// Clamping is ResolveRunwayProfile's job now (see IRoadEditTarget) - still done there for
	// the same reason it was done here: the list is content, so it can be shorter than an
	// index left over from a longer one, and wrapping would silently resolve a different
	// width from the one the HUD is showing.
	return Context.Target->ResolveRunwayProfile(WidthIndex);
}

void FRunwayTool::NextWidth(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		// A DIFFERENT REFUSAL from an empty content set, said differently: this is a caller
		// bug (a reselect wired without resolving a target first - see
		// URoadBuildEdMode::StartToolAction and URoadBuildEditorTool::Setup for the two
		// places this was missing, issue #78's review), not a fresh, unconfigured project.
		// The single message this used to share with the empty-content case would have
		// blamed the content set for what was actually a null Context.Target - exactly the
		// misdiagnosis this branch exists to prevent.
		UE_LOG(LogAirside, Warning,
			TEXT("Runway width unchanged: no edit target in context, so there is nothing to ask for widths"));
		return;
	}

	const int32 Count = Context.Target->GetRunwayProfileCount();
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

	StepAxis(Context, TEXT("Width"));
}

void FRunwayTool::NextSurface(const FToolContext& Context)
{
	StepAxis(Context, TEXT("Surface"));
}

void FRunwayTool::NextApproach(const FToolContext& Context)
{
	StepAxis(Context, TEXT("Approach"));
}

void FRunwayTool::StepAxis(const FToolContext& Context, FName AxisId)
{
	// THROUGH THE ROWS, BY ID, so the key walks the same list the bar draws and in its order.
	TArray<FToolVariantAxis> Axes;
	GetVariantAxes(Context, Axes);
	const int32 Axis = Axes.IndexOfByPredicate([AxisId](const FToolVariantAxis& A) { return A.Id == AxisId; });
	const int32 Next = Axis != INDEX_NONE ? NextEnabledVariant(Axes[Axis]) : INDEX_NONE;
	if (Next == INDEX_NONE)
	{
		return;
	}
	SelectVariant(Context, Axis, Next);
}

void FRunwayTool::GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const
{
	const int32 WidthCount = Context.Target != nullptr ? Context.Target->GetRunwayProfileCount() : 0;
	if (WidthCount > 0)
	{
		FToolVariantAxis& Width = Out.AddDefaulted_GetRef();
		Width.Id = TEXT("Width");
		Width.Label = LOCTEXT("RunwayAxisWidth", "Width");
		// Clamped for ProfileForWidth's reason: the list is content, and may have shrunk under
		// an index left from a longer one - the row must light the width that will be laid.
		Width.Current = FMath::Clamp(WidthIndex, 0, WidthCount - 1);
		for (int32 Index = 0; Index < WidthCount; ++Index)
		{
			const URoadProfile* Profile = Context.Target->ResolveRunwayProfile(Index);
			const double Metres = Profile != nullptr ? Profile->GetTotalWidth() : 0.0;
			FToolVariant& Option = Width.Options.AddDefaulted_GetRef();
			Option.Id = FName(*FString::Printf(TEXT("W%d"), FMath::RoundToInt(Metres)));
			Option.Label = VariantWidthLabel(Metres);
		}
	}

	// THE ENUMS' OWN NAMES, from RunwayFacts.h - the same strings the drag readout prints, so
	// the row and the readout cannot name one surface two ways.
	FToolVariantAxis& SurfaceAxis = Out.AddDefaulted_GetRef();
	SurfaceAxis.Id = TEXT("Surface");
	SurfaceAxis.Label = LOCTEXT("RunwayAxisSurface", "Surface");
	SurfaceAxis.Current = static_cast<int32>(Surface);
	for (uint8 Each = 0; Each < static_cast<uint8>(ERunwaySurface::Count); ++Each)
	{
		const TCHAR* Name = RunwaySurfaceName(static_cast<ERunwaySurface>(Each));
		FToolVariant& Option = SurfaceAxis.Options.AddDefaulted_GetRef();
		Option.Id = Name;
		Option.Label = FText::FromString(Name);
	}

	FToolVariantAxis& ApproachAxis = Out.AddDefaulted_GetRef();
	ApproachAxis.Id = TEXT("Approach");
	ApproachAxis.Label = LOCTEXT("RunwayAxisApproach", "Approach");
	ApproachAxis.Current = static_cast<int32>(Approach);
	for (uint8 Each = 0; Each < static_cast<uint8>(ERunwayApproach::Count); ++Each)
	{
		const TCHAR* Name = RunwayApproachName(static_cast<ERunwayApproach>(Each));
		FToolVariant& Option = ApproachAxis.Options.AddDefaulted_GetRef();
		Option.Id = Name;
		Option.Label = FText::FromString(Name);
	}
}

bool FRunwayTool::SelectVariant(const FToolContext& Context, int32 Axis, int32 Option)
{
	// ROWS ARE NUMBERED AS GetVariantAxes NUMBERED THEM, and that numbering shifts when there are
	// no runway profiles (no Width row). Resolving the index to an Id through the same function
	// is what keeps a click on "Surface" from setting the approach on an unconfigured project.
	TArray<FToolVariantAxis> Axes;
	GetVariantAxes(Context, Axes);
	if (!Axes.IsValidIndex(Axis) || !Axes[Axis].Options.IsValidIndex(Option)
		|| !Axes[Axis].Options[Option].bEnabled)
	{
		return false;
	}

	const FName Id = Axes[Axis].Id;
	if (Id == TEXT("Width"))
	{
		WidthIndex = Option;

		// The width is otherwise visible ONLY in the drag preview, so a player who has not
		// started a drag has no way to tell whether the key did anything. One line per press,
		// naming the width in metres, is what makes "the key does nothing" answerable. MOVED
		// HERE from NextWidth so a bar click logs it too.
		const URoadProfile* Profile = ProfileForWidth(Context);
		UE_LOG(LogAirside, Log, TEXT("Runway width -> %d of %d, %.0f m"),
			WidthIndex + 1, Axes[Axis].Options.Num(), Profile != nullptr ? Profile->GetTotalWidth() / 100.0 : 0.0);
	}
	else if (Id == TEXT("Surface"))
	{
		Surface = static_cast<ERunwaySurface>(Option);
		UE_LOG(LogAirside, Log, TEXT("Runway surface -> %s"), RunwaySurfaceName(Surface));
	}
	else
	{
		Approach = static_cast<ERunwayApproach>(Option);
		UE_LOG(LogAirside, Log, TEXT("Runway approach -> %s"), RunwayApproachName(Approach));
	}
	return true;
}

FRunwayFacts FRunwayTool::Facts() const
{
	FRunwayFacts Out;
	Out.Surface = Surface;
	Out.Approach = Approach;
	return Out;
}

bool FRunwayTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// A RUNWAY'S CURSOR IS ITS CENTRELINE, like a road's - the strip is laid either side of the
	// line between the two thresholds. So an apron edge guide displaces by its half-width.
	Out.Point = EDragPoint::Centreline;

	// THE WIDTH THIS CLICK WOULD LAY. Clamped by the target, which owns the standard set - the
	// tool holds an INDEX into it and nothing more (see WidthIndex).
	//
	// FILLED BEFORE THE DECLINE BELOW: the FIRST threshold has a width too, and a strip laid
	// flush along an apron edge is displaced by exactly this figure on the free start as much
	// as on the second click.
	if (Target != nullptr)
	{
		if (const URoadProfile* Profile = Target->ResolveRunwayProfile(WidthIndex))
		{
			Out.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Out.HalfWidthRight = Profile->GetHalfWidthRight();
		}
	}

	// NO THRESHOLD YET MEANS NO POINT FOR A LINE TO SWING AROUND - so the base answers with a
	// FREE START instead, putting the cursor there and leaving exactly the positional guides.
	// Placing a threshold in line with another runway, or a standard separation off it, is what
	// that buys; ruled 2026-09-20. DELEGATING rather than returning false is what opts this
	// tool in - see IBuildTool::DescribeGuideAnchor.
	if (!bHasThreshold)
	{
		return IBuildTool::DescribeGuideAnchor(Network, Target, Out);
	}

	Out.Origin = Threshold;
	return true;
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
		NextApproach(Context);
	}
	else if (Context.bInsertModifier)
	{
		NextSurface(Context);
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

	// NOT THE SNAP. A runway threshold is a place on the ground, not a point on the road graph -
	// snapping it to an existing node would drag a threshold onto a taxiway junction, which is
	// the one place a runway must never start. See FToolContext, where the same distinction is
	// drawn for the stand and route tools.
	//
	// THE GUIDE IS NOT THE SNAP, and this used to read Context.Cursor for both. GuidedCursor
	// returns a point on the PLANE, constrained to a line the player can see - never a node off
	// the graph - so it keeps the rule above while letting a runway be squared to an existing
	// one. A guide drawn and then not obeyed is a mark whose meaning has gone.
	if (!bHasThreshold)
	{
		Threshold = Context.GuidedCursor();
		bHasThreshold = true;
		return;
	}

	// Cleared BEFORE the placement, so a refusal - too short, no profile - leaves the tool
	// idle rather than holding a threshold the player can no longer see the preview for.
	const FVector2D Far = Context.GuidedCursor();
	bHasThreshold = false;

	Context.Target->PlaceRunway(Threshold, Far, ProfileForWidth(Context), Facts());
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
	const URoadProfile* Profile = ProfileForWidth(Context);

	if (!bHasThreshold)
	{
		// Nothing placed yet: show where the threshold would go, and which width is armed, so
		// the choice is visible before it is committed rather than after.
		Sink.Marker(Context.GuidedCursor(), EPreviewStyle::Pending);

		// AND WHAT THE THRESHOLD IS LINED UP WITH. This was a deliberate no-op until 2026-09-20 -
		// DescribeGuideAnchor declined before the first threshold, and the call was kept only so
		// the two branches could not drift. It DRAWS now: the free start gives the first click
		// its own guides, and this is the consumer that makes them visible. A call kept honest
		// for months turned out to be the whole of the second half of that feature.
		if (Context.Guide.bActive)
		{
			Sink.Guides(Context.Guide, Context.GuidedCursor());
		}
		if (Profile != nullptr)
		{
			// All three choices, so what the next click commits to is readable before it is
			// committed - "45 m, concrete, precision".
			Sink.Label(Context.GuidedCursor(),
				FString::Printf(TEXT("%.0f m, %s, %s"), Profile->GetTotalWidth() / 100.0,
					RunwaySurfaceName(Surface), RunwayApproachName(Approach)),
				EPreviewStyle::Pending);
		}
		else
		{
			Sink.Label(Context.GuidedCursor(), TEXT("no runway profile"), EPreviewStyle::Refused);
		}
		return;
	}

	const FVector2D Far = Context.GuidedCursor();
	if (Context.Guide.bActive)
	{
		Sink.Guides(Context.Guide, Far);
	}

	const FVector2D Along = Far - Threshold;
	const double Length = Along.Size();

	const double Minimum = Context.Target != nullptr
		? Context.Target->GetMinimumRunwayLength() : 0.0;
	const bool bLongEnough = Length >= Minimum;

	// THE PRICE BEFORE THE CLICK - RoadDrawTool's rule, for runways. Until 2026-09-26 a strip the
	// ledger would refuse was drawn exactly like one it would take, and the click then did
	// nothing but write a log line; in PIE that read as "visual and precision runways don't
	// build", the approach being whatever happened to be set when the money ran out. Priced
	// through the TARGET, which PlaceRunway charges through too, so the two cannot disagree.
	const IBuildPurse* Purse = Context.Target != nullptr ? Context.Target->GetPurse() : nullptr;
	const FBuildQuote Quote = Purse != nullptr && Profile != nullptr
		? Context.Target->QuoteForRunway(Threshold, Far, Profile) : FBuildQuote();
	const bool bAffordable = Quote.IsFree() || Purse->CanAfford(Quote);

	const EPreviewStyle Style = bLongEnough && bAffordable ? EPreviewStyle::Pending : EPreviewStyle::Refused;

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

	// SAID, not only coloured: red alone cannot tell "too expensive" from any other refusal.
	// The purse formats the money, so no currency symbol enters this plugin.
	if (!Quote.IsFree())
	{
		const FString Price = Purse->Describe(Quote).ToString();
		Sink.Label(Far + FVector2D(0.0, -Profile->GetTotalWidth()),
			bAffordable ? Price : FString::Printf(TEXT("can't afford: %s"), *Price), Style);
	}
}

#undef LOCTEXT_NAMESPACE
