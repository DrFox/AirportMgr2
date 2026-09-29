#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/PavementAxis.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RunwayTool.h"
#include "Tool/ToolPreferences.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Tv prefix: these test files share one translation unit (unity build).

	/**
	 * A target with transient taxiway widths and a settable LEVEL DEFAULT - the one input the
	 * variant row reads that the width cycle never did. Everything else is FNullEditTarget's
	 * inert default (#189), so a tool that asks for more has grown a dependency.
	 */
	struct FTvWidthTarget : FNullEditTarget
	{
		TArray<URoadProfile*> Widths;

		/** What ResolveProfileFor(Kind, INDEX_NONE) answers - the level's own tuning. */
		URoadProfile* LevelDefault = nullptr;

		virtual int32 GetWidthCount(ERoadKind Kind) const override
		{
			return Kind == ERoadKind::Taxiway ? Widths.Num() : 0;
		}
		virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const override
		{
			if (Kind != ERoadKind::Taxiway || Widths.Num() == 0)
			{
				return nullptr;
			}
			// Clamped, mirroring ARoadNetworkActor - see FFakeWidthTarget's same line.
			return Widths[FMath::Clamp(Index, 0, Widths.Num() - 1)];
		}
		virtual URoadProfile* ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) override
		{
			return WidthIndex == INDEX_NONE ? LevelDefault : ResolveWidthProfile(Kind, WidthIndex);
		}
	};

	TArray<FToolVariantAxis> TvAxes(const IBuildTool& Tool, const FToolContext& Context)
	{
		TArray<FToolVariantAxis> Out;
		Tool.GetVariantAxes(Context, Out);
		return Out;
	}

	int32 TvLit(const IBuildTool& Tool, const FToolContext& Context)
	{
		// THE WIDTH ROW BY ITS Id - row 0 is Mode since strip stage 6.
		const TArray<FToolVariantAxis> Axes = TvAxes(Tool, Context);
		const FToolVariantAxis* Width = Axes.FindByPredicate([](const FToolVariantAxis& A) { return A.Id == FName(TEXT("Width")); });
		return Width != nullptr ? Width->Current : -2;
	}
}

