# Game UI: style system, the build bar, and the notification centre

2026-09-12. Companion to `2026-09-12-environment-art-direction-design.md`, which this
borrows its palette from on purpose: UI and world should look like one design.

## 1. What is wrong now

Measured from a PIE screenshot, not from an impression.

The bar renders as one row of seventeen identically-weighted buttons in the engine's default
Slate skin:

```
Day 1 09:21 x1  Slower(Comma) Pause(P) Faster(Period) Select(4) Taxiway(1) Apron(2)
Stand(3) Guidelines(5) Runway(6) Holding point(8) Road(0) Fuel depot(0) Remove Insert
Undo(Ctrl+Z) Redo(Ctrl+Y) Clear(Backspace) ...
```

- **No icons anywhere**, and the key is baked into the label - `Taxiway (1)` reads as a
  debug menu.
- **The sections already exist in code and render as nothing.** `EActionSection` has Time,
  Tools, Edit, Aircraft, Selection and Game; the bar draws them as one undifferentiated run.
- **About 30 px tall** in a 761 px window. Hit targets are the size of the text.
- **No panel.** The bar floats on a black strip.
- `Offers 0` is an unstyled grey box in a corner.

And one defect rather than a matter of taste. `BuildBarWidget.cpp:272-276`:

```cpp
void UBuildBarWidget::OnNotification(const FString& Text)
{
    if (NotificationText != nullptr) { NotificationText->SetText(FText::FromString(Text)); }
}
```

Every notification **overwrites the previous one** and nothing ever clears it. Two events in
the same second and only the second is ever seen. There is no notification system today;
there is one label.

## 2. Direction

**Cities: Skylines / Two Point tier.** Icon-led buttons with labels beneath, visibly separate
sections, a distinct status strip, states that read instantly, hit targets you do not have to
aim at.

### 2.1 Palette

The **buildings** row of the concept sheet, so the UI is literally the colour the hangars and
terminal will be.

| Slot | Value | From |
|---|---|---|
| `Panel` | `#4F5E6A` | sheet, buildings: slate blue |
| `PanelDark` | `#3E4A54` | darkened slate, for the status strip |
| `Button` | `#5D6D7A` | lightened slate |
| `Accent` | `#F4BA38` | sheet, vehicles: yellow. The selected tool, and nothing else. |
| `Text` | `#E4E0D9` | sheet, buildings: cream |
| `TextMuted` | `#9FB0BD` | desaturated slate |

**SEMANTIC SLOTS, NOT PER-WIDGET COLOURS**, and that is the whole of the answer to "is it
easy to change later". Two other directions were mocked up - a dark HUD and a cream
paper-and-ink - and each is reachable from these six values alone. Name a slot for what it
IS (`Panel`, `Accent`) and never for where it appears (`BarBackground`, `SelectedToolTint`),
or a re-skin becomes a hunt.

## 3. Authoring: C++ widgets, styled from a Data Asset

`UUIStyle`, a `UDataAsset`, resolved in exactly one function in the shape
`UAirsideSettings::Resolve*` already uses. It holds the six colours above plus fonts, sizes,
paddings, corner radius, brushes for the four button states, and the icon map.

Rejected, and why:

- **A hand-authored Widget Blueprint** gives WYSIWYG layout, and `BuildBarWidget.h:41-50`
  already documents the seam for one. But it cannot be scripted at all - that header records
  the finding: *"Authoring that asset from Python was tried and is not possible on this
  engine build: `UWidgetBlueprint::WidgetTree` is not a scriptable property."* It also stops
  the bar being unit-testable and reintroduces the stale-Blueprint failure mode CLAUDE.md
  warns about.
- **CommonUI** is built for exactly this and is the right answer for a UI of many screens.
  It is also opinionated, wants the existing widgets rebuilt on its base classes, and puts a
  long runway before anything looks better.

The cost of this choice, stated plainly: **colours and fonts are free forever; layout is
code.** Moving a button between sections is one line in the registry and a rebuild. Changing
two rows into three is C++.

## 4. Icons

From game-icons.net, CC-BY. `Tools/Python/fetch_ui_icons.py` downloads each by author and
name and imports it as a texture, following the `Tools/Python/build_*.py` convention.

The icons are 512x512 white-on-black silhouettes, which tint cleanly to any slot colour.
Sampled three during design - `delapouite/road` is a road in perspective with a dashed
centreline, `delapouite/fuel-tank` a tank with a pump, `delapouite/airplane-departure` an
aircraft over a runway line. The set covers this domain unusually well.

