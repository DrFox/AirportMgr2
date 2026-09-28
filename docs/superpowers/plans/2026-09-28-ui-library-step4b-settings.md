# UI Library Step 4b - Modal Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A modal Settings window (Escape / gear) editing real player settings live, with Cancel restoring and Save persisting.

**Architecture:** The host gains modality (a scrim that swallows presses, one modal at a time). Settings values travel as one struct `FPlayerSettings` through an `IPlayerSettingsSink` (the `IUiLayoutStore` pattern): tests use a memory sink, the game a sink that writes `UAirportMgrUserSettings` and pushes each value to its consumer. `USettingsPanelWidget` knows only the sink and the 4a controls.

**Tech Stack:** UE 5.8 C++, UMG, `UGameUserSettings`, automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-ui-widget-library-design.md` (sections 2 Modal, 3, 5 Settings)

## Global Constraints

- Settings: modal, 420 px wide, Heading-role captions Display / Camera / Driving / Building / Windows; footer Cancel (Secondary), Save (Primary).
- Ranges: UI scale 0.75-1.5 default 1.0; pan and zoom speed 0.5-2.0 default 1.0; graphics Low/Medium/High/Epic.
- Every control applies LIVE. Opening snapshots; Cancel / Escape / close restore it; Save calls `SaveSettings()`.
- `game.settings` in `BuildActions()`: gear on the bar (Game section) and Escape, from the one registry.
- Modal scrim swallows every press and key except Escape.
- Every settings field has a consumer in this plan (CLAUDE.md "Check where a list is CONSUMED").
- No push, no PR (user, 2026-09-28).

## Review Focus

1. Cancel after changing every control: every consumer is back where it was, and nothing on disk says otherwise (a layout save mid-dialog must not persist live values).
2. Opening the dialog must apply nothing: loading controls from code raises no event (4a Focus 1), so opening never re-lanes traffic or re-applies scalability.
3. Keys under the modal: tool keys, Ctrl chords and WASD do nothing; Escape still closes.
4. Graphics reported as custom (-1) by the engine: the dropdown shows something sane and Cancel does not force a preset.
5. UI scale dragged: applied on release, not per step (4a review, Important 2).

---

### Task 1: Modal windows in the host

**Files:**
- Modify: `Source/AirportMgr/UI/UiWindowSpec.h` (`bModal`, `EUiWindowAnchor::Centre`)
- Create: `Source/AirportMgr/UI/UiScrim.h/.cpp`
- Modify: `Source/AirportMgr/UI/UiWindowHost.h/.cpp`, `Source/AirportMgr/UIStyle.h` (`Scrim` slot)
- Test: `Source/AirportMgr/UI/UiWindowHostTest.cpp`

**Interfaces:**
- Produces: `FUiWindowSpec::bModal`; `EUiWindowAnchor::Centre`; `UUiWindowHost::IsModalOpen() const`; `UUiWindowHost::ScrimForTest()`; `UUIStyle::Scrim` (FLinearColor, default black at 0.30).

- [ ] **Step 1: failing tests** - a test panel `UUiModalTestPanel` (in the test file, `WantsWindow` returns id `modaltest`, `bModal=true`, `Anchor=Centre`; `Open()`/`Close()` call `SetShown`).
  - `AirportMgr.UI.WindowHost.ModalScrimSwallowsPresses`: not open -> `IsModalOpen()` false, scrim Collapsed. Open -> true, scrim not Collapsed, scrim Z below the modal window's Z and above every other window's, and `TakeWidget()->OnMouseButtonDown`, `OnMouseButtonDoubleClick` and `OnMouseWheel` are handled. Close -> false, scrim Collapsed.
  - `AirportMgr.UI.WindowHost.ModalOpensCentred`: anchors (0.5,0.5), alignment (0.5,0.5), position 0.
- [ ] **Step 2: run, expect FAIL (no bModal/Centre - compile) then assertion failures.**
- [ ] **Step 3: implement.** Scrim: `UUserWidget`, root `UBorder` brushed `Style.Scrim`, self `Visible`; the three mouse overrides return `FReply::Handled()`. Host `Initialize` builds it full-canvas (anchors 0..1), Collapsed. `Apply(E)`: after a visibility change on a modal entry, `UpdateScrim()`: scrim visible iff any modal wanted-and-not-closed; when shown, `scrim Z = ++TopZ`, `window Z = ++TopZ`. Log `Window host: modal <id> up / down`.
- [ ] **Step 4: run `-Filter AirportMgr.UI.WindowHost`, expect PASS.**
- [ ] **Step 5: commit** `feat(ui): modal windows - a scrim that swallows presses`.

### Task 2: Player settings - the values, their store, and three consumers

**Files:**
- Create: `Source/AirportMgr/PlayerSettings.h/.cpp`, `Source/AirportMgr/PlayerSettingsTest.cpp`
- Modify: `Source/AirportMgr/AirportMgrUserSettings.h` (four config fields), `BuildCameraComponent.h/.cpp`, `BuildCameraComponentTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct FPlayerSettings { float UIScale=1; float PanSpeedScale=1; float ZoomSpeedScale=1;
      bool bGridSnapOnStart=false; int32 GraphicsQuality=-1; EDriveSide DriveSide=EDriveSide::Right;
      bool operator==(const FPlayerSettings&) const = default; };
  class IPlayerSettingsSink { virtual FPlayerSettings Read() const = 0; virtual void Apply(const FPlayerSettings&) = 0; virtual void Save() = 0; };
  class FMemoryPlayerSettingsSink : IPlayerSettingsSink { FPlayerSettings Values; int32 Applies=0; int32 Saves=0; FPlayerSettings Saved; };
  namespace PlayerSettings { void ApplyStartGrid(FSnapGuideSettings& Guides, bool bOn); }
  void UBuildCameraComponent::SetPlayerSpeedScales(double Pan, double Zoom);
  ```
  `UAirportMgrUserSettings`: `UPROPERTY(config) float UIScale=1, PanSpeedScale=1, ZoomSpeedScale=1; bool bGridSnapOnStart=false;`

- [ ] **Step 1: failing tests.**
  - `AirportMgr.Camera.PlayerSpeedScalesMultiply`: same one-second pan with scale 2 moves twice as far as with 1; one wheel notch with zoom scale 2 changes distance by twice the fraction (read `ZoomStep` from the component, not a literal).
  - `AirportMgr.Settings.StartGrid`: Off + on -> the first step `CycleGridStep` gives; Off + off -> Off; a level-set 10 m + off -> 10 m (the toggle turns a grid ON at start, never a level's own off).
- [ ] **Step 2: run, expect FAIL (compile).**
- [ ] **Step 3: implement.** Camera: `PanScale`/`ZoomScale` members (not UPROPERTY - the player's, not the designer's), `PanRate * PanScale` at both pan sites, `FMath::Min(ZoomStep * ZoomScale, 0.9)`. Fields on `UAirportMgrUserSettings` with the comment naming each consumer.
- [ ] **Step 4: run `-Filter AirportMgr.Camera+AirportMgr.Settings`, expect PASS.**
- [ ] **Step 5: commit** `feat(settings): FPlayerSettings, its sink, camera speed scales, grid on start`.

### Task 3: The Settings panel

**Files:**
- Create: `Source/AirportMgr/SettingsPanelWidget.h/.cpp`, `Source/AirportMgr/SettingsPanelTest.cpp`
- Modify: `AirportMgrPanelWidget.h` (protected `GetWindowHost()`)

**Interfaces:**
- Consumes: Task 1 `bModal`/`Centre`; Task 2 `IPlayerSettingsSink`; 4a `UUiSlider` (`OnValueChanged`, `OnValueCommitted`), `UUiToggle`, `UUiRadioGroup`, `UUiDropdown`.
- Produces: `USettingsPanelWidget::SetSink(TSharedPtr<IPlayerSettingsSink>)`, `Open()`, `Cancel()`, `SaveAndClose()`, `Toggle()`, `IsShowing()`. Controls named `UiScale`, `PanSpeed`, `ZoomSpeed`, `Graphics`, `DriveSide`, `GridSnap`, `ResetLayout`, `Cancel`, `Save`.

- [ ] **Step 1: failing tests** (memory sink; a host with the panel added; controls found by name, driven through their ENGINE-facing public handlers or engine delegates as 4a's tests do):
  - `OpenAppliesNothing`: sink values non-default; `Open()` -> each control reads its value; `Applies == 0`.
  - `EachControlAppliesLive`: pan slider moved -> `Applies==1` and sink `PanSpeedScale` matches; toggle, segmented, dropdown likewise; UI scale moved -> no apply, released -> applied.
  - `CancelRestoresEverything`: change all six, `Cancel()` -> sink values == snapshot, hidden, and a save happened (the re-save ruling, Focus 1).
  - `SaveSaves`: change one, Save -> `Saves==1`, sink values keep the change, hidden.
  - `CloseIsCancel`: host `CloseByPlayer("settings")` -> values == snapshot, `IsShowing()` false.
  - `CustomGraphicsStaysCustom`: sink GraphicsQuality -1, Open, Cancel -> still -1 (Focus 4).
  - `ResetLayoutResetsTheHost`: a memory layout store with a saved entry; click `ResetLayout` -> store empty.
- [ ] **Step 2: run, expect FAIL (compile).**
- [ ] **Step 3: implement.** `WantsWindow`: id `settings`, title Settings, modal, Centre, not resizable, closable. `BuildOnce`: 420 wide `USizeBox`, captions (Heading role, `InkMuted`), `UUiRow` rows (label left, control right), footer right-aligned. Dropdown index = level (0..3); a -1 shows High (index 2) but is left untouched unless the player picks. Each handler: `Current.X = v; Sink->Apply(Current)`. Logs `Settings: opened / <field> -> <value> / cancelled / saved / window layout reset`.
- [ ] **Step 4: run `-Filter AirportMgr.Settings`, expect PASS.**
- [ ] **Step 5: commit** `feat(settings): the modal Settings panel - live, cancel restores, save persists`.

### Task 4: Wiring - the game sink, the action, the key gate

**Files:**
- Modify: `PlayerSettings.h/.cpp` (`FGamePlayerSettingsSink`), `BuildHudLayer.h/.cpp` (SettingsPanel), `RoadBuildController.h/.cpp`, `BuildActions.cpp`, `BuildActionsTest.cpp`, `Tools/Python/fetch_ui_icons.py`, style asset via `build_ui_style.py`
- Test: `BuildActionsTest.cpp`, `PlayerSettingsTest.cpp`

**Interfaces:**
- Produces: `ARoadBuildController::ToggleSettings()`, `IsSettingsShowing()`, `IsModalOpen()`; `FGamePlayerSettingsSink(ARoadBuildController&)`; `RestoreEngineScale()`.

- [ ] **Step 1: failing tests.**
  - `AirportMgr.Actions.SettingsOnEscape`: `FindAction(Escape,false)` is `game.settings`, Game section. The old "unbound key" example moves to `EKeys::Insert`.
  - `AirportMgr.Actions.KeysIgnoredUnderModal`: a spawned controller whose hud has a test host with a modal open: `OnActionKeyForTest(One)` leaves the tool unchanged; with the modal closed it selects.
  - `AirportMgr.Settings.DriveSideIsOneValue`: controller + road actor; game sink `Apply` with Left -> `game.driveside` `IsActive` true; `Read().DriveSide` follows a `SetDriveSide` made by the bar.
- [ ] **Step 2: run, expect FAIL.**
- [ ] **Step 3: implement.**
  - Game sink `Read`: user settings fields, `GetOverallScalabilityLevel()`, `Target->GetDriveSide()`. `Apply`: writes the four fields to the settings object; `ApplicationScale` on `GetMutableDefault<UUserInterfaceSettings>()`; camera scales; scalability only if changed, via `SetOverallScalabilityLevel` + `ApplyNonResolutionSettings()` (not `ApplySettings`, which saves - `GameUserSettings.cpp:600`); drive side only if changed. `Save`: `SaveSettings()`.
  - Controller BeginPlay: sink made, `Apply(Read())`, `PlayerSettings::ApplyStartGrid`. EndPlay: `RestoreEngineScale()` (the CDO outlives PIE).
  - Gate: `RunActionForKey` and `OnCtrlActionKey` skip any action but `game.settings` while `IsModalOpen()`; `UpdateView` passes zero pan/turn.
  - `game.settings`: Escape, `ToggleSettings`, lit while open, Always enabled. Icon `lorc/cog` (checked 200 image/png 2026-09-28).
- [ ] **Step 4: run full suite, expect 0 failed 0 crashed.**
- [ ] **Step 5: commit** `feat(settings): game.settings on Escape and the gear; keys wait under the modal`.

### Task 5: Look at it

- [ ] PIE on the worktree editor (port 8002), open Settings from the gear, `shot x editor`, sample the scrim and card colours against their slots. Log lines: `Window host: modal settings up`, `Settings: opened`.
- [ ] Hand the user: drag each control, Cancel, Save, Escape (Stop rebound - see memory `unreal-escape-stops-pie`), and what each log line says.
