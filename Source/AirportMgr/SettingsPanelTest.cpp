#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Misc/AutomationTest.h"
#include "PlayerSettings.h"
#include "SettingsPanelWidget.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiDropdown.h"
#include "UI/UiLayoutStore.h"
#include "UI/UiRadioGroup.h"
#include "UI/UiSlider.h"
#include "UI/UiToggle.h"
#include "UI/UiWindowHost.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SettingsPanelTest
{
	/** Values away from every default, so a control that loaded a default instead reads wrong. */
	FPlayerSettings Seeded()
	{
		FPlayerSettings S;
		S.UIScale = 1.25f;
		S.PanSpeedScale = 1.5f;
		S.ZoomSpeedScale = 0.5f;
		S.bGridSnapOnStart = true;
		S.GraphicsQuality = 1;
		S.DriveSide = EDriveSide::Left;
		return S;
	}

	/**
	 * A hosted Settings panel over a memory sink. Every control is driven through the ENGINE's
	 * own event (USlider, UButton), never its handler - the seam a deleted binding would cut
	 * (step 4a final review, Important 3).
	 */
	struct FFixture
	{
		FAirsideTestWorld TestWorld{ /*bSpawnActor=*/false };
		UUiWindowHost* Host = nullptr;
		USettingsPanelWidget* Panel = nullptr;
		TSharedPtr<FMemoryPlayerSettingsSink> Sink = MakeShared<FMemoryPlayerSettingsSink>();

		explicit FFixture(const FPlayerSettings& Start = Seeded())
		{
			Sink->Values = Start;
			Host = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
			Panel = CreateWidget<USettingsPanelWidget>(TestWorld.World, USettingsPanelWidget::StaticClass());
			if (Host != nullptr && Panel != nullptr)
			{
				Host->SetViewSizeForTest(FVector2D(1920.0, 1080.0));
				Host->AddWindow(*Panel);
				Panel->SetSink(Sink);
			}
		}
		bool Ok() const { return Host != nullptr && Panel != nullptr; }

		template<class T> T* Find(const TCHAR* Name) const { return Panel->WidgetTree->FindWidget<T>(Name); }
		USlider* Bar(const TCHAR* Name) const
		{
			UUiSlider* S = Find<UUiSlider>(Name);
			return S != nullptr ? S->WidgetTree->FindWidget<USlider>(TEXT("SliderBar")) : nullptr;
		}
		void Drag(const TCHAR* Name, float V) const { if (USlider* B = Bar(Name)) { B->OnValueChanged.Broadcast(V); } }
		void Release(const TCHAR* Name) const { if (USlider* B = Bar(Name)) { B->OnMouseCaptureEnd.Broadcast(); } }
		void Click(UUserWidget* Owner, const TCHAR* Name) const
		{
			if (UButton* B = Owner != nullptr ? Owner->WidgetTree->FindWidget<UButton>(Name) : nullptr) { B->OnClicked.Broadcast(); }
		}
		void ClickButton(const TCHAR* Name) const { if (UButton* B = Find<UButton>(Name)) { B->OnClicked.Broadcast(); } }
		void ChooseGraphics(int32 Index) const
		{
			UUiDropdown* D = Find<UUiDropdown>(TEXT("Graphics"));
			Click(D != nullptr ? D->BuildMenu() : nullptr, *FString::Printf(TEXT("Option%d"), Index));
		}
		/** Moves every control off the seeded value. */
		void ChangeEverything() const
		{
			Drag(TEXT("UiScale"), 1.0f);
			Release(TEXT("UiScale"));
			Drag(TEXT("PanSpeed"), 1.8f);
			Drag(TEXT("ZoomSpeed"), 1.2f);
			Click(Find<UUiToggle>(TEXT("GridSnap")), TEXT("ToggleHit"));
			Click(Find<UUiRadioGroup>(TEXT("DriveSide")), TEXT("Segment1"));
			ChooseGraphics(3);
		}
	};
}