**EVERY DOWNLOAD IS VALIDATED, because a wrong name fails silently.** `lorc/cancel` returned
**HTTP 200 with an HTML 404 page** - `curl` reported success and wrote a file that is not an
image. The script opens each result with PIL and fails loudly naming the icon.

**Attribution is an obligation, not a nicety.** CC-BY requires crediting each author. The
script writes `Content/UI/Icons/ATTRIBUTION.md` from the same table that drives the
downloads, so the credits cannot drift from what was actually fetched.

### 4.1 Icons attach by action Id, not by a field on the action

`UUIStyle` carries `TMap<FName, TSoftObjectPtr<UTexture2D>> IconsByActionId`, and the bar
looks an icon up by `FBuildAction::Id`.

Deliberately NOT a field on `FBuildAction`. That table's Tools section is generated from
Airside's `ToolRegistry()` (`BuildActions.h:47-49`), and Airside must not learn about game
module UI textures - `Check-Architecture.ps1` enforces exactly that direction and has already
failed this session on a COMMENT that named a forbidden module.

## 5. The bar

Two rows. The second row is what creates the hierarchy the first one lacks.

```
+--------------------------------------------------------------+
| DAY 1   09:21   [<<] [||] [>>]   x1              (ledger)     |   status strip
+--------------------------------------------------------------+
|   BUILD                          |  EDIT      |  VIEW         |
|  [road] [taxi] [apron] [stand]   | [x]  [+]   | [>]  [o]      |   grouped tools
|   Road  Taxi   Apron   Stand     | Rmv  Ins   | Sel  Insp     |
+--------------------------------------------------------------+
```

- Each section gets a panel background and a heading from `ActionSectionName`.
- Icon above label. The key leaves the label and becomes a tooltip.
- Hit targets about 52-64 px against today's 30.
- The selected tool takes `Accent`; disabled takes `TextMuted` on `Button`.

**The registry is untouched.** `BuildActions()` remains THE list that key bindings, the
startup banner and the bar are all generated from, so
`AirportMgr.Actions.BarBuildsFromRegistry`, `RegistryIsComplete` and `ToolCommandsMatchRegistry`
keep their meaning and a new action still appears on the bar the moment it is registered.

### 5.1 The ledger slot stays empty

`UScenario::StartingBalance` is authored, but nothing consumes it -
`OpsRuntime.cpp:123` says *"Balance goes to the ledger when it exists (M3)"*. A money readout
would show a number that never changes, which is worse than showing none. The strip reserves
the space and draws nothing in it.

## 6. The notification centre

**Offers are not notifications**, and merging them is how these systems go wrong. Three kinds,
distinguished by what they need from the player:

| Kind | Behaviour | Example | Publisher today |
|---|---|---|---|
| **Offer** | needs a decision, has a deadline, persists until answered | an airline offer | `UFlightBoard` |
| **Alert** | persists while a condition holds, clears when fixed | "no route from runway to any stand" | **none** |
| **Feed** | transient, informational, must not demand attention | "arrival refused - runway too short" | `OnNotification`, `OnArrivalRefused` |

Missing an offer costs money, so it must never scroll away. A feed item scrolling away is
CORRECT. An alert must sit there while the problem exists. One list cannot serve all three.

### 6.1 Model

`UNotificationCentre` - a `UObject`, world-free, built with `NewObject` and tested with no
level, the same shape as `USimClock` and `UFlightBoard`. It owns an array of entries
`{ Kind, Severity, Text, RaisedAt, SourceId }` and three rules.

**Feed entries expire on REAL seconds, not game seconds**, and that distinction is the one
`SimClock.h:25-34` exists to keep straight. A toast timed in game seconds would vanish
instantly at x32 - and x32 now exists - because the clock runs 72x the day compression times
the speed multiplier. A toast is a piece of UI a human reads, so it is timed the way the
movement layer is: `Multiplier()` only, never `TimeScale()`. Eight real seconds at x1 is the
starting value. The scrollback holds the last 50 entries and drops oldest first, so a long
session cannot grow it without bound.

**Alerts are keyed by `SourceId`** so raising the same condition twice does not stack, and
clearing it removes exactly one entry.

