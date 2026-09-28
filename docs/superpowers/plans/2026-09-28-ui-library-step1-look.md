# UI Library Step 1 - The Look: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every existing panel draws in the new look - white surfaces, our palette under meaning-named slots, Inter, material-drawn rounded gradient buttons - through two library widgets, `UUiButton` and `UUiRow`.

**Architecture:** `UUIStyle` gains meaning-named slots, a button-fill brush (M_UI_Rounded through one cached dynamic instance) and an Inter composite font built in C++ from two imported `UFontFace` assets. `UUiButton` (a `UButton` subclass) owns the enabled/selected/kind colour rule that four widgets copy today; `UUiRow` (a `UBorder` subclass) owns the Well row. The bar, inbox, Land, ledger and inspector move onto them; a lint rule forbids the old shape.

**Tech Stack:** UE 5.8 C++ (UMG, Slate, SlateCore), editor Python (headless commandlet), PowerShell lint.

**Spec:** `docs/superpowers/specs/2026-09-28-ui-widget-library-design.md` - this plan is its Delivery step 1. Steps 2-4 get their own plans, written against the code this one leaves.

## Global Constraints

- Worktree `C:\repos\airportmgr2-ui-widget-library`, branch `feature/ui-widget-library`. Build with `-NoHotReloadFromIDE` only if an editor is open on ANOTHER checkout (check `Get-CimInstance Win32_Process | ? Name -like 'UnrealEditor*' | select CommandLine`).
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-ui-widget-library\AirportMgr.uproject" -WaitMutex`
- Tests: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-ui-widget-library\AirportMgr.uproject" -Filter AirportMgr.UI` - read its `N test(s) run, N failed, N crashed` line, never the exit code.
- A NEW test .cpp needs two builds: the first reports Succeeded without compiling it. Grep the build log for the file name.
- Headless scripts: `D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<uproject>" -run=pythonscript -script="<abs path>" -unattended -nosplash -nopause`, then `Select-String Saved\Logs\AirportMgr.log -Pattern MARKER`. Editor must be closed.
- Write Python and C++ edits with the Write/Edit tools, not bash heredocs or `python -c` - those mangle `\n` inside string literals (spike, 2026-09-28). `python -c "import ast; ast.parse(open(p).read())"` before running a script.
- Slot values (spec section 4, verbatim): Surface FFFFFF, Well F0F2F4, Control E2E6EA, Ink 3E4A54, InkMuted 7D8B96, InkOnAccent 3E4A54, Rule E2E6EA, Shadow 000000 a0.16; Accent F4BA38, Warning C45D45, Positive 7E9C6B, HudGround unchanged. WindowRadius 8, ControlRadius 5.
- The material outputs SHADE and MASK only. Slate multiplies by the vertex colour itself (`SlateElementPixelShader.usf` GetColor). Never multiply by VertexColor in the material.
- Commit messages: concise, no Co-Authored-By trailer (user rule).
- Comments explain WHY and travel with their code; count `UE_LOG(` in touched files before and after - none may be lost.

## Review Focus

1. **A disabled Primary (Accept on an unacceptable offer) must not shout.** Expect Control fill + InkMuted, not Accent. Pinned in Task 4's `LookFor` table test.
2. **Per-tick repaint must be free when nothing changed.** The bar calls `SetState` ~70 times a tick. Expect no `SetBackgroundColor` when state is unchanged. Pinned by `PaintCountForTest` in Task 4.
3. **No style asset configured (fresh checkout, CDO style).** Expect buttons to draw a flat rounded brush and text to fall back to the engine font - no null deref, no invisible text. Pinned in Tasks 2 and 3 against `GetDefault<UUIStyle>()`.
4. **Text contrast on every surface it lands on.** Expect Ink on Surface >= 4.5:1, InkOnAccent on Accent >= 4.5:1, InkMuted on Surface and on Well >= 3:1. Pinned in Task 1.
5. **A button whose label changes every tick (bar DynamicLabel) must not rebuild its content.** Expect `SetLabel` on a built button to only `SetText`. Pinned in Task 4.

---

### Task 1: Meaning-named slots

Renames and adds `UUIStyle` colour slots, renames `CornerRadius` to `WindowRadius`, adds `ControlRadius`, and updates every reader. Pure recolour: no widget structure changes.

**Files:**
- Modify: `Source/AirportMgr/UIStyle.h` (colour block, metrics)
- Modify: every reader - `AirportMgrPanelWidget.cpp`, `BuildBarWidget.cpp`, `BuildBarVariants.cpp`, `InspectorWidget.cpp`, `InspectorWidget.h`, `LandAircraftPanelWidget.cpp`, `LedgerPanelWidget.cpp`, `OfferInboxWidget.cpp`, `ToastStackWidget.cpp`, `RoadBuildHUD.cpp` (HudGround only, check)
- Modify tests: `UIStyleTest.cpp`, `InspectorWidgetTest.cpp`, `ToastStackWidgetTest.cpp`
- Modify: `Tools/Python/build_ui_style.py`

**Interfaces:**
- Produces (used by every later task): `UUIStyle::Surface, Well, Control, Ink, InkMuted, InkOnAccent, Rule, Shadow, Accent, Warning, Positive, HudGround` (all `FLinearColor`); `float WindowRadius = 8`, `float ControlRadius = 5`.
- Rename map (mechanical): `Panel`->`Well`, `Button`(the colour)->`Control`, `Text`->`Ink`, `TextMuted`->`InkMuted`, `CornerRadius`->`WindowRadius`. `PanelDark` is REMOVED: where it was a surface it becomes `Surface` (bar border, card, toast) or `Well` (status strip); where it was ink on Accent it becomes `InkOnAccent` (BuildBarWidget.cpp:480, BuildBarVariants.cpp:95, OfferInboxWidget.cpp:296); LandAircraftPanelWidget.cpp:142's disabled-row fill becomes `Control`.

- [ ] **Step 1: Write the failing contrast test** - append to `Source/AirportMgr/UIStyleTest.cpp` before `#endif`:

```cpp
namespace UIStyleContrast
{
	/** WCAG relative luminance of a LINEAR colour - the slots are stored linear already. */
	double Luminance(const FLinearColor& C) { return 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B; }
	double Ratio(const FLinearColor& A, const FLinearColor& B)
	{
		const double LA = Luminance(A), LB = Luminance(B);
		return (FMath::Max(LA, LB) + 0.05) / (FMath::Min(LA, LB) + 0.05);
	}
}

/**
 * THE WHITE GROUND IS ONLY SAFE IF EVERY INK STILL READS ON IT. The old palette was cream on
 * slate; flipping the ground to white flips which slot is the ink, and PanelDark was quietly
 * doing two jobs (a surface AND the text on yellow) - so each ink is pinned against the surface
 * it actually lands on. 4.5:1 is WCAG AA for body text, 3:1 for the muted headings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleContrastTest,
	"AirportMgr.UI.EveryInkReadsOnItsSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleContrastTest::RunTest(const FString& Parameters)
{
	using namespace UIStyleContrast;
	for (const UUIStyle* Style : { GetDefault<UUIStyle>(), UAirportMgrUISettings::ResolveStyle() })
	{
		if (!TestNotNull(TEXT("a style"), Style)) { return false; }
		TestTrue(TEXT("Ink on Surface >= 4.5"), Ratio(Style->Ink, Style->Surface) >= 4.5);
		TestTrue(TEXT("Ink on Well >= 4.5"), Ratio(Style->Ink, Style->Well) >= 4.5);
		TestTrue(TEXT("InkOnAccent on Accent >= 4.5"), Ratio(Style->InkOnAccent, Style->Accent) >= 4.5);
		TestTrue(TEXT("InkMuted on Surface >= 3"), Ratio(Style->InkMuted, Style->Surface) >= 3.0);
		TestTrue(TEXT("InkMuted on Well >= 3"), Ratio(Style->InkMuted, Style->Well) >= 3.0);
		TestTrue(TEXT("Surface and Well are distinguishable"), !Style->Surface.Equals(Style->Well));
	}
	return true;
}
```

- [ ] **Step 2: Build; verify it fails to COMPILE** (`Ink`, `Surface`, `Well`, `InkOnAccent`, `InkMuted` undeclared). Expected: `error C2039`.

- [ ] **Step 3: Replace the colour block in `UIStyle.h`.** Replace everything from `/** Section panels. Sheet: buildings, slate blue. */` through the `TextMuted` UPROPERTY line with:

```cpp
	/*
	 * SLOTS ARE NAMED FOR WHAT THEY MEAN, AND EACH MEANS ONE THING (2026-09-28, UI library
	 * step 1). The slate-blue palette had PanelDark doing two jobs - the darkest SURFACE and the
	 * INK drawn on Accent - which only worked while those were the same dark colour. The white
	 * ground split them, so they are two slots now. Colours from the concept sheet's buildings
	 * row, re-cast for a white ground; Accent/Warning/Positive unchanged.
	 */

	/** The ground every window, the bar and a toast sit on. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Surface = FLinearColor::White;

	/** A recessed surface ON Surface: a row, a bar section. Reads as a well, not a slab. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Well = FLinearColor::FromSRGBColor(FColor(0xF0, 0xF2, 0xF4));

	/** A Secondary button's fill. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Control = FLinearColor::FromSRGBColor(FColor(0xE2, 0xE6, 0xEA));

	/** Text and glyphs on Surface or Well. Sheet: buildings, dark slate. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(0x3E, 0x4A, 0x54));

	/** Section headings, secondary facts, disabled labels. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor InkMuted = FLinearColor::FromSRGBColor(FColor(0x7D, 0x8B, 0x96));

	/** Text and glyphs on Accent, Warning or Positive. Same value as Ink today; a separate slot
	 *  because a darker Accent would want it lighter, and Ink must not move with it. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor InkOnAccent = FLinearColor::FromSRGBColor(FColor(0x3E, 0x4A, 0x54));

	/** Hairlines: under a window title, a card's outline, a toggle track when off. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Rule = FLinearColor::FromSRGBColor(FColor(0xE2, 0xE6, 0xEA));

	/** A window's drop shadow. Translucent black, so it darkens grass and concrete alike. */
	UPROPERTY(EditAnywhere, Category = "Colours") FLinearColor Shadow = FLinearColor(0.0f, 0.0f, 0.0f, 0.16f);
```

