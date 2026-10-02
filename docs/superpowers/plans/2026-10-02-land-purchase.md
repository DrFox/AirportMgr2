# Land Purchase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Owned land becomes 600 m grid tiles the player buys with a "Buy land" tool; the diorama edge, camera, grass and every build tool follow the owned tiles.

**Architecture:** `FLandGrid` (Airside `Model/`, world-free) is the one record of owned land, saved on `URoadNetwork` and carried across undo. `URoadEditFacade` is its only writer and fires `OnOwnedLandChanged`; Airside presentation (walls, ground clip, grass) and the game module's camera listen to that. Buying charges the purse like any build; `UOpsRuntime` bridges the facade's `OnLandBought` onto the ops bus as `FLandPurchasedEvent` for the toast.

**Tech Stack:** UE 5.8 C++, Airside / AirportOps plugins, AirportMgr game module, UE automation tests, editor Python (`Tools/Python`).

**Spec:** `docs/superpowers/specs/2026-10-02-land-purchase-design.md` (agreed 2026-10-02).

## Global Constraints

- Tile 600 m (`TileSize = 60000` uu), grid 8 x 8, owned mask `uint64` - bit `Row * Columns + Column`; `FIntPoint(X = column along world X, Y = row along world Y)`.
- New game owns 1 x 2 tiles at the bottom centre: column 0 (lowest X, the bottom of the default camera, which looks along +X), rows 3 and 4.
- Buyable = unowned, on the grid, edge-adjacent (not diagonal) to an owned tile.
- Price = `Base x (1 + Growth x NumOwned())`; Base 150,000, Growth 0.5.
- An INVALID grid owns everything: no clip, no walls, no camera bound, no grass bound, no refusal. Every existing map keeps working.
- Owned land is NOT undone: undo/redo/clear carry the live mask over.
- Tool: id `BuyLand`, name "Buy land", key `T`. Refusal text: `Outside your land`.
- Builds: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-slot5\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE` (worktree; editor on slot5 must be closed).
- Tests: `./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-slot5\AirportMgr.uproject -Filter <prefix>`; full suite once at the end. Read the `N test(s) run, N failed, N crashed` line, never the exit code.
- A new test .cpp needs two builds (the first can say Succeeded without compiling it) - check the test ran by name.
- Every `UE_LOG` and WHY comment survives; comments carry dates and numbers; a comment claiming a fact about other code carries `// ENFORCED BY:`.
- Python edits: `io.open(..., encoding="utf-8")` default newlines or the Edit tool; never sed on Windows paths.
- Three PRs, in order: **A** (Tasks 1-6, owned tiles replace the rectangle), **B** (Tasks 7-9, refusal), **C** (Tasks 10-13, purchase). Each ends green and is merged before the next starts.

## Review Focus

1. **Undo after buying land** - expected: the tile stays owned and stays paid for; Ctrl+Z undoes only the road. (Task 2 test `Airside.Present.OwnedLand.UndoKeepsLand`.)
2. **Load a save made before this feature** (network blob with no grid) - expected: grid invalid, owns everything, nothing clipped or refused. (Task 1 test `Airside.Model.LandGrid.DefaultOwnsEverything`; Task 2 test `SaveRoundTripsLand`.)
3. **A road along the exact edge of owned land / a taxiway whose shoulder crosses the cut** - expected: the first builds, the second is refused. (Task 7 tests `EdgeTouchingIsOwned`, `ShoulderOverTheCutIsNot`.)
4. **An L-shaped plot and a build across its inner notch** - expected: refused even though every vertex is on owned tiles. (Task 7 test `NotchIsNotOwned`.)
5. **Buying the 64th tile / clicking a tile off the grid or diagonal to owned land** - expected: refused with a reason, no charge. (Task 10 test `BuyRefusals`.)

---

# PR A - owned tiles replace the rectangle