/**
 * THE ROAD TOOL LISTS ITS WIDTHS, LIGHTS THE ONE IT WILL LAY, AND TAKES A PICK - the data the
 * bar's variant row draws. World-free: the list comes through IRoadEditTarget and nothing else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvRoadWidthTest,
	"Airside.Tool.Variants.RoadWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvRoadWidthTest::RunTest(const FString& Parameters)
{
	FTvWidthTarget Target;
	Target.Widths = {
		URoadProfile::MakeTransient(1050.0, 1500.0),
		URoadProfile::MakeTransient(1500.0, 1500.0),
		URoadProfile::MakeTransient(2300.0, 1500.0),
	};
	Target.LevelDefault = Target.Widths[1];
	FToolContext Context;
	Context.Target = &Target;

	// 1. WIDTH FIRST, ONE BUTTON PER WIDTH, labelled in metres - then the surface row
	//    (Airside.Tool.Variants.RoadSurface covers that one).
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		const TArray<FToolVariantAxis> Axes = TvAxes(Tool, Context);
		// ROW 0 IS MODE since strip stage 6 (ModeAxis leads every road tool's rows), so Width is 1.
		if (!TestEqual(TEXT("a taxiway offers three choices, mode, width and surface"), Axes.Num(), 3)) { return false; }
		TestEqual(TEXT("the axis is Width"), Axes[1].Id, FName(TEXT("Width")));
		if (!TestEqual(TEXT("one option per standard width"), Axes[1].Options.Num(), 3)) { return false; }
		TestEqual(TEXT("labelled by width"), Axes[1].Options[0].Label.ToString(), FString(TEXT("10.5 m")));
		TestEqual(TEXT("whole metres drop the decimal"), Axes[1].Options[2].Label.ToString(), FString(TEXT("23 m")));
		TestNotEqual(TEXT("Ids are distinct, since the bar rebuilds on them"),
			Axes[1].Options[0].Id, Axes[1].Options[1].Id);

		// 2. A FRESH TOOL LIGHTS AND LAYS THE NARROWEST (2026-09-28) - not the level default,
		//    which Target sets to the middle width precisely so the two cannot be confused.
		TestEqual(TEXT("a fresh tool lights the narrowest"), Axes[1].Current, 0);
		TestEqual(TEXT("and holds it"), Tool.GetWidthIndex(), 0);
	}

	// 3. AN OFF-LIST LEVEL WIDTH STILL LIGHTS THE NARROWEST. It lit nothing before 2026-09-28 -
	//    the empty taxiway row reported on M_Test - because an unset tool lit the level's match.
	{
		FTvWidthTarget OffList = Target;
		OffList.LevelDefault = URoadProfile::MakeTransient(1800.0, 1500.0);
		FToolContext OffContext;
		OffContext.Target = &OffList;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestEqual(TEXT("a level width that is no preset does not empty the row"), TvLit(Tool, OffContext), 0);
	}

	// 4. A PICK IS HONOURED, AND A BAD ONE CHANGES NOTHING.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestTrue(TEXT("picking the widest is accepted"), Tool.SelectVariant(Context, 1, 2));
		TestEqual(TEXT("it is what the next click lays"), Tool.GetWidthIndex(), 2);
		TestEqual(TEXT("and what is lit"), TvLit(Tool, Context), 2);

		TestFalse(TEXT("past the end is refused"), Tool.SelectVariant(Context, 1, 3));
		TestFalse(TEXT("a negative option is refused"), Tool.SelectVariant(Context, 1, -1));
		TestFalse(TEXT("an axis the tool does not have is refused"), Tool.SelectVariant(Context, 3, 0));
		TestEqual(TEXT("none of them moved the width"), Tool.GetWidthIndex(), 2);
	}

	// 5. THE KEY STEPS FROM WHAT IS LIT - the row and the cycle are one list. Lit is 0 (the
	//    narrowest), so presses go 1, 2, then wrap to 0.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		Tool.OnReselect(Context);
		TestEqual(TEXT("the first press steps on from the lit narrowest"), Tool.GetWidthIndex(), 1);
		Tool.OnReselect(Context);
		Tool.OnReselect(Context);
		TestEqual(TEXT("then wraps"), Tool.GetWidthIndex(), 0);
	}

	// 6. ONE WIDTH is still a row of one, and the key stays on it.
	{
		FTvWidthTarget One;
		One.Widths = { Target.Widths[0] };
		FToolContext OneContext;
		OneContext.Target = &One;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		const TArray<FToolVariantAxis> Axes = TvAxes(Tool, OneContext);
		TestTrue(TEXT("one width is one option"), Axes.Num() == 3 && Axes[1].Options.Num() == 1);
		Tool.OnReselect(OneContext);
		Tool.OnReselect(OneContext);
		TestEqual(TEXT("and cycling one option stays on it"), Tool.GetWidthIndex(), 0);
	}

	// 7. NO WIDTHS, NO WIDTH ROW - it hides rather than showing an empty strip. The surface row
	//    stays: it is the tool's own enum and needs no content.
	{
		FTvWidthTarget Empty;
		FToolContext EmptyContext;
		EmptyContext.Target = &Empty;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		const TArray<FToolVariantAxis> EmptyAxes = TvAxes(Tool, EmptyContext);
		TestTrue(TEXT("an empty content set offers only the mode and the surface"),
			EmptyAxes.Num() == 2 && EmptyAxes[1].Id == FName(TEXT("Surface")));
	}

	// 8. A TOOL WITH NOTHING TO CHOOSE says so - the base's silence, reached through the registry.
	{
		const TUniquePtr<IBuildTool> Select = ToolRegistry()[0].Make();
		TestEqual(TEXT("the select tool offers no choice"), TvAxes(*Select, Context).Num(), 0);
	}

	return true;
}

namespace
{
	/** Transient runway widths and nothing else - FFakeRunwayTarget's shape (RunwayToolTest). */
	struct FTvRunwayTarget : FNullEditTarget
	{
		TArray<URoadProfile*> Profiles;
		virtual int32 GetRunwayProfileCount() const override { return Profiles.Num(); }
		virtual URoadProfile* ResolveRunwayProfile(int32 Index) const override
		{
			return Profiles.Num() == 0 ? nullptr : Profiles[FMath::Clamp(Index, 0, Profiles.Num() - 1)];
		}
	};
}

