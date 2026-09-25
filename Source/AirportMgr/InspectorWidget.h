#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Tool/Selection.h"
#include "InspectorWidget.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;
class UButton;
class UTextBlock;
class UUIStyle;
struct FAgentFacts;

/**
 * What Refresh needs to know has NOT changed to skip RECOMPOSING the aircraft Title/Facts/
 * Status sentences - issue #309. Refresh used to run every Printf/FString::Format in the
 * aircraft branch (four Printfs plus two NSLOCTEXT lookups) EVERY TICK regardless of whether
 * the facts had moved, and only gated the resulting SetText call (see LastTitle/LastFacts/
 * LastStatus) - so a parked aircraft awaiting dispatch rebuilt the same three sentences a tick
 * for a SetText that then did nothing.
 *
 * ROUNDED, not raw: HeadingRounded/SpeedTenthsRounded/AltitudeRounded each round to the FINEST
 * precision the composed sentence shows for that quantity (see Refresh's own Printf specifiers),
 * so two facts that would compose to the IDENTICAL string never miss this cheaper equality check
 * first - the same reasoning LastTitle/LastFacts/LastStatus's own comment gives for comparing
 * composed text over a bare (id, phase) key, one step earlier.
 *
 * SpeedTenthsRounded is TENTHS OF m/s, not whole knots, even though the sentence prints BOTH
 * from the same Shown value: m/s prints to one decimal place (%.1f) and knots to zero (%.0f),
 * and 1 kt is 0.514 m/s - finer than a whole knot - so keying on the coarser knots figure alone
 * left a real change (3.4 -> 3.5 m/s, same 7 kt) uncomposed and the m/s line stale on screen.
 * PR #329 review caught this: whichever of a quantity's several displayed roundings is FINEST is
 * the one the key must use, since it changes at least as often as every coarser sibling derived
 * from the same raw value.
 *
 * Phase HOLDS F.Status (the exact string the Status line prints), not F.Phase (the enum): the
 * whole point named in that same comment is that phase ALONE (Taxiing, Rolling) sits still for
 * many seconds while heading/speed/altitude keep moving, so gating on the enum here would
 * freeze this struct's equality while the sentence it stands for kept changing underneath it.
 *
 * AIRCRAFT ONLY, not the stand/depot branch: issue #309's evidence cites this file's aircraft
 * composition (InspectorWidget.cpp:163-190, :198) and nothing about the stand branch, which
 * composes far less per call (one Printf, no NSLOCTEXT sentence) and is not what was reported -
 * gating it too would be scope the issue did not ask for.
 */
struct FInspectorKey
{
	int32 Id = INDEX_NONE;
	FString Phase;
	int32 HeadingRounded = 0;
	/** Tenths of m/s, not whole knots - see the class comment on why the finer of the two
	 *  displayed roundings is the one that must gate composition. */
	int32 SpeedTenthsRounded = 0;
	int32 AltitudeRounded = 0;
	FString Destination;
	bool bEngineRunning = false;
	FString Fuel;

	bool operator==(const FInspectorKey& Other) const
	{
		return Id == Other.Id && Phase == Other.Phase && HeadingRounded == Other.HeadingRounded
			&& SpeedTenthsRounded == Other.SpeedTenthsRounded && AltitudeRounded == Other.AltitudeRounded
			&& Destination == Other.Destination && bEngineRunning == Other.bEngineRunning
			&& Fuel == Other.Fuel;
	}
	bool operator!=(const FInspectorKey& Other) const { return !(*this == Other); }
};

/**
 * The inspector: what the selected aircraft or stand is doing, and the verbs for it.
 *
 * The same recipe as UBuildBarWidget: a C++ base that builds a working panel with no asset,
 * and BindWidgetOptional slots a Widget Blueprint fills to restyle it. Set the Blueprint
 * as InspectorClass on the controller.
 *
 * POLLED each tick from InspectFacts, never subscribed: the bar's enabled states were
 * event-driven once and went stale, and a panel that shows speed needs every frame anyway.
 * It reads FAgentFacts / FStandFacts and never FRoadAgent, so M3's UFlight fills the same
 * struct and this file does not change.
 *
 * Its buttons run rows of BuildActions() by id, so the panel, the bar and the C key are one
 * list (spec §6.2).
 *
 * PanelTint/ButtonTint/DisabledTint/FontSize are GONE (issue #91) - EVERY COLOUR AND FONT
 * COMES FROM UUIStyle now, the rule UBuildBarWidget's own header already states. Only metrics
 * this panel alone needs (its width, how far it floats) stay as knobs.
 */
