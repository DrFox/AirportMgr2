#include "PlayerSettings.h"

#include "Tool/SnapGuideSettings.h"

void PlayerSettings::ApplyStartGrid(FSnapGuideSettings& Guides, bool bOn)
{
	if (bOn && Guides.GridStep == EGridStep::Off)
	{
		// THE BUTTON'S OWN FIRST STEP, not a pitch chosen here: the step list and its order are
		// FSnapGuideSettings' decision, and a second copy of "1 m first" would drift from it.
		Guides.CycleGridStep();
	}
}
