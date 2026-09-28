#include "PlayerSettings.h"

#include "AirportMgrUserSettings.h"
#include "BuildCameraComponent.h"
#include "Engine/UserInterfaceSettings.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "Tool/SnapGuideSettings.h"

FGamePlayerSettingsSink::FGamePlayerSettingsSink(ARoadBuildController& InController)
	: Controller(&InController)
	, EngineScaleAtStart(GetDefault<UUserInterfaceSettings>()->ApplicationScale)
{
}

FPlayerSettings FGamePlayerSettingsSink::Read() const
{
	FPlayerSettings V;
	if (const UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		V.UIScale = Settings->UIScale;
		V.PanSpeedScale = Settings->PanSpeedScale;
		V.ZoomSpeedScale = Settings->ZoomSpeedScale;
		V.bGridSnapOnStart = Settings->bGridSnapOnStart;
		V.GraphicsQuality = Settings->GetOverallScalabilityLevel();
	}
	const ARoadBuildController* C = Controller.Get();
	if (const ARoadNetworkActor* Target = C != nullptr ? C->GetTarget() : nullptr)
	{
		V.DriveSide = Target->GetDriveSide();
	}
	return V;
}

void FGamePlayerSettingsSink::Apply(const FPlayerSettings& V)
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->UIScale = V.UIScale;
		Settings->PanSpeedScale = V.PanSpeedScale;
		Settings->ZoomSpeedScale = V.ZoomSpeedScale;
		Settings->bGridSnapOnStart = V.bGridSnapOnStart;
		// ONLY A REAL CHANGE, and never -1: the engine's "custom mix" is a reading, not a preset,
		// and applying scalability recreates every render state - not a thing to do per Apply.
		// ApplyNonResolutionSettings, NOT ApplySettings: that one also SAVES (GameUserSettings.cpp:600),
		// which would persist a value the player may yet cancel.
		if (V.GraphicsQuality >= 0 && V.GraphicsQuality != Settings->GetOverallScalabilityLevel())
		{
			Settings->SetOverallScalabilityLevel(V.GraphicsQuality);
			Settings->ApplyNonResolutionSettings();
			UE_LOG(LogRoadBuild, Log, TEXT("Settings: graphics preset %d applied"), V.GraphicsQuality);
		}
	}

	// THE ENGINE'S DPI MULTIPLIER, on its settings CDO: SGameLayerManager asks for it on every
	// layout (UserInterfaceSettings.cpp:109), so a change shows next frame with no restart. The CDO
	// outlives a PIE session - RestoreEngineScale puts it back at EndPlay.
	UUserInterfaceSettings* Ui = GetMutableDefault<UUserInterfaceSettings>();
	if (!FMath::IsNearlyEqual(Ui->ApplicationScale, V.UIScale))
	{
		Ui->ApplicationScale = V.UIScale;
		UE_LOG(LogRoadBuild, Log, TEXT("Settings: UI scale %.2f applied"), V.UIScale);
	}

	ARoadBuildController* C = Controller.Get();
	if (C == nullptr)
	{
		return;
	}
	if (UBuildCameraComponent* Camera = C->FindComponentByClass<UBuildCameraComponent>())
	{
		Camera->SetPlayerSpeedScales(V.PanSpeedScale, V.ZoomSpeedScale);
	}
	// THE AIRPORT'S SIDE, the value the bar's "Drive left" lights from - only on a change, since each
	// SetDriveSide re-lanes every road and is an undo step.
	// ENFORCED BY: AirportMgr.Settings.DriveSideIsOneValue.
	ARoadNetworkActor* Target = C->GetTarget();
	if (Target != nullptr && Target->GetDriveSide() != V.DriveSide && Target->SetDriveSide(V.DriveSide) && bEditing)
	{
		++DriveSideSteps;
	}
}

void FGamePlayerSettingsSink::BeginEdit()
{
	if (const UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		LevelsAtEdit = Settings->ScalabilityQuality;
	}
	DriveSideSteps = 0;
	bEditing = true;
}

void FGamePlayerSettingsSink::Revert(const FPlayerSettings& Snapshot)
{
	if (!bEditing)
	{
		Apply(Snapshot);
		return;
	}
	UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get();
	if (Settings != nullptr && !(Settings->ScalabilityQuality == LevelsAtEdit))
	{
		Settings->ScalabilityQuality = LevelsAtEdit;
		Settings->ApplyNonResolutionSettings();
		UE_LOG(LogRoadBuild, Log, TEXT("Settings: graphics groups restored"));
	}
	ARoadBuildController* C = Controller.Get();
	ARoadNetworkActor* Target = C != nullptr ? C->GetTarget() : nullptr;
	// UNDONE, not set: nothing else can edit the airport while the modal is up (keys and presses
	// wait), so the top DriveSideSteps entries of the undo stack are exactly this dialog's.
	for (; DriveSideSteps > 0 && Target != nullptr; --DriveSideSteps)
	{
		Target->Undo();
	}
	DriveSideSteps = 0;
	bEditing = false;
	// The rest by value - graphics and the side now already match, so Apply leaves them alone.
	FPlayerSettings Rest = Snapshot;
	Rest.GraphicsQuality = -1;
	Apply(Rest);
}

void FGamePlayerSettingsSink::Save()
{
	bEditing = false;   // kept: nothing left to revert
	DriveSideSteps = 0;
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->SaveSettings();
	}
}

void FGamePlayerSettingsSink::RestoreEngineScale()
{
	GetMutableDefault<UUserInterfaceSettings>()->ApplicationScale = EngineScaleAtStart;
}

void PlayerSettings::ApplyStartGrid(FSnapGuideSettings& Guides, bool bOn)
{
	if (bOn && Guides.GridStep == EGridStep::Off)
	{
		// THE BUTTON'S OWN FIRST STEP, not a pitch chosen here: the step list and its order are
		// FSnapGuideSettings' decision, and a second copy of "1 m first" would drift from it.
		Guides.CycleGridStep();
	}
}
