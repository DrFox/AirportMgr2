#pragma once

#include "CoreMinimal.h"
#include "BuildHudLayer.generated.h"

class UBuildBarWidget;
class UInspectorWidget;
class UOfferInboxWidget;
class ULedgerPanelWidget;
class UToastStackWidget;
class APlayerController;

/**
 * Owns the five HUD widgets a build driver shows, and the one recipe that creates each of
 * them the same way - see CreateConfiguredWidget.
 *
 * Pulled out of ARoadBuildController by issue #94: BeginPlay repeated "the configured
 * Blueprint if there is one, else the C++ class itself; make it; add it to the viewport at
 * this Z-order; log which" four times, once per widget, differing only in the class, the
 * Z-order and the two strings in the log line - the shape of duplication a template exists
 * to remove before a fifth widget copies it slightly wrong. The ledger panel was that fifth
 * widget (issue #192): it arrived with a non-Config, non-Transient pair of UPROPERTYs, which
 * meant DefaultGame.ini could not name a ledger Blueprint the way it can the other four. Its
 * blocks now match theirs exactly.
 *
 * UCLASS(Config=Game), as ARoadBuildController was for these before the move: DefaultGame.ini
 * can name a Blueprint for any of the five without a Blueprint subclass of the controller
 * existing to hold the default. Checked against every asset in Content/ and DefaultGame.ini
 * (issue #94's PR, re-checked for #192): nothing configures any of the five today, so nothing
 * here falls back to a changed value because of the move.
 */
UCLASS(Config = Game)
class AIRPORTMGR_API UBuildHudLayer : public UObject
{
	GENERATED_BODY()

public:
	/** The bar's Blueprint class; null means the plain C++ bar, which works and says so in
	 *  the log. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UBuildBarWidget> BuildBarClass;

	/** The bar on screen, created by CreateAll. */
	UPROPERTY(Transient)
	TObjectPtr<UBuildBarWidget> BuildBar;

	/** The inspector's Blueprint class; null means the plain C++ panel. Config, like the bar's. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UInspectorWidget> InspectorClass;

	/** The inspector on screen, created beside the bar. */
	UPROPERTY(Transient)
	TObjectPtr<UInspectorWidget> Inspector;

	/** The offer inbox's Blueprint class; null means the plain C++ panel, as above. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UOfferInboxWidget> OfferInboxClass;

	/** The ledger panel's Blueprint class; null means the plain C++ panel. Config, like the
	 *  other four - see this class's own comment for why the ledger was the odd one out. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<ULedgerPanelWidget> LedgerPanelClass;

	/** The inbox on screen. Play-mode only: the editor mode has no runtime to read. */
	UPROPERTY(Transient)
	TObjectPtr<UOfferInboxWidget> OfferInbox;

	/** Where the money went. Hidden until the player asks - see ULedgerPanelWidget. */
	UPROPERTY(Transient)
	TObjectPtr<ULedgerPanelWidget> LedgerPanel;

	/** The toast stack's Blueprint class; null means the plain C++ stack, as above. */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UToastStackWidget> ToastStackClass;

	/** The feed on screen. Owns the notification centre; see UToastStackWidget. */
	UPROPERTY(Transient)
	TObjectPtr<UToastStackWidget> ToastStack;

	/**
	 * Creates all four onto Owner's viewport, at the fixed Z-orders they have always used:
	 * bar 0; inspector and inbox both 1 (the inspector's card sits over the bar's canvas
	 * where the two overlap at the bottom-left, and the inbox never overlaps the inspector -
	 * it anchors bottom-right); toast stack 2, above both - a toast covered by anything else
	 * is one the player never saw, which is the defect this whole surface exists to fix.
	 */
	void CreateAll(APlayerController& Owner);

private:
	/**
	 * ConfiguredClass if set, else T's own C++ class; makes the widget, adds it to Owner's
	 * viewport at ZOrder, and logs which class it used. The one recipe all four widgets
	 * follow now, rather than four hand-written copies of it.
	 */
	template<class T>
	T* CreateConfiguredWidget(APlayerController& Owner, TSubclassOf<T> ConfiguredClass,
		int32 ZOrder, const TCHAR* DisplayName, const TCHAR* PropertyName);
};
