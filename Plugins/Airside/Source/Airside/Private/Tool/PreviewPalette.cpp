#include "Tool/PreviewPalette.h"

FLinearColor PreviewPalette::Default(EPreviewStyle Style)
{
	// Values as they stood split across ARoadBuildHUD's UPROPERTY defaults and
	// FViewportPreviewSink::Colour before this table - moved here verbatim so nothing on
	// screen shifts on this refactor, per the contract. Deliberately no `default:`: this
	// project does not build switches as exhaustive-or-error (UBT's SwitchWarningLevel is
	// off), so a missing case does not fail to compile - checkNoEntry() below is what
	// actually catches it, at the first frame that asks for the missing style's colour.
	switch (Style)
	{
	case EPreviewStyle::Pending:                     return FLinearColor(0.2f, 1.0f, 0.3f);
	case EPreviewStyle::Snap:                        return FLinearColor(1.0f, 0.9f, 0.15f);
	case EPreviewStyle::Doomed:                      return FLinearColor(1.0f, 0.15f, 0.1f);
	case EPreviewStyle::Heal:                        return FLinearColor(0.3f, 1.0f, 0.5f);
	case EPreviewStyle::Refused:                     return FLinearColor(1.0f, 0.25f, 0.2f);
	case EPreviewStyle::Guideline:                   return FLinearColor(0.35f, 0.45f, 0.6f);
	case EPreviewStyle::Route:                       return FLinearColor(0.2f, 0.85f, 1.0f);

	// Amber, because that is what one is painted on a real taxiway - and it reads as a
	// warning against the blue-grey guideline dots it sits among rather than as one more
	// piece of context.
	case EPreviewStyle::RunwayHoldingPosition:        return FLinearColor(1.0f, 0.8f, 0.1f);
	// The same amber at half strength: the player's line, lighter than the runway's.
	case EPreviewStyle::IntermediateHoldingPosition:  return FLinearColor(1.0f, 0.8f, 0.1f, 0.5f);

	// The editor's old switch had no cases for these two at all and fell through to
	// Pending's green for both - the exact bug this table exists to make impossible.
	case EPreviewStyle::Hover:                       return FLinearColor(1.0f, 1.0f, 1.0f);

	// WHITE, BOTH, and deliberately not the palette's greens. These say "decided" and "not
	// yet", which is a statement about the GESTURE rather than about whether the thing under
	// them is good or bad - Pending's green already carries that. Manor Lords draws its plot
	// boundary white for the same reason: on grass it is the one colour that always reads.
	case EPreviewStyle::Pinned:                      return FLinearColor(1.0f, 1.0f, 1.0f);
	case EPreviewStyle::Provisional:                 return FLinearColor(1.0f, 1.0f, 1.0f);
	// Warm, so it reads against the cyan route and grey nodes.
	case EPreviewStyle::Selected:                    return FLinearColor(1.0f, 0.75f, 0.2f);

	// DARK CYAN. A guide line is drawn TOUCHING a Provisional edge every frame it exists, so
	// white was out, and Snap's amber and Pending's green both already mean something a
	// gesture would DO - this means something the gesture is measured AGAINST.
	//
	// DARK, and that is the whole distinction from Route's bright cyan above: the two share a
	// hue, so the difference has to be value rather than colour. They are never drawn by the
	// same gesture (the plot tool draws no routes), which is what makes sharing a hue safe at
	// all; if a future tool draws both at once, this is the pair to re-check.
	case EPreviewStyle::Guide:                       return FLinearColor(0.0f, 0.55f, 0.55f);

	// VIOLET, a hue nothing else in this table uses. A handle is not something the gesture
	// would DO (Pending's green, Snap's amber) nor something it is measured against
	// (Guide's dark cyan) - it is a thing the player may take hold of, and it is drawn over
	// the graph's own orange, grey and blue node marks, so it has to be legible against all
	// three at once.
	case EPreviewStyle::Handle:                      return FLinearColor(0.8f, 0.4f, 1.0f);

	// GraphOverlay's context styles - the same colours ARoadBuildHUD::DrawNodes/DrawStands
	// used to wire to their own StubColour/EndColour/JunctionColour/StandColour/
	// ServiceAnchorColour UPROPERTYs, now the default ARoadBuildHUD::Looks is seeded from.
	case EPreviewStyle::NodeStub:                    return FLinearColor(1.0f, 0.55f, 0.1f);
	case EPreviewStyle::NodeThrough:                 return FLinearColor(0.85f, 0.85f, 0.85f);
	case EPreviewStyle::NodeJunction:                return FLinearColor(0.15f, 0.85f, 1.0f);
	case EPreviewStyle::StandPose:                   return FLinearColor(0.25f, 0.7f, 1.0f);
	// Where the service vehicles park - a consequence of where the aircraft sits, not a
	// thing that IS one (contrast StandPose).
	case EPreviewStyle::ServiceAnchor:                return FLinearColor(0.9f, 0.6f, 0.2f);
	}

	// Reached only if EPreviewStyle grew a value with no case above - not caught at compile
	// time here (see the top of this function), so this check is the actual backstop. Not a
	// `default:` return, which is what let this go quiet before.
	checkNoEntry();
	return FLinearColor::Black;
}

