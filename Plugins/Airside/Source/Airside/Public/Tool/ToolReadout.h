#pragma once

#include "CoreMinimal.h"

/**
 * What a tool tells the player about the gesture in progress - bays, cost, what is wrong.
 *
 * A SECOND SINK, NOT AN EXTENSION OF IToolPreviewSink. That interface's whole contract is to
 * describe intent in ROAD PLANE coordinates naming a MEANING, and a bay count is not
 * road-plane geometry. Putting it there would blur the one boundary that keeps Tool/ free of
 * presentation - the boundary the whole plugin/game-module split rests on.
 *
 * AND NOT STATE THE HUD POLLS. This is filled from a const per-frame call made beside the
 * one that draws the preview, so the number the player reads cannot describe a different
 * gesture from the one they are looking at. A GetReadout() the bar pulled would be a second
 * thing that must agree with the preview, which is the failure this codebase names most
 * often and has shipped three times.
 *
 * STRINGS, NOT NUMBERS. Display-ready values, because the alternative is an enum of fact
 * KINDS the bar switches on - which puts the plugin straight back in the business of knowing
 * what the bar can render. The plugin still names no widget and no colour.
 */
struct AIRSIDE_API IToolReadoutSink
{
	virtual ~IToolReadoutSink() = default;

	/** A named value the player is deciding on: "Bays", "3". */
	virtual void Fact(const FString& Label, const FString& Value) = 0;

	/** Something wrong with the gesture that does not stop it. */
	virtual void Warning(const FString& Text) = 0;

	/**
	 * Whether committing now would succeed - the Build button's enabled state.
	 *
	 * TRAVELS WITH THE FACTS rather than being asked for separately, because a button lit
	 * while committing would fail is precisely the drift this sink exists to make
	 * impossible. One call, one frame, one answer.
	 */
	virtual void Committable(bool bCan) = 0;
};

/** One frame's worth, as collected. */
struct AIRSIDE_API FToolReadout
{
	/**
	 * Label and value, IN THE ORDER EMITTED. The bar renders them in sequence, so the order
	 * a tool emits them is the order the player reads them - the tool's decision to make and
	 * this struct's to preserve. A map would lose it.
	 */
	TArray<TPair<FString, FString>> Facts;

	TArray<FString> Warnings;

	/**
	 * FALSE BY DEFAULT, and that default is load-bearing: every tool but one never calls
	 * Committable, and none of them should light the Build button by saying nothing.
	 */
	bool bCommittable = false;
};

/** The sink a driver hands to the active tool each frame. */
struct AIRSIDE_API FToolReadoutCollector final : public IToolReadoutSink
{
	FToolReadout Readout;

	/**
	 * Refilled every frame, never appended to.
	 *
	 * A fact left over from last frame describes a gesture the player has already changed,
	 * and a stale `bCommittable` is a Build button lit over nothing.
	 */
	void Reset() { Readout = FToolReadout(); }

	virtual void Fact(const FString& Label, const FString& Value) override
	{
		Readout.Facts.Emplace(Label, Value);
	}

	virtual void Warning(const FString& Text) override { Readout.Warnings.Add(Text); }

	virtual void Committable(bool bCan) override { Readout.bCommittable = bCan; }
};
