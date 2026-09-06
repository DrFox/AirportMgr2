# Build Bar HUD — Design

**Status:** design. A sidequest between M1 and M2 of
`2026-09-05-game-systems-map-design.md`. Implements the first slice of that spec's §5.1
(HUD, build palette, notification feed) and nothing else from it.

**Goal:** every key the game module binds today is also a mouse click on a horizontal bar
across the bottom of the screen, sectioned Cities Skylines style, and the bar cannot show
a control that goes nowhere.

**Nothing here is law** — §0 of the systems map applies.

---

## 1. Decisions

| Question | Decision | Why |
|---|---|---|
| UI tech | UMG. C++ base class owns logic; a Blueprint widget supplies layout and style. | Restyle without a build; logic stays in C++ where tests reach it. |
| Layout | One horizontal bar along the bottom, sections: Time, Tools, Edit, Aircraft, Game. | The reference games' idiom; the user thinks in it. Never a vertical toolbar. |
| Who owns what | Designer owns the chrome (bar, section containers); code fills each section from the registries at runtime. | A new action appears when registered. The asset never has to list actions, so the "two lists" bug cannot return. |
| Click modifiers | Sticky Remove and Insert buttons, one `EClickModifier` enum, ORed with the held keys. | A phase is an enum, never a set of bools. Keys keep working and light the button. |
| Asset authoring | Headless Python creates the Blueprint widget with five named empty panels and two text blocks. | The user asked me to author it; the panel API is BlueprintCallable so a script can build containers. Buttons are NOT in the asset. |
| No asset | The C++ class creates plain panels for any missing slot. | The bar works with zero content; a missing asset degrades, never breaks. |

Rejected: building the whole tree in C++ (designer sees nothing); Python authoring every
button into the asset (two lists); Slate (a second UI API for no gain here).

---

## 2. The action registry (game module)

`Source/AirportMgr/BuildActions.h/.cpp`

```cpp
enum class EActionSection : uint8 { Time, Tools, Edit, Aircraft, Game };

struct FBuildAction
{
	FName Id;                 // "tool.road", "time.pause", "edit.undo", "game.save"
	EActionSection Section;
	FText Label;
	FKey Key;                 // EKeys::Invalid for none
	bool bRequiresCtrl = false;
	TFunction<void(ARoadBuildController&)> Execute;
	TFunction<bool(const ARoadBuildController&)> IsActive;   // lit
	TFunction<bool(const ARoadBuildController&)> IsEnabled;  // greyed when false
};

TConstArrayView<FBuildAction> BuildActions();
```

- Function-local static, the same shape and for the same reason as `ToolRegistry()`.
- The **Tools** section is generated from `ToolRegistry()`: one entry per registration,
  `Id = tool.<name>`, `Key = Registration.Key`, `Execute` selects that tool by index,
  `IsActive` compares the active tool index. The tool table stays the one source for tools.
- The other entries, in bar order:

| Section | Id | Label | Key | Execute | IsActive | IsEnabled |
|---|---|---|---|---|---|---|
| Time | time.slower | Slower | Comma | StepSpeed(-1) | – | runtime present |
| Time | time.pause | Pause | P | TogglePause | paused | runtime present |
| Time | time.faster | Faster | Period | StepSpeed(+1) | – | runtime present |
| Edit | edit.remove | Remove | – | toggle modifier Remove | modifier == Remove | always |
| Edit | edit.insert | Insert | – | toggle modifier Insert | modifier == Insert | always |
| Edit | edit.undo | Undo | Ctrl+Z | OnUndo | – | CanUndo |
| Edit | edit.redo | Redo | Ctrl+Y | OnRedo | – | CanRedo |
| Edit | edit.clear | Clear | Backspace | OnClearNetwork | – | network non-empty |
| Aircraft | aircraft.land | Land | Seven | OnLandAircraft | – | a runway exists |
| Aircraft | aircraft.watch | Watch | C | ToggleWatchAgent | watching | an agent exists |
| Aircraft | aircraft.guidelines | Guidelines | G | OnToggleGuidelines | overlay on | always |
| Game | game.save | Save | K | OnQuickSave | – | runtime present |
| Game | game.load | Load | L | OnQuickLoad | – | runtime present |

- Three consumers, one list: `SetupInputComponent` binds every entry with a key (Ctrl
  entries bind the chord, replacing the "check Ctrl inside the handler" rule, so key and
  button mean the same thing); the "Road building ready" banner prints every entry; the bar
  builds one button per entry.
- Camera keys (WASD, Q/E, wheel, mouse buttons) are not actions. They are continuous
  inputs and stay bound by hand.

---

## 3. Controller changes

- Handlers stay as they are and become the executors. The hand-written `BindKey` block is
  replaced by a loop over the registry. `UE_LOG` count does not fall.
