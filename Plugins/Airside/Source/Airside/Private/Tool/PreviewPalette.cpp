#include "Tool/PreviewPalette.h"

FLinearColor PreviewPalette::Default(EPreviewStyle Style)
{
	// Values as they stood split across ARoadBuildHUD's UPROPERTY defaults and
	// FViewportPreviewSink::Colour before this table - moved here verbatim so nothing on
	// screen shifts on this refactor, per the contract. Deliberately no `default:`; see the
	// header for why a missing case must fail to compile rather than fall through.
	switch (Style)
	{
	case EPreviewStyle::Pending:                     return FLinearColor(0.2f, 1.0f, 0.3f);
	case EPreviewStyle::Snap:                        return FLinearColor(1.0f, 0.9f, 0.15f);
	case EPreviewStyle::Doomed:                      return FLinearColor(1.0f, 0.15f, 0.1f);
	case EPreviewStyle::Heal:                        return FLinearColor(0.3f, 1.0f, 0.5f);
	case EPreviewStyle::Refused:                     return FLinearColor(1.0f, 0.25f, 0.2f);
	case EPreviewStyle::Guideline:                   return FLinearColor(0.35f, 0.45f, 0.6f);
	case EPreviewStyle::Route:                       return FLinearColor(0.2f, 0.85f, 1.0f);
	case EPreviewStyle::RunwayHoldingPosition:        return FLinearColor(1.0f, 0.8f, 0.1f);
	case EPreviewStyle::IntermediateHoldingPosition:  return FLinearColor(1.0f, 0.8f, 0.1f, 0.5f);

	// The editor's old switch had no cases for these two at all and fell through to
	// Pending's green for both - the exact bug this table exists to make impossible.
	case EPreviewStyle::Hover:                       return FLinearColor(1.0f, 1.0f, 1.0f);
	case EPreviewStyle::Selected:                    return FLinearColor(1.0f, 0.75f, 0.2f);

	// GraphOverlay's context styles - the same colours ARoadBuildHUD::DrawNodes/DrawStands
	// wired to StubColour/EndColour/JunctionColour/StandColour/ServiceAnchorColour, now the
	// default those designer-overridable UPROPERTYs are seeded from.
	case EPreviewStyle::NodeStub:                    return FLinearColor(1.0f, 0.55f, 0.1f);
	case EPreviewStyle::NodeThrough:                 return FLinearColor(0.85f, 0.85f, 0.85f);
	case EPreviewStyle::NodeJunction:                return FLinearColor(0.15f, 0.85f, 1.0f);
	case EPreviewStyle::StandPose:                   return FLinearColor(0.25f, 0.7f, 1.0f);
	case EPreviewStyle::ServiceAnchor:                return FLinearColor(0.9f, 0.6f, 0.2f);
	}

	// Unreachable while the switch above stays exhaustive - the compiler warns on a style
	// added without a case. Not a `default:` return, which is what let this go quiet before.
	checkNoEntry();
	return FLinearColor::Black;
}
