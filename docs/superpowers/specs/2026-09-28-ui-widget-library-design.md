# UI widget library - design

2026-09-28. Branch `feature/ui-widget-library`. Spike: `feature/ui-shape-spike` (throwaway,
worktree `C:\repos\airportmgr2-ui-shape-spike`) - reference only, not merged.

## Why

User: the UI looks amateur; wants reusable widgets, and dialogs/popups that drag and resize.
Reference look: Honeti "Flat Light Universal GUI" (Fab listing 6c1d771a-...) - white rounded
cards, hairline under a centred title, outlined-circle close, grey pill rows, gradient pill
buttons, toggles/sliders/radios.

The spike (PIE 2026-09-28, pixel-measured) proved every part of that is reachable in code with no
purchased assets: white surfaces, material-drawn rounded gradient buttons, a draggable/resizable
window on the existing card panels.

## Outcome

1. A `UUi*` widget library in `Source/AirportMgr/UI/`; every panel built from it.
2. Every floating panel is a window: drag, resize, close, bring-to-front, snap, remembered layout.
3. A modal Settings dialog that proves the new controls against real, persisted values.
4. The look: the kit's shape language, our palette, Inter, white ground.

## Rulings (from the brainstorm)

- **Shape language, not the kit's look.** Less rounded than the kit (8 px windows, 5 px controls,
  kit ~16). Move off the slate-blue ground to WHITE; keep the rest of our palette (Accent yellow,
  Warning brick, Positive sage, slate ink). No kit purchase.
- **Gradients come from a UI material**, not PNGs.
- **Scope:** existing panels + one new dialog, so every widget has a real consumer. The dialog is
  **Settings**.
- **Font: Inter** (OFL) - legible small, tabular figures, "not as cutesy" as Poppins.
- **Windows v1:** drag, resize, close, remember position/size, snap to edges, bring to front on
  click, Settings modal.
- **Approach A:** composite widgets built in C++ over UMG (the pattern today's panels use), plus
  one window host. Rejected: native Slate + UMG wrappers (twice the code, the wrappers a second
  list that must agree); styled stock UMG (the look retyped per call site - what
  `ApplyButtonLook` in the spike already had to fix; no stock window).
- **Settings key: Escape.** Unbound in the play driver (only `BuildActionsTest.cpp:84` names it,
  as an example unbound key - that test moves to another key). Settings is play-only, so the
  editor mode's Escape-cancels is untouched.
- The bar stays a bottom bar (standing ruling, `ui-bottom-bar-cities-skylines-style`); toasts stay
  toasts. Both are rebuilt from library widgets, neither becomes a window.

## Design

### 1. Widgets (`Source/AirportMgr/UI/`)

Each builds its tree in C++ from `UUIStyle`, exposes dynamic multicast delegates (code and a later
Blueprint both bind), and names no colour - it asks the style for a MEANING.

| Widget | What | First consumers |
|---|---|---|
| `UUiButton` | label, optional icon, `EUiButtonKind` Primary (Accent) / Secondary (Control) / Danger (Warning) / Ghost (transparent); `Selected` | bar tools + snap toggles, Accept/Decline, inspector verbs, Land rows, window close, Settings Save/Cancel |
| `UUiRow` | Well-coloured rounded row: leading text, trailing value or control | ledger, offer, Land and Settings rows |
| `UUiToggle` | switch: pill track (Rule off, Accent on), sliding knob | Settings: grid snap on start |
| `UUiSlider` | track, round knob, formatted value readout, min/max/step | Settings: UI scale, pan speed, zoom speed |
| `UUiRadioGroup` | one-of-N as a segmented pill | Settings: drive side |
| `UUiDropdown` | button opening a popup list | Settings: graphics quality |
| `UUiWindow` | title, optional close, hairline, scroll body with one content slot, resize grip, min/max size | inspector, ledger, Land, offers, Settings |

- **One fill recipe.** `M_UI_Rounded` draws every gradient fill; `FSlateRoundedBoxBrush` every
  flat one. The material outputs a grey SHADE and a MASK only - Slate multiplies by the vertex
  colour itself (`SlateElementPixelShader.usf` GetColor, `FinalColor = BaseColor * InVertex.Color`);
  multiplying again squared the tint in the spike. Corners come from uv3 (element size, px) x uv4
  (0..1 across the element) - no per-widget material instance.
- A phase is an enum: button kind, toggle state, window gesture (`None/Move/Resize`).
- Out of v1 (no consumer): checkbox, text input, tabs, progress bar.

### 2. Window host

`UUiWindowHost`: one full-screen `UUserWidget` over a canvas, added by `UBuildHudLayer` at Z 1
(bar 0, toasts 2 - unchanged). The only object that sees more than one window.

- `UBuildHudLayer` keeps `CreateConfiguredWidget` (the Blueprint-class hook survives) but hands
  each panel to `Host->AddWindow(Panel, FUiWindowSpec)` instead of `AddToViewport`.
  `FUiWindowSpec`: id (`"ledger"`), title, closable, resizable, modal, default anchor + offset,
  min size.
- The host wraps the panel in a `UUiWindow`. Panels keep their logic; they show/hide through
  `Host->Show(Id) / Hide(Id) / IsShown(Id)`. `EnsureCardRoot`, `SetCardShown`, `CardWidget` are
  removed. `DockAbove` becomes the inspector's DEFAULT placement (bottom-left, clear of the bar's
  live height) until the player moves it.
