#pragma once

#include "CoreMinimal.h"
#include "UiLayoutStore.generated.h"

/**
 * Where a window was left: host-local top-left, and - only once the player resized it - its size.
 * bSized false means "auto-sized to its panel", which is what an untouched window is; restoring a
 * size for it would freeze a window that should grow with its content. Plain UPROPERTYs: the
 * struct is saved through UAirportMgrUserSettings::WindowLayout, whose config flag covers it.
 */
USTRUCT()
struct FUiWindowPlacement
{
	GENERATED_BODY()

	UPROPERTY() FVector2D TopLeft = FVector2D::ZeroVector;
	UPROPERTY() FVector2D Size = FVector2D::ZeroVector;
	UPROPERTY() bool bSized = false;
};

/**
 * Where the window layout is kept between launches (UI library step 3).
 *
 * AN INTERFACE, not UAirportMgrUserSettings called from the host - IToolPreferences' shape (#376)
 * and its reason: a test's host must never read or write the player's own GameUserSettings.ini,
 * or a player's dragged ledger would move where every test's ledger starts. A host starts with no
 * store (nothing remembered, nothing written); UBuildHudLayer alone hands it the real one.
 * ENFORCED BY: Check-Architecture rule 29 (layout-store-wired).
 */
class AIRPORTMGR_API IUiLayoutStore
{
public:
	virtual ~IUiLayoutStore() = default;
	/** What was stored for Id, or unset when nothing ever was. */
	virtual TOptional<FUiWindowPlacement> Read(FName Id) const = 0;
	virtual void Write(FName Id, const FUiWindowPlacement& Placement) = 0;
	/** Forgets every window - "Reset window layout". */
	virtual void Clear() = 0;
};

/** Held in memory and lost with the object - what a test hands a host. */
class AIRPORTMGR_API FMemoryUiLayoutStore : public IUiLayoutStore
{
public:
	virtual TOptional<FUiWindowPlacement> Read(FName Id) const override;
	virtual void Write(FName Id, const FUiWindowPlacement& Placement) override;
	virtual void Clear() override;
	/** How many writes reached the store - a drag must cost one, not one per frame. */
	int32 GetWriteCount() const { return WriteCount; }

private:
	TMap<FName, FUiWindowPlacement> Values;
	int32 WriteCount = 0;
};
