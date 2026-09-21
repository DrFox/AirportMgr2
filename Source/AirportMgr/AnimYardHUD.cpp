#include "AnimYardHUD.h"

#include "AnimYard.h"
#include "AnimYardController.h"
#include "AnimYardMotion.h"

#include "Animation/SkeletalMeshActor.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

namespace
{
	constexpr float LeftMargin = 24.0f;
	constexpr float TopMargin = 24.0f;
	constexpr float LineHeight = 18.0f;

	const FLinearColor Heading(0.95f, 0.95f, 0.95f);
	const FLinearColor Body(0.75f, 0.78f, 0.82f);
	const FLinearColor Caret(1.0f, 0.82f, 0.25f);
	const FLinearColor Quiet(0.45f, 0.47f, 0.50f);

	/**
	 * A channel's value as a bar, so a sweep is readable at a glance.
	 *
	 * TEXT AND NOT A DRAWN RECTANGLE. The whole readout is one font and one DrawText call per
	 * line; a drawn bar would need its own coordinates to stay in step with the text beside it,
	 * and this is a debug panel whose only requirement is that it can be read in a screenshot.
	 */
	FString Meter(double Value, double Min, double Max)
	{
		constexpr int32 Cells = 20;
		const double Span = Max - Min;
		const int32 At = Span > 0.0
			? FMath::Clamp(FMath::RoundToInt((Value - Min) / Span * (Cells - 1)), 0, Cells - 1)
			: 0;

		FString Bar;
		for (int32 Cell = 0; Cell < Cells; ++Cell)
		{
			Bar += Cell == At ? TEXT("O") : TEXT("-");
		}
		return Bar;
	}
}

void AAnimYardHUD::DrawHUD()
{
	Super::DrawHUD();

	AAnimYardController* Controller = Cast<AAnimYardController>(PlayerOwner);
	if (Controller == nullptr || Canvas == nullptr)
	{
		return;
	}

	UFont* Font = GEngine != nullptr ? GEngine->GetMediumFont() : nullptr;
	if (Font == nullptr)
	{
		return;
	}

	float Y = TopMargin;
	const auto Line = [this, Font, &Y](const FLinearColor& Colour, const FString& Text)
	{
		DrawText(Text, Colour, LeftMargin, Y, Font);
		Y += LineHeight;
	};

	const AAnimYard* Yard = Controller->Yard();
	if (Yard == nullptr)
	{
		Line(Caret, TEXT("No AAnimYard in this level - nothing will move."));
		Line(Body, TEXT("Re-run Tools/Python/build_model_yard.py to place one."));
		return;
	}

	const FYardMotion& Bench = Yard->GetMotion();

	Line(Heading, FString::Printf(TEXT("%s    stage: %s    loop %.1f / %.0fs"),
		Bench.bPaused ? TEXT("|| PAUSED") : TEXT(">  RUNNING"),
		FYardMotion::StageName(Bench.CurrentStage()),
		Bench.LoopTime, FYardMotion::LoopSeconds()));
	Y += LineHeight * 0.5f;

	// THE CHANNELS, FROM THE SAME LIST Tab WALKS. A hand-written set of rows here would be a
	// second list, and the one it disagreed with would be the one with the new channel in it.
	for (const EYardChannel Channel : FYardMotion::Channels())
	{
		double Min = 0.0;
		double Max = 0.0;
		Bench.ChannelRange(Channel, Min, Max);

		const bool bIsCaret = Channel == Controller->Caret();
		Line(bIsCaret ? Caret : Body, FString::Printf(TEXT("%s %-9s %10.2f   %s"),
			bIsCaret ? TEXT(">") : TEXT(" "),
			FYardMotion::ChannelName(Channel),
			Bench.Value(Channel),
			*Meter(Bench.Value(Channel), Min, Max)));
	}

	Line(Body, FString::Printf(TEXT("  %-9s %10s"), TEXT("wheels"),
		Bench.bAirborne ? TEXT("airborne") : TEXT("on ground")));

	// THE UNDRIVEN MODELS, NAMED. Three of the four ground vehicles have no Animation Blueprint
	// at all today, so a still model in this yard is usually correct - and a bench that left you
	// guessing which stillness was which would be worse than no bench.
	int32 Driven = 0;
	FString Undriven;
	for (const FYardSubject& Subject : Yard->Subjects())
	{
		if (Subject.Agent != nullptr)
		{
			++Driven;
		}
		else if (Subject.Source != nullptr)
		{
			Undriven += FString::Printf(TEXT("%s  "), *Subject.Source->GetName());
		}
	}

	Y += LineHeight * 0.5f;
	Line(Body, FString::Printf(TEXT("%d model(s), %d driven%s"),
		Yard->Subjects().Num(), Driven,
		Yard->Solo() != nullptr ? *FString::Printf(TEXT("   SOLO: %s"), *Yard->Solo()->GetName()) : TEXT("")));

	if (!Undriven.IsEmpty())
	{
		Line(Quiet, FString::Printf(TEXT("no anim class: %s"), *Undriven));
	}

	// THE KEYS, FROM THE TABLE THAT BINDS THEM. See YardActions().
	Y += LineHeight * 0.5f;
	for (const FYardActionBinding& Action : YardActions())
	{
		Line(Quiet, FString::Printf(TEXT("[%s]  %s"),
			*Action.Key.GetDisplayName().ToString(), Action.Help));
	}
	Line(Quiet, TEXT("[WASD] fly   [Q/E] turn   [middle-drag] turn"));
}