### Task 1: `FLandGrid` model

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/LandGrid.h`
- Create: `Plugins/Airside/Source/Airside/Private/Model/LandGrid.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/LandGridTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct FLandEdgeRun { FVector2D A; FVector2D B; FVector2D Outward; };
  USTRUCT() struct AIRSIDE_API FLandGrid {
    UPROPERTY() FVector2D Origin; UPROPERTY() double TileSize = 60000.0;
    UPROPERTY() int32 Columns = 8; UPROPERTY() int32 Rows = 8; UPROPERTY() uint64 Owned = 0;
    static constexpr int32 MaxTiles = 64;
    bool IsValid() const; bool IsOnGrid(FIntPoint) const; int32 BitOf(FIntPoint) const;
    bool IsTileOwned(FIntPoint) const; void SetTileOwned(FIntPoint, bool);
    int32 NumOwned() const; bool IsBuyable(FIntPoint) const;
    FIntPoint TileAt(FVector2D) const; FBox2D TileBox(FIntPoint) const;
    bool IsOwned(FVector2D) const; FVector2D ClampToOwned(FVector2D) const;
    TArray<FLandEdgeRun> Outline() const;
    uint16 MaskWord(int32 Word) const;   // 0..3, for the material
    static FLandGrid Make(FVector2D Origin, double TileSize, int32 Columns, int32 Rows, TConstArrayView<FIntPoint> Start);
  };
  ```

- [ ] **Step 1: Write the failing tests** - `LandGridTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/LandGrid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The R3 start on a 600 m 8x8 grid with its origin at the world origin: column 0, rows 3-4.
	FLandGrid LgStart()
	{
		const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
		return FLandGrid::Make(FVector2D::ZeroVector, 60000.0, 8, 8, Start);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridDefaultOwnsEverything, "Airside.Model.LandGrid.DefaultOwnsEverything",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridDefaultOwnsEverything::RunTest(const FString&)
{
	// A NETWORK SAVED BEFORE LAND EXISTED loads a default grid. It must own everything, or every old map
	// and every old save would refuse every build.
	const FLandGrid Grid;
	TestFalse(TEXT("a default grid is invalid"), Grid.IsValid());
	TestTrue(TEXT("and owns a point anywhere"), Grid.IsOwned(FVector2D(1.0e7, -1.0e7)));
	TestEqual(TEXT("and clamps nothing"), Grid.ClampToOwned(FVector2D(1.0e7, 5.0)), FVector2D(1.0e7, 5.0));
	TestEqual(TEXT("and has no outline to wall"), Grid.Outline().Num(), 0);
	FLandGrid TooBig = Grid;
	TooBig.Columns = 9;
	TooBig.Rows = 8;
	TestFalse(TEXT("9x8 tiles do not fit a 64-bit mask, so it is invalid rather than wrapping bits"), TooBig.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridOwnership, "Airside.Model.LandGrid.Ownership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridOwnership::RunTest(const FString&)
{
	const FLandGrid Grid = LgStart();
	TestTrue(TEXT("valid"), Grid.IsValid());
	TestEqual(TEXT("two tiles owned"), Grid.NumOwned(), 2);
	TestTrue(TEXT("inside tile (0,3)"), Grid.IsOwned(FVector2D(30000.0, 210000.0)));
	TestFalse(TEXT("tile (1,3) is not owned"), Grid.IsOwned(FVector2D(90000.0, 210000.0)));
	TestTrue(TEXT("the shared edge between two owned tiles is owned"), Grid.IsOwned(FVector2D(30000.0, 240000.0)));
	TestFalse(TEXT("the edge between owned and unowned is NOT - a tuft or a wall there leans over the cut"),
		Grid.IsOwned(FVector2D(60000.0, 210000.0)));
	TestFalse(TEXT("off the grid is not owned"), Grid.IsOwned(FVector2D(-1.0, 210000.0)));
	TestEqual(TEXT("TileAt"), Grid.TileAt(FVector2D(90000.0, 250000.0)), FIntPoint(1, 4));
	TestEqual(TEXT("mask word 0 holds bits 0-15: (0,3) is bit 24, so word 0 is empty"), int32(Grid.MaskWord(0)), 0);
	TestEqual(TEXT("word 1 holds bits 16-31: bit 24 is 1 << 8"), int32(Grid.MaskWord(1)), 1 << 8);
	TestEqual(TEXT("word 2 holds bits 32-47: bit 32 (0,4) is 1 << 0"), int32(Grid.MaskWord(2)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridBuyable, "Airside.Model.LandGrid.Buyable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridBuyable::RunTest(const FString&)
{
	const FLandGrid Grid = LgStart();
	TestTrue(TEXT("edge-adjacent (1,3) is buyable"), Grid.IsBuyable(FIntPoint(1, 3)));
	TestTrue(TEXT("(0,2) beside (0,3) is buyable"), Grid.IsBuyable(FIntPoint(0, 2)));
	TestFalse(TEXT("diagonal (1,2) is not - R4"), Grid.IsBuyable(FIntPoint(1, 2)));
	TestFalse(TEXT("an owned tile is not"), Grid.IsBuyable(FIntPoint(0, 3)));
	TestFalse(TEXT("off the grid is not"), Grid.IsBuyable(FIntPoint(-1, 3)));
	TestFalse(TEXT("far away is not"), Grid.IsBuyable(FIntPoint(5, 5)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridOutlineAndClamp, "Airside.Model.LandGrid.OutlineAndClamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridOutlineAndClamp::RunTest(const FString&)
{
	// AN L: (0,3), (0,4), (1,3). Six boundary runs - the 1x2's west and its two ends merge across the
	// shared edge, and the notch at (1,4) adds two.
	FLandGrid Grid = LgStart();
	Grid.SetTileOwned(FIntPoint(1, 3), true);
	const TArray<FLandEdgeRun> Runs = Grid.Outline();
	TestEqual(TEXT("an L has six runs"), Runs.Num(), 6);
	double Perimeter = 0.0;
	for (const FLandEdgeRun& Run : Runs)
	{
		Perimeter += FVector2D::Distance(Run.A, Run.B);
		const FVector2D Mid = (Run.A + Run.B) * 0.5;
		TestTrue(TEXT("one step inward from a run is owned"), Grid.IsOwned(Mid - Run.Outward * 100.0));
		TestFalse(TEXT("one step outward is not"), Grid.IsOwned(Mid + Run.Outward * 100.0));
	}
	TestEqual(TEXT("the L's perimeter is 8 tile sides"), Perimeter, 8.0 * 60000.0, 1e-6);

	// (100000, 290000) is over the notch (1,4). The bounding box would leave it there; the nearest owned
	// point is on (0,4)'s east edge, 400 m away, nearer than (1,3)'s north edge at 500 m.
	TestEqual(TEXT("a point over the notch clamps to the nearest owned point, not the bounding box"),
		Grid.ClampToOwned(FVector2D(100000.0, 290000.0)), FVector2D(60000.0, 290000.0));
	TestEqual(TEXT("an owned point is left alone"), Grid.ClampToOwned(FVector2D(30000.0, 200000.0)), FVector2D(30000.0, 200000.0));
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify they fail** - build. Expected: compile error, `Model/LandGrid.h` not found.

- [ ] **Step 3: Implement** - `LandGrid.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "LandGrid.generated.h"

/** One straight stretch of the owned land's boundary, A to B, with the normal pointing into the void. */
struct FLandEdgeRun
{
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;
	FVector2D Outward = FVector2D::ZeroVector;
};

/**
 * The land the player owns, as tiles of a fixed grid (land purchase spec 2026-10-02 R1-R4). THE ONE RECORD:
 * the ground clip, the plinth walls, the build camera, the grass and every build refusal read this, through
 * URoadNetwork::GetOwnedLand. URoadEditFacade is the only writer.
 *
 * AN INVALID GRID OWNS EVERYTHING - the default, so a network saved before land existed, and every map
 * that never set one, keeps building anywhere.
 *
 * uint64 AND 8x8 ARE ONE FACT: a bigger grid is a different mask type, and IsValid refuses one rather
 * than letting bits wrap. Tiles are FIntPoint(X = column along world X, Y = row along world Y), bit
 * Y * Columns + X.
 * ENFORCED BY: Airside.Model.LandGrid.DefaultOwnsEverything
 */
USTRUCT()
struct AIRSIDE_API FLandGrid
{
	GENERATED_BODY()

	static constexpr int32 MaxTiles = 64;

	/** South-west (min X, min Y) corner of tile (0,0), uu. */
	UPROPERTY() FVector2D Origin = FVector2D::ZeroVector;
	UPROPERTY() double TileSize = 60000.0;
	UPROPERTY() int32 Columns = 8;
	UPROPERTY() int32 Rows = 8;
	UPROPERTY() uint64 Owned = 0;

	static FLandGrid Make(FVector2D InOrigin, double InTileSize, int32 InColumns, int32 InRows, TConstArrayView<FIntPoint> Start);

	/** At least one tile owned, a real tile size, and at most MaxTiles tiles. */
	bool IsValid() const;
	bool IsOnGrid(FIntPoint Tile) const { return Tile.X >= 0 && Tile.Y >= 0 && Tile.X < Columns && Tile.Y < Rows; }
	int32 BitOf(FIntPoint Tile) const { return Tile.Y * Columns + Tile.X; }
	bool IsTileOwned(FIntPoint Tile) const;
	void SetTileOwned(FIntPoint Tile, bool bOwned);
	int32 NumOwned() const;
	/** R4: on the grid, unowned, and sharing an EDGE (not a corner) with an owned tile. */
	bool IsBuyable(FIntPoint Tile) const;
	FIntPoint TileAt(FVector2D Point) const;
	FBox2D TileBox(FIntPoint Tile) const;

	/**
	 * Is this ground the player's? A point ON a tile boundary is owned only if every tile it touches is - the
	 * inclusive-covered rule FGroundCoverMask uses for slabs: a tuft or a wall centred there leans over the cut.
	 * Invalid grid: true.
	 */
	bool IsOwned(FVector2D Point) const;

	/** The nearest owned point - per tile, not the bounding box, so an L cannot leave the camera over its notch. */
	FVector2D ClampToOwned(FVector2D Point) const;

	/** The boundary as straight runs: unit tile edges with an unowned neighbour, merged where collinear and touching. */
	TArray<FLandEdgeRun> Outline() const;

	/** Bits 16*Word .. 16*Word+15 - four of these carry the mask to the material, one float each (exact to 2^24). */
	uint16 MaskWord(int32 Word) const { return static_cast<uint16>((Owned >> (16 * Word)) & 0xFFFFu); }
};
```

`LandGrid.cpp`:

```cpp
#include "Model/LandGrid.h"

FLandGrid FLandGrid::Make(FVector2D InOrigin, double InTileSize, int32 InColumns, int32 InRows, TConstArrayView<FIntPoint> Start)
{
	FLandGrid Grid;
	Grid.Origin = InOrigin;
	Grid.TileSize = InTileSize;
	Grid.Columns = InColumns;
	Grid.Rows = InRows;
	for (const FIntPoint& Tile : Start)
	{
		Grid.SetTileOwned(Tile, true);
	}
	return Grid;
}

bool FLandGrid::IsValid() const
{
	return TileSize > 0.0 && Columns > 0 && Rows > 0 && Columns * Rows <= MaxTiles && Owned != 0;
}

bool FLandGrid::IsTileOwned(FIntPoint Tile) const
{
	return IsOnGrid(Tile) && ((Owned >> BitOf(Tile)) & 1ull) != 0;
}

void FLandGrid::SetTileOwned(FIntPoint Tile, bool bOwned)
{
	if (!IsOnGrid(Tile) || Columns * Rows > MaxTiles)
	{
		return;
	}
	const uint64 Bit = 1ull << BitOf(Tile);
	Owned = bOwned ? (Owned | Bit) : (Owned & ~Bit);
}

int32 FLandGrid::NumOwned() const
{
	return FMath::CountBits(Owned);
}

bool FLandGrid::IsBuyable(FIntPoint Tile) const
{
	if (!IsOnGrid(Tile) || IsTileOwned(Tile))
	{
		return false;
	}
	return IsTileOwned(Tile + FIntPoint(1, 0)) || IsTileOwned(Tile - FIntPoint(1, 0))
		|| IsTileOwned(Tile + FIntPoint(0, 1)) || IsTileOwned(Tile - FIntPoint(0, 1));
}

FIntPoint FLandGrid::TileAt(FVector2D Point) const
{
	return FIntPoint(
		FMath::FloorToInt32((Point.X - Origin.X) / TileSize),
		FMath::FloorToInt32((Point.Y - Origin.Y) / TileSize));
}

FBox2D FLandGrid::TileBox(FIntPoint Tile) const
{
	const FVector2D Min = Origin + FVector2D(Tile.X, Tile.Y) * TileSize;
	return FBox2D(Min, Min + FVector2D(TileSize, TileSize));
}

bool FLandGrid::IsOwned(FVector2D Point) const
{
	if (!IsValid())
	{
		return true;
	}
	// Every tile the point touches: one, or two on an edge, or four at a corner.
	const double FX = (Point.X - Origin.X) / TileSize;
	const double FY = (Point.Y - Origin.Y) / TileSize;
	const int32 X = FMath::FloorToInt32(FX);
	const int32 Y = FMath::FloorToInt32(FY);
	const bool bOnX = FX == double(X);
	const bool bOnY = FY == double(Y);
	for (int32 DX = bOnX ? -1 : 0; DX <= 0; ++DX)
	{
		for (int32 DY = bOnY ? -1 : 0; DY <= 0; ++DY)
		{
			if (!IsTileOwned(FIntPoint(X + DX, Y + DY)))
			{
				return false;
			}
		}
	}
	return true;
}

FVector2D FLandGrid::ClampToOwned(FVector2D Point) const
{
	if (!IsValid())
	{
		return Point;
	}
	// A linear walk of at most MaxTiles boxes - 64, fixed by the mask type (2026-10-02).
	FVector2D Best = Point;
	double BestDistSq = TNumericLimits<double>::Max();
	for (int32 Y = 0; Y < Rows; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			if (!IsTileOwned(FIntPoint(X, Y)))
			{
				continue;
			}
			const FBox2D Box = TileBox(FIntPoint(X, Y));
			const FVector2D Clamped(FMath::Clamp(Point.X, Box.Min.X, Box.Max.X), FMath::Clamp(Point.Y, Box.Min.Y, Box.Max.Y));
			const double DistSq = FVector2D::DistSquared(Point, Clamped);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = Clamped;
			}
		}
	}
	return Best;
}

TArray<FLandEdgeRun> FLandGrid::Outline() const
{
	TArray<FLandEdgeRun> Runs;
	if (!IsValid())
	{
		return Runs;
	}
	// Unit edges first, keyed by (side, line, start) so collinear neighbours meet in order. Side 0..3 =
	// -X, +X, -Y, +Y; the line is the tile coordinate of the boundary, the start the tile along it.
	struct FUnit { int32 Side; int32 Line; int32 Along; };
	TArray<FUnit> Units;
	const FIntPoint Steps[4] = { FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1) };
	for (int32 Y = 0; Y < Rows; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			if (!IsTileOwned(FIntPoint(X, Y)))
			{
				continue;
			}
			for (int32 Side = 0; Side < 4; ++Side)
			{
				if (IsTileOwned(FIntPoint(X, Y) + Steps[Side]))
				{
					continue;
				}
				const bool bAlongY = Side < 2;   // an X-facing side runs along Y
				const int32 Line = bAlongY ? X + (Side == 1 ? 1 : 0) : Y + (Side == 3 ? 1 : 0);
				Units.Add({ Side, Line, bAlongY ? Y : X });
			}
		}
	}
	Units.Sort([](const FUnit& L, const FUnit& R)
	{
		return L.Side != R.Side ? L.Side < R.Side : L.Line != R.Line ? L.Line < R.Line : L.Along < R.Along;
	});
	for (int32 I = 0; I < Units.Num();)
	{
		int32 J = I + 1;
		while (J < Units.Num() && Units[J].Side == Units[I].Side && Units[J].Line == Units[I].Line
			&& Units[J].Along == Units[J - 1].Along + 1)
		{
			++J;
		}
		const FUnit& U = Units[I];
		const bool bAlongY = U.Side < 2;
		const int32 End = Units[J - 1].Along + 1;
		FLandEdgeRun Run;
		Run.A = Origin + (bAlongY ? FVector2D(U.Line, U.Along) : FVector2D(U.Along, U.Line)) * TileSize;
		Run.B = Origin + (bAlongY ? FVector2D(U.Line, End) : FVector2D(End, U.Line)) * TileSize;
		Run.Outward = FVector2D(Steps[U.Side].X, Steps[U.Side].Y);
		Runs.Add(Run);
		I = J;
	}
	return Runs;
}
```

- [ ] **Step 4: Build, run** `-Filter Airside.Model.LandGrid` (twice-build rule: confirm the four names appear). Expected: 4 run, 0 failed.

- [ ] **Step 5: Commit** - `git add` the three files; `git commit -m "model: FLandGrid - owned land as 600 m tiles"`.

### Task 2: Land on the network, facade writer, survives undo and save

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` (beside `NextDepotNumber`, ~line 1307; public getter/setter near `GetNodes`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp` (`CopyFrom`, ~line 426)
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h` (beside `OnRefused`, ~line 235)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp` (`AdoptNetwork` line 370, `RestoreInPlace` ~line 446)
- Test: `Plugins/Airside/Source/AirsideTests/Private/OwnedLandTest.cpp` (replace #529's rectangle tests - Task 3 rewrites the rest)

**Interfaces:**
- Consumes: `FLandGrid` (Task 1).
- Produces: `const FLandGrid& URoadNetwork::GetOwnedLand() const`; `void URoadNetwork::SetOwnedLand(const FLandGrid&)` (facade only); `URoadEditFacade::FOnOwnedLandChanged OnOwnedLandChanged` (`DECLARE_MULTICAST_DELEGATE_OneParam(FOnOwnedLandChanged, const FLandGrid&)`); `void URoadEditFacade::AuthorOwnedLand(const FLandGrid&)`.

- [ ] **Step 1: Failing tests** - replace the body of `OwnedLandTest.cpp` with (Task 3 appends more):

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FLandGrid OlStart()
	{
		const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
		return FLandGrid::Make(FVector2D(-100000.0, -300000.0), 60000.0, 8, 8, Start);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandUndoKeepsLand, "Airside.Present.OwnedLand.UndoKeepsLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandUndoKeepsLand::RunTest(const FString&)
{
	// SPEC 3.1: undo is a Memento of the whole network, so without the carry an undo would hand back land
	// the player paid for while keeping the money (or, refunded, make land free to try).
	FAirsideTestWorld Fixture;
	URoadEditFacade* Facade = Fixture.Actor->GetEditFacade();
	int32 Heard = 0;
	Facade->OnOwnedLandChanged.AddLambda([&Heard](const FLandGrid&) { ++Heard; });
	Facade->AuthorOwnedLand(OlStart());
	TestEqual(TEXT("authoring announces"), Heard, 1);

	const int32 A = Fixture.Actor->PlaceNode(FVector2D(-90000.0, -100000.0));
	const int32 B = Fixture.Actor->PlaceNode(FVector2D(-90000.0, -60000.0));
	TestTrue(TEXT("a road is drawn"), Fixture.Actor->ConnectNodes(A, B, ERoadKind::Taxiway));

	FLandGrid Grown = Fixture.Actor->Network->GetOwnedLand();
	Grown.SetTileOwned(FIntPoint(1, 3), true);
	Facade->AuthorOwnedLand(Grown);   // stands in for a purchase until Task 10

	TestTrue(TEXT("undo of the road succeeds"), Facade->Undo());
	TestTrue(TEXT("the tile bought after the road is STILL owned"), Fixture.Actor->Network->GetOwnedLand().IsTileOwned(FIntPoint(1, 3)));
	TestTrue(TEXT("redo too"), Facade->Redo());
	TestTrue(TEXT("still owned after redo"), Fixture.Actor->Network->GetOwnedLand().IsTileOwned(FIntPoint(1, 3)));
	Facade->ClearNetwork();
	TestEqual(TEXT("clearing the airport keeps the land"), Fixture.Actor->Network->GetOwnedLand().NumOwned(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandSaveRoundTrips, "Airside.Present.OwnedLand.SaveRoundTripsLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandSaveRoundTrips::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(OlStart());
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	Fixture.Actor->Network->Serialize(Writer);

	URoadNetwork* Loaded = NewObject<URoadNetwork>();
	FMemoryReader Reader(Bytes);
	Loaded->Serialize(Reader);
	TestEqual(TEXT("the mask survives the network's own serialisation (the save's Network blob)"),
		Loaded->GetOwnedLand().Owned, OlStart().Owned);
	TestEqual(TEXT("and the origin"), Loaded->GetOwnedLand().Origin, OlStart().Origin);
	return true;
}

#endif
```

- [ ] **Step 2: Build** - expected: compile errors (`GetOwnedLand`, `AuthorOwnedLand`, `OnOwnedLandChanged` undefined).

- [ ] **Step 3: Implement.**

`RoadNetwork.h` - add `#include "Model/LandGrid.h"`; in the public section beside the other const getters:

```cpp
	/** The land the player owns - see FLandGrid. Invalid (the default) owns everything. */
	const FLandGrid& GetOwnedLand() const { return OwnedLand; }

	/**
	 * URoadEditFacade's door only (AuthorOwnedLand, BuyLandTile): the facade announces the change, and a write
	 * past it would leave the walls, clip, camera and grass on the old land.
	 * ENFORCED BY: Check-Architecture rule 63 (owned-land-one-writer)
	 */
	void SetOwnedLand(const FLandGrid& Land) { OwnedLand = Land; }
```

and beside `NextDepotNumber`:

```cpp
	/**
	 * Owned land (land purchase spec 2026-10-02). A UPROPERTY so it rides the save's Network blob and the
	 * level. NOT UNDONE: URoadEditFacade::AdoptNetwork carries the live value onto every snapshot it adopts
	 * (spec 3.1) - so it is copied by CopyFrom like every UPROPERTY, and overridden only at that one door.
	 */
	UPROPERTY() FLandGrid OwnedLand;
```

`RoadNetwork.cpp` `CopyFrom` - after `NextDepotNumber = Source.NextDepotNumber;` (or the last UPROPERTY assignment): `OwnedLand = Source.OwnedLand;` (`Airside.Model.CopyFromCoversEveryProperty` fails without it.)

`RoadEditFacade.h`, after `OnRefused`:

```cpp
	/**
	 * The owned land changed - authored, bought, or a load brought different land. Airside's presentation
	 * (AAirsideOwnedLandActor, AAirsideGroundCoverActor) and the game module's camera listen; ops hears a
	 * PURCHASE through OnLandBought, not this. Not fired by undo, redo or clear: land is not undone (AdoptNetwork).
	 * ENFORCED BY: Airside.Present.OwnedLand.UndoKeepsLand, Airside.Present.OwnedLand.WallsFollowAPurchase
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnOwnedLandChanged, const FLandGrid& /*Land*/);
	FOnOwnedLandChanged OnOwnedLandChanged;

	/** Set the owned land outright - the level authoring script and tests. Not a purchase: charges nothing. */
	void AuthorOwnedLand(const FLandGrid& Land);
```

`RoadEditFacade.cpp`:

```cpp
void URoadEditFacade::AuthorOwnedLand(const FLandGrid& Land)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return;
	}
	Network->SetOwnedLand(Land);
	UE_LOG(LogRoadMesh, Log, TEXT("OwnedLand: authored - %d tile(s) of %dx%d"), Land.NumOwned(), Land.Columns, Land.Rows);
	OnOwnedLandChanged.Broadcast(Land);
}
```

`AdoptNetwork` - first lines become:

```cpp
void URoadEditFacade::AdoptNetwork(URoadNetwork& NewNetwork)
{
	// LAND IS NOT UNDONE (land purchase spec 3.1): an undo, redo or clear adopts a snapshot taken before the
	// latest purchase, and taking its mask would hand back paid-for land. The live mask travels onto whatever is
	// adopted. RollBackOpenEdit adopts the live object itself, so this is a no-op there; a load goes through
	// RestoreInPlace, whose deserialised mask is the save's and stands.
	// ENFORCED BY: Airside.Present.OwnedLand.UndoKeepsLand
	if (Actor().Network != nullptr && Actor().Network != &NewNetwork)
	{
		NewNetwork.SetOwnedLand(Actor().Network->GetOwnedLand());
	}
	Actor().Network = &NewNetwork;
```

`RestoreInPlace` - after `AdoptNetwork(*Owner.Network);` add `OnOwnedLandChanged.Broadcast(Owner.Network->GetOwnedLand());` (a load may bring different land).

Check-Architecture: add rule 63 `owned-land-one-writer` after the last rule, modelled on an existing token rule (e.g. rule 52's loop): fail any `\bSetOwnedLand\s*\(` in `Plugins/Airside`, `Plugins/AirportOps`, `Source/AirportMgr`, `Plugins/AirsideEditor` outside `RoadEditFacade*.cpp`, `RoadNetwork.h` and `*Test.cpp`; `$ranRules.Add('owned-land-one-writer')`. Verify red by temporarily adding `Network->SetOwnedLand(FLandGrid());` to `AirsideGroundCoverActor.cpp`, run `./Tools/Check-Architecture.ps1`, see the failure, remove it.

- [ ] **Step 4: Build; run** `-Filter Airside.Present.OwnedLand` and `-Filter Airside.Model.CopyFrom`. Expected: all pass.

- [ ] **Step 5: Commit** - `"model: owned land on the network; facade writer; not undone"`.

### Task 3: The edge follows the tiles (actor walls + material clip)

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideOwnedLandActor.h`, `Private/Present/AirsideOwnedLandActor.cpp` (rewrite)
- Modify: `Tools/Python/airside_matnodes.py` (`OWNED_RECT_*`, `owned_land_collection`, `owned_rect_clip`)
- Modify: `Tools/Python/build_diorama_prototype.py`
- Test: `OwnedLandTest.cpp` (append), `Plugins/Airside/Source/AirsideTests/Private/OwnedLandTest.cpp` `OwnedLandWired`

**Interfaces:**
- Consumes: `URoadEditFacade::OnOwnedLandChanged`, `URoadNetwork::GetOwnedLand`, `FLandGrid::Outline/MaskWord`.
- Produces: `AAirsideOwnedLandActor` with `UFUNCTION(BlueprintCallable) void AuthorStartingLand(FVector2D Origin, double TileSize, int32 Columns, int32 Rows, const TArray<FIntPoint>& StartTiles)`; `static const FName CollectionParams[10]` = `LandValid, LandOriginX, LandOriginY, LandTileSize, LandColumns, LandRows, LandMask0, LandMask1, LandMask2, LandMask3`; `const TArray<TObjectPtr<UStaticMeshComponent>>& GetWalls() const` (only the first `NumWalls()` are live); `int32 NumWalls() const`.

- [ ] **Step 1: Failing tests** - append to `OwnedLandTest.cpp` (add `#include "Present/AirsideOwnedLandActor.h"`, `#include "Components/StaticMeshComponent.h"`); delete #529's `WallsStandUnderTheEdge`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnedLandWallsFollowAPurchase, "Airside.Present.OwnedLand.WallsFollowAPurchase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOwnedLandWallsFollowAPurchase::RunTest(const FString&)
{
	// THE SEAM: the actor must hear OnOwnedLandChanged. Unwired, the walls stay on the old outline - a cut
	// through owned ground and a void where land was bought.
	FAirsideTestWorld Fixture;
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(OlStart());
	AAirsideOwnedLandActor* Edge = Fixture.World->SpawnActor<AAirsideOwnedLandActor>(FVector(0.0, 0.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("edge spawned"), Edge)) { return false; }
	Edge->DispatchBeginPlay();   // binds to the airport, as play does
	TestEqual(TEXT("a 1x2 has four walls"), Edge->NumWalls(), 4);

	FLandGrid Grown = Fixture.Actor->Network->GetOwnedLand();
	Grown.SetTileOwned(FIntPoint(1, 3), true);
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(Grown);
	TestEqual(TEXT("an L has six walls, at once"), Edge->NumWalls(), 6);

	// Each live wall's outer face lies on its run - measured from the transform (a fresh checkout may name no mesh).
	const TArray<FLandEdgeRun> Runs = Grown.Outline();
	for (int32 I = 0; I < Edge->NumWalls(); ++I)
	{
		const UStaticMeshComponent* Wall = Edge->GetWalls()[I];
		const FVector2D Centre(Wall->GetComponentLocation());
		// Half the default WallThickness (100) out along the run's normal is the outer face.
		const FVector2D Outer = Centre + Runs[I].Outward * 50.0;
		const FVector2D Mid = (Runs[I].A + Runs[I].B) * 0.5;
		TestEqual(*FString::Printf(TEXT("wall %d's outer face is on its run"), I), Outer, Mid);
	}
	return true;
}
```

Update `Airside.Content.OwnedLandWired`'s loop to `AAirsideOwnedLandActor::CollectionParams` (10 names) and its default check to: `LandValid` defaults to 0, the rest exist.

- [ ] **Step 2: Build; expect compile errors** (`NumWalls`, the new params).

- [ ] **Step 3: Implement the actor.** Header: remove `OwnedMin/OwnedMax/GetOwnedLand/SetOwnedLand/Find`'s rectangle docs; keep `PlinthDepth`, `WallThickness`, `Find`. New members:

```cpp
	/**
	 * Author this level's starting land on its airport - the level script's door (build_diorama_prototype.py).
	 * The land then lives on the network (saved with the level and every save); this actor only draws it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside|OwnedLand")
	void AuthorStartingLand(FVector2D Origin, double TileSize, int32 Columns, int32 Rows, const TArray<FIntPoint>& StartTiles);

	static const FName CollectionParams[10];
	int32 NumWalls() const { return LiveWalls; }

	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

private:
	/** Draw Land: walls under every outline run, and the clip written. Invalid land hides the walls and clips nothing. */
	void Apply(const FLandGrid& Land);
	void WriteCollection(const FLandGrid& Land) const;
	void Bind();
	void Unbind();
	UStaticMeshComponent* WallAt(int32 Index);

	/** Pooled: an L-shape adds walls, a purchase that straightens an edge frees one. At most 4 x 32 runs on 8x8. */
	UPROPERTY(VisibleAnywhere, Category = "Airside|OwnedLand")
	TArray<TObjectPtr<UStaticMeshComponent>> Walls;
	int32 LiveWalls = 0;
	TWeakObjectPtr<URoadEditFacade> BoundFacade;
	FDelegateHandle BoundHandle;
```

`.cpp` essentials (keep #529's file-level comments that still hold; drop the rectangle ones):

```cpp
const FName AAirsideOwnedLandActor::CollectionParams[10] = {
	TEXT("LandValid"), TEXT("LandOriginX"), TEXT("LandOriginY"), TEXT("LandTileSize"), TEXT("LandColumns"),
	TEXT("LandRows"), TEXT("LandMask0"), TEXT("LandMask1"), TEXT("LandMask2"), TEXT("LandMask3") };

static const FLandGrid& LandOf(const UWorld* World)
{
	static const FLandGrid None;
	const ARoadNetworkActor* Road = ARoadNetworkActor::Find(World);
	return Road != nullptr && Road->Network != nullptr ? Road->Network->GetOwnedLand() : None;
}

void AAirsideOwnedLandActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Apply(LandOf(GetWorld()));   // the editor shows the level's authored land
}

void AAirsideOwnedLandActor::BeginPlay()
{
	Super::BeginPlay();
	Bind();
}

void AAirsideOwnedLandActor::Bind()
{
	Unbind();
	ARoadNetworkActor* Road = ARoadNetworkActor::Find(GetWorld());
	URoadEditFacade* Facade = Road != nullptr ? Road->GetEditFacade() : nullptr;
	if (Facade != nullptr)
	{
		BoundFacade = Facade;
		BoundHandle = Facade->OnOwnedLandChanged.AddUObject(this, &AAirsideOwnedLandActor::Apply);
	}
	Apply(LandOf(GetWorld()));   // the catch-up: the land may have been set before this bound
}

void AAirsideOwnedLandActor::Unbind()
{
	if (URoadEditFacade* Facade = BoundFacade.Get())
	{
		Facade->OnOwnedLandChanged.Remove(BoundHandle);
	}
	BoundFacade.Reset();
	BoundHandle.Reset();
}

void AAirsideOwnedLandActor::AuthorStartingLand(FVector2D Origin, double TileSize, int32 Columns, int32 Rows,
	const TArray<FIntPoint>& StartTiles)
{
	ARoadNetworkActor* Road = ARoadNetworkActor::Find(GetWorld());
	if (Road == nullptr || Road->GetEditFacade() == nullptr)
	{
		UE_LOG(LogAirside, Warning, TEXT("OwnedLand: no airport in this level to author land on"));
		return;
	}
	Road->GetEditFacade()->AuthorOwnedLand(FLandGrid::Make(Origin, TileSize, Columns, Rows, StartTiles));
	Apply(Road->Network->GetOwnedLand());   // editor: no BeginPlay bound us
}

UStaticMeshComponent* AAirsideOwnedLandActor::WallAt(int32 Index)
{
	while (Walls.Num() <= Index)
	{
		UStaticMeshComponent* Wall = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Wall->SetupAttachment(RootComponent);
		Wall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Wall->SetCanEverAffectNavigation(false);
		Wall->RegisterComponent();
		Walls.Add(Wall);
	}
	return Walls[Index];
}

void AAirsideOwnedLandActor::Apply(const FLandGrid& Land)
{
	WriteCollection(Land);
	const FOwnedLandKit Kit = UAirsideSettings::ResolveOwnedLandKit();
	const TArray<FLandEdgeRun> Runs = Land.Outline();   // empty when invalid
	const double GroundZ = GetActorLocation().Z;
	const double Depth = FMath::Max(PlinthDepth, 100.0);
	const double T = FMath::Max(WallThickness, 1.0);
	const double CentreZ = GroundZ - 5.0 - Depth * 0.5;   // tops 5 uu under the ground: no z-fight along the rim
	for (int32 I = 0; I < Runs.Num(); ++I)
	{
		const FLandEdgeRun& Run = Runs[I];
		const FVector2D Along = Run.B - Run.A;
		// Inside the land, so the outer face IS the edge the clip cuts at.
		const FVector2D Centre = (Run.A + Run.B) * 0.5 - Run.Outward * (T * 0.5);
		const bool bAlongX = FMath::Abs(Along.X) > FMath::Abs(Along.Y);
		const FVector Extent(bAlongX ? FMath::Abs(Along.X) : T, bAlongX ? T : FMath::Abs(Along.Y), Depth);
		UStaticMeshComponent* Wall = WallAt(I);
		Wall->SetStaticMesh(Kit.WallMesh);
		Wall->SetMaterial(0, Kit.WallMaterial);
		Wall->SetWorldLocationAndRotation(FVector(Centre, CentreZ), FRotator::ZeroRotator);
		Wall->SetWorldScale3D(Extent / 100.0);   // the engine cube is 100 uu on a side
		Wall->SetVisibility(Kit.WallMesh != nullptr);
	}
	for (int32 I = Runs.Num(); I < Walls.Num(); ++I)
	{
		Walls[I]->SetVisibility(false);
	}
	LiveWalls = Runs.Num();
	UE_LOG(LogAirside, Log, TEXT("OwnedLand: %d tile(s), %d wall run(s) in %s"), Land.IsValid() ? Land.NumOwned() : 0, LiveWalls,
		GetWorld() != nullptr ? *GetWorld()->GetName() : TEXT("?"));
}

void AAirsideOwnedLandActor::WriteCollection(const FLandGrid& Land) const
{
	UWorld* World = GetWorld();
	UMaterialParameterCollection* Collection = UAirsideSettings::ResolveOwnedLandKit().Collection;
	UMaterialParameterCollectionInstance* Instance = World != nullptr && Collection != nullptr
		? World->GetParameterCollectionInstance(Collection) : nullptr;
	if (Instance == nullptr)
	{
		if (World != nullptr && Collection == nullptr)
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: no OwnedLandCollection in the content set - the ground is not clipped."));
		}
		return;
	}
	const float Values[10] = {
		Land.IsValid() ? 1.0f : 0.0f, float(Land.Origin.X), float(Land.Origin.Y), float(Land.TileSize),
		float(Land.Columns), float(Land.Rows),
		float(Land.MaskWord(0)), float(Land.MaskWord(1)), float(Land.MaskWord(2)), float(Land.MaskWord(3)) };
	for (int32 I = 0; I < 10; ++I)
	{
		if (!Instance->SetScalarParameterValue(CollectionParams[I], Values[I]))
		{
			UE_LOG(LogAirside, Warning, TEXT("OwnedLand: %s has no scalar '%s'."), *Collection->GetName(), *CollectionParams[I].ToString());
		}
	}
}
```

`EndPlay`/`Destroyed`: `Unbind(); WriteCollection(FLandGrid());` then Super. Remove the constructor's four default wall subobjects (walls are pooled now). Includes: `Present/RoadNetworkActor.h`, `Present/RoadEditFacade.h`, `Model/RoadNetwork.h`, `Model/LandGrid.h`.

- [ ] **Step 4: Material.** In `airside_matnodes.py` replace the rect params and clip:

```python
OWNED_LAND_PARAMS = (("LandValid", 0.0), ("LandOriginX", 0.0), ("LandOriginY", 0.0), ("LandTileSize", 60000.0),
                     ("LandColumns", 8.0), ("LandRows", 8.0),
                     ("LandMask0", 0.0), ("LandMask1", 0.0), ("LandMask2", 0.0), ("LandMask3", 0.0))
```

`owned_land_collection` iterates `OWNED_LAND_PARAMS` (names + defaults; still in place, keeping Ids; REMOVE the four `Owned*` rect parameters from the array). `owned_rect_clip` -> `owned_land_clip(lib, mat, mpc, x, y)` with inputs `WP, Valid, OX, OY, Size, Cols, Rows, M0, M1, M2, M3` and code:

```hlsl
if (Valid < 0.5) return 1.0;
float2 t = floor((WP.xy - float2(OX, OY)) / Size);
if (t.x < 0 || t.y < 0 || t.x >= Cols || t.y >= Rows) return 0.0;
float bit = t.y * Cols + t.x;
float word = floor(bit / 16.0);
float m = word < 0.5 ? M0 : (word < 1.5 ? M1 : (word < 2.5 ? M2 : M3));
return fmod(floor(m / exp2(bit - word * 16.0)), 2.0) >= 0.5 ? 1.0 : 0.0;
```

In `build_ground_material.py` call `nodes.owned_land_clip`.

- [ ] **Step 5: Script.** `build_diorama_prototype.py`: replace the `set_owned_land` call with `AuthorStartingLand`. Origin chosen so the start tiles (column 0, rows 3-4) are centred on the road's footprint:

```python
TILE = 60000.0
COLUMNS, ROWS = 8, 8
START = [(0, 3), (0, 4)]   # R3: bottom centre - column 0 is the default camera's bottom edge (it looks along +X)
...
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2          # owned_rect() as before: road footprint + margin
    origin = unreal.Vector2D(cx - 0.5 * TILE, cy - 4.0 * TILE)   # start block spans X [0,1) tiles, Y [3,5) tiles
    tiles = [unreal.IntPoint(c, r) for c, r in START]
    edge.author_starting_land(origin, TILE, COLUMNS, ROWS, tiles)
```

and the reload check reads `land.network`-free: log `edge`'s `num_walls()` is not reflected - instead verify by `MARKER` the actor exists and grep `OwnedLand: 2 tile(s), 4 wall run(s)` in the log.

- [ ] **Step 6: Build; rerun** `build_ground_material.py` then `build_diorama_prototype.py` headless (editor closed; `git checkout Content/Maps/M_Test.umap` after the first). Grep the log for `OwnedLand: authored - 2 tile(s) of 8x8` and `2 tile(s), 4 wall run(s)`.

- [ ] **Step 7: Tests** `-Filter Airside.Present.OwnedLand` and `-Filter Airside.Content.OwnedLandWired`. Expected: pass.

- [ ] **Step 8: Commit** - `"edge: walls and clip follow owned tiles"` (include the .uassets and M_Diorama).

### Task 4: Camera clamps to owned tiles and follows changes

**Files:**
- Modify: `Source/AirportMgr/BuildCameraRig.h/.cpp` (`FocusBounds` -> `FocusLand`)
- Modify: `Source/AirportMgr/BuildCameraComponent.h/.cpp`
- Test: `Source/AirportMgr/BuildCameraRigTest.cpp`, `Source/AirportMgr/BuildCameraComponentTest.cpp` (rewrite #529's two)

**Interfaces:**
- Consumes: `FLandGrid::ClampToOwned`, `URoadEditFacade::OnOwnedLandChanged`.
- Produces: `FLandGrid FBuildCameraRig::FocusLand`; `void UBuildCameraComponent::SetFocusLand(const FLandGrid&)` (replaces `SetFocusBounds`).

- [ ] **Step 1: Rewrite the two tests.** Rig test (`Airside.View.BuildCameraRig.FocusStaysOnTheLand`): L-shaped land (as Task 1), `Rig.FocusLand = L; Rig.Focus = (100000, 200000); Rig.Yaw = 90 (pan along +Y); Rig.Pan(0, 1, 1, 10)` -> expect `Rig.Focus == (100000, 240000)` (stopped at the notch, not the bounding box's 300000); an invalid `FocusLand` pans freely (`Focus.Y > 1e5` after a long pan from 0). Component test (`...FocusLandHoldsTheBuildView`): `SetFocusLand(L)` then `FocusOn((1e6, 1e6))` -> `ViewFocus() == L.ClampToOwned((1e6,1e6))`; `SetFocusLand(FLandGrid())` releases.

- [ ] **Step 2: Build; expect compile errors.**

- [ ] **Step 3: Implement.** Rig: replace `FBox2D FocusBounds` with `FLandGrid FocusLand;` (include `Model/LandGrid.h`) keeping the comment's reasoning, and `ClampFocus()` body: `Focus = FocusLand.ClampToOwned(Focus);` (an invalid grid returns the point). Component: `SetFocusLand(const FLandGrid& Land) { TargetView.FocusLand = Land; TargetView.ClampFocus(); }`; `FocusOn` unchanged (still calls `ClampFocus`). In `CreateBuildCamera(APlayerController&, double)` delete the `AAirsideOwnedLandActor::Find` block and its include. In `CreateBuildCamera(APlayerController& Owner, const ARoadNetworkActor& Target)`:

```cpp
void UBuildCameraComponent::CreateBuildCamera(APlayerController& Owner, const ARoadNetworkActor& Target)
{
	// THE OWNED LAND BOUNDS THE FOCUS, and follows it: a purchase moves the edge mid-session.
	if (Target.Network != nullptr)
	{
		TargetView.FocusLand = Target.Network->GetOwnedLand();
	}
	if (URoadEditFacade* Facade = Target.GetEditFacade())
	{
		Facade->OnOwnedLandChanged.AddUObject(this, &UBuildCameraComponent::SetFocusLand);
	}
	// The actor was only ever read for this one figure - see the header's overload.
	CreateBuildCamera(Owner, Target.SurfaceZ);
	UE_LOG(LogRoadBuild, Log, TEXT("Build camera: focus held on %d owned tile(s)."), TargetView.FocusLand.IsValid() ? TargetView.FocusLand.NumOwned() : 0);
}
```

`SetFocusLand` must be a plain member function (AddUObject needs a UObject method; it is one). Keep the `TargetView.Reset(ViewLimits)` in the double overload - `Reset` already clamps.

- [ ] **Step 4: Build; run** `-Filter Airside.View`. Expected: pass.

- [ ] **Step 5: Commit** - `"camera: focus clamps to owned tiles, follows purchases"`.

### Task 5: Grass follows owned tiles

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Build/GroundCoverMask.h/.cpp` (`SetLand(FBox2D)` -> `SetLand(const FLandGrid&)`)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/AirsideGroundCoverActor.cpp`, `.h` (bind to the facade)
- Test: `Plugins/Airside/Source/AirsideTests/Private/GroundCoverTest.cpp` (rewrite #529's two)

- [ ] **Step 1: Rewrite tests.** `OutsideLandIsCovered`: with the 1x2 start, `(30000,210000)` bare, `(90000,210000)` covered, `(60000,210000)` (the cut) covered, default grid covers nothing far out. `NoTuftPastTheOwnedLand`: author the 1x2 on `Fixture.Actor` at origin `(-30000,-240000)` (so tile (0,3) spans X [-30000,30000), Y [-60000,0) and (0,4) Y [0,60000)), bind grass, stream at `(0,0,300)`, assert every tuft `Grid.IsOwned(XY)`; then author an L adding (1,3) and assert tufts now appear with X > 30000 after `GcStreamFully` (proves the rebuild on change).

- [ ] **Step 2: Build; expect failures/compile errors.**

- [ ] **Step 3: Implement.** Mask: member `FLandGrid Land;`, `void SetLand(const FLandGrid& InLand) { Land = InLand; }`, `IsCovered` first line `if (!Land.IsOwned(Point)) { return true; }` (invalid owns all). Include `Model/LandGrid.h` (Build may include Model). Grass actor: `RebuildMask` replaces the `AAirsideOwnedLandActor::Find` block with `if (Road->Network != nullptr) { Mask.SetLand(Road->Network->GetOwnedLand()); }` (drop the include). `BindTo` adds `if (URoadEditFacade* Facade = Road->GetEditFacade()) { LandHandle = Facade->OnOwnedLandChanged.AddUObject(this, &AAirsideGroundCoverActor::OnOwnedLandChanged); }`; `Unbind` removes it; `void OnOwnedLandChanged(const FLandGrid&) { RebuildMask(); }` (a purchase is a single event - no settle delay). Header: `FDelegateHandle LandHandle;`, `TWeakObjectPtr<URoadEditFacade> BoundFacade;`. Extend the `GroundCover: mask rebuilt` log line with `, land %d tile(s)`.

- [ ] **Step 4: Build; run** `-Filter Airside.Build.GroundCoverMask`, `-Filter Airside.Present.GroundCover`. Expected: pass.

- [ ] **Step 5: Commit** - `"grass: keeps to owned tiles, rebuilds on a purchase"`.

### Task 6: PR A gate

- [ ] `./Tools/Check-Architecture.ps1` - PASS.
- [ ] Full suite - `0 failed, 0 crashed`.
- [ ] PIE `M_Diorama` (launch the slot5 editor with `-ModelContextProtocolPort=8002`, `AIRSIDE_MCP_PORT=8002`): grep the log for `OwnedLand: 2 tile(s), 4 wall run(s)` and `Build camera: focus held on 2 owned tile(s).`; `python Tools/Mcp.py shot` at 150 m over a corner. The user checks the 1x2 plinth.
- [ ] Push, `gh pr create` (template filled), merge after the user's look. Then rebase PR B's branch on main.

---

# PR B - building past owned land is refused (R7)

### Task 7: Area ownership in the model

**Files:**
- Modify: `Public/Model/LandGrid.h`, `Private/Model/LandGrid.cpp`
- Test: `LandGridTest.cpp` (append)

**Interfaces:**
- Produces: `bool FLandGrid::IsAreaOwned(TConstArrayView<FVector2D> Polygon) const`; `bool FLandGrid::IsStripOwned(FVector2D A, FVector2D B, double HalfWidth) const`; `static const FString OutsideText` = `TEXT("Outside your land")`.

- [ ] **Step 1: Failing tests** (append, `Airside.Model.LandGrid.*`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridAreas, "Airside.Model.LandGrid.Areas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridAreas::RunTest(const FString&)
{
	FLandGrid Grid = LgStart();
	Grid.SetTileOwned(FIntPoint(1, 3), true);   // the L: (0,3), (0,4), (1,3)
	TestTrue(TEXT("a strip inside one tile"), Grid.IsStripOwned(FVector2D(10000, 200000), FVector2D(50000, 200000), 1200));
	TestTrue(TEXT("EdgeTouchingIsOwned: a strip whose side lies ON the outer edge"),
		Grid.IsStripOwned(FVector2D(1200, 190000), FVector2D(1200, 290000), 1200));
	TestFalse(TEXT("ShoulderOverTheCutIsNot: centreline inside, shoulder 1 m over the west cut"),
		Grid.IsStripOwned(FVector2D(1100, 190000), FVector2D(1100, 290000), 1200));
	// NotchIsNotOwned: every vertex on owned tiles, but the strip crosses unowned (1,4).
	TestFalse(TEXT("a diagonal across the L's notch"), Grid.IsStripOwned(FVector2D(50000, 290000), FVector2D(110000, 230000), 500));
	TestTrue(TEXT("across the shared edge between two owned tiles"), Grid.IsStripOwned(FVector2D(30000, 200000), FVector2D(90000, 200000), 2000));
	const TArray<FVector2D> OffGrid = { FVector2D(-5000, 200000), FVector2D(5000, 200000), FVector2D(5000, 210000) };
	TestFalse(TEXT("off the grid"), Grid.IsAreaOwned(OffGrid));
	TestTrue(TEXT("an invalid grid owns any area"), FLandGrid().IsAreaOwned(OffGrid));
	return true;
}
```

- [ ] **Step 2: Build; expect compile errors.**

- [ ] **Step 3: Implement** (`#include "Solve/RoadGeom.h"` in the .cpp):

```cpp
namespace
{
	/** Interior overlap of a polygon (any winding, may be concave) and a box, the box inset 1 uu so touching
	 *  along an edge or corner is not overlap - a road laid exactly to the cut is on owned land. */
	bool PolygonEntersBox(TConstArrayView<FVector2D> Polygon, const FBox2D& Box)
	{
		const FBox2D In(Box.Min + FVector2D(1.0, 1.0), Box.Max - FVector2D(1.0, 1.0));
		const FVector2D Corners[4] = { In.Min, FVector2D(In.Max.X, In.Min.Y), In.Max, FVector2D(In.Min.X, In.Max.Y) };
		for (const FVector2D& P : Polygon)
		{
			if (In.IsInside(P)) { return true; }
		}
		for (const FVector2D& C : Corners)
		{
			if (RoadGeom::PointInPolygon(Polygon, C)) { return true; }
		}
		for (int32 I = 0; I < Polygon.Num(); ++I)
		{
			const FVector2D& P0 = Polygon[I];
			const FVector2D& P1 = Polygon[(I + 1) % Polygon.Num()];
			for (int32 K = 0; K < 4; ++K)
			{
				if (RoadGeom::SegmentsCross(P0, P1, Corners[K], Corners[(K + 1) % 4])) { return true; }
			}
		}
		return false;
	}
}

bool FLandGrid::IsAreaOwned(TConstArrayView<FVector2D> Polygon) const
{
	if (!IsValid())
	{
		return true;
	}
	FBox2D Bounds(ForceInit);
	for (const FVector2D& P : Polygon) { Bounds += P; }
	const FIntPoint Lo = TileAt(Bounds.Min);
	const FIntPoint Hi = TileAt(Bounds.Max);
	for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
	{
		for (int32 X = Lo.X; X <= Hi.X; ++X)
		{
			const FIntPoint Tile(X, Y);
			// Off-grid tiles are never owned, so a polygon reaching past the grid fails here too.
			if (!IsTileOwned(Tile) && PolygonEntersBox(Polygon, TileBox(Tile)))
			{
				return false;
			}
		}
	}
	return true;
}

bool FLandGrid::IsStripOwned(FVector2D A, FVector2D B, double HalfWidth) const
{
	const FVector2D Dir = (B - A).GetSafeNormal();
	const FVector2D Side = FVector2D(-Dir.Y, Dir.X) * HalfWidth;
	const FVector2D Quad[4] = { A + Side, B + Side, B - Side, A - Side };
	return IsAreaOwned(Quad);
}
```

Check `RoadGeom::SegmentsCross`'s full parameter list in `Public/Solve/RoadGeom.h:136` before calling - if it takes four `FVector2D` as above, use it; otherwise adapt the call (do not reimplement it).

- [ ] **Step 4: Build; run** `-Filter Airside.Model.LandGrid`. Expected: pass.
- [ ] **Step 5: Commit** - `"model: is an area on owned land - footprint, not centreline"`.

### Task 8: Every build asks the land

**Files:**
- Modify: `Public/Tool/RoadEditTarget.h` (add `WhyRunwayRefused`, `WhyApronRefused`, both pure virtual beside `WhySegmentRefused`)
- Modify: `Public/Present/RoadEditFacade.h`, `Private/Present/RoadEditFacade.cpp`, `Private/Present/RoadEditFacadeSurfaces.cpp`
- Modify: every other `IRoadEditTarget` implementer (find with `grep -rn "public IRoadEditTarget" Plugins Source`) - test doubles return `FString()`
- Test: `Plugins/Airside/Source/AirsideTests/Private/OwnedLandRefusalTest.cpp` (new)

**Interfaces:**
- Consumes: `FLandGrid::IsStripOwned/IsAreaOwned/IsOwned`, `FLandGrid::OutsideText`.
- Produces: `virtual FString WhyRunwayRefused(FVector2D From, FVector2D To, const URoadProfile* Profile) const = 0;` `virtual FString WhyApronRefused(TArrayView<const FVector2D> Outline) const = 0;`

- [ ] **Step 1: Failing test** - `OwnedLandRefusalTest.cpp`, one test `Airside.Present.OwnedLand.EveryBuildAsksTheLand`: author the 1x2 (origin `(-30000,-240000)`: column 0 spans X [-30000,30000), rows 3-4 span Y [-60000,60000)), then for each: a taxiway (0,-50000)->(0,50000) inside -> `ConnectNodes` true; a taxiway (0,0)->(90000,0) (into (1,4)) -> `WhySegmentRefused` == `Outside your land` and `ConnectNodes` false with the segment count unchanged; `WhyRunwayRefused((0,-50000),(0,150000), RunwayProfile)` refused (past Y 60000), `PlaceRunway` refuses; `WhyApronRefused` / `AddApron` with a square straddling X=30000 refused and one inside accepted; `WhyPlotRefused` / `WhyStandSiteRefused` with a rectangle past the edge says `Outside your land`; `PlaceNode((90000,0))` returns `INDEX_NONE`. Use `Fixture.Actor->ResolveProfileFor(ERoadKind::Runway, 0)` for the runway profile (check the exact accessor name in `RoadNetworkActor.h` first).

- [ ] **Step 2: Build; expect compile errors** (the two new Why functions).

- [ ] **Step 3: Implement.** In the facade add a private helper:

```cpp
/** "Outside your land" when the footprint leaves owned tiles, else empty - every Why* below asks this FIRST:
 *  a site in the void has nothing else worth saying about it. */
FString URoadEditFacade::LandRefusal(TConstArrayView<FVector2D> Footprint) const
{
	const URoadNetwork* Network = Actor().Network;
	return Network != nullptr && !Network->GetOwnedLand().IsAreaOwned(Footprint) ? FLandGrid::OutsideText : FString();
}
```

Wire it:
- `WhySegmentRefused` (RoadEditFacade.cpp:685): after `Shape.HalfWidth` is set: `if (Network->GetOwnedLand().IsValid() && !Network->GetOwnedLand().IsStripOwned(Shape.A, Shape.B, Shape.HalfWidth)) { return FLandGrid::OutsideText; }`. `ConnectNodes` already calls it at commit (line ~794).
- `WhyUpgradeSiteRefused` (RoadEditFacade.cpp:1033): the segment's endpoints and the new profile's half-width, same check.
- `WhyStandSiteRefused` (Surfaces.cpp:694) and `WhyPlotRefused` (Surfaces.cpp:362): first line `const FString Land = LandRefusal(Outline); if (!Land.IsEmpty()) { return Land; }`. Confirm `PlaceStandInPlot` / `PlaceEntityInPlot` call these at commit; if either does not, add the call with a `UE_LOG(LogRoadMesh, Log, TEXT("... refused: %s"), *Why)`.
- New `WhyRunwayRefused(From, To, Profile)`: `Profile == nullptr ? FString() : (land valid && !IsStripOwned(From, To, Profile->GetMaxHalfWidth()) ? OutsideText : FString())`. Call it at the top of `PlaceRunway` (RoadEditFacade.cpp:851) after the null checks: refuse with `UE_LOG(LogRoadMesh, Log, TEXT("PlaceRunway refused: %s"), *Why); return false;`.
- New `WhyApronRefused(Outline)`: `return LandRefusal(Outline);`. Call at the top of `AddApron` (Surfaces.cpp:157), returning `INDEX_NONE` with a log line.
- `PlaceNode` (RoadEditFacade.cpp:532): `if (!Network->GetOwnedLand().IsOwned(Where)) { UE_LOG(...); return INDEX_NONE; }` - a chain cannot start in the void. `PlaceEntity` (Surfaces.cpp:270): same on `Where`.

Commit refusals are LOGGED, not broadcast on `OnRefused`: every one is shown red before the click by the Why* above, which is `EBuildRefusal`'s own rule ("every other refusal is explained before the click"). Spec 5's `EBuildRefusal::OutsideOwnedLand` is therefore not added - note this deviation in the PR.

- [ ] **Step 4: Build; run** `-Filter Airside.Present.OwnedLand` and `-Filter Airside.Tool`. Expected: pass (test doubles updated).
- [ ] **Step 5: Commit** - `"build: every placement refused outside owned land"`.

### Task 9: Runway and apron previews go red before the click

**Files:**
- Modify: `Private/Tool/RunwayTool.cpp` (~line 317, the `Style` choice)
- Modify: `Public/Tool/OutlineDrawTool.h` (`IOutlineTarget`), `Private/Tool/OutlineDrawTool.cpp` (~line 183), `Public/Tool/ApronDrawTool.h`, `Private/Tool/ApronDrawTool.cpp`
- Test: `OwnedLandRefusalTest.cpp` (append `Airside.Tool.OwnedLand.PreviewsSayOutside`)

- [ ] **Step 1: Failing test** - drive `FRunwayTool` and `FApronDrawTool` with an `FToolContext` whose `Target` is the fixture's facade and a recording `IToolPreviewSink` (copy the recording sink used in an existing tool test - `grep -rln "IToolPreviewSink" Plugins/Airside/Source/AirsideTests/Private` and reuse its class): a runway from inside to outside the land records a `Label` with text `Outside your land` in style `Refused`; an apron with corners inside and the cursor outside records the same.

- [ ] **Step 2: Run; expect FAIL** (no such label).

- [ ] **Step 3: Implement.** Runway tool, after `bAffordable`:

```cpp
	const FString LandWhy = Context.Target != nullptr ? Context.Target->WhyRunwayRefused(Threshold, Far, Profile) : FString();
	const EPreviewStyle Style = bLongEnough && bAffordable && LandWhy.IsEmpty() ? EPreviewStyle::Pending : EPreviewStyle::Refused;
```

and after the `!bLongEnough` early return: `if (!LandWhy.IsEmpty()) { Sink.Label(Far, LandWhy, EPreviewStyle::Refused); return; }`. Outline target: add `virtual FString WhyRefused(const FToolContext& Context, TArrayView<const FVector2D> Corners) const { return FString(); }` to `IOutlineTarget`; in the drawing state's `BuildPreview`, build `TArray<FVector2D> Candidate = Corners; Candidate.Add(Ahead);` when not closing, ask `Target.WhyRefused(Context, Candidate)`, and if non-empty treat it like `bCrosses` (Refused style, label it at the cursor). `FApronOutlineTarget::WhyRefused` returns `Context.Target != nullptr ? Context.Target->WhyApronRefused(Corners) : FString()`.

- [ ] **Step 4: Run** `-Filter Airside.Tool`. Expected: pass.
- [ ] **Step 5: PR B gate** - Check-Architecture, full suite, PIE on `M_Diorama`: draw a taxiway over the cut, see red "Outside your land". Commit `"tools: runway and apron ghosts say Outside your land"`, PR, merge after the user's look.

---

# PR C - buying land (R5, R6, R10)

### Task 10: The price and the facade's buy

**Files:**
- Modify: `Public/Content/AirsideContent.h` (two fields beside `PlinthWallMaterial`), `Public/Content/AirsideSettings.h/.cpp` (`ResolveLandPrice`)
- Modify: `Public/Tool/RoadEditTarget.h` (`QuoteLandTile`, `WhyLandTileRefused`, `BuyLandTile` - defaults: free quote / "No land here" / false)
- Modify: `Public/Present/RoadEditFacade.h/.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/LandPurchaseTest.cpp` (new)

**Interfaces:**
- Produces:
  ```cpp
  struct FLandPrice { double Base = 150000.0; double Growth = 0.5; double For(int32 Owned) const { return Base * (1.0 + Growth * Owned); } };
  static FLandPrice UAirsideSettings::ResolveLandPrice();
  virtual FBuildQuote QuoteLandTile(FIntPoint Tile) const;      // What = "Land, 600 m tile"
  virtual FString WhyLandTileRefused(FIntPoint Tile) const;      // "" = buyable and affordable
  virtual bool BuyLandTile(FIntPoint Tile);
  DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLandBought, FIntPoint /*Tile*/, const FBuildQuote& /*Quote*/);
  FOnLandBought URoadEditFacade::OnLandBought;
  ```

- [ ] **Step 1: Failing test** `Airside.Present.OwnedLand.BuyRefusals` in `LandPurchaseTest.cpp`, with a test purse (find the existing fake `IBuildPurse` used by `Airside.Present.BuildPurseRefusalIsAnnounced` and reuse it; give it a balance):
  - quote for (1,3) with 2 owned = 150000 x (1 + 0.5 x 2) = 300000 (`QuoteLandTile(...).BaseAmount()`).
  - `WhyLandTileRefused`: owned (0,3) -> `Already yours`; diagonal (1,2) -> `Not next to your land`; (-1,3) -> `Off the map`; with balance 100000, (1,3) -> starts with `Can't afford`.
  - balance 1e6: `BuyLandTile((1,3))` true; owned; purse charged 300000; `OnLandBought` fired once with that tile; `OnOwnedLandChanged` fired once.
  - buying the same tile again false, no second charge.
  - with an invalid grid: `BuyLandTile` false, `No land here`.

- [ ] **Step 2: Build; expect compile errors.**

- [ ] **Step 3: Implement.** Content fields:

```cpp
	/** A land tile's price with no tiles owned. Land purchase spec R5; first guess 2026-10-02, tune in play. */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand", meta = (ClampMin = "0"))
	double LandTileBase = 150000.0;

	/** Each tile already owned adds this fraction of Base to the next: Base x (1 + Growth x Owned). */
	UPROPERTY(EditAnywhere, Category = "Airside|OwnedLand", meta = (ClampMin = "0"))
	double LandTileGrowth = 0.5;
```

`FLandPrice` in `Public/Content/OwnedLandKit.h`; `ResolveLandPrice` reads `GetContent()` (defaults when null). Facade:

```cpp
FBuildQuote URoadEditFacade::QuoteLandTile(FIntPoint Tile) const
{
	const URoadNetwork* Network = Actor().Network;
	FBuildQuote Quote;
	if (Network == nullptr || !Network->GetOwnedLand().IsValid())
	{
		return Quote;
	}
	FBuildLine Line;
	Line.Unit = EBuildUnit::Each;
	Line.Quantity = 1.0;
	Line.RatePerUnit = UAirsideSettings::ResolveLandPrice().For(Network->GetOwnedLand().NumOwned());
	Quote.Lines.Add(Line);
	Quote.What = FText::Format(LOCTEXT("LandTile", "Land, {0} m tile"), FText::AsNumber(FMath::RoundToInt(Network->GetOwnedLand().TileSize / 100.0)));
	return Quote;
}

FString URoadEditFacade::WhyLandTileRefused(FIntPoint Tile) const
{
	const URoadNetwork* Network = Actor().Network;
	if (Network == nullptr || !Network->GetOwnedLand().IsValid()) { return TEXT("No land here"); }
	const FLandGrid& Land = Network->GetOwnedLand();
	if (!Land.IsOnGrid(Tile)) { return TEXT("Off the map"); }
	if (Land.IsTileOwned(Tile)) { return TEXT("Already yours"); }
	if (!Land.IsBuyable(Tile)) { return TEXT("Not next to your land"); }
	const FBuildQuote Quote = QuoteLandTile(Tile);
	if (Purse != nullptr && !Purse->CanAfford(Quote)) // preview: the commit announces through AffordOrRefuse
	{
		return FString::Printf(TEXT("Can't afford %s"), *Purse->Describe(Quote).ToString());
	}
	return FString();
}

bool URoadEditFacade::BuyLandTile(FIntPoint Tile)
{
	URoadNetwork* Network = Actor().Network;
	const FString Why = WhyLandTileRefused(Tile);
	if (Network == nullptr || (!Why.IsEmpty() && !Why.StartsWith(TEXT("Can't afford"))))
	{
		UE_LOG(LogRoadMesh, Log, TEXT("BuyLandTile (%d,%d) refused: %s"), Tile.X, Tile.Y, *Why);
		return false;
	}
	const FBuildQuote Quote = QuoteLandTile(Tile);
	if (!AffordOrRefuse(Quote))   // announces "cannot afford" on OnRefused, the one silent refusal (rule 32)
	{
		return false;
	}
	FLandGrid Land = Network->GetOwnedLand();
	Land.SetTileOwned(Tile, true);
	Network->SetOwnedLand(Land);
	// NOT ON THE UNDO STACK (spec 3.1): land is not undone, so its charge has no step to be reversed by.
	if (Purse != nullptr)
	{
		Purse->Charge(Quote);
	}
	UE_LOG(LogRoadMesh, Log, TEXT("OwnedLand: bought tile (%d,%d) for %.0f - %d owned"), Tile.X, Tile.Y, Quote.BaseAmount(), Land.NumOwned());
	OnOwnedLandChanged.Broadcast(Land);
	OnLandBought.Broadcast(Tile, Quote);
	return true;
}
```

(Check the facade's purse member name - `grep -n "Purse" Public/Present/RoadEditFacade.h` - and the `LOCTEXT_NAMESPACE` in use.) Rule 32 counts `CanAfford(` in RoadEditFacade*.cpp: the preview line carries `// preview` as rule 32 requires.

- [ ] **Step 4: Build twice (new test file); run** `-Filter Airside.Present.OwnedLand`. Expected: pass.
- [ ] **Step 5: Commit** - `"land: quote, refuse and buy a tile through the facade"`.

### Task 11: The purchase on the ops bus, and its toast

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEventBus.h` (event + `FOpsEvent` variant list), `Private/Model/OpsEventBus.cpp` (`Describe`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEvents.h` (`EOpsPurchaseKind::LandBought`, appended)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (bridge list ~line 935; Presentation subscriber ~line 700), `Public/Present/OpsRuntime.h`
- Modify: `Source/AirportMgr/ToastStackWidget.cpp` (~line 190), `Source/AirportMgr/ToastStackWidgetTest.cpp` (~line 450)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/` - add `AirportOps.Present.LandBoughtIsPublished` beside the facility tests (find them with `grep -rln "FacilityUpgraded" Plugins/AirportOps/Source/AirportOpsTests`)

**Interfaces:**
- Produces: `struct AIRPORTOPS_API FLandPurchasedEvent { FIntPoint Tile; double Amount = 0.0; static const TCHAR* EventName() { return TEXT("LandPurchased"); } FString Describe() const; };`

- [ ] **Step 1: Failing tests.** Ops: attach a runtime to a test airport with land authored and a funded ledger, call `Facade->BuyLandTile((1,3))`, drain the bus, assert exactly one `FLandPurchasedEvent` with `Amount == 300000` reached a test subscriber and the ledger fell by 300000. Toast: add `{ EOpsPurchaseKind::LandBought, 300000.0, TEXT("Bought land — $300,000"), ENotificationSeverity::Info }` to `PurchaseCases` (the `Name` the test feeds must be "land" - match how that table supplies `Name`).

- [ ] **Step 2: Build; expect failures.**

- [ ] **Step 3: Implement.** Event appended to the variant list (the wiring test `AirportOps.Model.Bus.EveryEventDescribesItself` walks it). `Describe`: `FString::Printf(TEXT("tile (%d,%d) for %.0f"), Tile.X, Tile.Y, Amount)`. Bridge (the `Out.Add` list):

```cpp
	// A LAND PURCHASE, bridged (land purchase spec R10): the facade bought and charged; ops announces it.
	Out.Add({ TEXT("LandBought"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			URoadEditFacade* Facade = Actor.GetEditFacade();
			return Facade != nullptr ? Facade->OnLandBought.AddUObject(&Runtime, &UOpsRuntime::OnLandBought) : FDelegateHandle();
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (URoadEditFacade* Facade = Actor.GetEditFacade()) { Facade->OnLandBought.Remove(Handle); } } });
```

```cpp
void UOpsRuntime::OnLandBought(FIntPoint Tile, const FBuildQuote& Quote)
{
	// PRICED BY THE PURSE (OnBuildRefused's reason): the amount the ledger actually took.
	Bus.Publish(FLandPurchasedEvent{ Tile, Pricing->PriceOfBuild(Quote) });
}
```

(Check `UPricing`'s price function name - `grep -n "PriceOfBuild" Plugins/AirportOps/Source/AirportOps/Public/Model/Pricing.h`.) Presentation subscriber beside the module one:

```cpp
	Bus.Subscribe<FLandPurchasedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FLandPurchasedEvent& E)
	{
		FOpsPurchase Purchase;
		Purchase.Kind = EOpsPurchaseKind::LandBought;
		Purchase.Name = LOCTEXT("Land", "land");
		Purchase.Amount = E.Amount;
		Purchase.Money = Pricing->Format(E.Amount);
		Events->NotifyPurchase(Purchase);
	});
```

Toast: add `case EOpsPurchaseKind::LandBought:` to the `VehicleBought/ModuleBought` group ("Bought {0} — {1}"). Rule 31 (bus-wired-once) may need the new subscription listed - run Check-Architecture and follow its message.

- [ ] **Step 4: Build; run** `-Filter AirportOps.Present`, `-Filter AirportOps.Model.Bus`, `-Filter AirportMgr.UI`. Expected: pass.
- [ ] **Step 5: Commit** - `"ops: land purchase on the bus; toast"`.

### Task 12: The Buy land tool

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/LandBuyTool.h`, `Private/Tool/LandBuyTool.cpp`
- Modify: `Private/Tool/BuildSession.cpp` (registry, after FuelDepot), `Public/Tool/BuildSession.h:136` (the keys comment)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ToolRegistryRulings.h` (row)
- Modify: `Tools/Python/fetch_ui_icons.py` (`"tool.buy land": ("delapouite", "house-keys")` - if the fetch reports the name missing, pick another delapouite icon and rerun)
- Test: `LandPurchaseTest.cpp` (append `Airside.Tool.BuyLand.GhostsAndClick`)

**Interfaces:**
- Consumes: `IRoadEditTarget::QuoteLandTile/WhyLandTileRefused/BuyLandTile`, `IBuildPurse::Describe`, `FLandGrid`.
- Produces: `class FLandBuyTool : public IBuildTool` (`GetDisplayName` "Buy land").

- [ ] **Step 1: Failing test** - with the 1x2 authored and a funded purse, `FLandBuyTool` `BuildPreview` into a recording sink records exactly 4 price labels (the four buyable tiles: (1,3),(1,4),(0,2),(0,5)) at tile centres, style `Pending`; with the cursor over (1,3) that tile's outline is `Hover`; `OnClick` with the cursor in (1,3) buys it (owned afterwards); a click in (5,5) buys nothing.

- [ ] **Step 2: Build; expect compile errors.**

- [ ] **Step 3: Implement.**

```cpp
// LandBuyTool.h
#pragma once
#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * Buy land (land purchase spec R6): every buyable tile past the edge as a ghost with its price; a click buys
 * the tile under the cursor. Unowned tiles that cannot be bought are not drawn - the void stays void.
 * ITS OWN MODE, not a gesture on the plain view, by the deliberate-mode ruling: buying spends real money.
 */
class AIRSIDE_API FLandBuyTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;
	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override {}
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const override;
	virtual bool IsIdle() const override { return true; }
};
```

```cpp
// LandBuyTool.cpp
#include "Tool/LandBuyTool.h"
#include "Model/BuildPurse.h"
#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/ToolReadout.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	const FLandGrid* LandOf(const FToolContext& Context)
	{
		const URoadNetwork* Network = Context.Network();
		return Network != nullptr && Network->GetOwnedLand().IsValid() ? &Network->GetOwnedLand() : nullptr;
	}
}

FText FLandBuyTool::GetDisplayName() const
{
	return LOCTEXT("BuyLandTool", "Buy land");
}

void FLandBuyTool::OnClick(const FToolContext& Context)
{
	const FLandGrid* Land = LandOf(Context);
	if (Land != nullptr && Context.Target != nullptr)
	{
		Context.Target->BuyLandTile(Land->TileAt(Context.Cursor));
	}
}

void FLandBuyTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const FLandGrid* Land = LandOf(Context);
	if (Land == nullptr || Context.Target == nullptr)
	{
		return;
	}
	const IBuildPurse* Purse = Context.Target->GetPurse();
	const FIntPoint Hovered = Land->TileAt(Context.Cursor);
	for (int32 Y = 0; Y < Land->Rows; ++Y)
	{
		for (int32 X = 0; X < Land->Columns; ++X)
		{
			const FIntPoint Tile(X, Y);
			if (!Land->IsBuyable(Tile))
			{
				continue;
			}
			const FString Why = Context.Target->WhyLandTileRefused(Tile);
			const EPreviewStyle Style = !Why.IsEmpty() ? EPreviewStyle::Refused
				: Tile == Hovered ? EPreviewStyle::Hover : EPreviewStyle::Pending;
			const FBox2D Box = Land->TileBox(Tile);
			const FVector2D Corners[4] = { Box.Min, FVector2D(Box.Max.X, Box.Min.Y), Box.Max, FVector2D(Box.Min.X, Box.Max.Y) };
			Sink.Polygon(Corners, Style);
			const FBuildQuote Quote = Context.Target->QuoteLandTile(Tile);
			const FString Price = Purse != nullptr ? Purse->Describe(Quote).ToString() : FString();
			Sink.Label(Box.GetCenter(), Why.IsEmpty() ? Price : Why, Style == EPreviewStyle::Hover ? EPreviewStyle::Pending : Style);
		}
	}
}

void FLandBuyTool::BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const
{
	const FLandGrid* Land = LandOf(Context);
	if (Land == nullptr)
	{
		Sink.Warning(TEXT("This map has no land to buy"));
		Sink.Committable(false);
		return;
	}
	Sink.Fact(TEXT("Owned"), FString::Printf(TEXT("%d of %d tiles"), Land->NumOwned(), Land->Columns * Land->Rows));
	const FIntPoint Tile = Land->TileAt(Context.Cursor);
	const FString Why = Context.Target != nullptr ? Context.Target->WhyLandTileRefused(Tile) : FString();
	Sink.Committable(Why.IsEmpty());
	if (!Why.IsEmpty())
	{
		Sink.Warning(Why);
	}
}

#undef LOCTEXT_NAMESPACE
```

(Confirm `FToolContext::Network()` exists - `OutlineDrawTool`/`ApronDrawTool.cpp` call `Context.Network()`; and the readout sink header name.) Registry entry after FuelDepot:

```cpp
		// BUY LAND (land purchase spec R6). T, because every digit is taken and "Land" is aircraft.land's
		// label (BuildActions.cpp:355) - a second "Land" on the bar would read as the same action.
		{ EKeys::T,     TEXT("BuyLand"), LOCTEXT("BuyLand", "Buy land"),
			LOCTEXT("BuyLandTooltip", "Buy a 600 m tile of land next to yours: click a priced tile past the edge."),
			[] { return MakeUnique<FLandBuyTool>(); },
			EEditHandleKind::None },
```

Rulings row: `{ TEXT("BuyLand"), E::None, false, false, S::PlacesNoPavement },` with a comment "Buys ground, lays no pavement."

- [ ] **Step 4: Build twice; run** `-Filter Airside.Tool`, `-Filter Airside.Editor.ToolCommandsMatchRegistry`, `-Filter AirportMgr.Actions`. Expected: pass (bar rows and PIE key come from the registry). Run `python Tools/Python/fetch_ui_icons.py` per its header (editor closed) and confirm `tool.buy land` resolves.
- [ ] **Step 5: Commit** - `"tool: Buy land (T) - priced ghost tiles, click buys"`.

### Task 13: PR C gate

- [ ] Check-Architecture PASS; full suite `0 failed, 0 crashed`.
- [ ] PIE `M_Diorama`: press `T`; four priced ghosts past the 1x2; click one; log shows `OwnedLand: bought tile (...)` and `[Bus] LandPurchased tile (...)`; toast "Bought land — ..."; walls re-laid, camera can now go there, grass grows there; Ctrl+Z after a later road keeps the tile.
- [ ] Update `docs/AirportManagerGDD.md`'s land paragraph to point at the spec; update memory `diorama-edge-prototype.md`.
- [ ] PR, merge after the user's play-test.