UCLASS()
class AIRPORTMGR_API UInspectorWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FactsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> DepartButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UButton> FollowButton;

	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double PanelWidth = 300.0;
	/** Distance above the bottom edge, so it clears the build bar. */
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double BottomOffset = 72.0;

	/**
	 * Re-reads the facts for Selection over Target and repaints. What NativeTick calls with
	 * the controller's target and selection; public so a headless test can drive it with
	 * no controller.
	 *
	 * PrecomputedAgentFacts is issue #187: NativeTick already asked the controller for the
	 * selected aircraft's FAgentFacts to answer the bar's selection.depart row, and passes the
	 * SAME struct here so this does not call InspectFacts::DescribeAgent a second time for one
	 * frame's one selection. Null (the default, and always null from a headless test with no
	 * controller) falls back to asking DescribeAgent itself, unchanged from before.
	 */
	void Refresh(const ARoadNetworkActor* Target, const FSelection& Selection,
		const FAgentFacts* PrecomputedAgentFacts = nullptr);

	bool IsShownForTest() const;
	bool IsDepartEnabledForTest() const;
	FString TitleForTest() const;
	/** Depart's CAPTION colour - the thing that must actually change with enabled state.
	 *  See Refresh: the button's own background stays Style->Button always. */
	FLinearColor DepartLabelColourForTest() const;
	/** How many times Refresh actually called SetText on one of its three fields, as opposed
	 *  to how many times it was asked - the seam issue #187's gate is measured through: an
	 *  idle tick (same selection, same facts) must add nothing to this. */
	int32 SetTextCallCountForTest() const { return SetTextCalls; }

	/** How many times Refresh actually RECOMPOSED the aircraft Title/Facts/Status sentences
	 *  (the Printf/FString::Format work FInspectorKey gates), as opposed to how many times it
	 *  was asked - issue #309, one level upstream of SetTextCallCountForTest: an idle tick on a
	 *  PARKED aircraft (same selection, same rounded facts) must add nothing to this, even
	 *  though the earlier SetText gate already read as flat by comparing the strings this
	 *  count now stops building in the first place. */
	int32 ComposeCountForTest() const { return ComposeCalls; }

protected:
	/** Builds the panel's chrome and binds its two verbs. See
	 *  UAirportMgrPanelWidget::Initialize for why this runs from Initialize. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	bool bDepartEnabled = false;

	/**
	 * INDICES INTO BuildActions(), never ids to look up - the same reason UBuildBarEntry
	 * holds one. Found once, in EnsureSlots, by walking the Selection section positionally:
	 * BuildActions.cpp adds selection.depart then selection.follow so the panel, the bar and
	 * the C key stay one list (its own comment, spec §6.2) - the pair this panel needs is
	 * exactly those two rows, in that order.
	 */
	int32 DepartActionIndex = INDEX_NONE;
	int32 FollowActionIndex = INDEX_NONE;

	/** DepartButton's own caption, held so Refresh can recolour it without re-finding it
	 *  through GetContent() every tick - the same reason UBuildBarEntry holds its Label. */
	UPROPERTY() TObjectPtr<UTextBlock> DepartLabel;

	/**
	 * What Title/Facts/Status last actually SET, so a repeat with nothing changed - the
	 * common case, since heading/speed/altitude only move while an agent is actually taxiing
	 * or flying and most ticks are "still parked" or "nothing selected" - calls SetText zero
	 * times (issue #187: SetText has no early-out of its own, same reason UBuildBarWidget
	 * gates its clock and balance). Compared against the COMPOSED string rather than a
	 * (selection id, phase) key: phase alone stays "Taxiing" or "Rolling / Climbing" for many
	 * seconds while heading, speed and altitude keep changing every one of them, and gating on
	 * phase would freeze those numbers mid-motion - a real behaviour change, not a saving.
	 */
	FString LastTitle, LastFacts, LastStatus;

	/** See SetTextCallCountForTest. */
	int32 SetTextCalls = 0;

	/** The aircraft facts the composed Title/Facts/Status were last built from - see
	 *  FInspectorKey's own comment. Compared before Refresh does any Printf/FString::Format
	 *  work, not after: this is the gate ONE STEP EARLIER than LastTitle/LastFacts/LastStatus. */
	FInspectorKey LastComposedKey;

	/** What Refresh composed last time FInspectorKey changed - reused verbatim on an unchanged
	 *  tick rather than recomposed, which is the entire saving ComposeCountForTest measures. */
	FString LastComposedTitle, LastComposedFacts, LastComposedStatus;

	/** See ComposeCountForTest. */
	int32 ComposeCalls = 0;

	void EnsureSlots(const UUIStyle* Style);
	void RunAction(int32 ActionIndex);

	UFUNCTION() void HandleDepart();
	UFUNCTION() void HandleFollow();
};
