#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"
#include "PreviewPalette.generated.h"

/**
 * A style's colour plus how it draws: everything ARoadBuildHUD::Marker/Line/CrossMark/Label
 * used to work out per-style through a StyleColour switch AND a chain of if/else on Style
 * for ring radius, ring thickness and line weight (#104) - now one row of data instead of
 * three parallel places that had to name the same style to agree.
 *
 * RadiusScale multiplies ARoadBuildHUD::NodeRingRadius; ThicknessScale multiplies
 * ARoadBuildHUD::PreviewThickness - those two stay the shared sliders every style answers
 * to, so a style that used to read its own separate knob (NodeRingThickness for the graph
 * styles, ServiceAnchorRadius for the anchor ring) now has that knob's CURRENT DEFAULT baked
 * in as a ratio here instead - nothing on screen moves, but retuning one style no longer
 * moves its neighbours the way a shared float did.
 */
USTRUCT()
struct FPreviewLook
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FLinearColor Colour = FLinearColor::White;

	UPROPERTY(EditAnywhere)
	float RadiusScale = 1.0f;

	UPROPERTY(EditAnywhere)
	float ThicknessScale = 1.0f;

	/** A second, larger ring at the same colour - Doomed/Pending/Selected's emphasis ring. */
	UPROPERTY(EditAnywhere)
	bool bDoubleRing = false;
};

/**
 * The canonical colour for each EPreviewStyle - the ONE typing of this table.
 *
 * Tool/ otherwise never speaks in colours - see EPreviewStyle's own comment - but the HUD
 * and the editor viewport are two independent IToolPreviewSink implementations that must
 * still agree, pixel for pixel, on what a style looks like. "Agree" enforced only by two
 * people reading each other's switch statement is exactly the failure mode this issue was
 * filed against: the HUD's colour table and the editor's got typed twice, once per sink,
 * and the editor's got a `default:` that silently mapped an unhandled style to Pending's
 * green. A SHARED DEFAULT two presentation layers both read is not itself presentation, any
 * more than GuidelineOverlay's geometry is - it is the one place "these must match" lives,
 * once, instead of in two switch statements a reader has to compare by eye.
 *
 * Default(), not a constant table, because ARoadBuildHUD's UPROPERTYs still let a designer
 * override any one of these per level - this only supplies the value nobody has touched.
 */
namespace PreviewPalette
{
	/**
	 * No `default:` in the implementation - see the .cpp for what actually catches a style
	 * added to EPreviewStyle without a case here (this project does not build switches as
	 * exhaustive-or-error, so it is not the compiler).
	 */
	AIRSIDE_API FLinearColor Default(EPreviewStyle Style);

	/**
	 * Default(Style) plus the radius/thickness/double-ring facts ARoadBuildHUD needs -
	 * see FPreviewLook. Colour still comes from Default() so the editor viewport's own call
	 * to Default() and the HUD's Looks table can never disagree on what a style's colour is;
	 * only the HUD reads the rest.
	 */
	AIRSIDE_API FPreviewLook DefaultLook(EPreviewStyle Style);
}
