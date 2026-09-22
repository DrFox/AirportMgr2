#include "Present/PlotPresenter.h"

#include "AirsideLog.h"
#include "Build/DepotKit.h"
#include "Build/PlotLayoutStrategy.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** The engine cube is 100 uu on a side with a CENTRED pivot. */
	constexpr double CubeUu = 100.0;

	/**
	 * Module heights, uu, so the three read apart at a glance while they are still boxes.
	 *
	 * A shed is tall enough to swallow a 6.2 m truck, a tank sits lower and wider, a pump is
	 * knee-high plant. Grey-box figures chosen for LEGIBILITY rather than measured off the
	 * concept sheet - measuring them would be authoring the model in the wrong file, and the
	 * real heights arrive with the meshes.
	 */
	constexpr double ShedHeightUu = 400.0;
	constexpr double TankHeightUu = 250.0;
	constexpr double PumpHeightUu = 150.0;

	/** Fence panel: 2.5 m of run, 2 m tall, a hand's breadth thick. */
	constexpr double FenceBayUu = 250.0;
	constexpr double FenceHeightUu = 200.0;
	constexpr double FenceThicknessUu = 10.0;

	double HeightFor(EDepotModule Module)
	{
		switch (Module)
		{
		case EDepotModule::Shed: return ShedHeightUu;
		case EDepotModule::Tank: return TankHeightUu;
		case EDepotModule::Pump: return PumpHeightUu;
		// THE SENTINEL, NOT A MODULE - named explicitly so a genuinely new module still falls
		// through with no case here and keeps warning.
		case EDepotModule::Count: break;
		}
		return ShedHeightUu;
	}

	/**
	 * A box resting ON the ground at Where, facing Heading.
	 *
	 * THE HALF-HEIGHT IN Z IS NOT OPTIONAL. The engine cube's pivot is its centre, so a box
	 * placed at Z=0 sinks to its waist in the apron - which reads as a modelling error in
	 * the mesh that is not there yet.
	 */
	FTransform BoxAt(const FVector2D& Where, double Heading,
		double LengthUu, double WidthUu, double HeightUu)
	{
		const FVector Scale(LengthUu / CubeUu, WidthUu / CubeUu, HeightUu / CubeUu);
		const FVector Location(Where.X, Where.Y, HeightUu * 0.5);
		const FRotator Rotation(0.0, FMath::RadiansToDegrees(Heading), 0.0);
		return FTransform(Rotation, Location, Scale);
	}

	/**
	 * Which edge of the plot is its frontage, recovered from the entity alone.
	 *
	 * EXACT, NOT A GUESS, and not a second search either. URoadEditFacade::PlaceEntityInPlot
	 * puts the pose at the MIDPOINT of the frontage edge, so the edge whose midpoint equals
	 * Position is that edge by construction - this reads back a value rather than deriving a
	 * new opinion.
	 *
	 * ASKING FAnchorLink AGAIN WAS REJECTED. It would search the live graph, so a road laid
	 * or deleted after the depot was built could move the frontage, and every shed in the
	 * yard would jump to a new edge without the player touching the depot. Where the thing
	 * faces was decided when it was placed, and it stays decided.
	 */
	bool RecoverFrontage(const FEntityInstance& Entity, FVector2D& OutA, FVector2D& OutB)
	{
		double BestDistance = TNumericLimits<double>::Max();
		int32 BestEdge = INDEX_NONE;

		for (int32 I = 0; I < Entity.Outline.Num(); ++I)
		{
			const FVector2D& A = Entity.Outline[I];
			const FVector2D& B = Entity.Outline[(I + 1) % Entity.Outline.Num()];
			const double Distance = FVector2D::Distance((A + B) * 0.5, Entity.Position);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestEdge = I;
			}
		}

		if (BestEdge == INDEX_NONE)
		{
			return false;
		}

		OutA = Entity.Outline[BestEdge];
		OutB = Entity.Outline[(BestEdge + 1) % Entity.Outline.Num()];
		return true;
	}

}