FPreviewLook PreviewPalette::DefaultLook(EPreviewStyle Style)
{
	FPreviewLook Look;
	Look.Colour = Default(Style);

	// Ratios frozen at the CURRENT defaults of the two shared sliders these styles used to
	// read directly - ARoadBuildHUD::NodeRingThickness (2.0) over PreviewThickness (3.0) for
	// the graph styles and ServiceAnchor, ServiceAnchorRadius (5.0) over NodeRingRadius (9.0)
	// for ServiceAnchor's radius - moved here verbatim so nothing on screen shifts, same
	// contract as Default() above. See FPreviewLook's own comment for why a ratio, not the
	// old shared float, is what a style reads from now on.
	constexpr float GraphThicknessScale = 2.0f / 3.0f;

	// No `default:` - see Default() above for why, and what actually catches a style added
	// to EPreviewStyle without a case here.
	switch (Style)
	{
	case EPreviewStyle::Pending:  Look.bDoubleRing = true; break;
	case EPreviewStyle::Snap:     break;
	case EPreviewStyle::Doomed:   Look.bDoubleRing = true; break;
	case EPreviewStyle::Heal:     break;
	case EPreviewStyle::Refused:  break;

	// Context, not intent: a dot rather than a ring, so hundreds of guideline nodes read as
	// background instead of swamping every gesture drawn over them. Also the Line() weight a
	// route runs under - ARoadBuildHUD::Line's own comment on why the route reads heavier.
	case EPreviewStyle::Guideline:
		Look.RadiusScale = 0.35f;
		Look.ThicknessScale = 0.5f;
		break;

	case EPreviewStyle::Route:
		Look.ThicknessScale = 2.0f;
		break;

	// THE SAME WEIGHT AS EACH OTHER, heavier than ordinary geometry so a plot boundary reads
	// over grass. Equal on purpose: the DASH is what distinguishes them, and a difference in
	// thickness as well would make the pair harder to compare rather than easier.
	case EPreviewStyle::Pinned:
		Look.ThicknessScale = 2.0f;
		break;

	case EPreviewStyle::Provisional:
		Look.ThicknessScale = 2.0f;
		break;

	// THINNER THAN THE BOUNDARY IT HELPS DRAW. Pinned and Provisional are 2.0 because a plot
	// edge has to read over grass; the guide is an aid to that edge, and at the same weight
	// it competes with the shape the player is actually making.
	case EPreviewStyle::Guide:
		Look.ThicknessScale = 1.0f;
		break;

	case EPreviewStyle::RunwayHoldingPosition:       break;
	case EPreviewStyle::IntermediateHoldingPosition: break;
	case EPreviewStyle::Hover:                       break;
	case EPreviewStyle::Selected:                    Look.bDoubleRing = true; break;

	case EPreviewStyle::NodeStub:
	case EPreviewStyle::NodeThrough:
	case EPreviewStyle::NodeJunction:
		Look.ThicknessScale = GraphThicknessScale;
		break;

	case EPreviewStyle::ServiceAnchor:
		Look.RadiusScale = 5.0f / 9.0f;
		Look.ThicknessScale = GraphThicknessScale;
		break;

	// DELIBERATELY not NodeRingRadius (1.0) - see ARoadBuildHUD::Marker's old comment on
	// this exact number, moved here: GraphOverlay::DescribeStands draws this AFTER
	// StandPreview::Describe's own Pending mark at the same position, and a ring at the same
	// radius would just overdraw it instead of sitting visibly alongside it.
	case EPreviewStyle::StandPose:
		Look.RadiusScale = 2.2f;
		Look.ThicknessScale = GraphThicknessScale;
		break;

	// SMALLER THAN Hover's ring (1.0), for the reason StandPose's comment gives just above:
	// FEditTool marks the handle under the cursor a second time as Hover, in the same frame
	// and at the same position, and equal radii would simply overdraw rather than read as
	// "grabbable, and this is the one". Heavier in thickness to stay visible at that size.
	case EPreviewStyle::Handle:
		Look.RadiusScale = 0.6f;
		Look.ThicknessScale = 1.5f;
		break;
	}

	return Look;
}