- **Drag/resize:** the spike's gesture code, moved here. Clamped to the screen with the bar's live
  height excluded, so no window hides under the bar.
- **Snap:** while dragging, an edge within `SnapDistance` (12 px) of a screen edge, the bar's top
  or another window's edge snaps to it. Pure functions in `UI/WindowSnap.h`, no engine types beyond
  `CoreMinimal.h`, table-tested.
- **Bring to front:** a press anywhere on a window raises it in the canvas z-order.
- **Modal:** `bModal` inserts a dim full-screen scrim under the window that swallows every press
  and key except Escape (closes). Settings only.
- **Persistence:** on move/resize end, write `{id -> rect}` to `UAirportMgrUserSettings`. On open,
  use the saved rect if it is still on screen, else the default. Settings has "Reset window layout".
- **Input:** host root `SelfHitTestInvisible` - empty screen still reaches the game. A window eats
  presses over its own pixels: #377's rule (`UAirportMgrPanelWidget::NativeOnMouseButtonDown`)
  moves into `UUiWindow` with its test (`ChromeEatsClicks`).
- **Logs (`LogRoadBuild`):** window opened / closed; move / resize begin and end with the rect;
  snapped to what; layout restored from settings vs default.

### 3. Settings and player settings

`UAirportMgrUserSettings : UGameUserSettings`, set as `GameUserSettingsClassName` in
`DefaultEngine.ini` - the engine's per-player object, which already owns scalability and saves to
the user's `GameUserSettings.ini`. `UAirportMgrUserSettings::Get()` is the one accessor.

| Field | Range, default | Consumer |
|---|---|---|
| `UIScale` | 0.75-1.5, 1.0 | multiplies the engine DPI curve (1080p = 1.0 still holds) |
| `PanSpeedScale`, `ZoomSpeedScale` | 0.5-2.0, 1.0 | multiply `UBuildCameraComponent`'s editor-set speeds - tuning stays in the component |
| `bDriveOnLeft` | bool | the "Drive left" bar action and the setting become ONE value |
| `bGridSnapDefault` | bool | grid snap state at start |
| `WindowLayout` | `TMap<FName, FBox2D>` | section 2 |
| graphics quality | engine scalability | `SetOverallScalabilityLevel` + `ApplySettings(false)` |

`USettingsPanelWidget`, modal, 420 px wide, Heading-role group captions:

- **Display:** UI scale (slider), Graphics (dropdown Low / Medium / High / Epic)
- **Camera:** Pan speed, Zoom speed (sliders)
- **Driving:** Traffic drives on (segmented Left | Right)
- **Building:** Grid snap on start (toggle)
- **Windows:** Reset window layout (Secondary)
- **Footer:** Cancel (Secondary), Save (Primary)

Every control applies LIVE. Opening snapshots all values; Cancel / Escape / close restore the
snapshot; Save calls `SaveSettings()`.

Opened by a new `game.settings` action in `BuildActions()` - bar button (gear, Game section) and
Escape come from that one registry.

### 4. Style (`UUIStyle`) and assets

Slots renamed to what they MEAN; the spike showed `PanelDark` doing two jobs (surface and the ink on
yellow). No player saves exist (`no-player-saves-yet`), so the rename is free; note the break in
the PR.

