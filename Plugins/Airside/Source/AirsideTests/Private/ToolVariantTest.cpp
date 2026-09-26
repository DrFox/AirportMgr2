#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RoadEditTarget.h"

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
		const TArray<FToolVariantAxis> Axes = TvAxes(Tool, Context);
		return Axes.Num() > 0 ? Axes[0].Current : -2;
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

	// 1. ONE AXIS, ONE BUTTON PER WIDTH, labelled in metres.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		const TArray<FToolVariantAxis> Axes = TvAxes(Tool, Context);
		if (!TestEqual(TEXT("a taxiway offers one choice, its width"), Axes.Num(), 1)) { return false; }
		TestEqual(TEXT("the axis is Width"), Axes[0].Id, FName(TEXT("Width")));
		if (!TestEqual(TEXT("one option per standard width"), Axes[0].Options.Num(), 3)) { return false; }
		TestEqual(TEXT("labelled by width"), Axes[0].Options[0].Label.ToString(), FString(TEXT("10.5 m")));
		TestEqual(TEXT("whole metres drop the decimal"), Axes[0].Options[2].Label.ToString(), FString(TEXT("23 m")));
		TestNotEqual(TEXT("Ids are distinct, since the bar rebuilds on them"),
			Axes[0].Options[0].Id, Axes[0].Options[1].Id);

		// 2. THE LEVEL DEFAULT LIGHTS ITS PRESET BUT IS NOT WRITTEN - drawing keeps the level's
		//    own tuning, which is what TaxiwayWidthTest's block 1 pins against a real actor.
		TestEqual(TEXT("the level default lights the preset it matches"), Axes[0].Current, 1);
		TestEqual(TEXT("and the tool still has chosen nothing"), Tool.GetWidthIndex(), INDEX_NONE);
	}

	// 3. AN OFF-LIST DEFAULT LIGHTS NOTHING, rather than lighting a width it will not lay.
	{
		FTvWidthTarget OffList = Target;
		OffList.LevelDefault = URoadProfile::MakeTransient(1800.0, 1500.0);
		FToolContext OffContext;
		OffContext.Target = &OffList;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestEqual(TEXT("a tuned width that is no preset lights nothing"), TvLit(Tool, OffContext), INDEX_NONE);

		Tool.OnReselect(OffContext);
		TestEqual(TEXT("and the key from nothing lit picks the first"), Tool.GetWidthIndex(), 0);
	}

	// 4. A PICK IS HONOURED, AND A BAD ONE CHANGES NOTHING.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestTrue(TEXT("picking the widest is accepted"), Tool.SelectVariant(Context, 0, 2));
		TestEqual(TEXT("it is what the next click lays"), Tool.GetWidthIndex(), 2);
		TestEqual(TEXT("and what is lit"), TvLit(Tool, Context), 2);

		TestFalse(TEXT("past the end is refused"), Tool.SelectVariant(Context, 0, 3));
		TestFalse(TEXT("a negative option is refused"), Tool.SelectVariant(Context, 0, -1));
		TestFalse(TEXT("an axis the tool does not have is refused"), Tool.SelectVariant(Context, 1, 0));
		TestEqual(TEXT("none of them moved the width"), Tool.GetWidthIndex(), 2);
	}

	// 5. THE KEY STEPS FROM WHAT IS LIT - the row and the cycle are one list. Lit is 1 (the
	//    default), so the first press goes to 2, the second wraps to 0.
	{
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		Tool.OnReselect(Context);
		TestEqual(TEXT("the first press steps on from the lit default"), Tool.GetWidthIndex(), 2);
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
		TestTrue(TEXT("one width is one option"), Axes.Num() == 1 && Axes[0].Options.Num() == 1);
		Tool.OnReselect(OneContext);
		Tool.OnReselect(OneContext);
		TestEqual(TEXT("and cycling one option stays on it"), Tool.GetWidthIndex(), 0);
	}

	// 7. NO WIDTHS, NO AXIS - the row hides rather than showing an empty strip.
	{
		FTvWidthTarget Empty;
		FToolContext EmptyContext;
		EmptyContext.Target = &Empty;
		FRoadDrawTool Tool(ERoadKind::Taxiway);
		TestEqual(TEXT("an empty content set offers no choice"), TvAxes(Tool, EmptyContext).Num(), 0);
	}

	// 8. A TOOL WITH NOTHING TO CHOOSE says so - the base's silence, reached through the registry.
	{
		const TUniquePtr<IBuildTool> Select = ToolRegistry()[0].Make();
		TestEqual(TEXT("the select tool offers no choice"), TvAxes(*Select, Context).Num(), 0);
	}

	return true;
}

#endif
