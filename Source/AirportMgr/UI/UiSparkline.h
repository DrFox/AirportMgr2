#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "UiSparkline.generated.h"

class SUiSparkline;
class UUIStyle;

/**
 * A tiny trend line (Airlines window, Task 6): 1-7 values on a FIXED 0..1 scale with a faint 50%
 * baseline. Fixed, not auto-scaled, because the figure is a satisfaction share - an auto-fit axis
 * would make a flat 0.52 week look as dramatic as a collapse from 0.9 to 0.1.
 *
 * A UWidget over a private SLeafWidget (not a UUserWidget like its siblings): it has no children
 * and must draw lines, which UMG has no stock widget for. Colour is by meaning from UUIStyle
 * (Ink for the line, InkMuted for the baseline), never a literal.
 */
UCLASS()
class AIRPORTMGR_API UUiSparkline : public UWidget
{
	GENERATED_BODY()

public:
	/** Replaces the series. Each value is clamped to 0..1 when laid out, so callers need not. */
	void SetValues(TArrayView<const double> Values01);
	void SetStyle(const UUIStyle* InStyle);

	/**
	 * The geometry, pure so a test can pin it without painting: values clamped to 0..1, x spread
	 * evenly across Size.X (a lone value sits at mid x), y = 0 at the TOP so 1 maps to y 0 - Slate's
	 * screen space, not a chart's.
	 */
	static void LayOut(TArrayView<const double> Values01, FVector2D Size, TArray<FVector2D>& OutPoints);

	static FVector2D DesiredSize() { return FVector2D(120.0, 28.0); }

	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void PushToSlate();

	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	TArray<double> Values;
	TSharedPtr<SUiSparkline> Slate;
};