**Offers are NOT stored here.** `UFlightBoard` already owns them, and a second store would be
a second source of truth about what the player has been offered - the failure this codebase
has shipped three times. The centre knows the offer KIND exists so the three surfaces can be
reasoned about together, and the inbox widget continues to read the board directly through
its existing viewmodel.

It subscribes to `UOpsEvents`. It is the ONLY subscriber that turns events into user-visible
entries, so there is one place that decides what is worth telling the player.

### 6.2 Surfaces

- **Inbox, top right.** The existing `UOfferInboxWidget` and its viewmodel, restyled onto the
  new panel style, still reading `UFlightBoard` and not the centre (see 6.1). Shows fee and a
  live countdown. Never auto-dismisses.
- **Feed, bottom right above the bar.** Stacked toasts, newest at the bottom, fading
  oldest-first. Replaces the single overwritten label and fixes the defect in section 1.
- **Alert strip, top centre.** Absent when nothing is wrong, so its presence alone carries
  meaning.

### 6.3 Alerts get a category and a slot, and no event types

`OpsEvents.h:26-28` states the rule: *"Only events with a PUBLISHER in this milestone exist
here. Flight, job, ledger and contract events arrive with the systems that raise them;
declaring them now would be a list nothing consumes, which is the bug CLAUDE.md names three
times."*

So `ENotificationKind::Alert` exists and the widget exists, and **no alert event is declared
until a system raises one**. The first real publisher is likely a routing check - "no route
from runway to any stand" - which belongs to the slice that adds it, not to this one.

## 7. Slices

| | Slice | |
|---|---|---|
| **A** | `UUIStyle` + resolver + fetched icons + attribution | nothing visible changes; everything later depends on it |
| **B** | The bar restructured onto the style | the thing the player looks at constantly |
| **C** | `UNotificationCentre` + feed toasts | fixes the overwrite defect |
| **D** | Offer inbox restyled | reuses the existing viewmodel |
| - | Alert strip | slot only; deferred until a publisher exists |

A first, because a style asset with nothing using it is provably correct and cheap to change.
B before C because the bar is seen constantly and the feed is not.

## 8. Testing

- `BarBuildsFromRegistry`, `RegistryIsComplete` and `ToolCommandsMatchRegistry` must keep
  passing untouched. If restyling breaks them, the registry seam has been damaged.
- **Every action in `BuildActions()` resolves an icon.** Walks the registry, not a
  hand-written list, so an action added without an icon fails loudly - the same shape as
  `SpeedLadderCoversEveryRung`.
- **The style resolver never returns null.** It falls back to the CDO, which is the lesson
  from `ResolveDefaultScenario`: that function returned null, every caller guarded with
  `if (...)`, and the "built-in defaults" it logged were applied to nothing for weeks.
- `UNotificationCentre` is world-free, so all of this is testable with no level: a feed entry
  expires after its real-seconds lifetime and NOT sooner at x32 - the test advances at a high
  multiplier and asserts the toast is still up, which is the regression a game-seconds
  implementation would fail; the scrollback drops oldest first at 50; raising the same alert
  `SourceId` twice yields one entry; clearing an alert removes it.
- Not unit-testable and therefore verified by screenshot: that it LOOKS right. Three
  captures - bar idle, a tool selected, an offer pending - per CLAUDE.md's rule that a
  screenshot is admissible evidence and an assertion about appearance is not.

## 9. Out of scope, named

- **The alert publisher.** Category and widget only.
- **The ledger readout.** Slot reserved, empty until M3.
- **Animation, transitions and UI sound.** The tier above the one chosen. The style asset
  should not make them harder, and no more than that.
- **The inspector panel.** It exists and works; it gets the new style in a later slice.
- **Tooltips beyond the key hint.** Cost and description in a tooltip wants data the actions
  do not carry yet.

## 10. Verified during design, and not

**Verified.** The current bar and its defect, read from source and a PIE screenshot. That
game-icons.net is reachable and its PNG endpoint returns real 512x512 images. That a wrong
icon name returns HTTP 200 and an HTML page. That `UOpsEvents` already carries
`OnNotification` and that its only consumer overwrites a single label. That the sections
already exist in `EActionSection` and render as nothing.

**Not verified.** That `UUIStyle`'s brushes produce the mocked appearance in UMG - the
mockups are HTML and CSS, and Slate's box brushes, nine-slice margins and font rendering will
not match them exactly. Expect a tuning pass against screenshots, and expect the first
attempt to look flatter than the mockup.
