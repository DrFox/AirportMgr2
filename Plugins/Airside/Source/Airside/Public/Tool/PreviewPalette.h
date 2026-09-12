#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

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
}
