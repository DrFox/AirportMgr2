#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Widgets/Layout/Anchors.h"
#include "AirportMgrPanelWidget.generated.h"

class ARoadBuildController;
class UPanelWidget;
class UUIStyle;

/**
 * Shared base for the code-built HUD panels: the bar, the inspector, the offer inbox, the
 * toast stack.
 *
 * A TEMPLATE METHOD, not four copies of the same guard. Every one of the four used to repeat,
 * byte-for-byte: `Super::Initialize(); if (!bOk||bBuilt||RF_ClassDefaultObject||WidgetTree==
 * nullptr) return bOk; bBuilt=true; EnsureSlots();` (issue #90). Initialize() is sealed here
 * so that guard exists in exactly one place; a subclass builds its content in BuildOnce,
 * called exactly once, with the resolved style already in hand so it never has to ask whether
 * ResolveStyle() came back null (it never does - see UAirportMgrUISettings::ResolveStyle).
 *
 * Controller() and EnsureCardRoot() are the other two things duplicated across two or more of
 * the four: a click-driven panel asks the same question about who is playing, and a floating
 * corner card builds the same canvas-root-plus-bordered-card skeleton before it ever reaches
 * its own content.
 */
UCLASS(Abstract)
class AIRPORTMGR_API UAirportMgrPanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Builds the panel right after the base class has bound the asset's slots.
	 *
	 * Initialize rather than NativeOnInitialized: UUserWidget::Initialize only calls the
	 * latter when a PLAYER CONTEXT is valid (or a Blueprint class opts in), so a panel created
	 * from a world with no player - which is what a headless test does - would never build
	 * and would silently pass through empty. Overriding the one call every creation path
	 * takes keeps the build unconditional. Moved here from each of the four subclasses, which
	 * carried this exact reasoning on an identical override (issue #90).
	 */
	virtual bool Initialize() override final;

protected:
	/**
	 * Builds this panel's content. Called exactly once, right after WidgetTree is confirmed
	 * usable, with a style that is NEVER NULL - do here what each subclass's own EnsureSlots
	 * plus any one-time wiring (button bindings, initial visibility) used to do from
	 * Initialize() directly.
	 */
	virtual void BuildOnce(const UUIStyle& Style) PURE_VIRTUAL(UAirportMgrPanelWidget::BuildOnce, );

	/**
	 * The owning player when the controller created this widget; the first controller
	 * otherwise (tests create a panel straight from a world). Null is a supported state: a
	 * panel still builds, and whatever polls this simply has nothing to ask.
	 */
	ARoadBuildController* Controller() const;

	/**
	 * A canvas root (if the asset gave none) plus ONE bordered card, anchored, aligned and
	 * positioned as given, auto-sized to its content. Returns the panel INSIDE the card that
	 * a subclass adds its own rows to, or nullptr when an asset already supplied a root - in
	 * that case BindWidgetOptional has already filled every slot the asset supplies, and a
	 * code-built card would replace the designer's layout.
	 *
	 * PanelDark, ALWAYS: the card is the outer surface everything a subclass draws sits ON,
	 * and Panel is left free for whatever goes inside it (a button, an offer row) - the same
	 * split UBuildBarWidget's two rows and UOfferInboxWidget's offer cards already draw.
	 */
	UPanelWidget* EnsureCardRoot(FName CardName, const FAnchors& Anchors, FVector2D Alignment,
		FVector2D Position, bool bRounded);

private:
	bool bBuilt = false;
};