- `EClickModifier { None, Remove, Insert }` replaces the held-key checks as the source of
  the tool context's two flags: `bRemove = Mode == Remove || CtrlHeld`,
  `bInsert = Mode == Insert || ShiftHeld`. Selecting any tool resets the mode to None, so a
  mode cannot outlive the tool it was chosen for. `OnUndo`/`OnRedo` drop their
  `IsRemoveHeld()` guard because the chord binding now carries it.
- Read-only queries the bar and the registry need: `GetClickModifier()`,
  `IsWatchingAgent()`, `CanRedo()`, `HasNetworkContent()`, `HasRunway()`. `GetActiveTool()`
  and `CanUndo()` exist.
- Input mode `FInputModeGameAndUI` with the cursor shown at BeginPlay, so a click on the bar
  is consumed by UMG and never reaches the road tool.
- `UPROPERTY(EditAnywhere) TSubclassOf<UBuildBarWidget> BuildBarClass;` created and added to
  the viewport at BeginPlay. Null means the C++ class is used and the log says so.

---

## 4. The bar widget

`Source/AirportMgr/BuildBarWidget.h/.cpp`, `UBuildBarWidget : UUserWidget`.

- Slots, all `BindWidgetOptional`: `UPanelWidget* TimeSection, ToolsSection, EditSection,
  AircraftSection, GameSection; UTextBlock* ClockText; UTextBlock* NotificationText`.
  A missing slot is created in code inside a fallback root, so the bar exists with no asset.
- `NativeOnInitialized`: for each action, construct a `UButton` holding a `UTextBlock` with
  the label, add it to the section's panel, bind `OnClicked` to run the action on the
  controller. The button remembers its action index; nothing is found by name.
- `NativeTick`: for each button, tint from `IsActive`, `SetIsEnabled` from `IsEnabled`;
  refresh `ClockText` from `UOpsRuntimeSubsystem::Get(World)->GetClock()` as
  `Day N  HH:MM  xS` plus ` PAUSED`. Polling every frame is deliberate: the fifteen states
  come from four owners, and fifteen boolean reads a frame cost nothing next to four
  subscriptions and their lifetime rules.
- `NotificationText` binds to `UOpsEvents::OnNotification` (the runtime's bus) and shows the
  latest line. The first UI consumer of the bus, as the systems spec §5.1 intended.
- Style knobs as `UPROPERTY(EditAnywhere)`: normal, active and disabled tints, button
  padding, font size. The Blueprint subclass overrides them without code.
- The canvas `ARoadBuildHUD` keeps every world-space preview. The tool-name and clock text
  lines leave it; the bar shows both now, one owner each.

---

## 5. The asset

*Amended 2026-09-06 during implementation:* the Python route is closed on this engine build.
`UWidgetBlueprint::WidgetTree` is a plain `UPROPERTY()` with no scripting exposure, so the
script cannot reach the tree (`Failed to find property 'widget_tree'`). Per the plan's
one-attempt rule the script was dropped. In its place the code-built fallback became real
chrome: a canvas root, the bar in a tinted Border anchored across the bottom edge, the
notification line above it, `BarTint`/`BarHeight` as style knobs. That is the default look.
A Blueprint is now an optional restyle authored by hand from the seven slot names listed
in `UBuildBarWidget`'s header comment; no config default is shipped until one exists.
The original plan for this section follows for the record.


`Tools/Python/build_bar_widget.py` creates `/Game/UI/WBP_BuildBar`, parent
`UBuildBarWidget`: a Canvas root; a Border anchored to the bottom edge, full width; inside
it a HorizontalBox holding five named HorizontalBoxes (`TimeSection` … `GameSection`)
separated by spacers, `ClockText` inside the Time section, `NotificationText` above the bar.
The script asserts the seven names exist after building, and logs the class path to put in
`BuildBarClass`. Editor closed to run it, like the other authoring scripts.

`BP_RoadBuildGameMode`'s controller gets `BuildBarClass = WBP_BuildBar` (set by the user in
the editor, or by the script if the game mode asset can be edited headlessly; the script
reports which).

---

## 6. Tests

- `AirportMgr.Actions.RegistryIsComplete`: every entry has a section, a label and an
  executor; each `ToolRegistry()` tool appears exactly once and with its key; no two entries
  share key plus modifier.
- `AirportMgr.Actions.ClickModifier`: Remove then Insert leaves Insert only; selecting a
  tool clears the mode; the tool context reports the mode as the flag.
- `AirportMgr.Actions.BarBuildsFromRegistry`: create `UBuildBarWidget` with no asset in a
  test world and count buttons per section; equal to the registry's counts per section.
- Runtime check: PIE, click Road on the bar and twice on the plane, a road appears; click
  Pause, the clock shows PAUSED; the log banner lists every bar action.

---

## 7. Out of scope

Info panels, finance and other screens, tooltips, keyboard focus handling, controller
support, styling beyond the knobs above, replacing the canvas previews with UMG.
