#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/GridOverlay.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE OVERLAY FROM A REAL SESSION CONTEXT - the seam both drivers call. Asserted through
 * MakeContext rather than a hand-built context, because what gates the overlay is the step
 * MakeContext decides to put on the context, and a test that set GridStepUu itself would pass
 * with that decision deleted.
 */
namespace GridOverlayFixture
{
	struct FLineSink : IToolPreviewSink
	{
		struct FRecorded { FVector2D From; FVector2D To; EPreviewStyle Style; };
		TArray<FRecorded> Lines;

		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			Lines.Add({ From, To, Style });
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

		int32 CountOf(EPreviewStyle Style) const
		{
			return Lines.FilterByPredicate([Style](const FRecorded& L) { return L.Style == Style; }).Num();
		}
	};

	/** The overlay a session with ToolId selected would draw for a cursor at Where. */
	bool DrawFor(const TCHAR* ToolId, EGridStep Step, const FVector2D& Where, FLineSink& Out, FVector2D& OutCentre)
	{
		FAirsideTestWorld TestWorld;
		if (TestWorld.World == nullptr || TestWorld.Actor == nullptr) { return false; }

		FBuildSession Session;
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 I = 0; I < Registry.Num(); ++I)
		{
			if (Registry[I].Id == FName(ToolId)) { Session.SelectTool(I); }
		}
		if (Session.GetActiveTool() == nullptr) { return false; }

		FBuildSessionTunables Tunables = TestWorld.Actor->MakeTunables(10000.0);
		Tunables.GuideSources.GridStep = Step;
		const FToolContext Context = Session.MakeContext(TestWorld.Actor, Where, Tunables, false, false);
		OutCentre = Context.GuidedCursor();
		GridOverlay::Describe(Context, Out);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridOverlayDrawsRoundTheCursorTest,
	"Airside.Tool.GridOverlay.DrawsRoundTheBuildPoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridOverlayDrawsRoundTheCursorTest::RunTest(const FString& Parameters)
{
	GridOverlayFixture::FLineSink Sink;
	FVector2D Centre;
	if (!TestTrue(TEXT("taxiway, 5 m"), GridOverlayFixture::DrawFor(TEXT("Taxiway"), EGridStep::FiveMetres,
		FVector2D(1234.0, 5678.0), Sink, Centre))) { return false; }

	TestTrue(TEXT("minor lines drawn"), Sink.CountOf(EPreviewStyle::GridMinor) > 0);
	TestTrue(TEXT("major lines drawn"), Sink.CountOf(EPreviewStyle::GridMajor) > 0);
	TestEqual(TEXT("nothing but grid"), Sink.CountOf(EPreviewStyle::GridMinor) + Sink.CountOf(EPreviewStyle::GridMajor), Sink.Lines.Num());

	bool bInside = true;
	for (const GridOverlayFixture::FLineSink::FRecorded& L : Sink.Lines)
	{
		bInside &= FVector2D::Distance(L.From, Centre) <= 6000.0 + 1e-6 && FVector2D::Distance(L.To, Centre) <= 6000.0 + 1e-6;
	}
	TestTrue(TEXT("all within the 5 m radius (60 m) of the guided cursor"), bInside);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridOverlayOnlyWhereTheGridAppliesTest,
	"Airside.Tool.GridOverlay.OnlyWhereTheGridApplies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridOverlayOnlyWhereTheGridAppliesTest::RunTest(const FString& Parameters)
{
	FVector2D Centre;

	GridOverlayFixture::FLineSink Off;
	if (!TestTrue(TEXT("taxiway, off"), GridOverlayFixture::DrawFor(TEXT("Taxiway"), EGridStep::Off, FVector2D(0.0, 0.0), Off, Centre))) { return false; }
	TestEqual(TEXT("grid off draws nothing"), Off.Lines.Num(), 0);

	GridOverlayFixture::FLineSink Select;
	if (!TestTrue(TEXT("select, 5 m"), GridOverlayFixture::DrawFor(TEXT("Select"), EGridStep::FiveMetres, FVector2D(0.0, 0.0), Select, Centre))) { return false; }
	TestEqual(TEXT("Select never shows the grid - nothing it does lands on it"), Select.Lines.Num(), 0);

	GridOverlayFixture::FLineSink Stand;
	if (!TestTrue(TEXT("stand, 1 m"), GridOverlayFixture::DrawFor(TEXT("Stand"), EGridStep::OneMetre, FVector2D(0.0, 0.0), Stand, Centre))) { return false; }
	TestTrue(TEXT("an idle stand tool does - its anchor lands on it"), Stand.Lines.Num() > 0);
	return true;
}

#endif
