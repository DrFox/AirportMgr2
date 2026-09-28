#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiWindowHost.generated.h"

/** UI library step 2, Task 3 scaffold: the calls UAirportMgrPanelWidget makes. Task 4 fills it in. */
UCLASS()
class AIRPORTMGR_API UUiWindowHost : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetShown(FName Id, bool bShown);
	bool IsShown(FName Id) const;
	void ForgetDismissal(FName Id);
};