Keep the `Accent` UPROPERTY and its comment exactly as they are (it sits between the old Button and Text lines - move it below `Shadow` unchanged). Keep `Warning`, `Positive`, `HudGround` unchanged.

- [ ] **Step 4: Replace `CornerRadius` in `UIStyle.h`** - keep its comment, change the declaration and add ControlRadius beneath:

```cpp
	UPROPERTY(EditAnywhere, Category = "Metrics") float WindowRadius = 8.0f;

	/** Corner rounding of a CONTROL - a button, a row. Smaller than WindowRadius on purpose: the
	 *  reference kit's 16 px read as a toy (user, 2026-09-28: "not as cutesy rounded"). */
	UPROPERTY(EditAnywhere, Category = "Metrics") float ControlRadius = 5.0f;
```

Edit the comment's first line from `Corner rounding, uu. CONSUMED by the toast cards` to `Window and card corner rounding, uu. CONSUMED by the toast cards`.

- [ ] **Step 5: Apply the rename map to every reader.** Use Grep `Style(->|\.)(Panel|PanelDark|Button|Text|TextMuted|CornerRadius)\b|PanelStyle->(Text|Panel|Button)\b` over `Source/AirportMgr` and edit each hit with the Edit tool per the map in **Interfaces**. Surfaces explicitly:
  - `AirportMgrPanelWidget.cpp` card: `FSlateRoundedBoxBrush(Style->Surface, Style->WindowRadius, Style->Rule, 1.0f)` (outline, because a white card on white-ish concrete needs an edge until step 2's shadow); the non-rounded branch `SetBrushColor(Style->Surface)`.
  - `BuildBarWidget.cpp`: `Border` and `ToolsBorder` and `VariantBorder` -> `Surface`; `StatusBorder` -> `Well`; section `Frame` -> `Frame->SetBrush(FSlateRoundedBoxBrush(Style->Well, Style->ControlRadius));` (add `#include "Brushes/SlateRoundedBoxBrush.h"`).
  - `ToastStackWidget.cpp:252`: `Style.PanelDark` -> `Style.Surface`, `Style.CornerRadius` -> `Style.WindowRadius`.
  - `OfferInboxWidget.cpp:250`: `FSlateRoundedBoxBrush(Style.Well, Style.ControlRadius)`.
  - Update comments that name the old slots in the same lines you edit (e.g. "cream glyph" in BuildBarWidget.cpp's RefreshStateFor becomes "the Ink glyph would sit dark-on-yellow, so the armed button draws its contents in InkOnAccent").
  - Tests: `InspectorWidgetTest.cpp` `Style->TextMuted`->`InkMuted`, `Style->Text`->`Ink`; `ToastStackWidgetTest.cpp:135` `CornerRadius`->`WindowRadius`; `UIStyleTest.cpp:24-25` `Panel`->`Well`, `Text`->`Ink`, and `ApplyText` test lines using `Style->Text/TextMuted`.

- [ ] **Step 6: Update `Tools/Python/build_ui_style.py`.** Replace the `COLOURS` dict:

```python
# Spec 2026-09-28 (UI library) section 4. SEMANTIC SLOTS: each named for what it MEANS.
COLOURS = {
    "surface":       "FFFFFF",
    "well":          "F0F2F4",
    "control":       "E2E6EA",
    "ink":           "3E4A54",
    "ink_muted":     "7D8B96",
    "ink_on_accent": "3E4A54",
    "rule":          "E2E6EA",
    "accent":        "F4BA38",
    "warning":       "C45D45",
    "positive":      "7E9C6B",
}
```

Make a missing manifest a warning that skips the icon block instead of returning: replace the `if not os.path.isfile(manifest_path):` block with

```python
    manifest = None
    if os.path.isfile(manifest_path):
        with io.open(manifest_path, encoding="utf-8") as handle:
            manifest = json.load(handle)
    else:
        # A WORKTREE HAS NO Saved/UIIcons. Refusing here used to leave the colours unsaved too;
        # the icon map already on the asset survives a load-and-save, so skip only this part.
        unreal.log_warning("MARKER: no %s - icon map left as it is on the asset" % manifest_path)
```

then wrap the icon-map and notification-icon blocks (from `icons = {}` through the notification loop) in `if manifest is not None:`, and the two icon read-back checks likewise. Change the `warning` read-back to also check `ink`:

```python
    got_ink = reloaded.get_editor_property("ink")
    want_ink = srgb(COLOURS["ink"])
    if abs(got_ink.r - want_ink.r) > 1e-4:
        fail("ink read back as %r" % got_ink)
    else:
        say("PASS ink survived the save")
```

`shadow` is left at its C++ default (translucent black is not an sRGB hex).

- [ ] **Step 7: Build.** Expected `Result: Succeeded`. Then run `build_ui_style.py` headlessly. Expected MARKER lines: `PASS accent survived the save`, `PASS ink survived the save`, `ALL VERIFIED` (the icon lines may be the manifest warning instead).

- [ ] **Step 8: Run `-Filter AirportMgr.UI`.** Expected: all pass including `EveryInkReadsOnItsSurface`. Then control-check the new test measures something: temporarily set `InkMuted` default to `F0F2F4`, rebuild, confirm it FAILS, revert.

- [ ] **Step 9: Commit**

```bash
git add Source/AirportMgr Tools/Python/build_ui_style.py Content/UI/DA_UIStyle.uasset
git commit -m "feat(ui): meaning-named style slots on a white ground"
```

---

### Task 2: M_UI_Rounded and the button fill brush

**Files:**
- Create: `Tools/Python/build_ui_material.py`
- Create: `Content/UI/M_UI_Rounded.uasset` (by the script)
- Modify: `Source/AirportMgr/UIStyle.h`, `UIStyle.cpp`
- Modify: `Tools/Python/build_ui_style.py` (point the style at the material)
- Test: `Source/AirportMgr/UIStyleTest.cpp`

**Interfaces:**
- Produces: `TSoftObjectPtr<UMaterialInterface> UUIStyle::ButtonMaterial`; `FSlateBrush UUIStyle::ControlFill() const` - a brush drawing `ButtonMaterial` through one cached dynamic instance whose `RadiusPx = ControlRadius`, or `FSlateRoundedBoxBrush(White, ControlRadius)` when no material is set. Always WHITE-tinted so the caller's `SetBackgroundColor` picks the colour.

- [ ] **Step 1: Write `Tools/Python/build_ui_material.py`** (the spike's, corrected):

```python
"""Authors M_UI_Rounded, the one UI material every rounded, gradient-shaded control draws with.
Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

WHY A MATERIAL AND NOT FSlateRoundedBoxBrush: the rounded box shader draws one flat colour.
The gradient needs a material, and once a material draws the fill it must draw the corners too,
because Slate does not clip a material to a rounded shape.

WHY IT NEEDS NO SIZE PARAMETER: Slate hands every material two things per vertex
(Engine/Shaders/Private/SlateVertexShader.usf, UVArrays): uv channel 3 is the element's size
in SCREEN PIXELS, uv channel 4 is the vertex's position across the element, 0..1. Their
product is the pixel position, so one material draws exact-radius corners at any size. The
9-slice alternative (DrawAs Box, margin 0.5) keeps corners but squeezes the gradient into the
corner bands.

TINT IS APPLIED BY SLATE, NOT HERE: SlateElementPixelShader.usf GetColor does
`FinalColor = BaseColor * InVertex.Color` for every material element, so this outputs only a
grey SHADE and a MASK. Multiplying by VertexColor here too squared the tint - F4BA38 drew as
orange and every grey a step darker (spike PIE 2026-09-28: blue 0.0395 squared encodes to 5,
exactly what the capture read).
"""
import unreal

MAT_DIR = "/Game/UI"
MAT_NAME = "M_UI_Rounded"
EXPECTED_EXPRESSIONS = 7

lib = unreal.MaterialEditingLibrary


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def connect(src, src_pin, dst, dst_pin):
    # connect_material_expressions returns False on a pin name that matches nothing, and the
    # material still compiles - wrong. Honoured at every call.
    if not lib.connect_material_expressions(src, src_pin, dst, dst_pin):
        fail("could not connect %s.%s -> %s.%s" % (src.get_name(), src_pin, dst.get_name(), dst_pin))


def custom(material, x, y, name, code, inputs, output_type):
    node = lib.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", name)
    node.set_editor_property("code", code)
    node.set_editor_property("output_type", output_type)
    # unreal.CustomInput(input_name=...) raises "call() takes at most 0 arguments".
    pins = []
    for n in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", n)
        pins.append(pin)
    node.set_editor_property("inputs", pins)
    return node


def scalar(material, x, y, name, value):
    node = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", value)
    return node


def run():
    path = "%s/%s" % (MAT_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        say("replaced existing %s" % path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(MAT_NAME, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        fail("create_asset returned None for %s" % path)
        say("DONE")
        return

    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_UI)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    uv_norm = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, -100)
    uv_norm.set_editor_property("coordinate_index", 4)
    uv_px = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 20)
    uv_px.set_editor_property("coordinate_index", 3)
    radius = scalar(material, -800, 300, "RadiusPx", 5.0)
    top = scalar(material, -800, 400, "TopShade", 1.0)
    bottom = scalar(material, -800, 500, "BottomShade", 0.92)

    shade = custom(material, -400, -100, "Shade",
                   "return lerp(TopShade, BottomShade, saturate(UVn.y)).xxx;",
                   ["UVn", "TopShade", "BottomShade"],
                   unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    connect(uv_norm, "", shade, "UVn")
    connect(top, "", shade, "TopShade")
    connect(bottom, "", shade, "BottomShade")

    # Rounded-rectangle signed distance in pixels; 0.5 - d is a one-pixel antialiased edge.
    # Radius clamped to half the short side, so a small chip becomes a pill, not garbage.
    mask = custom(material, -400, 200, "RoundedMask",
                  "float2 h = Px * 0.5;\n"
                  "float r = min(Radius, min(h.x, h.y));\n"
                  "float2 q = abs(UVn * Px - h) - (h - r);\n"
                  "float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
                  "return saturate(0.5 - d);",
                  ["UVn", "Px", "Radius"],
                  unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    connect(uv_norm, "", mask, "UVn")
    connect(uv_px, "", mask, "Px")
    connect(radius, "", mask, "Radius")

    if not lib.connect_material_property(shade, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        fail("final colour not connected")
    if not lib.connect_material_property(mask, "", unreal.MaterialProperty.MP_OPACITY):
        fail("opacity not connected")

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    n = lib.get_num_material_expressions(reloaded)
    if n != EXPECTED_EXPRESSIONS:
        fail("expected %d expressions after reload, found %d" % (EXPECTED_EXPRESSIONS, n))
    else:
        say("PASS %s saved with %d expressions" % (path, n))
    if reloaded.get_editor_property("material_domain") != unreal.MaterialDomain.MD_UI:
        fail("domain is not UI after reload")
    else:
        say("PASS domain is UI")
    say("DONE")


run()
```

- [ ] **Step 2: Run it headlessly.** Expected: `PASS /Game/UI/M_UI_Rounded saved with 7 expressions`, `PASS domain is UI`, `DONE`. (A shader compile error only surfaces as `LogShaderCompilers: Warning: Failed to compile Material /Game/UI/M_UI_Rounded` when first rendered - Task 9 greps for it.)

- [ ] **Step 3: Write the failing test** - append to `UIStyleTest.cpp`:

```cpp
/**
 * THE FILL IS WHITE, ALWAYS - the caller's SetBackgroundColor is the one place a state colour is
 * chosen (the rule UOfferInboxWidget::MakeAnswerButton's comment states). A fill that carried
 * its own colour would multiply with that choice and every button would draw the wrong shade.
 * And a style with no material (the CDO: a fresh checkout) must still round its corners.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleControlFillTest,
	"AirportMgr.UI.ControlFillIsWhiteAndAlwaysRounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleControlFillTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Cdo = GetDefault<UUIStyle>();
	const FSlateBrush Fallback = Cdo->ControlFill();
	TestEqual(TEXT("no material: a rounded box"), Fallback.DrawAs, ESlateBrushDrawType::RoundedBox);
	TestTrue(TEXT("no material: corner is ControlRadius"),
		FMath::IsNearlyEqual(static_cast<float>(Fallback.OutlineSettings.CornerRadii.X), Cdo->ControlRadius));
	TestEqual(TEXT("no material: tint is white"), Fallback.TintColor.GetSpecifiedColor(), FLinearColor::White);

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (Style == Cdo || Style->ButtonMaterial.IsNull())
	{
		AddInfo(TEXT("no style asset with a ButtonMaterial configured - material half skipped"));
		return true;
	}
	const FSlateBrush Fill = Style->ControlFill();
	TestEqual(TEXT("material: drawn as an image"), Fill.DrawAs, ESlateBrushDrawType::Image);
	TestNotNull(TEXT("material: has a resource"), Fill.GetResourceObject());
	TestEqual(TEXT("material: tint is white"), Fill.TintColor.GetSpecifiedColor(), FLinearColor::White);
	TestTrue(TEXT("the SAME instance twice - one cached MID, not one per button"),
		Style->ControlFill().GetResourceObject() == Fill.GetResourceObject());
	return true;
}
```

- [ ] **Step 4: Build; expect compile failure** (`ControlFill`, `ButtonMaterial` undeclared).

- [ ] **Step 5: Implement.** In `UIStyle.h` add `class UMaterialInterface; class UMaterialInstanceDynamic;` to the forward declarations, `#include "UObject/StrongObjectPtr.h"` and `#include "Styling/SlateBrush.h"` to the includes, then after `ControlRadius`:

```cpp
	/**
	 * The UI material every Primary/Secondary/Danger control fill draws with: rounded corners and
	 * a vertical shade in one, tinted by the button's background colour - see
	 * Tools/Python/build_ui_material.py for why it needs no per-widget size. Unset (the CDO, a
	 * fresh checkout) falls back to a flat FSlateRoundedBoxBrush, so nothing ever draws square.
	 */
	UPROPERTY(EditAnywhere, Category = "Metrics") TSoftObjectPtr<UMaterialInterface> ButtonMaterial;

	/** A control's fill brush, WHITE - see ButtonMaterial. Cheap after the first call. */
	FSlateBrush ControlFill() const;
```

and in the `private:` section (add one after `ApplyText` if the class has none):

```cpp
private:
	/**
	 * ONE dynamic instance for the whole UI, carrying RadiusPx = ControlRadius. Held by a strong
	 * pointer, not a UPROPERTY: ControlFill is const (callers hold a const style), and the
	 * instance is a cache of values this asset already owns, not state worth serialising.
	 */
	mutable TStrongObjectPtr<UMaterialInstanceDynamic> ControlFillInstance;
```

In `UIStyle.cpp` add includes `Brushes/SlateRoundedBoxBrush.h`, `Materials/MaterialInstanceDynamic.h`, `Materials/MaterialInterface.h`, `UObject/Package.h`, and:

```cpp
FSlateBrush UUIStyle::ControlFill() const
{
	if (!ControlFillInstance.IsValid())
	{
		if (UMaterialInterface* Material = ButtonMaterial.LoadSynchronous())
		{
			// The transient package, not this asset: a MID outered to a saved asset would be
			// dragged into its package on the next save.
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
			Instance->SetScalarParameterValue(TEXT("RadiusPx"), ControlRadius);
			ControlFillInstance.Reset(Instance);
		}
	}
	if (ControlFillInstance.IsValid())
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(ControlFillInstance.Get());
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.ImageSize = FVector2D(32.0, 32.0);
		Brush.TintColor = FSlateColor(FLinearColor::White);
		return Brush;
	}
	return FSlateRoundedBoxBrush(FLinearColor::White, ControlRadius);
}
```

- [ ] **Step 6: Point the style at the material** in `build_ui_style.py`, after the colours loop:

```python
    material = unreal.EditorAssetLibrary.load_asset("/Game/UI/M_UI_Rounded")
    if material is None:
        fail("no /Game/UI/M_UI_Rounded - run build_ui_material.py first")
    else:
        style.set_editor_property("button_material", material)
```

and a read-back: `if reloaded.get_editor_property("button_material") is None: fail("button_material unset after save")` else `say("PASS button_material survived the save")`.

- [ ] **Step 7: Build, run build_ui_style.py, run `-Filter AirportMgr.UI`.** Expected: `PASS button_material survived the save`; `ControlFillIsWhiteAndAlwaysRounded` passes WITHOUT the "skipped" info line.

- [ ] **Step 8: Commit**

```bash
git add Tools/Python/build_ui_material.py Tools/Python/build_ui_style.py Content/UI Source/AirportMgr/UIStyle.* Source/AirportMgr/UIStyleTest.cpp
git commit -m "feat(ui): M_UI_Rounded and the one control fill brush"
```

---

### Task 3: Inter

**Files:**
- Create: `Tools/Python/build_ui_font.py`
- Create: `Content/UI/Fonts/FF_Inter_Regular.uasset`, `FF_Inter_SemiBold.uasset`, `Content/UI/Fonts/OFL.txt` (by the script)
- Modify: `Source/AirportMgr/UIStyle.h`, `UIStyle.cpp`, `UIStyleTest.cpp`, `Tools/Python/build_ui_style.py`

**Interfaces:**
- Produces: `TSoftObjectPtr<UFontFace> UUIStyle::FontRegular, FontSemiBold`; `ApplyText` unchanged in signature - Heading/Title/Clock draw SemiBold, Label/Body Regular. `TitleFont`/`LabelFont` REMOVED (nothing sets them: `build_ui_style.py` never did, and a grep finds no other writer).

- [ ] **Step 1: Write `Tools/Python/build_ui_font.py`:**

```python
"""Downloads Inter and imports its two faces as UFontFace assets. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

FACES, NOT A UFont. A composite UFont cannot be authored from Python on 5.8: FFontData's
FontFaceAsset is a private UPROPERTY. UUIStyle builds the composite in C++ from these two faces
instead (FFontData's UObject constructor is public). An AUTOMATED import creates only the
UFontFace and skips the "create a Font asset too?" dialog (EditorFactories.cpp, UFontFileImportFactory).

EVERY DOWNLOAD IS VALIDATED, as fetch_ui_icons.py's are: a TTF starts 00 01 00 00, and anything
else is an HTML error page that would import as nothing.

OFL: Inter is SIL Open Font License 1.1; the licence text ships beside the faces.
"""
import io
import os
import urllib.request
import zipfile

import unreal

URL = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
OUT_DIR = "/Game/UI/Fonts"
FACES = {
    "extras/ttf/Inter-Regular.ttf": "FF_Inter_Regular",
    "extras/ttf/Inter-SemiBold.ttf": "FF_Inter_SemiBold",
}
TTF_SIGNATURE = b"\x00\x01\x00\x00"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    cache = os.path.join(unreal.Paths.project_saved_dir(), "UIFonts")
    os.makedirs(cache, exist_ok=True)
    archive = os.path.join(cache, "Inter-4.1.zip")
    if not os.path.isfile(archive):
        urllib.request.urlretrieve(URL, archive)
    with zipfile.ZipFile(archive) as z:
        licence = z.read("LICENSE.txt")
        extracted = {}
        for member, asset_name in FACES.items():
            data = z.read(member)
            if data[:4] != TTF_SIGNATURE:
                fail("%s does not start with the TTF signature" % member)
                say("DONE")
                return
            local = os.path.join(cache, asset_name + ".ttf")
            with open(local, "wb") as f:
                f.write(data)
            extracted[asset_name] = local

    content_fonts = os.path.join(unreal.Paths.project_content_dir(), "UI", "Fonts")
    os.makedirs(content_fonts, exist_ok=True)
    with open(os.path.join(content_fonts, "OFL.txt"), "wb") as f:
        f.write(licence)

    tasks = []
    for asset_name, local in extracted.items():
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", local)
        task.set_editor_property("destination_path", OUT_DIR)
        task.set_editor_property("destination_name", asset_name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.FontFileImportFactory())
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    for asset_name in extracted:
        path = "%s/%s" % (OUT_DIR, asset_name)
        face = unreal.EditorAssetLibrary.load_asset(path)
        if face is None or not isinstance(face, unreal.FontFace):
            fail("%s did not import as a FontFace (got %r)" % (path, face))
            continue
        # INLINE: the face data is cooked into the asset, so a packaged build needs no loose .ttf.
        face.set_editor_property("loading_policy", unreal.FontLoadingPolicy.INLINE)
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        say("PASS %s is a FontFace" % path)
    say("DONE")


run()
```

- [ ] **Step 2: Run it headlessly.** Expected: two `PASS ... is a FontFace`, `DONE`. Check on disk: `Content/UI/Fonts/FF_Inter_Regular.uasset` exists and is > 100 KB (Inline data), `OFL.txt` exists.

- [ ] **Step 3: Write the failing test** - REPLACE the `LetterSpacing` assertion in the existing ApplyText test (`UIStyleTest.cpp`, "Label leaves the base font's own letter-spacing untouched") with `TestEqual(TEXT("Label is not tracked out - only Heading is"), Label->GetFont().LetterSpacing, 0);`, and append:

```cpp
/**
 * INTER, OR THE ENGINE FONT - NEVER NOTHING. The composite is built from two faces the style
 * asset points at; a style without them (the CDO) must still draw text in the widget's own
 * font rather than an empty FSlateFontInfo, which renders nothing at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUIStyleInterTest,
	"AirportMgr.UI.TextDrawsInInterWithWeightByRole",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUIStyleInterTest::RunTest(const FString& Parameters)
{
	UTextBlock* Plain = NewObject<UTextBlock>();
	GetDefault<UUIStyle>()->ApplyText(*Plain, EUITextRole::Body, FLinearColor::Black);
	TestTrue(TEXT("no faces: still a valid font"), Plain->GetFont().HasValidFont());

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	if (Style->FontRegular.IsNull() || Style->FontSemiBold.IsNull())
	{
		AddInfo(TEXT("no style asset with Inter faces configured - Inter half skipped"));
		return true;
	}
	UTextBlock* Title = NewObject<UTextBlock>();
	Style->ApplyText(*Title, EUITextRole::Title, Style->Ink);
	UTextBlock* Body = NewObject<UTextBlock>();
	Style->ApplyText(*Body, EUITextRole::Body, Style->Ink);
	TestTrue(TEXT("Title uses the composite"), Title->GetFont().CompositeFont.IsValid());
	TestEqual(TEXT("Title is SemiBold"), Title->GetFont().TypefaceFontName, FName(TEXT("SemiBold")));
	TestEqual(TEXT("Body is Regular"), Body->GetFont().TypefaceFontName, FName(TEXT("Regular")));
	TestTrue(TEXT("one composite for the whole UI"), Title->GetFont().CompositeFont == Body->GetFont().CompositeFont);
	return true;
}
```

- [ ] **Step 4: Build; expect compile failure** (`FontRegular` undeclared).

- [ ] **Step 5: Implement.** In `UIStyle.h`: forward-declare `class UFontFace; struct FCompositeFont;`; DELETE the `TitleFont` and `LabelFont` UPROPERTYs; add in their place:

```cpp
	/**
	 * Inter, as two faces (Tools/Python/build_ui_font.py). Heading, Title and Clock draw SemiBold;
	 * Label and Body draw Regular. FACES, NOT A UFont: a composite UFont cannot be authored from
	 * Python on 5.8 (FFontData::FontFaceAsset is private), so ApplyText builds the composite from
	 * these once. Unset (the CDO) draws in the widget's own engine font.
	 */
	UPROPERTY(EditAnywhere, Category = "Type") TSoftObjectPtr<UFontFace> FontRegular;
	UPROPERTY(EditAnywhere, Category = "Type") TSoftObjectPtr<UFontFace> FontSemiBold;
```

and in `private:`

```cpp
	/** Built once from FontRegular/FontSemiBold, shared by every text block. See Composite(). */
	mutable TSharedPtr<const FCompositeFont> CompositeFontCache;
	/** Keeps the two faces loaded: FFontData holds them by raw pointer, invisible to GC. */
	mutable TStrongObjectPtr<UFontFace> RegularFaceRef;
	mutable TStrongObjectPtr<UFontFace> SemiBoldFaceRef;
	/** The composite, or null when either face is unset or fails to load. */
	TSharedPtr<const FCompositeFont> Composite() const;
```

In `UIStyle.cpp` include `Engine/FontFace.h` and `Fonts/CompositeFont.h`, and replace the font-choice lines at the top of `ApplyText` (from `// Title and Clock read off TitleFont` through `FSlateFontInfo Font = ...;`) with:

```cpp
	// Heading, Title and Clock SemiBold; Label and Body Regular - the weight split the old
	// TitleFont/LabelFont pair made, now read off one composite. No faces (the CDO) keeps the
	// widget's own engine font, same as every site this replaced did.
	const bool bHeavy = (Role == EUITextRole::Heading || Role == EUITextRole::Title || Role == EUITextRole::Clock);
	FSlateFontInfo Font = TextBlock.GetFont();
	if (const TSharedPtr<const FCompositeFont> Inter = Composite())
	{
		Font = FSlateFontInfo(Inter, Font.Size, bHeavy ? FName(TEXT("SemiBold")) : FName(TEXT("Regular")));
	}
	Font.LetterSpacing = 0;
```

(The switch that follows still sets `Size` and Heading's `LetterSpacing = 120`.) Then add:

```cpp
TSharedPtr<const FCompositeFont> UUIStyle::Composite() const
{
	if (CompositeFontCache.IsValid())
	{
		return CompositeFontCache;
	}
	UFontFace* Regular = FontRegular.LoadSynchronous();
	UFontFace* SemiBold = FontSemiBold.LoadSynchronous();
	if (Regular == nullptr || SemiBold == nullptr)
	{
		return nullptr;
	}
	RegularFaceRef.Reset(Regular);
	SemiBoldFaceRef.Reset(SemiBold);
	TSharedRef<FCompositeFont> Built = MakeShared<FCompositeFont>();
	auto Add = [&Built](const TCHAR* Name, UFontFace* Face)
	{
		FTypefaceEntry& Entry = Built->DefaultTypeface.Fonts.AddDefaulted_GetRef();
		Entry.Name = FName(Name);
		Entry.Font = FFontData(Face);
	};
	// Regular FIRST: a typeface name that matches nothing falls back to the first entry.
	Add(TEXT("Regular"), Regular);
	Add(TEXT("SemiBold"), SemiBold);
	CompositeFontCache = Built;
	UE_LOG(LogUIStyle, Log, TEXT("UI font: Inter composite built from %s and %s"), *Regular->GetName(), *SemiBold->GetName());
	return CompositeFontCache;
}
```

- [ ] **Step 6: Point the style at the faces** in `build_ui_style.py` after the material block:

```python
    for prop, face_path in (("font_regular", "/Game/UI/Fonts/FF_Inter_Regular"),
                            ("font_semi_bold", "/Game/UI/Fonts/FF_Inter_SemiBold")):
        face = unreal.EditorAssetLibrary.load_asset(face_path)
        if face is None:
            fail("no %s - run build_ui_font.py first" % face_path)
        else:
            style.set_editor_property(prop, face)
```

with read-backs `PASS font_regular survived the save` / `font_semi_bold`.

- [ ] **Step 7: Build, run build_ui_style.py, run `-Filter AirportMgr.UI`.** Expected: `TextDrawsInInterWithWeightByRole` passes without the skip info; log shows `UI font: Inter composite built from FF_Inter_Regular and FF_Inter_SemiBold`.

- [ ] **Step 8: Commit**

```bash
git add Tools/Python/build_ui_font.py Tools/Python/build_ui_style.py Content/UI Source/AirportMgr/UIStyle.* Source/AirportMgr/UIStyleTest.cpp
git commit -m "feat(ui): Inter, built as a composite from two imported faces"
```

---

### Task 4: UUiButton

**Files:**
- Create: `Source/AirportMgr/UI/UiButton.h`, `UI/UiButton.cpp`, `UI/UiButtonTest.cpp`

**Interfaces:**
- Consumes: `UUIStyle::ControlFill()`, slots from Task 1, `ApplyText` from Task 3.
- Produces:

```cpp
UENUM() enum class EUiButtonKind : uint8 { Primary, Secondary, Danger, Ghost };
UENUM() enum class EUiButtonLayout : uint8 { Inline, Stacked };
struct FUiButtonLook { FLinearColor Fill; FLinearColor Ink; };
class UUiButton : public UButton {
  void Build(const UUIStyle& Style, EUiButtonKind Kind, EUiButtonLayout Layout = EUiButtonLayout::Inline, bool bStylePadding = true);
  void SetLabel(const FText& Text);          // before Build's content exists: records; after: SetText only
  void SetDetail(const FText& Text);         // a second, muted line/column
  void SetIcon(UTexture2D* Icon, float Size);
  void SetLabelMinWidth(float Width);
  void SetState(bool bEnabled, bool bSelected); // idempotent; the ONE colour rule
  UTextBlock* GetLabel() const; UImage* GetIcon() const; EUiButtonKind GetKind() const;
  static FUiButtonLook LookFor(const UUIStyle&, EUiButtonKind, bool bEnabled, bool bSelected);
  int32 PaintCountForTest() const;
};
```

Content rule: label only -> the content IS the `UTextBlock` (so `Cast<UTextBlock>(GetContent())` in the inspector keeps working); icon or detail -> HorizontalBox (Inline) or VerticalBox (Stacked). Content is built by `SetLabel/SetDetail/SetIcon` calls made BEFORE the first `SetState`; after the first paint, `SetLabel` only calls `SetText`.

- [ ] **Step 1: Write the failing tests** `Source/AirportMgr/UI/UiButtonTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Misc/AutomationTest.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ONE COLOUR RULE, AS A TABLE. Four widgets carried a copy of it (the bar, the variant bar,
 * the inspector, the inbox) and one copy had already drifted: a disabled Depart painted LIGHTER
 * than an enabled one. Accent means armed/affirmative and nothing else; a DISABLED Primary must
 * not keep it, or an offer that cannot be accepted shouts "accept me".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiButtonLookTest, "AirportMgr.UI.Button.LookTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiButtonLookTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	struct FRow { EUiButtonKind Kind; bool bEnabled; bool bSelected; FLinearColor Fill; FLinearColor Ink; const TCHAR* Why; };
	const FRow Rows[] = {
		{ EUiButtonKind::Secondary, true,  false, S.Control, S.Ink,         TEXT("an idle tool") },
		{ EUiButtonKind::Secondary, true,  true,  S.Accent,  S.InkOnAccent, TEXT("the armed tool") },
		{ EUiButtonKind::Secondary, false, false, S.Control, S.InkMuted,    TEXT("disabled dims the INK, never lightens the fill") },
		{ EUiButtonKind::Secondary, false, true,  S.Control, S.InkMuted,    TEXT("a disabled tool cannot read as armed") },
		{ EUiButtonKind::Primary,   true,  false, S.Accent,  S.InkOnAccent, TEXT("Accept") },
		{ EUiButtonKind::Primary,   false, false, S.Control, S.InkMuted,    TEXT("an offer that cannot be taken must not shout") },
		{ EUiButtonKind::Danger,    true,  false, S.Warning, S.InkOnAccent, TEXT("a destructive verb") },
		{ EUiButtonKind::Danger,    false, false, S.Control, S.InkMuted,    TEXT("disabled destructive verb") },
		{ EUiButtonKind::Ghost,     true,  false, FLinearColor::White, S.Ink, TEXT("ghost: fill comes from its own brushes") },
		{ EUiButtonKind::Ghost,     false, false, FLinearColor::White, S.InkMuted, TEXT("disabled ghost") },
	};
	for (const FRow& R : Rows)
	{
		const FUiButtonLook Look = UUiButton::LookFor(S, R.Kind, R.bEnabled, R.bSelected);
		TestEqual(FString::Printf(TEXT("fill: %s"), R.Why), Look.Fill, R.Fill);
		TestEqual(FString::Printf(TEXT("ink: %s"), R.Why), Look.Ink, R.Ink);
	}
	return true;
}

/**
 * THE BAR CALLS SetState ~70 TIMES A TICK. SetBackgroundColor has no early-out of its own
 * (the SetCardShown/SetText lesson, issue #187), so an unchanged state must paint nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiButtonIdempotentTest, "AirportMgr.UI.Button.UnchangedStatePaintsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiButtonIdempotentTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiButton* B = NewObject<UUiButton>();
	B->SetLabel(FText::FromString(TEXT("Road")));
	B->Build(S, EUiButtonKind::Secondary);
	B->SetState(true, false);
	const int32 After = B->PaintCountForTest();
	B->SetState(true, false);
	B->SetState(true, false);
	TestEqual(TEXT("repeating a state paints nothing"), B->PaintCountForTest(), After);
	B->SetState(true, true);
	TestEqual(TEXT("a change paints once"), B->PaintCountForTest(), After + 1);
	TestEqual(TEXT("selected shows Accent"), B->GetBackgroundColor(), S.Accent);
	TestEqual(TEXT("label follows"), B->GetLabel()->GetColorAndOpacity().GetSpecifiedColor(), S.InkOnAccent);
	return true;
}

/**
 * CONTENT SHAPE. A label-only button's content IS its text block - the inspector (and its test)
 * read a caption by casting GetContent(). And a label that changes every tick (the bar's
 * DynamicLabel) must not rebuild the content tree.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiButtonContentTest, "AirportMgr.UI.Button.ContentShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiButtonContentTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiButton* Plain = NewObject<UUiButton>();
	Plain->SetLabel(FText::FromString(TEXT("Depart")));
	Plain->Build(S, EUiButtonKind::Secondary);
	TestTrue(TEXT("label only: content is the text block"), Plain->GetContent() == Plain->GetLabel());

	UTextBlock* Before = Plain->GetLabel();
	Plain->SetLabel(FText::FromString(TEXT("Unfollow")));
	TestTrue(TEXT("relabel keeps the same text block"), Plain->GetLabel() == Before);
	TestEqual(TEXT("relabel sets the text"), Plain->GetLabel()->GetText().ToString(), FString(TEXT("Unfollow")));

	UUiButton* WithDetail = NewObject<UUiButton>();
	WithDetail->SetLabel(FText::FromString(TEXT("A320")));
	WithDetail->SetDetail(FText::FromString(TEXT("runway too short")));
	WithDetail->Build(S, EUiButtonKind::Secondary, EUiButtonLayout::Inline);
	TestNotNull(TEXT("label + detail inline: a horizontal box"), Cast<UHorizontalBox>(WithDetail->GetContent()));
	return true;
}

#endif
```

- [ ] **Step 2: Build TWICE** (new test file). Expected: compile error, `UI/UiButton.h` not found.

- [ ] **Step 3: Write `Source/AirportMgr/UI/UiButton.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "UiButton.generated.h"

class UImage;
class UTextBlock;
class UTexture2D;
class UUIStyle;

/** What a button is FOR - picks its resting fill. See UUiButton::LookFor for the whole rule. */
UENUM()
enum class EUiButtonKind : uint8
{
	Primary,    // the affirmative verb: Accept, Save. Accent.
	Secondary,  // everything else, and every bar tool. Control; Accent while Selected.
	Danger,     // a destructive verb. Warning.
	Ghost,      // no fill until hovered: a window's close, an icon-only affordance.
};

UENUM()
enum class EUiButtonLayout : uint8
{
	Inline,   // [icon] label  detail - rows, verbs
	Stacked,  // icon over label over detail - the bar's tool buttons
};

struct FUiButtonLook
{
	FLinearColor Fill;
	FLinearColor Ink;
};

/**
 * The game's one button (UI library step 1, spec 2026-09-28).
 *
 * A UButton SUBCLASS, not a UUserWidget wrapping one: it is placed wherever a UButton was, keeps
 * UButton's OnClicked and BindWidgetOptional compatibility, and costs no extra widget tree. What
 * it adds is the ONE place the enabled / selected / kind -> colour rule lives - four widgets each
 * carried a copy (the bar, the variant bar, the inspector, the inbox), and one copy drifted.
 *
 * Inner widgets are NewObject'd with this button as outer rather than through the owning user
 * widget's WidgetTree: a UWidget has no tree of its own, and SetContent's slot is what holds them.
 */
UCLASS()
class AIRPORTMGR_API UUiButton : public UButton
{
	GENERATED_BODY()

public:
	/**
	 * Styles the button and builds its content from whatever SetLabel/SetDetail/SetIcon recorded.
	 * Call ONCE, after those. bStylePadding false keeps UButton's own padding (the bar's square
	 * tool buttons are sized by their content, not by ButtonPadding).
	 */
	void Build(const UUIStyle& Style, EUiButtonKind InKind, EUiButtonLayout InLayout = EUiButtonLayout::Inline,
		bool bStylePadding = true);

	/** Before Build: records the caption. After: SetText on the existing block, nothing rebuilt. */
	void SetLabel(const FText& Text);
	/** A second, muted piece of text - beside the label (Inline) or under it (Stacked). */
	void SetDetail(const FText& Text);
	void SetIcon(UTexture2D* InIcon, float Size);
	void SetLabelMinWidth(float Width);

	/** Enabled + selected -> fill, ink, IsEnabled. A no-op when neither changed. */
	void SetState(bool bInEnabled, bool bInSelected);

	UTextBlock* GetLabel() const { return Label; }
	UImage* GetIcon() const { return Icon; }
	EUiButtonKind GetKind() const { return Kind; }

	/** The whole colour rule. Pure, so the table test reads it without a widget. */
	static FUiButtonLook LookFor(const UUIStyle& Style, EUiButtonKind Kind, bool bEnabled, bool bSelected);

	/** How many times SetState actually painted - see UnchangedStatePaintsNothing. */
	int32 PaintCountForTest() const { return PaintCount; }

private:
	void BuildContent();
	void Paint();

	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UTextBlock> Label;
	UPROPERTY() TObjectPtr<UTextBlock> Detail;
	UPROPERTY() TObjectPtr<UImage> Icon;
	UPROPERTY() TObjectPtr<UTexture2D> IconTexture;

	FText PendingLabel;
	FText PendingDetail;
	float IconSize = 0.0f;
	float LabelMinWidth = 0.0f;
	EUiButtonKind Kind = EUiButtonKind::Secondary;
	EUiButtonLayout Layout = EUiButtonLayout::Inline;
	bool bEnabled = true;
	bool bSelected = false;
	bool bPainted = false;
	int32 PaintCount = 0;
};
```

- [ ] **Step 4: Write `Source/AirportMgr/UI/UiButton.cpp`:**

```cpp
#include "UI/UiButton.h"

#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "UIStyle.h"

FUiButtonLook UUiButton::LookFor(const UUIStyle& S, EUiButtonKind Kind, bool bEnabled, bool bSelected)
{
	// GHOST TAKES ITS FILL FROM ITS OWN PER-STATE BRUSHES (transparent at rest, Well on hover),
	// so its background colour stays white and only the ink moves.
	if (Kind == EUiButtonKind::Ghost)
	{
		return { FLinearColor::White, bEnabled ? S.Ink : S.InkMuted };
	}
	// DISABLED IS CONTROL + MUTED INK, WHATEVER THE KIND. Disabled dims a button by its ink, never
	// by lightening the fill (the inspector's Depart bug), and a disabled Primary or Danger must
	// not keep the colour that says "press me".
	if (!bEnabled)
	{
		return { S.Control, S.InkMuted };
	}
	if (bSelected)
	{
		return { S.Accent, S.InkOnAccent };
	}
	switch (Kind)
	{
	case EUiButtonKind::Primary: return { S.Accent, S.InkOnAccent };
	case EUiButtonKind::Danger:  return { S.Warning, S.InkOnAccent };
	default:                     return { S.Control, S.Ink };
	}
}

void UUiButton::SetLabel(const FText& Text)
{
	PendingLabel = Text;
	if (Label != nullptr)
	{
		Label->SetText(Text);   // after Build: text only, the tree stays (ContentShape)
	}
}

void UUiButton::SetDetail(const FText& Text)
{
	PendingDetail = Text;
	if (Detail != nullptr)
	{
		Detail->SetText(Text);
	}
}

void UUiButton::SetIcon(UTexture2D* InIcon, float Size)
{
	IconTexture = InIcon;
	IconSize = Size;
}

void UUiButton::SetLabelMinWidth(float Width)
{
	LabelMinWidth = Width;
	if (Label != nullptr)
	{
		Label->SetMinDesiredWidth(Width);
	}
}

void UUiButton::Build(const UUIStyle& InStyle, EUiButtonKind InKind, EUiButtonLayout InLayout, bool bStylePadding)
{
	Style = &InStyle;
	Kind = InKind;
	Layout = InLayout;

	FButtonStyle ButtonStyle = GetStyle();
	if (Kind == EUiButtonKind::Ghost)
	{
		const FSlateRoundedBoxBrush Hot(InStyle.Well, InStyle.ControlRadius);
		ButtonStyle.SetNormal(FSlateNoResource());
		ButtonStyle.SetHovered(Hot);
		ButtonStyle.SetPressed(Hot);
		ButtonStyle.SetDisabled(FSlateNoResource());
	}
	else
	{
		// Tint STEPS on one white fill: the tint reaches the shader as an 8-bit vertex colour,
		// so nothing can exceed 1 - hover is "full", rest and press step down from it.
		const FSlateBrush Fill = InStyle.ControlFill();
		auto Step = [&Fill](float Grey)
		{
			FSlateBrush B = Fill;
			B.TintColor = FSlateColor(FLinearColor(Grey, Grey, Grey, 1.0f));
			return B;
		};
		ButtonStyle.SetNormal(Step(0.96f));
		ButtonStyle.SetHovered(Step(1.0f));
		ButtonStyle.SetPressed(Step(0.85f));
		ButtonStyle.SetDisabled(Step(0.96f));
	}
	if (bStylePadding)
	{
		ButtonStyle.SetNormalPadding(InStyle.ButtonPadding);
		ButtonStyle.SetPressedPadding(InStyle.ButtonPadding);
	}
	SetStyle(ButtonStyle);
	BuildContent();
	bPainted = false;
	Paint();
}

void UUiButton::BuildContent()
{
	if (!PendingLabel.IsEmpty())
	{
		Label = NewObject<UTextBlock>(this);
		Label->SetText(PendingLabel);
		Style->ApplyText(*Label, EUITextRole::Label, Style->Ink);
		if (LabelMinWidth > 0.0f) { Label->SetMinDesiredWidth(LabelMinWidth); }
		if (Layout == EUiButtonLayout::Stacked) { Label->SetJustification(ETextJustify::Center); }
	}
	if (!PendingDetail.IsEmpty())
	{
		Detail = NewObject<UTextBlock>(this);
		Detail->SetText(PendingDetail);
		Style->ApplyText(*Detail, EUITextRole::Label, Style->InkMuted);
	}
	if (IconTexture != nullptr)
	{
		Icon = NewObject<UImage>(this);
		Icon->SetBrushFromTexture(IconTexture, false);
		Icon->SetDesiredSizeOverride(FVector2D(IconSize));
	}

	// LABEL ONLY: THE CONTENT IS THE TEXT BLOCK, so a reader that casts GetContent() to a
	// UTextBlock (the inspector's Follow caption) keeps working without knowing about this class.
	if (Icon == nullptr && Detail == nullptr)
	{
		if (Label != nullptr) { SetContent(Label); }
		return;
	}
	if (Layout == EUiButtonLayout::Stacked)
	{
		UVerticalBox* Stack = NewObject<UVerticalBox>(this);
		if (Icon != nullptr) { Stack->AddChildToVerticalBox(Icon)->SetHorizontalAlignment(HAlign_Center); }
		if (Label != nullptr) { Stack->AddChildToVerticalBox(Label)->SetHorizontalAlignment(HAlign_Center); }
		if (Detail != nullptr) { Stack->AddChildToVerticalBox(Detail)->SetHorizontalAlignment(HAlign_Center); }
		SetContent(Stack);
		return;
	}
	UHorizontalBox* Line = NewObject<UHorizontalBox>(this);
	if (Icon != nullptr)
	{
		UHorizontalBoxSlot* IconSlot = Line->AddChildToHorizontalBox(Icon);
		IconSlot->SetVerticalAlignment(VAlign_Center);
		IconSlot->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));
	}
	if (Label != nullptr) { Line->AddChildToHorizontalBox(Label)->SetVerticalAlignment(VAlign_Center); }
	if (Detail != nullptr)
	{
		UHorizontalBoxSlot* DetailSlot = Line->AddChildToHorizontalBox(Detail);
		DetailSlot->SetVerticalAlignment(VAlign_Center);
		DetailSlot->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	}
	SetContent(Line);
}

void UUiButton::SetState(bool bInEnabled, bool bInSelected)
{
	if (bPainted && bInEnabled == bEnabled && bInSelected == bSelected)
	{
		return;   // UnchangedStatePaintsNothing: SetBackgroundColor has no early-out of its own
	}
	bEnabled = bInEnabled;
	bSelected = bInSelected;
	Paint();
}

void UUiButton::Paint()
{
	if (Style == nullptr)
	{
		return;   // SetState before Build: nothing to paint with yet; Build paints
	}
	const FUiButtonLook Look = LookFor(*Style, Kind, bEnabled, bSelected);
	SetIsEnabled(bEnabled);
	SetBackgroundColor(Look.Fill);
	if (Label != nullptr) { Label->SetColorAndOpacity(FSlateColor(Look.Ink)); }
	if (Icon != nullptr) { Icon->SetColorAndOpacity(Look.Ink); }
	// Detail stays muted unless the button is lit, where muted would vanish into the Accent.
	if (Detail != nullptr) { Detail->SetColorAndOpacity(FSlateColor(bSelected && bEnabled ? Look.Ink : Style->InkMuted)); }
	bPainted = true;
	++PaintCount;
}
```

- [ ] **Step 5: Build (twice if the test file was not compiled - grep the log for `UiButtonTest`) and run `-Filter AirportMgr.UI.Button`.** Expected: 3 tests pass. Note: `Build` paints once, so `UnchangedStatePaintsNothing`'s first `SetState(true,false)` after `Build` is itself a no-op - the test reads `After` after it, which is correct.

- [ ] **Step 6: Control-check:** change `LookFor`'s disabled branch to return `S.Accent` for Primary, rebuild, confirm `LookTable` FAILS on "an offer that cannot be taken must not shout", revert.

- [ ] **Step 7: Commit**

```bash
git add Source/AirportMgr/UI
git commit -m "feat(ui): UUiButton owns the one enabled/selected/kind colour rule"
```

---

### Task 5: UUiRow

**Files:**
- Create: `Source/AirportMgr/UI/UiRow.h`, `UI/UiRow.cpp`, `UI/UiRowTest.cpp`

**Interfaces:**
- Produces: `class UUiRow : public UBorder { void Build(const UUIStyle& Style, const FMargin& Padding); }` - a Well-filled, ControlRadius-rounded border; the caller `SetContent`s. (Leading/trailing helpers wait for step 4's Settings rows - no consumer yet.)

- [ ] **Step 1: Write the failing test** `UI/UiRowTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UI/UiRow.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A ROW IS A WELL, NOT A SLAB. The offer cards were drawn in Panel over a PanelDark ground; on a
 * white window the row must be the recessed Well, rounded like a control, or rows and window
 * merge into one surface and an offer reads as a line of text instead of a thing to answer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiRowTest, "AirportMgr.UI.Row.IsARoundedWell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiRowTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiRow* Row = NewObject<UUiRow>();
	Row->Build(S, FMargin(10.0f, 8.0f));
	const FSlateBrush& Brush = Row->Background;
	TestEqual(TEXT("rounded"), Brush.DrawAs, ESlateBrushDrawType::RoundedBox);
	TestEqual(TEXT("Well"), Brush.TintColor.GetSpecifiedColor(), S.Well);
	TestTrue(TEXT("control radius"), FMath::IsNearlyEqual(static_cast<float>(Brush.OutlineSettings.CornerRadii.X), S.ControlRadius));
	TestTrue(TEXT("padding as asked"), Row->GetPadding() == FMargin(10.0f, 8.0f));
	return true;
}

#endif
```

- [ ] **Step 2: Build twice; expect compile failure** (`UI/UiRow.h` not found). (`UBorder::Background` is a public, non-deprecated UPROPERTY in 5.8 - `Border.h:65` - so the test reads it directly.)

- [ ] **Step 3: Write `UI/UiRow.h` / `UI/UiRow.cpp`:**

```cpp
// UiRow.h
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
```

```cpp
// UiRow.cpp
#include "UI/UiRow.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "UIStyle.h"

void UUiRow::Build(const UUIStyle& Style, const FMargin& InPadding)
{
	SetBrush(FSlateRoundedBoxBrush(Style.Well, Style.ControlRadius));
	SetPadding(InPadding);
}
```

- [ ] **Step 4: Build, run `-Filter AirportMgr.UI.Row`.** Expected: PASS.

- [ ] **Step 5: Commit** `git add Source/AirportMgr/UI && git commit -m "feat(ui): UUiRow, the Well row"`

---

### Task 6: The bar on UUiButton

**Files:**
- Modify: `Source/AirportMgr/BuildBarWidget.h` (entry field types), `BuildBarWidget.cpp:340-405, 447-495`, `BuildBarVariants.cpp:80-145`
- Test: `Source/AirportMgr/BuildBarWidgetTest.cpp`

**Interfaces:**
- Consumes: `UUiButton` (Task 4).
- `UBuildBarEntry::Button` and `UBuildBarVariantEntry::Button` become `TObjectPtr<UUiButton>`; `UBuildBarEntry::Label`, `::Icon` and `UBuildBarVariantEntry::Label` are REMOVED (read through `Button->GetLabel()` / `GetIcon()`).

- [ ] **Step 1: Write the failing test** - append to `BuildBarWidgetTest.cpp` before `#endif` (same fixture as `AirportMgr.Actions.BarBuildsFromRegistry` at the top of that file):

```cpp
/**
 * THE BAR PAINTS THROUGH UUiButton, so the armed tool is Accent with InkOnAccent and every other
 * tool Control with Ink - the rule has one home now, and a bar that bypassed it (a raw UButton
 * slipped back in) would fail here rather than drift quietly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBuildBarUsesUiButtonTest, "AirportMgr.Actions.BarToolsAreUiButtons",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildBarUsesUiButtonTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }
	TestTrue(TEXT("every tool button is a UUiButton with a label"), Bar->AllButtonsAreUiButtonsForTest());
	return true;
}
```

Bar tests live under `AirportMgr.Actions.*`, so Step 7 runs `-Filter AirportMgr` (not just `.UI`).

Add to `BuildBarWidget.h` public test seams: `bool AllButtonsAreUiButtonsForTest() const;` implemented as

```cpp
bool UBuildBarWidget::AllButtonsAreUiButtonsForTest() const
{
	for (const UBuildBarEntry* Entry : Entries)
	{
		if (Entry == nullptr || Entry->Button == nullptr || Entry->Button->GetLabel() == nullptr) { return false; }
	}
	return Entries.Num() > 0;
}
```

- [ ] **Step 2: Build; expect compile failure** (`AllButtonsAreUiButtonsForTest`).

- [ ] **Step 3: Change the entry types** in `BuildBarWidget.h`: `class UUiButton;` forward declaration; `UBuildBarEntry`: `UPROPERTY() TObjectPtr<UUiButton> Button;`, delete `Label` and `Icon` (and their comments - move any WHY in them onto `Button`); same for `UBuildBarVariantEntry`.

- [ ] **Step 4: Rewrite the button construction in `BuildButtons`** (BuildBarWidget.cpp, from `Entry->Button = WidgetTree->ConstructWidget<UButton>` through `Entry->Button->SetBackgroundColor(Style->Button);`) as:

```cpp
		Entry->Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());

		// The time controls carry no texture, deliberately: slower, pause and faster are
		// geometric glyphs that render exactly as text. IconFor returns null for them and
		// AirportMgr.UI.EveryActionResolvesAnIcon exempts them BY SECTION, so a fourth time
		// control needs neither an icon nor that test edited.
		if (UTexture2D* IconTexture = Style->IconFor(Action.Id))
		{
			// The glyph is white with a transparent ground, so the button's ink IS the icon colour.
			Entry->Button->SetIcon(IconTexture, Style->ButtonSize * 0.5f);
			++WithIcon;
		}

		// THE LABEL IS NOW THE LABEL. The key used to be appended here - "Taxiway (1)" -
		// which is most of what made the bar read as a debug menu. It moves to the tooltip,
		// where it still teaches the shortcut without shouting it on every button forever.
		Entry->Button->SetLabel(Action.Label);
		// Stacked, and UButton's own padding: a tool button is sized by its icon, not ButtonPadding.
		Entry->Button->Build(*Style, EUiButtonKind::Secondary, EUiButtonLayout::Stacked, false);
		Entry->Button->OnClicked.AddDynamic(Entry, &UBuildBarEntry::HandleClicked);
```

Keep the tooltip block and the slot placement below it unchanged. Add `#include "UI/UiButton.h"`.

- [ ] **Step 5: Rewrite the per-tick paint in `RefreshStateFor`** (from `Entry->Button->SetIsEnabled(bEnabled);` through the icon block) as:

```cpp
		// ACCENT MEANS ARMED AND NOTHING ELSE - the rule now lives in UUiButton::LookFor, and
		// SetState is a no-op when nothing changed, so ~70 calls a tick cost nothing.
		Entry->Button->SetState(bEnabled, bActive);
		if (Action.DynamicLabel)
		{
			Entry->Button->SetLabel(Action.DynamicLabel(Ctx));
		}
```

Keep the `Entry->Button == nullptr` guard at the top of the loop.

- [ ] **Step 6: Variants** - in `BuildBarVariants.cpp` `RebuildVariants`, replace the `ConstructWidget<UButton>` + `Label` block through `SetBackgroundColor(Style->Button);` with:

```cpp
			Entry->Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
			// LABEL, AND THE DETAIL UNDER IT WHEN THERE IS ONE - stacked, because the detail is
			// part of what the option IS, not a caption beside it.
			Entry->Button->SetLabel(Option.Label);
			if (!Option.Detail.IsEmpty())
			{
				Entry->Button->SetDetail(Option.Detail);
			}
			Entry->Button->Build(*Style, EUiButtonKind::Secondary, EUiButtonLayout::Stacked, false);
```

and in the repaint loop replace `SetIsEnabled` + `SetBackgroundColor` + `Content` + `Label->SetColorAndOpacity` with `Entry->Button->SetState(bEnabled, bLit);`. If any existing variant test reads the label text as `"{0}\n{1}"`, update it to read `GetLabel()` for the label and assert the detail separately - the old single-block shape is gone on purpose.

- [ ] **Step 7: Build; run `-Filter AirportMgr`.** Expected: all pass, including `AirportMgr.Actions.BarToolsAreUiButtons`, `BarCachesStyleAcrossTicks`, `VariantRowFollowsTool`, and the `SetTextCallCountForTest` test (the dynamic label still goes through `SetText`).

- [ ] **Step 8: Commit** `git commit -am "feat(ui): the bar's tools and variants are UUiButtons"`

---

### Task 7: Inbox, Land, ledger, inspector, card

**Files:**
- Modify: `OfferInboxWidget.h/.cpp`, `LandAircraftPanelWidget.h/.cpp`, `LedgerPanelWidget.cpp`, `InspectorWidget.h/.cpp`
- Tests: `OfferInboxWidgetTest.cpp`, `LandAircraftPanelTest.cpp`, `InspectorWidgetTest.cpp`

**Interfaces:**
- Consumes: `UUiButton`, `UUiRow`.
- `UOfferRowEntry::AcceptButton`, `ULandRowEntry::Button`, `UInspectorWidget::DepartButton/FollowButton` become `TObjectPtr<UUiButton>`. `UInspectorWidget::DepartLabel` REMOVED (`DepartButton->GetLabel()`). `UOfferInboxWidget::MakeAnswerButton` returns `UUiButton*` and takes `EUiButtonKind` instead of Fill/Ink.

- [ ] **Step 1: Write the failing tests.** In `OfferInboxWidgetTest.cpp`, in the test that builds a real offer (the one whose comment mentions `Style->Accent / Style->Button on the Accept button`), after the rows are painted add:

```cpp
	// ACCEPT IS A PRIMARY UUiButton: the kind carries "affirmative", LookFor carries the colour.
	// An unacceptable offer dims to Control - the ReviewFocus #1 case, at the composition.
	const UUiButton* Accept = Inbox->AcceptButtonForTest(0);
	if (TestNotNull(TEXT("row 0 has an Accept UUiButton"), Accept))
	{
		TestEqual(TEXT("Accept is Primary"), Accept->GetKind(), EUiButtonKind::Primary);
	}
```

with `const UUiButton* AcceptButtonForTest(int32 Row) const;` added to `UOfferInboxWidget` (returns `Entries.IsValidIndex(Row) && Entries[Row] != nullptr ? Entries[Row]->AcceptButton.Get() : nullptr` - `Entries` is `OfferInboxWidget.h:141`, `TArray<TObjectPtr<UOfferRowEntry>>`). In `InspectorWidgetTest.cpp`, the two `DepartLabelColourForTest` assertions already pin InkMuted/Ink after Task 1; no new assertion needed - they now exercise `LookFor` through `SetState`.

- [ ] **Step 2: Build; expect compile failure** (`AcceptButtonForTest`, `GetKind`).

- [ ] **Step 3: Inbox.** Replace `BuildRow`'s card with a row:

```cpp
	// ONE ROW PER OFFER, a Well on the window's Surface, so it reads as a thing that can be
	// answered rather than as a line of text - UUiRow owns that choice now.
	UUiRow* Card = WidgetTree->ConstructWidget<UUiRow>(
		UUiRow::StaticClass(), *FString::Printf(TEXT("OfferCard%d"), Index));
	Card->Build(Style, FMargin(10.0f, 8.0f));
```

Replace the two `MakeAnswerButton` calls' colour arguments with kinds: `MakeAnswerButton(Style, TEXT("Accept"), NSLOCTEXT(...), EUiButtonKind::Primary, Index)` and `... EUiButtonKind::Secondary, Index)`. Replace `MakeAnswerButton`'s body:

```cpp
UUiButton* UOfferInboxWidget::MakeAnswerButton(const UUIStyle& Style, const TCHAR* Name,
	const FText& Label, EUiButtonKind Kind, int32 Index)
{
	UUiButton* Button = WidgetTree->ConstructWidget<UUiButton>(
		UUiButton::StaticClass(), *FString::Printf(TEXT("Offer%s%d"), Name, Index));
	// The rounded-white-brush recipe this used to type by hand now lives in UUiButton::Build,
	// ButtonPadding included (issue #192).
	Button->SetLabel(Label);
	Button->Build(Style, Kind);
	return Button;
}
```

and in the repaint: replace `SetIsEnabled(Row->IsAcceptable())` + `SetBackgroundColor(...)` with `Entry->AcceptButton->SetState(Row->IsAcceptable(), false);` keeping both comments above it (edit "takes ACCENT" comment to say the kind carries it). Update the header declaration and `AcceptButton` type; add `#include "UI/UiButton.h"`, `#include "UI/UiRow.h"`.

- [ ] **Step 4: Land rows.** Replace the `UButton` construction through `Button->AddChild(Line);` in `PaintRows` with:

```cpp
		UUiButton* Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		Button->SetLabel(Choice.Label);
		Button->SetLabelMinWidth(NameWidth);
		if (!Choice.Refusal.IsEmpty())
		{
			Button->SetDetail(FText::FromString(Choice.Refusal));
		}
		Button->Build(Style, EUiButtonKind::Secondary);
		// DISABLED, not merely tinted: a greyed row is a click the arrival would refuse, so
		// it must not be clickable at all. SetState both disables and dims it.
		Button->SetState(Choice.bAdmitted, false);
		Button->OnClicked.AddDynamic(Entry, &ULandRowEntry::HandleClick);
		Entry->Button = Button;
```

and change `RowColumn->AddChildToVerticalBox(Button)` accordingly. Drop the now-unused `USizeBox` include if nothing else uses it.

- [ ] **Step 5: Ledger rows.** In the row builder, wrap the `Line` HorizontalBox in a row: construct `UUiRow* RowBox = WidgetTree->ConstructWidget<UUiRow>(UUiRow::StaticClass()); RowBox->Build(Style, FMargin(8.0f, 4.0f)); RowBox->SetContent(Line);` and return `RowBox` instead of `Line` (check the function's return type - widen it to `UWidget*` if it is `UHorizontalBox*`, and adjust its one caller).

- [ ] **Step 6: Inspector.** Header: `TObjectPtr<UUiButton> DepartButton/FollowButton`, delete `DepartLabel`. In `EnsureSlots`'s `Button` lambda, construct `UUiButton`, `SetLabel(Action.Label)`, `Build(*Style, EUiButtonKind::Secondary)`, drop the manual `UTextBlock` and `SetBackgroundColor`; the `OutLabel` parameter goes. In `Refresh`, replace `SetIsEnabled` + `SetBackgroundColor(Style->Button)` + the `DepartLabel` recolour with `DepartButton->SetState(bDepartEnabled, false);` - keep the long "THE BAR'S OWN RULE" comment, shortened to say the rule now lives in `UUiButton::LookFor` and why (disabled dims ink, never lightens the fill). `DepartLabelColourForTest` returns `DepartButton && DepartButton->GetLabel() ? DepartButton->GetLabel()->GetColorAndOpacity().GetSpecifiedColor() : FLinearColor::Black`. The two `Cast<UTextBlock>(FollowButton->GetContent())` sites become `FollowButton->GetLabel()`.

- [ ] **Step 7: Build; run the full `-Filter AirportMgr`.** Expected: all pass. `UE_LOG(` count in the touched files unchanged (compare with `git diff --stat` + `grep -c "UE_LOG(" <file>` against `git show HEAD~6:<file>`).

- [ ] **Step 8: Commit** `git commit -am "feat(ui): inbox, Land, ledger and inspector on UUiButton and UUiRow"`

---

### Task 8: Lint rule - no hand-rolled button look outside UI/

**Files:**
- Modify: `Tools/Check-Architecture.ps1` (new rule 28 before `# --- Verdict`)

Deviation from spec, stated: the spec's rule also flagged `SetBrushColor(`. After this step the bar's borders still call `SetBrushColor(Style->Surface / Well)` - that names a MEANING slot, which is the correct shape, so the rule is narrowed to the shapes this step removed: `SetBackgroundColor(` (a state colour chosen outside `UUiButton::LookFor`) and `FButtonStyle` (a button look typed outside `UUiButton::Build`).

- [ ] **Step 1: Add the rule:**

```powershell
# --- 28. BUTTON LOOKS LIVE IN UI/ ------------------------------------------------------------
# UI library step 1 (2026-09-28): four widgets each carried the enabled/selected -> colour rule
# and two typed the rounded-white FButtonStyle recipe by hand; one copy drifted (the inspector's
# Depart painted lighter when disabled). UUiButton::LookFor and ::Build are now the one home, so
# a SetBackgroundColor( or an FButtonStyle anywhere else in the game module is that shape coming
# back (#255: enforce the shape, not the site). UI\ itself is exempt - it is where they live.
$gameSource = Join-Path $Root 'Source\AirportMgr'
$uiDir = Join-Path $gameSource 'UI'
if (-not (Test-Path $uiDir)) {
    $failures.Add("button-looks-in-ui: $uiDir is named by rule 28 but does not exist - update the rule")
}
Get-ChildItem -Path $gameSource -Recurse -Include *.cpp, *.h |
    Where-Object { -not $_.FullName.StartsWith($uiDir) -and $_.Name -notlike '*Test.cpp' } |
    ForEach-Object {
        $hits = Select-String -Path $_.FullName -Pattern 'SetBackgroundColor\(|FButtonStyle' |
            Where-Object { $_.Line -notmatch '^\s*//' -and $_.Line -notmatch '^\s*\*' }
        foreach ($h in $hits) {
            $failures.Add("button-looks-in-ui: $($_.Name):$($h.LineNumber) hand-rolls a button look - use UUiButton (SetState / Build)")
        }
    }
$ranRules.Add('button-looks-in-ui')
```

- [ ] **Step 2: Run `./Tools/Check-Architecture.ps1`.** Expected: PASS banner listing `button-looks-in-ui`. If any hit remains (e.g. a site this plan missed), move it onto `UUiButton` - do not exempt it.

- [ ] **Step 3: Control-check:** add `Button->SetBackgroundColor(FLinearColor::Red);` to `LedgerPanelWidget.cpp`, run the script, confirm it FAILS naming that line, revert.

- [ ] **Step 4: Commit** `git commit -am "chore(lint): rule 28 - button looks live in UI/"`

---

### Task 9: Author, look, verify, PR

- [ ] **Step 1: Author all content in order** (editor closed): `build_ui_material.py`, `build_ui_font.py`, `build_ui_style.py`. Expected MARKER lines from each as in Tasks 1-3.

- [ ] **Step 2: Authoritative test run:** `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-ui-widget-library\AirportMgr.uproject"`. Quote its `N test(s) run, N failed, N crashed` line in the PR.

- [ ] **Step 3: Look at it.** Launch the worktree editor, `python Tools/Mcp.py call EditorToolset.EditorAppToolset StartPIE '{"options":{"bSimulate":false,"playMode":"PlayMode_InEditorFloating","warmupSeconds":5}}'`, then `grep -c "Failed to compile Material /Game/UI" Saved/Logs/AirportMgr.log` (expect 0) and `python Tools/Mcp.py shot <scratch>/step1.png editor`. Sample pixels with PIL (Counter over a button's box, as the spike did): an idle tool within 8 of Control E2E6EA shaded (~DCE0E4), the Select tool within 8 of F4BA38. Text in Inter: the log line from Step 4 is the proof; by eye, Inter's digits are wider and its `1` has a flag and no foot serif, unlike Roboto's. Fix anything off, live, before handing over.

- [ ] **Step 4: Hand to the user** with the capture and what to check: bar, Offers card with an offer (wait for one or accept), press B (ledger) and 7 (Land), click an aircraft (inspector). Log lines that prove the path: `UI font: Inter composite built from FF_Inter_Regular and FF_Inter_SemiBold`.

- [ ] **Step 5: PR** - push `feature/ui-widget-library`, `gh pr create` to `main`, template filled: build line, test line, `UE_LOG` and comment-line deltas for the touched files, the rule-28 deviation from the spec, the save-compat note (style slot renames; no player saves exist).
