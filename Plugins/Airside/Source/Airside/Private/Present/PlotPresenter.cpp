#include "Present/PlotPresenter.h"

#include "AirsideLog.h"
#include "Build/DepotKit.h"
#include "Components/InstancedStaticMeshComponent.h"
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

	/**
	 * A stable seed for one depot.
	 *
	 * POSITION, NOT AN ENTITY HANDLE: handles are slot indices that get reused, and a
	 * position is set once at placement and never changes. Two depots cannot share one.
	 *
	 * QUANTISED to whole uu because a float that came back from a save one bit different
	 * would re-roll that depot and only that depot - the kind of bug that takes a day.
	 */
	int32 SeedFor(const FEntityInstance& Entity)
	{
		const int32 X = FMath::RoundToInt(Entity.Position.X);
		const int32 Y = FMath::RoundToInt(Entity.Position.Y);
		return static_cast<int32>(HashCombine(GetTypeHash(X), GetTypeHash(Y)));
	}
}

void UPlotPresenter::Initialise(UInstancedStaticMeshComponent* InBoxes)
{
	Boxes = InBoxes;
}

int32 UPlotPresenter::GetInstanceCount() const
{
	return Boxes != nullptr ? Boxes->GetInstanceCount() : 0;
}

bool UPlotPresenter::GetInstanceTransformForTest(int32 Index, FTransform& OutTransform) const
{
	if (Boxes == nullptr || Index < 0 || Index >= Boxes->GetInstanceCount())
	{
		return false;
	}
	return Boxes->GetInstanceTransform(Index, OutTransform, /*bWorldSpace=*/true);
}

void UPlotPresenter::RebuildFrom(const URoadNetwork& Network)
{
	if (Boxes == nullptr)
	{
		return;
	}

	// CLEARED AND REBUILT WHOLE, like every other derived geometry in this plugin. An
	// incremental update would need to know which instance belonged to which entity, which
	// is a second index that must agree with the model - and the counts here are tens.
	Boxes->ClearInstances();
	GateGaps = 0;
	RoomForMore = 0;
	ModuleBoxes = 0;
	Dropped = 0;

	int32 Plots = 0;

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

		// THE YARD, NOT A GRID. PlotFit::BuildGrid still answers what the PLOT is - the
		// gesture's preview uses it - but where each module stands is a different question
		// with different inputs, and a row of identical boxes all facing one way is what
		// made a built depot read as a placeholder. See the 2026-09-16 design doc.
		TArray<PlotYard::FFootprint> Footprints;
		Footprints.Reserve(Entity.Modules.Num());
		for (const EDepotModule Module : Entity.Modules)
		{
			Footprints.Add(DepotFootprint(Module));
		}

		// The gate is where the fence is left open, which is the entity's own pose - see the
		// fence loop below, which skips the bay nearest exactly this point.
		const PlotYard::FYard Yard = PlotYard::LayOut(Entity.Outline, FrontageA, FrontageB,
			Entity.Position, Footprints, SeedFor(Entity), DepotFootprint(EDepotModule::Tank));

		RoomForMore += Yard.RoomForMore;

		// MODULES BEFORE THE FENCE, always: Airside.Present.PlotPresenterScattersModules
		// names the first ModuleBoxes instances of a plot as its modules, and reordering
		// these two loops would silently make it measure fence panels instead.
		for (int32 I = 0; I < Yard.Stands.Num() && I < Entity.Modules.Num(); ++I)
		{
			const PlotYard::FStand& Stand = Yard.Stands[I];
			if (!Stand.bPlaced)
			{
				// REPORTED, not hidden. A module shoved in anyway would intersect something,
				// and a mesh through a mesh is the one failure no camera angle hides.
				++Dropped;
				continue;
			}
			Boxes->AddInstance(BoxAt(Stand.Centre, Stand.Heading,
				Footprints[I].LengthUu, Footprints[I].WidthUu, HeightFor(Entity.Modules[I])),
				/*bWorldSpace=*/true);
			++ModuleBoxes;
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

				Boxes->AddInstance(BoxAt(Centre, Heading,
					FenceBayUu, FenceThicknessUu, FenceHeightUu), /*bWorldSpace=*/true);
			}
		}
	}

	// ONE CENSUS LINE PER REBUILD, beside the surface builder's own. Zero plots is the
	// common idle rebuild and stays quiet.
	//
	// IT NAMES THE GATE GAPS, which is the one thing that cannot be seen from a box count:
	// a fence with no gap is a depot no truck can leave, and it looks completely correct
	// from every angle on screen.
	if (Plots > 0)
	{
		UE_LOG(LogAirside, Log,
			TEXT("Plots: %d plot(s), %d module box(es), %d dropped, room for %d more, "
				 "%d fence panel(s), %d gate gap(s)"),
			Plots, ModuleBoxes, Dropped, RoomForMore,
			Boxes->GetInstanceCount() - ModuleBoxes, GateGaps);
	}
}