/**
 * OPENING APPLIES NOTHING (plan 4b Review Focus 2): every control loads its value from code, and
 * code-set values raise no event - so opening the dialog never re-lanes traffic or re-applies the
 * graphics preset. And it opens as a modal.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsOpenTest, "AirportMgr.Settings.Panel.OpenAppliesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsOpenTest::RunTest(const FString& Parameters)
{
	SettingsPanelTest::FFixture F;
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	F.Panel->Open();
	TestTrue(TEXT("open"), F.Panel->IsShowing());
	TestTrue(TEXT("as a modal"), F.Host->IsModalOpen());
	UUiSlider* Ui = F.Find<UUiSlider>(TEXT("UiScale"));
	UUiSlider* Pan = F.Find<UUiSlider>(TEXT("PanSpeed"));
	UUiSlider* Zoom = F.Find<UUiSlider>(TEXT("ZoomSpeed"));
	UUiToggle* Grid = F.Find<UUiToggle>(TEXT("GridSnap"));
	UUiRadioGroup* Side = F.Find<UUiRadioGroup>(TEXT("DriveSide"));
	UUiDropdown* Gfx = F.Find<UUiDropdown>(TEXT("Graphics"));
	if (!TestTrue(TEXT("every control is built and named"), Ui && Pan && Zoom && Grid && Side && Gfx)) { return false; }
	TestEqual(TEXT("UI scale loads"), Ui->GetValue(), 1.25f, 1e-4f);
	TestEqual(TEXT("pan speed loads"), Pan->GetValue(), 1.5f, 1e-4f);
	TestEqual(TEXT("zoom speed loads"), Zoom->GetValue(), 0.5f, 1e-4f);
	TestTrue(TEXT("grid snap loads"), Grid->IsOn());
	TestEqual(TEXT("drive side loads - Left is the first segment"), Side->GetSelected(), 0);
	TestEqual(TEXT("graphics loads"), Gfx->GetSelected(), 1);
	TestEqual(TEXT("and nothing was applied"), F.Sink->Applies, 0);
	return true;
}

/**
 * EVERY CONTROL APPLIES LIVE (spec section 3) - except UI scale, which applies when the player
 * lets go (plan 4b ruling: re-scaling the screen under a captured mouse re-lays the slider being
 * dragged). Each change reaches the sink as the one field it edits.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsLiveTest, "AirportMgr.Settings.Panel.EachControlAppliesLive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsLiveTest::RunTest(const FString& Parameters)
{
	SettingsPanelTest::FFixture F;
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	F.Panel->Open();
	F.Drag(TEXT("PanSpeed"), 1.8f);
	TestEqual(TEXT("a pan drag applies"), F.Sink->Applies, 1);
	TestEqual(TEXT("the pan speed it set"), F.Sink->Values.PanSpeedScale, 1.8f, 1e-4f);
	F.Drag(TEXT("ZoomSpeed"), 1.2f);
	TestEqual(TEXT("zoom speed"), F.Sink->Values.ZoomSpeedScale, 1.2f, 1e-4f);
	F.Click(F.Find<UUiToggle>(TEXT("GridSnap")), TEXT("ToggleHit"));
	TestFalse(TEXT("grid snap"), F.Sink->Values.bGridSnapOnStart);
	F.Click(F.Find<UUiRadioGroup>(TEXT("DriveSide")), TEXT("Segment1"));
	TestEqual(TEXT("drive side"), F.Sink->Values.DriveSide, EDriveSide::Right);
	F.ChooseGraphics(3);
	TestEqual(TEXT("graphics"), F.Sink->Values.GraphicsQuality, 3);
	TestEqual(TEXT("five changes, five applies"), F.Sink->Applies, 5);
	F.Drag(TEXT("UiScale"), 1.0f);
	TestEqual(TEXT("a UI scale drag applies nothing yet"), F.Sink->Applies, 5);
	F.Release(TEXT("UiScale"));
	TestEqual(TEXT("letting go applies it"), F.Sink->Applies, 6);
	TestEqual(TEXT("the UI scale it set"), F.Sink->Values.UIScale, 1.0f, 1e-4f);
	TestEqual(TEXT("and nothing was saved"), F.Sink->Saves, 0);
	return true;
}

/**
 * CANCEL PUTS EVERYTHING BACK (Review Focus 1) - and saves what it put back, so a window-layout
 * save made while the dialog was open (FUserSettingsLayoutStore::Write saves the whole object)
 * cannot leave the live values on disk (plan 4b ruling).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsCancelTest, "AirportMgr.Settings.Panel.CancelRestoresEverything",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsCancelTest::RunTest(const FString& Parameters)
{
	SettingsPanelTest::FFixture F;
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	F.Panel->Open();
	F.ChangeEverything();
	TestFalse(TEXT("control: every control moved something"), F.Sink->Values == SettingsPanelTest::Seeded());
	F.ClickButton(TEXT("Cancel"));
	TestTrue(TEXT("every value is back"), F.Sink->Values == SettingsPanelTest::Seeded());
	TestTrue(TEXT("and what was saved is what was there before"), F.Sink->Saved == SettingsPanelTest::Seeded());
	TestFalse(TEXT("closed"), F.Panel->IsShowing());
	TestFalse(TEXT("and no modal is left"), F.Host->IsModalOpen());
	return true;
}

/** SAVE KEEPS THE CHANGE AND PERSISTS IT, and closes. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsSaveTest, "AirportMgr.Settings.Panel.SaveSaves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsSaveTest::RunTest(const FString& Parameters)
{
	SettingsPanelTest::FFixture F;
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	F.Panel->Open();
	F.Drag(TEXT("PanSpeed"), 1.8f);
	F.ClickButton(TEXT("Save"));
	TestEqual(TEXT("saved once"), F.Sink->Saves, 1);
	TestEqual(TEXT("the change was saved"), F.Sink->Saved.PanSpeedScale, 1.8f, 1e-4f);
	TestEqual(TEXT("and is still in force"), F.Sink->Values.PanSpeedScale, 1.8f, 1e-4f);
	TestFalse(TEXT("closed"), F.Panel->IsShowing());
	return true;
}

/** THE WINDOW'S CLOSE IS CANCEL (spec: Cancel / Escape / close restore the snapshot). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsCloseTest, "AirportMgr.Settings.Panel.CloseIsCancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsCloseTest::RunTest(const FString& Parameters)
{
	SettingsPanelTest::FFixture F;
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	F.Panel->Open();
	F.Drag(TEXT("PanSpeed"), 1.8f);
	TestEqual(TEXT("control: the drag was in force before the close"), F.Sink->Values.PanSpeedScale, 1.8f, 1e-4f);
	F.Host->CloseByPlayer(TEXT("settings"));
	TestTrue(TEXT("every value is back"), F.Sink->Values == SettingsPanelTest::Seeded());
	TestFalse(TEXT("closed"), F.Panel->IsShowing());
	F.Panel->Toggle();
	TestTrue(TEXT("and the next toggle opens it again"), F.Host->IsShown(TEXT("settings")));
	return true;
}

/**
 * A CUSTOM GRAPHICS MIX STAYS CUSTOM (Review Focus 4): the engine reports -1 when the player's
 * scalability is no one preset. The dropdown must show something, but neither Cancel nor a Save
 * that never touched it may turn the mix into that preset.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsCustomGraphicsTest, "AirportMgr.Settings.Panel.CustomGraphicsStaysCustom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsCustomGraphicsTest::RunTest(const FString& Parameters)
{
	FPlayerSettings Start = SettingsPanelTest::Seeded();
	Start.GraphicsQuality = -1;
	SettingsPanelTest::FFixture F(Start);
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	F.Panel->Open();
	UUiDropdown* Gfx = F.Find<UUiDropdown>(TEXT("Graphics"));
	if (!TestNotNull(TEXT("graphics"), Gfx)) { return false; }
	TestEqual(TEXT("a custom mix is no preset"), Gfx->GetSelected(), static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("and says so"), Gfx->LabelForTest().StartsWith(TEXT("Custom")));
	F.Drag(TEXT("PanSpeed"), 1.8f);
	F.ClickButton(TEXT("Save"));
	TestEqual(TEXT("a save that never touched graphics keeps it custom"), F.Sink->Saved.GraphicsQuality, -1);
	F.Panel->Open();
	F.ClickButton(TEXT("Cancel"));
	TestEqual(TEXT("and so does a cancel"), F.Sink->Values.GraphicsQuality, -1);

	// EVERY PRESET IS CHOOSABLE from a custom mix - High too, which a stand-in "High" swallowed
	// (the dropdown raises nothing for the choice already shown; 4b final review, Important 2).
	F.Panel->Open();
	const int32 AppliesBefore = F.Sink->Applies;
	F.ChooseGraphics(2);
	TestEqual(TEXT("choosing High from a custom mix applies"), F.Sink->Applies, AppliesBefore + 1);
	TestEqual(TEXT("High"), F.Sink->Values.GraphicsQuality, 2);
	return true;
}

/** "RESET WINDOW LAYOUT" FORGETS EVERY SAVED PLACEMENT (spec section 2, Persistence). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsResetLayoutTest, "AirportMgr.Settings.Panel.ResetLayoutResetsTheHost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsResetLayoutTest::RunTest(const FString& Parameters)
{
	SettingsPanelTest::FFixture F;
	if (!TestTrue(TEXT("a hosted panel"), F.Ok())) { return false; }
	TSharedPtr<FMemoryUiLayoutStore> Store = MakeShared<FMemoryUiLayoutStore>();
	FUiWindowPlacement Left;
	Left.TopLeft = FVector2D(400.0, 300.0);
	Store->Write(TEXT("ledger"), Left);
	F.Host->SetLayoutStore(Store);
	F.Panel->Open();
	F.ClickButton(TEXT("ResetLayout"));
	TestFalse(TEXT("the saved placement is gone"), Store->Read(TEXT("ledger")).IsSet());
	TestTrue(TEXT("and the dialog stays open - resetting is not closing"), F.Panel->IsShowing());
	return true;
}

#endif
