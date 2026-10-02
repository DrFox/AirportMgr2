#include "UI/UiSparkline.h"

#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"
#include "UIStyle.h"

/** The painter. Holds no model: UUiSparkline pushes it a copy of the series and the two colours. */
class SUiSparkline : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SUiSparkline) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&) {}

	void Set(const TArray<double>& InValues, const FLinearColor& InLine, const FLinearColor& InBaseline)
	{
		Values = InValues;
		Line = InLine;
		Baseline = InBaseline;
		Invalidate(EInvalidateWidgetReason::Paint);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Clip,
		FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle& WidgetStyle, bool bParentEnabled) const override
	{
		const FVector2D Size = FVector2D(Geo.GetLocalSize());
		// The 50% baseline first, so the line draws over it.
		const TArray<FVector2D> Mid = { FVector2D(0.0, Size.Y * 0.5), FVector2D(Size.X, Size.Y * 0.5) };
		FSlateDrawElement::MakeLines(Out, LayerId, Geo.ToPaintGeometry(), Mid, ESlateDrawEffect::None, Baseline, true, 1.0f);

		TArray<FVector2D> Points;
		UUiSparkline::LayOut(Values, Size, Points);
		if (Points.Num() >= 2)
		{
			FSlateDrawElement::MakeLines(Out, LayerId + 1, Geo.ToPaintGeometry(), Points, ESlateDrawEffect::None, Line, true, 2.0f);
		}
		else if (Points.Num() == 1)
		{
			// One point has no line to draw; a short tick stands in for the dot.
			const TArray<FVector2D> Dot = { Points[0] - FVector2D(2.0, 0.0), Points[0] + FVector2D(2.0, 0.0) };
			FSlateDrawElement::MakeLines(Out, LayerId + 1, Geo.ToPaintGeometry(), Dot, ESlateDrawEffect::None, Line, true, 4.0f);
		}
		return LayerId + 1;
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return UUiSparkline::DesiredSize(); }

private:
	TArray<double> Values;
	FLinearColor Line = FLinearColor::White;
	FLinearColor Baseline = FLinearColor::Gray;
};

void UUiSparkline::LayOut(TArrayView<const double> Values01, FVector2D Size, TArray<FVector2D>& OutPoints)
{
	OutPoints.Reset();
	const int32 N = Values01.Num();
	for (int32 i = 0; i < N; ++i)
	{
		const double X = N == 1 ? Size.X * 0.5 : Size.X * i / (N - 1);
		const double Y = Size.Y * (1.0 - FMath::Clamp(Values01[i], 0.0, 1.0));
		OutPoints.Add(FVector2D(X, Y));
	}
}

void UUiSparkline::SetValues(TArrayView<const double> Values01)
{
	Values = TArray<double>(Values01.GetData(), Values01.Num());
	PushToSlate();
}

void UUiSparkline::SetStyle(const UUIStyle* InStyle)
{
	Style = InStyle;
	PushToSlate();
}

void UUiSparkline::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	Slate.Reset();
}

TSharedRef<SWidget> UUiSparkline::RebuildWidget()
{
	Slate = SNew(SUiSparkline);
	PushToSlate();
	return Slate.ToSharedRef();
}

void UUiSparkline::PushToSlate()
{
	if (!Slate.IsValid())
	{
		return;
	}
	const UUIStyle* S = Style != nullptr ? Style.Get() : GetDefault<UUIStyle>();
	Slate->Set(Values, S->Ink, S->InkMuted);
}