/**
 * THE RUNWAY OFFERS THREE CHOICES, ONE ROW EACH - width, surface, approach - and each row moves
 * only its own field, on a click or on its modifier of the tool's key.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvRunwayTest,
	"Airside.Tool.Variants.Runway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvRunwayTest::RunTest(const FString& Parameters)
{
	FTvRunwayTarget Target;
	Target.Profiles = {
		URoadProfile::MakeTransient(2300.0, 1500.0),
		URoadProfile::MakeTransient(3000.0, 1500.0),
		URoadProfile::MakeTransient(4500.0, 1500.0),
	};
	FToolContext Context;
	Context.Target = &Target;

	FRunwayTool Tool;
	const TArray<FToolVariantAxis> Axes = TvAxes(Tool, Context);
	if (!TestEqual(TEXT("three rows: width, surface, approach"), Axes.Num(), 3)) { return false; }
	TestEqual(TEXT("row 0 is Width"), Axes[0].Id, FName(TEXT("Width")));
	TestEqual(TEXT("row 1 is Surface - the Shift cycle"), Axes[1].Id, FName(TEXT("Surface")));
	TestEqual(TEXT("row 2 is Approach - the Ctrl cycle"), Axes[2].Id, FName(TEXT("Approach")));
	TestEqual(TEXT("one width option per profile"), Axes[0].Options.Num(), 3);
	TestEqual(TEXT("labelled in metres, as the road row is"), Axes[0].Options[1].Label.ToString(), FString(TEXT("30 m")));
	TestEqual(TEXT("every surface"), Axes[1].Options.Num(), static_cast<int32>(EPavement::Count));
	TestEqual(TEXT("every approach"), Axes[2].Options.Num(), static_cast<int32>(ERunwayApproach::Count));
	TestEqual(TEXT("surfaces are named by Pavement::Name, the one source"),
		Axes[1].Options[static_cast<int32>(EPavement::Concrete)].Label.ToString(),
		FString(Pavement::Name(EPavement::Concrete)));
	TestEqual(TEXT("lit width is the first"), Axes[0].Current, 0);
	TestEqual(TEXT("lit surface is grass - the cheap start (2026-09-28)"), Axes[1].Current, static_cast<int32>(EPavement::Grass));
	TestEqual(TEXT("lit approach is visual"), Axes[2].Current, static_cast<int32>(ERunwayApproach::Visual));

	TestTrue(TEXT("a surface pick is accepted"),
		Tool.SelectVariant(Context, 1, static_cast<int32>(EPavement::Concrete)));
	TestEqual(TEXT("and sets the surface"), Tool.Surface, EPavement::Concrete);
	TestEqual(TEXT("and leaves the width"), Tool.WidthIndex, 0);
	TestTrue(TEXT("an approach pick is accepted"),
		Tool.SelectVariant(Context, 2, static_cast<int32>(ERunwayApproach::Precision)));
	TestEqual(TEXT("and sets the approach"), Tool.Approach, ERunwayApproach::Precision);
	TestTrue(TEXT("a width pick is accepted"), Tool.SelectVariant(Context, 0, 2));
	TestEqual(TEXT("and sets the width"), Tool.WidthIndex, 2);
	TestFalse(TEXT("a fourth row does not exist"), Tool.SelectVariant(Context, 3, 0));
	TestFalse(TEXT("a surface past the sentinel is refused"),
		Tool.SelectVariant(Context, 1, static_cast<int32>(EPavement::Count)));
	TestEqual(TEXT("and changed nothing"), Tool.Surface, EPavement::Concrete);

	// THE KEY WALKS THE SAME ROWS: plain the width, Shift (insert) the surface, Ctrl (remove)
	// the approach - each wrapping, each leaving the other two alone.
	Tool.OnReselect(Context);
	TestEqual(TEXT("plain key: width wraps from the last to the first"), Tool.WidthIndex, 0);
	FToolContext Shift = Context;
	Shift.bInsertModifier = true;
	Tool.OnReselect(Shift);
	TestEqual(TEXT("Shift: the next surface"), Tool.Surface, EPavement::Reinforced);
	FToolContext Ctrl = Context;
	Ctrl.bRemoveModifier = true;
	Tool.OnReselect(Ctrl);
	TestEqual(TEXT("Ctrl: approach wraps from precision to visual"), Tool.Approach, ERunwayApproach::Visual);
	TestEqual(TEXT("and neither modifier moved the width"), Tool.WidthIndex, 0);

	// NO RUNWAY PROFILES: no width row, but surface and approach are the tool's own enums and
	// still choosable - a runway with no width cannot be placed, and the preview says so.
	FTvRunwayTarget Empty;
	FToolContext EmptyContext;
	EmptyContext.Target = &Empty;
	FRunwayTool EmptyTool;
	const TArray<FToolVariantAxis> EmptyAxes = TvAxes(EmptyTool, EmptyContext);
	TestTrue(TEXT("no profiles: surface and approach only"),
		EmptyAxes.Num() == 2 && EmptyAxes[0].Id == FName(TEXT("Surface")));
	return true;
}

namespace
{
	/** A road tool with its middle width LOCKED - the unlock seam, before any unlock system. */
	struct FTvLockedRoadTool : FRoadDrawTool
	{
		FTvLockedRoadTool() : FRoadDrawTool(ERoadKind::Taxiway) {}
		virtual void GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const override
		{
			FRoadDrawTool::GetVariantAxes(Context, Out);
			// THE WIDTH ROW BY Id - row 0 is Mode since strip stage 6.
			for (FToolVariantAxis& Axis : Out)
			{
				if (Axis.Id == FName(TEXT("Width")) && Axis.Options.Num() > 1) { Axis.Options[1].bEnabled = false; }
			}
		}
	};

	/** A runway tool with Concrete LOCKED. */
	struct FTvLockedRunwayTool : FRunwayTool
	{
		virtual void GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const override
		{
			FRunwayTool::GetVariantAxes(Context, Out);
			for (FToolVariantAxis& Axis : Out)
			{
				if (Axis.Id == FName(TEXT("Surface")))
				{
					Axis.Options[static_cast<int32>(EPavement::Concrete)].bEnabled = false;
				}
			}
		}
	};
}

