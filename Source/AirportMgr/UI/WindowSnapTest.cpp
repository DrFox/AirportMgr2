#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UI/WindowSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace WindowSnapTest
{
	const FBox2D Screen(FVector2D(0.0, 0.0), FVector2D(1920.0, 900.0));   // bar's top at y 900
	const FVector2D Size(200.0, 150.0);
}

/**
 * SNAPPING IS WHAT MAKES DRAGGED WINDOWS LOOK LAID OUT RATHER THAN DROPPED. Each row is one
 * reason: 12 px pulls, 13 does not (the spec's figure, so the edge is exact), the bar's top is an
 * edge like the screen's, a neighbour's edge pulls only when the two actually face each other.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindowSnapPlaceTest, "AirportMgr.UI.WindowSnap.Place",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWindowSnapPlaceTest::RunTest(const FString& Parameters)
{
	using namespace WindowSnapTest;
	const TArray<FBox2D> None;
	struct FRow { FVector2D In; FVector2D Want; const TCHAR* Why; };
	// MARGIN 12: the default inset is itself a snap line, one margin in from each bounds edge, so a
	// window resting at its default place has nothing to jump to when a drag starts (final review
	// 2026-09-28: every default inset equalled SnapDistance, and the first pixel of a drag threw the
	// window flush into the corner).
	const FRow Rows[] = {
		{ FVector2D(12.0, 300.0),   FVector2D(12.0, 300.0),   TEXT("at the margin line it stays - the first pixel of a drag does not jump") },
		{ FVector2D(13.0, 300.0),   FVector2D(12.0, 300.0),   TEXT("1 px off the margin line snaps back onto it") },
		{ FVector2D(5.0, 300.0),    FVector2D(0.0, 300.0),    TEXT("nearer the edge than the margin snaps flush to the edge") },
		{ FVector2D(25.0, 300.0),   FVector2D(25.0, 300.0),   TEXT("13 px past the margin line: no pull - the distance is exact") },
		{ FVector2D(3.0, 4.0),      FVector2D(0.0, 0.0),      TEXT("a corner snaps on both axes") },
		{ FVector2D(-50.0, 300.0),  FVector2D(0.0, 300.0),    TEXT("dragged off the left is clamped back") },
		{ FVector2D(1800.0, 300.0), FVector2D(1720.0, 300.0), TEXT("dragged off the right is clamped back") },
		{ FVector2D(400.0, 745.0),  FVector2D(400.0, 750.0),  TEXT("the bar's top is an edge: bottom 895 snaps to 900, nearer than the 888 margin line") },
		{ FVector2D(400.0, 800.0),  FVector2D(400.0, 750.0),  TEXT("and a window cannot go under the bar") },
	};
	for (const FRow& R : Rows)
	{
		const FVector2D Got = WindowSnap::Place(R.In, Size, Screen, None, 12.0, 12.0);
		TestEqual(R.Why, Got, R.Want);
	}

	// NEIGHBOURS: the other window is at x 300..500, y 100..300.
	const TArray<FBox2D> Other = { FBox2D(FVector2D(300.0, 100.0), FVector2D(500.0, 300.0)) };
	TestEqual(TEXT("beside a neighbour it snaps flush to its right edge"),
		WindowSnap::Place(FVector2D(508.0, 120.0), Size, Screen, Other, 12.0, 12.0), FVector2D(500.0, 120.0));
	TestEqual(TEXT("a neighbour far below on the other axis does not pull sideways"),
		WindowSnap::Place(FVector2D(508.0, 500.0), Size, Screen, Other, 12.0, 12.0), FVector2D(508.0, 500.0));
	TestEqual(TEXT("under a neighbour it snaps flush to its bottom edge (x is 20 off both its edges: no pull)"),
		WindowSnap::Place(FVector2D(320.0, 309.0), Size, Screen, Other, 12.0, 12.0), FVector2D(320.0, 300.0));
	return true;
}

/** RESIZE SNAPS THE EDGES THAT MOVE and never below the floor the scroll box relies on. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindowSnapResizeTest, "AirportMgr.UI.WindowSnap.Resize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWindowSnapResizeTest::RunTest(const FString& Parameters)
{
	using namespace WindowSnapTest;
	const TArray<FBox2D> None;
	const FVector2D Min(180.0, 90.0);
	TestEqual(TEXT("never below the minimum"),
		WindowSnap::Resize(FVector2D(100.0, 100.0), FVector2D(20.0, 10.0), Min, Screen, None, 12.0, 12.0), FVector2D(180.0, 90.0));
	TestEqual(TEXT("never past the screen or the bar - but the minimum wins where they disagree"),
		WindowSnap::Resize(FVector2D(1800.0, 800.0), FVector2D(500.0, 500.0), Min, Screen, None, 12.0, 12.0), FVector2D(180.0, 100.0));
	TestEqual(TEXT("the right edge snaps onto the margin line 12 in from the screen's right (nearer than the edge)"),
		WindowSnap::Resize(FVector2D(1500.0, 100.0), FVector2D(410.0, 200.0), Min, Screen, None, 12.0, 12.0), FVector2D(408.0, 200.0));
	TestEqual(TEXT("and onto the edge itself when that is nearer"),
		WindowSnap::Resize(FVector2D(1500.0, 100.0), FVector2D(417.0, 200.0), Min, Screen, None, 12.0, 12.0), FVector2D(420.0, 200.0));
	return true;
}

#endif
