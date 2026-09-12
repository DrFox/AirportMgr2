#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

#include "OfferInboxWidget.generated.h"

class ARoadNetworkActor;
class UButton;
class UListView;
class UOfferInboxWidget;
class UOfferInboxViewModel;
class UOfferViewModel;
class UTextBlock;
class UVerticalBox;

/**
 * One row's two buttons, bound by index.
 *
 * A UObject per row for the same reason UBuildBarEntry is one: UButton::OnClicked is a
 * DYNAMIC delegate and binds only to a UFUNCTION on a UObject - a lambda cannot bind.
 */
UCLASS()
class UOfferRowEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 RowIndex = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UOfferInboxWidget> Owner;

	UFUNCTION() void HandleAccept();
	UFUNCTION() void HandleDecline();
};

/**
 * The offer inbox: what the airlines are asking for, and the two answers.
 *
 * C++ BASE, BLUEPRINT OPTIONAL - the rule the build bar and the inspector already follow.
 * The code builds a plain panel when no asset supplies one, so a missing Blueprint degrades
 * the look rather than breaking the feature.
 *
 * TWO LIST PATHS, ONE VIEWMODEL. If a Widget Blueprint supplies a UListView, the rows are
 * its entry widgets and MVVM hands each one its UOfferViewModel. Without one, the code
 * builds a vertical box of rows itself. A UListView cannot be built usefully in code here
 * because its entry widget class is a Blueprint asset, and virtualisation only earns its
 * keep at hundreds of rows - the inbox has a handful.
 *
 * TO RESTYLE IN THE DESIGNER: make a Widget Blueprint with this class as parent, name the
 * widgets to match the BindWidgetOptional members below, and set the list's entry widget
 * class. A RENAMED VIEWMODEL FIELD NEEDS THE BLUEPRINT RECOMPILED AND RESAVED, or the old
 * binding runs against the new class - the stale-Blueprint trap, in a new place.
 */
UCLASS()
class AIRPORTMGR_API UOfferInboxWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UListView> OfferList;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> OfferColumn;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BadgeText;

	/**
	 * Distance from the TOP of the screen for the code-built card.
	 *
	 * TOP RIGHT, moved from bottom right (spec section 6.2). Two things forced it: the feed
	 * now owns the bottom-right corner, and the two-row bar is 138 uu tall against the 56 it
	 * was, so the old 120 uu bottom offset put the card behind it. An offer must never be
	 * the thing that scrolls away or hides - missing one costs money.
	 */
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float TopOffset = 12.0f;

	UOfferInboxViewModel* GetInbox() const { return Inbox; }

	/**
	 * Re-read the board and repaint. What NativeTick calls, and what a headless test calls
	 * directly - the tick-to-Refresh seam is one line, and a test has no viewport to paint in.
	 */
	void Refresh(ARoadNetworkActor* Target);

	/** Called by a row's entry object. Public because UOfferRowEntry is a separate UObject. */
	void AcceptRow(int32 RowIndex);
	void DeclineRow(int32 RowIndex);

protected:
	virtual bool Initialize() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TObjectPtr<UOfferInboxViewModel> Inbox;
	UPROPERTY() TArray<TObjectPtr<UOfferRowEntry>> Entries;

	/** Built once, in Initialize. The bar's own rule: chrome only where the asset gave none. */
	bool bBuilt = false;

	void EnsureSlots();
	void PaintRows();
};
