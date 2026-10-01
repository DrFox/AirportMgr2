#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * WHAT EACH REGISTERED TOOL IS RULED TO DO, one row per ToolRegistry() entry and one column per
 * ruling - the ONE table the registry-agreement tests read (#462 M6; there were five, one per
 * column, and a new tool had to be added to every one of them).
 *
 * Airside.Tool.RegistryRulingsAreDeclaredForEveryEntry checks the registry against it by NAME,
 * both ways (CLAUDE.md: where UE forces two lists to agree the consumer checks IDENTITY, not
 * counts - a count passes on a table where two entries swapped answers), and
 * Airside.Tool.EveryPlacementToolHonoursTheStrip walks its Strip column.
 *
 * A NEW TOOL MUST RULE ON EVERY COLUMN, even where the answer is None / false / PlacesNoPavement.
 * Failing rather than defaulting is the point: the default is silent, and a tool that should have
 * been editable would simply never light a handle.
 */
namespace ToolRegistryRulings
{
	/** How a tool answers the clearance-strip rule (strip stage 3). */
	enum class EStripCoverage : uint8 { Judged, ExemptApron, ExemptRunway, PlacesNoPavement };

	struct FRow
	{
		const TCHAR* Id;

		/** FToolRegistration::EditHandles. None greys the Edit toggle out. */
		EEditHandleKind EditHandles;

		/**
		 * FToolRegistration::bShowsRoadNodes. THE FALSE ROWS ARE THE INTERESTING ONES. Holding-point
		 * and guideline LOOK like they need road nodes - both talk about clicking nodes - but they
		 * pick GUIDELINE nodes, which GuidelineOverlay draws under its own G toggle. The runway tool
		 * reads no snap at all. Getting any of those wrong puts the scaffolding back on screen for a
		 * tool that never wanted it.
		 */
		bool bShowsRoadNodes;

		/**
		 * FToolRegistration::bShowsPlotGhosts. Only the depot tool wants ghost bays: they are a
		 * plot's unbought capacity, and until a building edit mode can buy one, placing a depot is
		 * the only time that capacity is the question.
		 */
		bool bShowsPlotGhosts;

		/** How the tool answers the clearance-strip rule - see EveryPlacementToolHonoursTheStrip. */
		EStripCoverage Strip;
	};

	inline TConstArrayView<FRow> Rows()
	{
		using E = EEditHandleKind;
		using S = EStripCoverage;
		// COLUMNS: Id, EditHandles, bShowsRoadNodes, bShowsPlotGhosts, Strip.
		static const FRow Table[] = {
			// Normal operations: nothing to edit and no rings, and cyan ghost boxes would read as the building.
			{ TEXT("Select"),          E::None,             false, false, S::PlacesNoPavement },
			// Snaps to nodes to chain and close junctions, so the ring is the target. Strip stage 3, Task 4.
			{ TEXT("Taxiway"),         E::AirsideNode,      true,  false, S::Judged },
			// Aircraft pavement: plan ruling 1 exempts it from the strip.
			{ TEXT("Apron"),           E::ApronCorner,      false, false, S::ExemptApron },
			// A stand has no bays. Judged since #390.
			{ TEXT("Stand"),           E::None,             false, false, S::Judged },
			// Guideline nodes, not road nodes; routing links on existing pavement, so no pavement of its own.
			{ TEXT("Guideline"),       E::None,             false, false, S::PlacesNoPavement },
			// Reads no snap, so no rings; its own strip rules - plan ruling 2.
			{ TEXT("Runway"),          E::RunwayThreshold,  false, false, S::ExemptRunway },
			// Guideline nodes, not road nodes; a mark on an existing node.
			{ TEXT("HoldingPosition"), E::None,             false, false, S::PlacesNoPavement },
			// The same FRoadDrawTool as Taxiway. Judged, Task 4.
			{ TEXT("Road"),            E::ServiceRoadNode,  true,  false, S::Judged },
			// The one tool whose plot has capacity to show. Judged, Task 5.
			{ TEXT("FuelDepot"),       E::None,             false, true,  S::Judged },
		};
		return Table;
	}
}