| Slot | Value | Meaning |
|---|---|---|
| `Surface` | FFFFFF | windows, bar, toasts |
| `Well` (was Panel) | F0F2F4 | rows, bar sections |
| `Control` (was Button) | E2E6EA | Secondary button fill |
| `Ink` (was Text) | 3E4A54 | text on Surface / Well |
| `InkMuted` (was TextMuted) | 7D8B96 | headings, disabled |
| `InkOnAccent` (PanelDark's 2nd job) | 3E4A54 | text on Accent / Warning / Positive |
| `Rule` | E2E6EA | hairlines, toggle track off |
| `Shadow` | 000000 a0.16 | window shadow |
| `Accent`, `Warning`, `Positive`, `HudGround` | unchanged | |

Metrics: `WindowRadius` 8, `ControlRadius` 5, `TitleBarHeight` 34, `SnapDistance` 12,
`WindowMinSize` 180x90; existing paddings kept.

Type: Inter Regular + SemiBold as composite font `F_Inter`, imported headlessly by
`build_ui_font.py`, verified by reading the face back AND a PIE capture. `ApplyText` roles
unchanged, defaulting to Inter. Clock and money use tabular figures IF the engine exposes the
OpenType feature; if not, stated in the PR and left proportional.

Scripts:
- `build_ui_material.py` - `M_UI_Rounded` (the spike's, corrected).
- `build_ui_style.py` - gains the new slots; the ONE style script; warns rather than refusing to
  save colours when the icon manifest is absent.
- `build_ui_font.py` - `F_Inter`.
- Gear icon fetched as the others are (`fetch_ui_icons.py`).
- The spike's `spike_ui_style_white.py` is not carried over.

### 5. Testing

Headless `AirportMgr.UI.*`, each asserting behaviour with a named reason:

- **Per widget, at the composition level:** build, drive the delegate, assert state and event -
  slider clamps and quantises to step; toggle flips `Selected` and its slot; segmented keeps
  exactly one selected; dropdown raises the chosen index; button kind picks its slot.
- **Snap:** `WindowSnap` table tests - within / outside 12 px, two windows, bar top, a corner
  snapping on both axes.
- **Host:** show/hide by id; bring-to-front reorders; modal scrim swallows a press; an off-screen
  saved rect falls back to default; layout round-trips through `UAirportMgrUserSettings`.
- **Input:** a window eats presses over its pixels (`ChromeEatsClicks` ported); the empty host
  passes them through.
- **Settings:** Cancel restores every snapshotted value; Save persists; the drive-side setting and
  bar action read the same value.
- **Lists agree:** `game.settings` is on the bar and resolves Escape.
- **Check-Architecture rule:** `SetBrushColor(` / `SetBackgroundColor(` outside `UI/` is flagged -
  panels use widgets, never retype a look (#255: the shape, not the site).

By eye: a 1080p PIE capture after each step, pixels checked against slot values as the spike did
(`Tools/Mcp.py shot x editor` downsamples to ~1280 wide; judge colour by sampling, not squinting).
Drag, snap, modal and keys go to the user (synthetic input does not reach Slate here), with the
log lines to look for.

## Delivery

One branch, one PR per step, each built and visible:

1. Style renames, Inter, `M_UI_Rounded`, `UUiButton`, `UUiRow`; bar, offers, Land and ledger
   rows move over. Everything already looks new here.
2. `UUiWindow` + `UUiWindowHost`: drag, resize, close, bring-to-front, snap; four panels become
   windows; `EnsureCardRoot` goes.
3. `UAirportMgrUserSettings` + window layout persistence.
4. Toggle, slider, segmented, dropdown + the modal Settings dialog, Escape.

## Known risks

- **UI scale** multiplying the DPI curve: needs checking that `UUserInterfaceSettings` can be scaled
  at runtime without a restart; fallback is `SetApplicationScale` on the game viewport.
- **Tabular figures** in Slate fonts may not be reachable; stated fallback above.
- **Dropdown popup** must draw above every window and the modal scrim - a `UMenuAnchor` places it
  in its own layer; verify it is not clipped by the window's `ClipToBounds`.
- **Blueprint restyle hooks:** the `*Class` config properties on `UBuildHudLayer` survive, but a
  Blueprint panel that built its own card would now get a window around it. None is configured
  today (checked for #94 / #192); re-check before step 2.
