#pragma once

#include "CoreMinimal.h"
#include "Components/Border.h"
#include "UiRow.generated.h"

class UUIStyle;

/**
 * A list row: Well-filled, ControlRadius-rounded (UI library step 1). A UBorder SUBCLASS for the
 * reason UUiButton is a UButton one - it drops in wherever a bordered card was, and the only
 * thing it adds is the one place a row's surface is chosen. Callers SetContent their own lines.
 */
UCLASS()
class AIRPORTMGR_API UUiRow : public UBorder
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const FMargin& InPadding);
};