void UPlotPresenter::Initialise(UInstancedStaticMeshComponent* InBoxes,
	UInstancedStaticMeshComponent* InGhosts)
{
	Boxes = InBoxes;
	GhostBoxes = InGhosts;
}

int32 UPlotPresenter::GetInstanceCount() const
{
	// FROM Placed, NOT THE COMPONENT, so this and GetInstanceTransformForTest agree by
	// construction rather than by both happening to read the same place today.
	return Placed.Num();
}

bool UPlotPresenter::GetInstanceTransformForTest(int32 Index, FTransform& OutTransform) const
{
	if (!Placed.IsValidIndex(Index))
	{
		return false;
	}
	OutTransform = Placed[Index];
	return true;
}

void UPlotPresenter::Clear()
{
	if (Boxes != nullptr)
	{
		Boxes->ClearInstances();
	}
	if (GhostBoxes != nullptr)
	{
		GhostBoxes->ClearInstances();
	}
	Placed.Reset();
	GateGaps = 0;
	RoomForMore = 0;
	ModuleBoxes = 0;
	Dropped = 0;
	Ghosts = 0;
}

void UPlotPresenter::RebuildFrom(const URoadNetwork& Network,
	TArrayView<const PlotYard::FKitSpec> Specs)
{
	if (Boxes == nullptr)
	{
		return;
	}

	// CLEARED AND REBUILT WHOLE, like every other derived geometry in this plugin. An
	// incremental update would need to know which instance belonged to which entity, which
	// is a second index that must agree with the model - and the counts here are tens.
	Clear();

	// RESOLVED ONCE BY THE CALLER, not per plot: the specs are the same for every plot, and
	// resolving the same three kits once per depot would do the work once per building on
	// the airport.
	//
	// THROUGH ARoadNetworkActor::ResolveDepotKits, NOT DepotKitSpecs(GetContent()) (issue
	// #181): that was a second resolution of the table FPlotPlaceTool's ghost reads through
	// IRoadEditTarget::ResolveDepotKits, and the two drifted the moment DA_FuelDepot's layout
	// was authored - see ReservationFor's own comment on that failure. NULL CONTENT IS STILL A
	// LEGAL ANSWER and the tests rely on it: ResolveDepotKits falls every kit back to the
	// grey-box table.

	int32 Plots = 0;

	// INSTANCES, NOT BAYS, and the two parted company when runs arrived: a three-bay shed run
	// is three modules drawn as ONE box. ModuleBoxes counts what the player owns; this counts
	// what was handed to the component, which is the only thing the fence tally can subtract.
	int32 ModuleInstances = 0;

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || Entity.Outline.Num() < 3)
		{
			continue;
		}

		// THE SAME GEOMETRY THE PLACEMENT USED, not a remembered list of bay transforms.
		// Storing them on the instance would be a second copy of something PlotFit already
		// derives from the outline, and the two would drift the moment a bay size changed.
		FVector2D FrontageA = FVector2D::ZeroVector;
		FVector2D FrontageB = FVector2D::ZeroVector;
		if (!RecoverFrontage(Entity, FrontageA, FrontageB))
		{
			continue;
		}

		++Plots;

		// THE YARD, NOT A GRID. Where each module stands is a question about the plot's
		// CONTENTS, with different inputs from the plot's own shape - and a row of identical
		// boxes all facing one way is what made a built depot read as a placeholder. The bay
		// grid that used to answer it is gone entirely; see the 2026-09-16 design docs.
		// THE PLOT'S WHOLE CAPACITY, decided once. The gate is where the fence is left open,
		// which is the entity's own pose - see the fence loop below, which skips the bay
		// nearest exactly this point.
		//
		// RE-DERIVED, NEVER SAVED. Every input is already on the entity and DepotYardSeed
		// keys off the pose, so the same plot solves the same way on every rebuild and the
		// save keeps only what the player bought.
		//
		// THE PLOT TYPE DECIDES ITS OWN ARRANGEMENT. A fuel depot bands; something meant to
		// look unplanned still scatters. A null definition keeps the scatter, which is what
		// an un-migrated save has.
		FPlotSite PlotSite;
		PlotSite.Outline = Entity.Outline;
		PlotSite.FrontageA = FrontageA;
		PlotSite.FrontageB = FrontageB;
		PlotSite.Gate = Entity.Position;
		PlotSite.Seed = DepotYardSeed(Entity.Position);

		const EPlotLayout Layout = Entity.Definition != nullptr
			? Entity.Definition->Layout : EPlotLayout::Scatter;

		const PlotYard::FReservation Reservation =
			PlotLayoutFor(Layout)->Solve(PlotSite, Specs);

		// HOW MANY OF EACH THE PLAYER HAS BOUGHT. Entity.Modules is still the owned list and
		// still this depot's only record in the save.
		TArray<int32> Owned;
		Owned.SetNumZeroed(Specs.Num());
		for (const EDepotModule Module : Entity.Modules)
		{
			const int32 Kit = static_cast<int32>(Module);
			if (Owned.IsValidIndex(Kit))
			{
				++Owned[Kit];
			}
		}

		// MODULES BEFORE THE FENCE, always: Airside.Present.PlotPresenterScattersModules
		// names the first ModuleBoxes instances of a plot as its modules, and reordering
		// these two loops would silently make it measure fence panels instead. The ghosts go
		// in their own component, so they never enter that count at all.
		for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
		{
			if (!Specs.IsValidIndex(Stand.KitIndex))
			{
				continue;
			}
			const PlotYard::FFootprint& One = Specs[Stand.KitIndex].Footprint;
			const double HeightUu = HeightFor(static_cast<EDepotModule>(Stand.KitIndex));

			// A RUN FILLS FROM ONE END. Bays the player owns are drawn solid at that end and
			// the rest ghosted, so a run visibly GROWS along its length rather than appearing
			// whole. While these are boxes it is two boxes; with meshes it becomes
			// BakedMeshes[lit - 1] plus a ghost for the remainder.
			const int32 Lit = FMath::Clamp(Owned[Stand.KitIndex], 0, Stand.RunLength);
			Owned[Stand.KitIndex] -= Lit;
			const int32 Dark = Stand.RunLength - Lit;

			// Along the run's own width axis, which is the stand's LEFT - StandCorners builds
			// its corners from Forward and PerpCCW(Forward), so the same perpendicular here
			// keeps the two halves inside the ground the solver actually reserved.
			const FVector2D Forward(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
			const FVector2D Across = RoadGeom::PerpCCW(Forward);
			const double FullWidth = One.WidthUu * Stand.RunLength;

			// FLUSH TO THE BACK OF WHAT IT CLAIMED - ONLY WHEN THE STAND CLAIMED AN APRON AT
			// ALL. A stand's centre is the centre of its footprint PLUS its apron only under
			// UFuelYardBandsStrategy (Reservation.bStandsIncludeApron), and the apron reaches
			// towards the gate - so the object sits half an apron further back there, leaving
			// that ground clear in front of its door. PlotYard::Reserve (Scatter) never reads
			// ApronUu when it samples a pose, so its Stand.Centre is already the footprint's
			// own centre - applying this offset there drew the box half an apron outside the
			// ground the sampler actually fenced off (issue #193).
			const FVector2D ToBack = Reservation.bStandsIncludeApron
				? Forward * (Specs[Stand.KitIndex].ApronUu.X * 0.5)
				: FVector2D::ZeroVector;

			if (Lit > 0)
			{
				const double LitWidth = One.WidthUu * Lit;
				const FVector2D Centre =
					Stand.Centre + ToBack + Across * ((LitWidth - FullWidth) * 0.5);
				const FTransform ModuleAt =
					BoxAt(Centre, Stand.Heading, One.LengthUu, LitWidth, HeightUu);
				Boxes->AddInstance(ModuleAt, /*bWorldSpace=*/true);
				Placed.Add(ModuleAt);
				++ModuleInstances;
				ModuleBoxes += Lit;
			}

			if (Dark > 0 && GhostBoxes != nullptr)
			{
				const double DarkWidth = One.WidthUu * Dark;
				const FVector2D Centre =
					Stand.Centre + ToBack + Across * ((FullWidth - DarkWidth) * 0.5);
				GhostBoxes->AddInstance(
					BoxAt(Centre, Stand.Heading, One.LengthUu, DarkWidth, HeightUu),
					/*bWorldSpace=*/true);
			}
			Ghosts += Dark;
		}

		// AN OWNED MODULE THAT RESERVED NO STAND IS A DROP. Every stand of a kit already took
		// what it could hold (the Owned[Kit] -= Lit above), so whatever is left over is a
		// module the player bought that the reservation never offered any ground - the case
		// GetDroppedCount's own comment calls a bug rather than a refusal. This is the ++Dropped
		// the run-length rewrite (61f92fc) deleted along with the old one-module-per-stand loop;
		// see Airside.Present.PlotPresenterCountsDrops.
		for (const int32 Leftover : Owned)
		{
			Dropped += Leftover;
		}

		// --- The fence -----------------------------------------------------------------
		//
		// THE GATE IS A GAP, not a different mesh. The bay nearest the pose is left out, and
		// the pose is the gate - so the hole in the fence is where the truck actually
		// leaves, by construction rather than by a second decision that could disagree.
		for (int32 I = 0; I < Entity.Outline.Num(); ++I)
		{
			const FVector2D& From = Entity.Outline[I];
			const FVector2D& To = Entity.Outline[(I + 1) % Entity.Outline.Num()];

			const FVector2D Along = To - From;
			const double Length = Along.Size();
			if (Length < FenceBayUu)
			{
				continue;
			}

			const FVector2D Unit = Along / Length;
			const double Heading = FMath::Atan2(Unit.Y, Unit.X);
			const int32 Bays = FMath::FloorToInt(Length / FenceBayUu);

			// A bay that would overhang the end of the edge is DROPPED rather than
			// shortened: a stretched last panel is a second opinion about where the
			// boundary is, and the short remainder reads as a gap rather than a mistake.
			for (int32 B = 0; B < Bays; ++B)
			{
				const FVector2D Centre =
					From + Unit * ((static_cast<double>(B) + 0.5) * FenceBayUu);

				if (FVector2D::Distance(Centre, Entity.Position) < FenceBayUu)
				{
					++GateGaps;
					continue;
				}

				const FTransform PanelAt = BoxAt(Centre, Heading,
					FenceBayUu, FenceThicknessUu, FenceHeightUu);
				Boxes->AddInstance(PanelAt, /*bWorldSpace=*/true);
				Placed.Add(PanelAt);
			}
		}
	}

	// WHAT IS LEFT TO GROW INTO: a count of unlit bays, not a sampled estimate. It used to
	// continue the placement loop with a phantom tank until it failed; the reservation
	// already knows, so asking again would be a second opinion about one question.
	//
	// ASSIGNED ONCE, AFTER THE LOOP, because Ghosts accumulates across every plot - adding it
	// per entity counted the first depot's spare room again for the second.
	RoomForMore = Ghosts;

	// ONE CENSUS LINE PER REBUILD, beside the surface builder's own. Zero plots is the
	// common idle rebuild and stays quiet.
	//
	// IT NAMES THE GATE GAPS, which is the one thing that cannot be seen from a box count:
	// a fence with no gap is a depot no truck can leave, and it looks completely correct
	// from every angle on screen.
	if (Plots > 0)
	{
		// IT NAMES THE GHOSTS, because a depot drawn entirely in ghosts is a depot nobody has
		// bought anything for - which looks identical to a broken presenter from a box count
		// alone.
		UE_LOG(LogAirside, Log,
			TEXT("Plots: %d plot(s), %d module bay(s) built, %d ghosted, %d dropped, "
				 "%d fence panel(s), %d gate gap(s)"),
			Plots, ModuleBoxes, Ghosts, Dropped,
			Placed.Num() - ModuleInstances, GateGaps);
	}
}