/**
 * A LOCKED OPTION IS REFUSED BY A CLICK AND STEPPED OVER BY THE KEY - FToolVariant::bEnabled's
 * promise, on both tools. Without the skip, a key landing on a lock would be refused and never
 * move again: the cycle stuck on the option before it, forever.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvLockedOptionTest,
	"Airside.Tool.Variants.LockedOption",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvLockedOptionTest::RunTest(const FString& Parameters)
{
	FTvWidthTarget Target;
	Target.Widths = {
		URoadProfile::MakeTransient(1050.0, 1500.0),
		URoadProfile::MakeTransient(1500.0, 1500.0),
		URoadProfile::MakeTransient(2300.0, 1500.0),
	};
	FToolContext Context;
	Context.Target = &Target;

	FTvLockedRoadTool Road;
	TestFalse(TEXT("a click on the locked width is refused"), Road.SelectVariant(Context, 1, 1));   // row 1: Width, after Mode
	TestEqual(TEXT("and left the narrowest chosen"), Road.GetWidthIndex(), 0);
	Road.OnReselect(Context);
	TestEqual(TEXT("the key steps OVER the locked width"), Road.GetWidthIndex(), 2);
	Road.OnReselect(Context);
	TestEqual(TEXT("and wraps to the first"), Road.GetWidthIndex(), 0);

	FTvRunwayTarget RunwayTarget;
	RunwayTarget.Profiles = { URoadProfile::MakeTransient(2300.0, 1500.0) };
	FToolContext RunwayContext;
	RunwayContext.Target = &RunwayTarget;
	FTvLockedRunwayTool Runway;
	FToolContext Shift = RunwayContext;
	Shift.bInsertModifier = true;
	// FROM TARMAC, the step this test is about - grass is a fresh tool's default since 2026-09-28.
	Runway.RestoreSurface(EPavement::Tarmac);
	Runway.OnReselect(Shift);
	TestEqual(TEXT("Shift steps from tarmac over locked concrete to reinforced"),
		Runway.Surface, EPavement::Reinforced);
	return true;
}

/**
 * THE SESSION FORWARDS TO WHATEVER IS LIT - the seam both drivers reach the row through. Fails
 * if the forwarder is unwired (no axes) or points at the wrong tool (a pick that does not land).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvSessionTest,
	"Airside.Tool.Variants.Session",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvSessionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	const int32 Count = Actor->GetWidthCount(ERoadKind::Taxiway);
	if (!TestTrue(TEXT("the content set has taxiway widths"), Count > 1)) { return false; }

	int32 Taxiway = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Id == FName(TEXT("Taxiway"))) { Taxiway = Index; }
	}
	if (!TestTrue(TEXT("the registry has a Taxiway tool"), Taxiway != INDEX_NONE)) { return false; }

	FToolContext Context;
	Context.Target = Actor;
	FBuildSession Session;
	Session.SelectTool(Taxiway, Context);

	TArray<FToolVariantAxis> Axes;
	Session.GetActiveVariantAxes(Context, Axes);
	// THREE since strip stage 6: Mode leads, then Width and Surface.
	if (!TestEqual(TEXT("the lit taxiway tool's three rows reach the session"), Axes.Num(), 3)) { return false; }
	TestEqual(TEXT("with every content width"), Axes[1].Options.Num(), Count);

	TestTrue(TEXT("a pick through the session is accepted"), Session.SelectActiveVariant(Context, 1, 1));
	const FRoadDrawTool* Tool = static_cast<const FRoadDrawTool*>(Session.GetActiveTool());
	TestEqual(TEXT("and lands on the lit tool"), Tool->GetWidthIndex(), 1);

	// EDIT MODE SUPPRESSES THE TOOL (FBuildSession::GetActiveTool), and the row with it: the edit
	// tool has nothing to choose, so a width row lit over it would pick for a tool not in use.
	Session.ToggleGestureMode(EGestureMode::Edit, Context);
	Axes.Reset();
	Session.GetActiveVariantAxes(Context, Axes);
	TestEqual(TEXT("in edit mode there is no row"), Axes.Num(), 0);
	TestFalse(TEXT("and no pick lands"), Session.SelectActiveVariant(Context, 0, 0));
	return true;
}

/**
 * ONE SURFACE-ROW BUILDER, reading the profile's list - world-free. Fails if the row offers
 * the enum rather than the list, leaves it in the asset's order rather than the scale's, or
 * lights Current by its enum value rather than by its place in what was offered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSurfaceRowOffersTheProfileListTest,
	"Airside.Tool.Variants.RoadSurfaceRowOffersTheProfileList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSurfaceRowOffersTheProfileListTest::RunTest(const FString& Parameters)
{
	// THE PROFILE'S LIST, NOT A SECOND ENUM: the reason ERoadSurface existed - a road tool
	// must not offer a concrete service road - is now data on the profile.
	TArray<FToolVariantAxis> Axes;
	const EPavement RoadList[] = { EPavement::Tarmac, EPavement::Grass };
	Pavement::AppendAxis(Axes, EPavement::Tarmac, RoadList);
	if (!TestEqual(TEXT("one row"), Axes.Num(), 1)) { return false; }
	if (!TestEqual(TEXT("with exactly the profile's two options"), Axes[0].Options.Num(), 2)) { return false; }
	// SCALE ORDER since 2026-09-28, though the list here - like every road asset's - names tarmac
	// first: grass leads every surface row, the runway's and stand's included (Pavement::Offered).
	TestEqual(TEXT("in scale order - grass first, whatever order the profile lists"), Axes[0].Options[0].Id, FName(TEXT("grass")));
	TestEqual(TEXT("and tarmac lit by its place in that order"), Axes[0].Current, 1);

	TArray<FToolVariantAxis> All;
	Pavement::AppendAxis(All, EPavement::Tarmac, {});
	if (!TestEqual(TEXT("still one row"), All.Num(), 1)) { return false; }
	TestEqual(TEXT("an empty list offers all four - runways and stands"), All[0].Options.Num(), 4);
	TestEqual(TEXT("and lights the current one by its place in the OFFERED list"), All[0].Current, 1);
	return true;
}

namespace
{
	/** The registry index of the tool registered as Id, or INDEX_NONE. */
	int32 TvToolIndex(const TCHAR* Id)
	{
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(Id)) { return Index; }
		}
		return INDEX_NONE;
	}

	/** The four tools that lay pavement, by registry Id - what the ruling named. */
	const TCHAR* const TvSurfaceTools[] = { TEXT("Taxiway"), TEXT("Road"), TEXT("Runway"), TEXT("Stand") };

	/** The surface Id's tool would lay next, read through the session as a driver would. */
	TOptional<EPavement> TvSurfaceOf(FBuildSession& Session, const TCHAR* Id)
	{
		Session.SelectTool(TvToolIndex(Id));
		const IBuildTool* Tool = Session.GetActiveTool();
		return Tool != nullptr ? Tool->GetChosenSurface() : TOptional<EPavement>();
	}
}

