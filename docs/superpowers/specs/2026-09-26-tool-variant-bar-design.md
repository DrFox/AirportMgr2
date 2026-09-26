# Tool variant bar - design

2026-09-26. Branch `feature/tool-variant-bar`.

## Problem

Taxiway, Road and Runway each build several variants (widths; for runways also surface and
approach). Today the player cycles them by pressing the tool's key again (`OnReselect`),
Shift/Ctrl for the runway's other axes. Nothing on screen lists the choices or says which
is selected - the only feedback is a `UE_LOG` line.

## Goal

Cities-Skylines-style popout: selecting a tool with variants shows a second row of buttons
under the tool row, one button per variant, the current one lit. Clicking picks it. Key
cycling keeps working and drives the same list.

Success: the player can see and pick every width/surface/approach without a key, and the
lit button always matches what the next click will build.

## Decisions (agreed in chat)

- Runways: **one row per axis**, stacked (Width, Surface, Approach). Roads/taxiways: one
  row (Width).
- The level-default width is **not** a button. It resolves to the matching preset for the
  highlight (see "Level default").
- Unlocks: a per-variant `bEnabled` hook only, always true. No progression system.
- Editor mode (`URoadBuildEdMode`): no new UI. It keeps key cycling, which now runs
  through the same list.
- Buttons are text only (label + detail). No icons this round.

## 1. Plugin: variants as tool data (`Tool/`)

New types in `Public/Tool/RoadBuildTool.h`, beside `IBuildTool`:

```cpp
struct FToolVariant
{
	FName Id;        // stable, e.g. "CodeC", "Asphalt", "Precision"
	FText Label;     // "Code C"
	FText Detail;    // "15 m" - may be empty
	bool bEnabled = true;  // unlock hook; always true today
};

struct FToolVariantAxis
{
	FName Id;        // "Width", "Surface", "Approach"
	FText Label;
	TArray<FToolVariant> Options;
	int32 Current = INDEX_NONE;  // INDEX_NONE = nothing lit (see Level default)
};
```

New `IBuildTool` virtuals, silent by default so the six tools without variants change
nothing:

```cpp
virtual void GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const {}
virtual bool SelectVariant(const FToolContext& Context, int32 Axis, int32 Option) { return false; }
```

- `SelectVariant` returns false and changes nothing for an out-of-range axis/option or a
  disabled option. It writes the same fields `OnReselect` writes today, including the copy
  in the live state (`State->WidthIndex`), so a chain in progress takes the new width.
- **`OnReselect` is re-expressed as "select the next option"** on axis 0 (plain), 1 (Shift),
  2 (Ctrl), skipping disabled options. One list, two gestures - the bar and the key cannot
  disagree about order or count. The existing log line (`RoadDrawTool.cpp:416`,
  `RunwayTool.cpp:63`) survives, moved into `SelectVariant` so a click logs too.
- `FRoadDrawTool`: one Width axis from `IRoadEditTarget::GetWidthCount` /
  `ResolveWidthProfile`. Label from the profile asset (display name), Detail from
  `GetTotalWidth()` in metres.
- `FRunwayTool`: Width from `GetRunwayProfileCount` / `ResolveRunwayProfile`; Surface and
  Approach from `ERunwaySurface` / `ERunwayApproach` up to their `Count` sentinels.

### Level default

Road/taxiway `WidthIndex` starts at `INDEX_NONE`, meaning the level's own tuning
(`URoadEditFacade::ResolveProfileFor`): the actor's authored `Profile` for a taxiway,
`ResolveServiceRoadProfile` for a road. That profile need not be in the content set - it
can be tuned in the Details panel.

So `GetVariantAxes` reports `Current` as the preset matching the default - same asset,
else equal `GetTotalWidth()` - and `INDEX_NONE` (nothing lit) if none matches. It does NOT
write `WidthIndex`: drawing keeps the level's tuning until the player picks a preset.
Resolving it into the index would silently change the width of every level whose tuning
is off-list. Runway `WidthIndex` already starts at a real index; no rule needed.

## 2. Session and controller forwarding

- `FBuildSession::GetActiveVariantAxes(Out)` and `SelectActiveVariant(Axis, Option)`,
  forwarding to the active tool with the session's own context.
- `ARoadBuildController` forwards both (BlueprintCallable not needed; the bar is C++).

## 3. Game: the popout row (`UBuildBarWidget`)

- New `BindWidgetOptional` panel `VariantSection` directly under the tools row; created in
  C++ when the BP layout does not supply it, as the other sections are.
- One row per axis: axis label, then one `UBuildBarEntry` per option ("Code C" / "15 m").
  Hidden when the active tool reports no axes.
- **Rebuild** only when the active tool index, or the axis/option **Ids**, differ from the
  last build - compared by name, not count (CLAUDE.md: lists that must agree check
  identity). **Refresh** lit/enabled state each `NativeTick`, as `RefreshStateFor` does.
- Click calls `SelectActiveVariant`. A disabled option is greyed and not clickable.
- Style from `UUIStyle` (lit/unlit colours the tool row already uses).

## Logging

- `LogRoadBuild`: `Variant bar rebuilt for <tool>: <axis>[<ids>] ...` on each rebuild.
- `LogAirside`: `<tool> <axis> -> <option id> (<i>/<n>)` on each accepted `SelectVariant`
  (replaces, not adds to, the two existing cycle lines - same information plus the axis).

## Tests

World-free (`Model`-level, fake `IRoadEditTarget`):

1. Each tool's axes match its content arrays by Id and order.
2. `SelectVariant(0, k)` then a draw: `ConnectNodes`/`PlaceRunway` receives profile k.
3. `OnReselect` walks exactly the axis options in order and wraps; Shift/Ctrl move runway
   axes 1/2 only.
4. Level default: default profile in the set lights that index and leaves `WidthIndex`
   `INDEX_NONE`; off-list default lights nothing.
5. `SelectVariant` rejects out-of-range and disabled options without changing state.
6. Tools with no variants report no axes.

Composition (fails if the forwarder is unwired): `FBuildSession` with Taxiway active reports
the Width axis; `SelectActiveVariant` changes the tool's `GetWidthIndex()`.

The widget itself is verified in PIE: select Taxiway, row shows Code B-F with the default
lit; click Code E, draw, ghost and built segment are 23 m; press `1`, lit button advances.

## Out of scope

Icons per variant, editor-mode palette, a real unlock system, variants for Stand
(size is drag-derived) and FuelDepot.
