#include "UI/UiClicks.h"

#include "Blueprint/UserWidget.h"
#include "RoadBuildLog.h"

namespace
{
	/** Whether W's OWN rectangle takes hits - Visible - as opposed to only its children's. */
	bool HitsItself(const UWidget* W)
	{
		return W != nullptr && W->GetVisibility() == ESlateVisibility::Visible;
	}
}

FReply UiClicks::EatUnhandled(const UUserWidget& Widget, const UWidget* Root, FReply Reply, const TCHAR* What)
{
	if (Reply.IsEventHandled())
	{
		return Reply;   // a button (or a Blueprint) took it - its own answer stands
	}
	if (HitsItself(&Widget) || HitsItself(Root))
	{
		return Reply;
	}
	// AT THE BOUNDARY, so "a click on the bar drew a road" is answerable from the log: this
	// line present means the panel stopped it, absent means it never reached the panel.
	UE_LOG(LogRoadBuild, Log, TEXT("%s: mouse %s stopped at the panel"), *Widget.GetName(), What);
	return FReply::Handled();
}