/**
 * EVERY TOOL THAT LAYS PAVEMENT STARTS ON GRASS (ruled 2026-09-28): the cheapest surface is a
 * new player's default and upgrading it is progression. Through a fresh session with no store,
 * which is what a first launch is - nothing remembered yet.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvSurfaceDefaultsToGrassTest,
	"Airside.Tool.SurfacePreference.DefaultsToGrass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvSurfaceDefaultsToGrassTest::RunTest(const FString& Parameters)
{
	FBuildSession Session;
	for (const TCHAR* Id : TvSurfaceTools)
	{
		if (!TestTrue(FString::Printf(TEXT("%s is in the registry"), Id), TvToolIndex(Id) != INDEX_NONE)) { continue; }
		const TOptional<EPavement> Surface = TvSurfaceOf(Session, Id);
		if (TestTrue(FString::Printf(TEXT("%s lays a surface"), Id), Surface.IsSet()))
		{
			TestEqual(FString::Printf(TEXT("%s starts on grass"), Id), *Surface, EPavement::Grass);
		}
	}
	TestFalse(TEXT("a tool that lays nothing reports no surface"), TvSurfaceOf(Session, TEXT("Select")).IsSet());
	TestFalse(TEXT("a session nobody handed a store remembers nothing"), Session.HasToolPreferences());
	return true;
}

/**
 * A SURFACE PICK OUTLIVES THE SESSION, per tool, through both doors a pick comes in by - the
 * bar's click (SelectActiveVariant) and Shift+key again (SelectTool's reselect). The second
 * session stands for the next launch: it is handed the same store and must come up on what the
 * first one picked, while tools nobody touched stay on grass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvSurfaceRemembersAcrossSessionsTest,
	"Airside.Tool.SurfacePreference.RemembersAcrossSessions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvSurfaceRemembersAcrossSessionsTest::RunTest(const FString& Parameters)
{
	FTvRunwayTarget Target;
	Target.Profiles = { URoadProfile::MakeTransient(3000.0, 1500.0), URoadProfile::MakeTransient(4600.0, 1500.0) };
	FToolContext Context;
	Context.Target = &Target;

	const TSharedPtr<FMemoryToolPreferences> Store = MakeShared<FMemoryToolPreferences>();
	{
		FBuildSession First;
		First.SetToolPreferences(Store);
		TestEqual(TEXT("restoring from an empty store writes nothing"), Store->GetWriteCount(), 0);

		// THE BAR'S DOOR: the runway's Surface row is row 1 (Airside.Tool.Variants.Runway).
		First.SelectTool(TvToolIndex(TEXT("Runway")));
		if (!TestTrue(TEXT("the runway takes a concrete pick"),
			First.SelectActiveVariant(Context, 1, static_cast<int32>(EPavement::Concrete)))) { return false; }
		TestEqual(TEXT("the pick is written"), Store->Read(TEXT("Runway.Surface")).Get(FString()), FString(TEXT("Concrete")));
		TestEqual(TEXT("once"), Store->GetWriteCount(), 1);

		TestTrue(TEXT("a width pick is taken"), First.SelectActiveVariant(Context, 0, 1));
		TestEqual(TEXT("and writes nothing - the surface did not change"), Store->GetWriteCount(), 1);

		// THE KEY'S DOOR: Shift (the insert modifier) with the runway's key again steps the surface
		// one along the scale, Concrete -> Reinforced (FRunwayTool::OnReselect).
		FToolContext Shift = Context;
		Shift.bInsertModifier = true;
		First.SelectTool(TvToolIndex(TEXT("Runway")), Shift);
		TestEqual(TEXT("Shift+key's step is written too"), Store->Read(TEXT("Runway.Surface")).Get(FString()), FString(TEXT("Reinforced")));
	}

	FBuildSession Next;
	Next.SetToolPreferences(Store);
	const TOptional<EPavement> Runway = TvSurfaceOf(Next, TEXT("Runway"));
	TestTrue(TEXT("the next launch's runway comes up on the last pick"), Runway.IsSet() && *Runway == EPavement::Reinforced);
	const TOptional<EPavement> Taxiway = TvSurfaceOf(Next, TEXT("Taxiway"));
	TestTrue(TEXT("each tool remembers its own - the untouched taxiway is still grass"), Taxiway.IsSet() && *Taxiway == EPavement::Grass);

	// A NAME THE ENUM DOES NOT HAVE (a hand-edited ini) keeps the default rather than casting it.
	Store->Write(TEXT("Stand.Surface"), TEXT("Marble"));
	FBuildSession Tampered;
	AddExpectedMessage(TEXT("is not a pavement"), EAutomationExpectedMessageFlags::Contains, 1);
	Tampered.SetToolPreferences(Store);
	const TOptional<EPavement> Stand = TvSurfaceOf(Tampered, TEXT("Stand"));
	TestTrue(TEXT("an unknown stored name leaves the stand on grass"), Stand.IsSet() && *Stand == EPavement::Grass);
	return true;
}

/**
 * THE CONFIG STORE ROUND-TRIPS through GGameUserSettingsIni: what one instance writes, a fresh
 * one reads - the file, not the object, is what holds it. A TEST-ONLY KEY, removed after, so the
 * player's real Taxiway/Road/Runway/Stand picks are neither read nor disturbed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTvConfigPreferencesRoundTripTest,
	"Airside.Tool.SurfacePreference.ConfigRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTvConfigPreferencesRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Key = TEXT("AirsideTest.RoundTrip");
	FConfigToolPreferences Writer;
	Writer.Write(Key, TEXT("Concrete"));
	const FConfigToolPreferences Reader;
	TestEqual(TEXT("a fresh store reads what another wrote"), Reader.Read(Key).Get(FString()), FString(TEXT("Concrete")));
	Writer.Remove(Key);
	TestFalse(TEXT("and the test's key is gone again"), Reader.Read(Key).IsSet());
	return true;
}

#endif
