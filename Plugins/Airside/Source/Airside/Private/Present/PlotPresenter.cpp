#include "Present/PlotPresenter.h"

#include "AirsideLog.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Solve/PlotFit.h"

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
}

void UPlotPresenter::Initialise(UInstancedStaticMeshComponent* InBoxes)
{
	Boxes = InBoxes;
}

int32 UPlotPresenter::GetInstanceCount() const
{
	return Boxes != nullptr ? Boxes->GetInstanceCount() : 0;
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

	int32 Plots = 0;
	int32 ModuleBoxes = 0;

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || Entity.Outline.Num() < 3)
		{
			continue;
		}

		// THE SAME FIT THE PLACEMENT USED, not a remembered list of bay transforms. Storing
		// them on the instance would be a second copy of something PlotFit already derives
		// from the outline, and the two would drift the moment a bay size changed.
		FVector2D FrontageA = FVector2D::ZeroVector;
		FVector2D FrontageB = FVector2D::ZeroVector;
		if (!RecoverFrontage(Entity, FrontageA, FrontageB))
		{
			continue;
		}

		++Plots;

		const PlotFit::FPlotFit Fit = PlotFit::FitBays(Entity.Outline, FrontageA, FrontageB);

		// THE BAYS AND THE MODULES ARE SEPARATE COUNTS, and the pair is the whole diagnosis
		// when a depot looks wrong: fewer bays than modules is a plot too small for the mix,
		// and fewer modules than bays is a yard the player has not finished filling. One
		// combined number would say neither.
		if (Fit.Bays.Num() != Entity.Modules.Num())
		{
			UE_LOG(LogAirside, Log,
				TEXT("Plot at (%.0f, %.0f): %d bay(s) fit, %d module(s) chosen."),
				Entity.Position.X, Entity.Position.Y, Fit.Bays.Num(), Entity.Modules.Num());
		}

		const int32 Count = FMath::Min(Fit.Bays.Num(), Entity.Modules.Num());
		for (int32 I = 0; I < Count; ++I)
		{
			const PlotFit::FPlotBay& Bay = Fit.Bays[I];
			Boxes->AddInstance(BoxAt(Bay.Centre, Bay.Heading,
				PlotFit::BayDepthUu, PlotFit::BayWidthUu, HeightFor(Entity.Modules[I])),
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
			TEXT("Plots: %d plot(s), %d module box(es), %d fence panel(s), %d gate gap(s)"),
			Plots, ModuleBoxes, Boxes->GetInstanceCount() - ModuleBoxes, GateGaps);
	}
}
