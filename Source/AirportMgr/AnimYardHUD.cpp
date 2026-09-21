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
	// EVERY NAME HERE IS PREFIXED, and it is not decoration. This module is a UNITY build, so
	// an anonymous namespace is not file-private in practice: plain `Heading` and `LineHeight`
	// here broke the build at five unrelated lines in BuildBarWidget.cpp, RoadBuildHUD.cpp,
	// BuildCameraRigTest.cpp and UIStyleTest.cpp, each of which has a local of that name that
	// C4459 then calls a shadowed global. It is the same hazard CLAUDE.md records for duplicate
	// DEFINE_LOG_CATEGORY_STATIC - compiles alone, collides together.
	constexpr float YardLeftMargin = 24.0f;
	constexpr float YardTopMargin = 24.0f;
	constexpr float YardLineHeight = 18.0f;

	const FLinearColor YardHeadingColour(0.95f, 0.95f, 0.95f);
	const FLinearColor YardBodyColour(0.75f, 0.78f, 0.82f);
	const FLinearColor YardCaretColour(1.0f, 0.82f, 0.25f);
	const FLinearColor YardQuietColour(0.45f, 0.47f, 0.50f);

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

	float Y = YardTopMargin;
	const auto Line = [this, Font, &Y](const FLinearColor& Colour, const FString& Text)
	{
		DrawText(Text, Colour, YardLeftMargin, Y, Font);
		Y += YardLineHeight;
	};

	const AAnimYard* Yard = Controller->Yard();
	if (Yard == nullptr)
	{
		Line(YardCaretColour, TEXT("No AAnimYard in this level - nothing will move."));
		Line(YardBodyColour, TEXT("Re-run Tools/Python/build_model_yard.py to place one."));
		return;
	}

	const FYardMotion& Bench = Yard->GetMotion();

	Line(YardHeadingColour, FString::Printf(TEXT("%s    stage: %s    loop %.1f / %.0fs"),
		Bench.bPaused ? TEXT("|| PAUSED") : TEXT(">  RUNNING"),
		FYardMotion::StageName(Bench.CurrentStage()),
		Bench.LoopTime, FYardMotion::LoopSeconds()));
	Y += YardLineHeight * 0.5f;

	// THE CHANNELS, FROM THE SAME LIST Tab WALKS. A hand-written set of rows here would be a
	// second list, and the one it disagreed with would be the one with the new channel in it.
	for (const EYardChannel Channel : FYardMotion::Channels())
	{
		double Min = 0.0;
		double Max = 0.0;
		Bench.ChannelRange(Channel, Min, Max);

		const bool bIsCaret = Channel == Controller->Caret();
		Line(bIsCaret  ? YardCaretColour : YardBodyColour, FString::Printf(TEXT("%s %-9s %10.2f   %s"),
			bIsCaret ? TEXT(">") : TEXT(" "),
			FYardMotion::ChannelName(Channel),
			Bench.Value(Channel),
			*Meter(Bench.Value(Channel), Min, Max)));
	}

	Line(YardBodyColour, FString::Printf(TEXT("  %-9s %10s"), TEXT("wheels"),
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

	Y += YardLineHeight * 0.5f;
	Line(YardBodyColour, FString::Printf(TEXT("%d model(s), %d driven%s"),
		Yard->Subjects().Num(), Driven,
		Yard->Solo() != nullptr ? *FString::Printf(TEXT("   SOLO: %s"), *Yard->Solo()->GetName()) : TEXT("")));

	if (!Undriven.IsEmpty())
	{
		Line(YardQuietColour, FString::Printf(TEXT("no anim class: %s"), *Undriven));
	}

	// THE KEYS, FROM THE TABLE THAT BINDS THEM. See YardActions().
	Y += YardLineHeight * 0.5f;
	for (const FYardActionBinding& Action : YardActions())
	{
		Line(YardQuietColour, FString::Printf(TEXT("[%s]  %s"),
			*Action.Key.GetDisplayName().ToString(), Action.Help));
	}
	Line(YardQuietColour, TEXT("[WASD] fly   [Q/E] turn   [middle-drag] turn"));
}
