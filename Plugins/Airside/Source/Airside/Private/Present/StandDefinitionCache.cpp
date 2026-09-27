#include "Present/StandDefinitionCache.h"

#include "AirsideLog.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"
#include "Solve/StandBox.h"

// NAMED, NOT ANONYMOUS: the plugin is a UNITY build, and an anonymous helper of a common name
// compiles alone and collides with another file's copy once both land in one translation unit.
namespace StandDefinitionCacheLocal
{
	/**
	 * The stop-mark pose a drawn stand's OUTLINE implies for Letter, by the rule placement uses
	 * (URoadEditFacade::PlaceStandInPlot): StandBox::PoseFor off the entrance edge, inward by the
	 * outline's own winding (PlotYard::InwardOf), at the fleet-resolved envelope.
	 *
	 * THE ENTRANCE EDGE IS FOUND, NOT ASSUMED TO BE Outline[0..1]. PlaceStandInPlot reverses a
	 * clockwise outline before storing it, which puts the drawn FAR edge at 0->1 (see
	 * StandMarkingBuilder.h's own note). The stand's facing survives any change of geometry, so
	 * the entrance is the edge whose midpoint lies furthest BEHIND the current stop mark along it
	 * - whatever rule placed that mark - and the edge is taken in the stored order, so a stand
	 * placed today re-derives exactly the A, B and Inward its commit used.
	 */
	StandBox::FStandPose PoseFromOutline(const FEntityInstance& Stand, EIcaoCode Letter)
	{
		const TArray<FVector2D>& Outline = Stand.Outline;
		const FVector2D Facing(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
		int32 Entrance = 0;
		double Behind = TNumericLimits<double>::Max();
		for (int32 Corner = 0; Corner < Outline.Num(); ++Corner)
		{
			const FVector2D Mid = 0.5 * (Outline[Corner] + Outline[(Corner + 1) % Outline.Num()]);
			const double Along = FVector2D::DotProduct(Mid - Stand.Position, Facing);
			if (Along < Behind)
			{
				Behind = Along;
				Entrance = Corner;
			}
		}
		const FVector2D A = Outline[Entrance];
		const FVector2D B = Outline[(Entrance + 1) % Outline.Num()];
		return StandBox::PoseFor(A, B, PlotYard::InwardOf(Outline, A, B), Letter,
			UAirsideSettings::ResolveLetterEnvelope(Letter));
	}

	/**
	 * Does the stand already stand where Pose and Definition put it - stop mark, facing, and every
	 * anchor at its offset? Within RePoseToleranceUu (and a millionth of a radian): a stand placed
	 * today re-derives its own pose to the ulp, and one that does must not be re-posed on every
	 * load, which would churn its anchor handles for nothing.
	 *
	 * THE ANCHORS AS WELL AS THE POSE: an outline that reads as a new letter changes which
	 * fixtures the stand has and where, even in the (hypothetical) case its setback did not move.
	 */
	bool StandMatchesPose(const URoadNetwork& Network, FEntityInstanceId Id, const FEntityInstance& Stand,
		const StandBox::FStandPose& Pose, const UEntityDefinition& Definition)
	{
		// A CENTIMETRE: the outline's own measuring quantum (StandBox::WidthOf/DepthOf).
		constexpr double RePoseToleranceUu = 1.0;
		const double Heading = RoadGeom::Bearing(Pose.Facing);
		if (!Stand.Position.Equals(Pose.Position, RePoseToleranceUu)
			|| FMath::Abs(FMath::UnwindRadians(Stand.Heading - Heading)) > 1.0e-6
			|| Stand.ResolvedAnchors.Num() != Definition.Anchors.Num())
		{
			return false;
		}
		for (const FEntityAnchor& Anchor : Definition.Anchors)
		{
			const FGuidelineNode* Node = Network.GetAnchorNode(Id, Anchor.Id);
			if (Node == nullptr || !Node->Position.Equals(Anchor.WorldAt(Stand.Position, Stand.Heading), RePoseToleranceUu))
			{
				return false;
			}
		}
		return true;
	}
}

ARoadNetworkActor& UStandDefinitionCache::Actor() const
{
	ARoadNetworkActor* Owner = GetTypedOuter<ARoadNetworkActor>();
	checkf(Owner != nullptr,
		TEXT("UStandDefinitionCache created without an owning ARoadNetworkActor - it is only "
			 "ever valid as that actor's CreateDefaultSubobject, see the class comment."));
	return *Owner;
}

UEntityDefinition* UStandDefinitionCache::ResolveStandDefinitionFor(EIcaoCode Letter)
{
	// CODE C KEEPS ITS AUTHORED ASSET - see the header. Every other letter has none, so it
	// falls through to the lazily-built cache below.
	if (Letter == EIcaoCode::C)
	{
		return Actor().ResolveStandDefinition();
	}

	// SIZED ON FIRST USE rather than in the constructor, so an actor spawned before this cache
	// existed (or a saved level from before it) still starts with an empty array rather than six
	// null-but-present slots nobody asked for.
	const int32 Ordinal = static_cast<int32>(Letter);
	if (LetterStandDefinitions.Num() <= Ordinal)
	{
		LetterStandDefinitions.SetNum(Ordinal + 1);
	}

	if (LetterStandDefinitions[Ordinal] == nullptr)
	{
		UEntityDefinition* Definition = UEntityDefinition::MakeStandTransient(Letter, this);

		// NEVER SAVED - see LetterStandDefinitions. RF_Transient is what makes a level save
		// write a stand's reference to this object as null (RebindStandDefinitions fills it
		// back in on load) instead of serialising a frozen copy of the template beside it.
		Definition->SetFlags(RF_Transient);

		// THE ONE PLACE a per-letter design aircraft could be filled in, matching every other
		// Resolve* here (CLAUDE.md's "Content/ resolves every content default in exactly one
		// function") - see UAirsideSettings::ResolveLargestAircraftOfLetter's own header for
		// why it returns null for every letter today rather than scanning for one.
		Definition->DesignAircraft = UAirsideSettings::ResolveLargestAircraftOfLetter(Letter);

		LetterStandDefinitions[Ordinal] = Definition;

		if (!UEntityDefinition::FitsItsLetter(*Definition, Letter))
		{
			// LOGGED ONCE, HERE, ON THE CACHE MISS - not on every ResolveStandDefinitionFor
			// call, which WhyStandRefused and PlaceStandInPlot both make per player action.
			UE_LOG(LogRoadMesh, Warning,
				TEXT("ResolveStandDefinitionFor: Code %s's own template does not fit its "
					 "letter's floor - Code %s stands cannot be built yet."),
				IcaoCode::ToLetter(Letter), IcaoCode::ToLetter(Letter));
		}
	}

	// FITNESS IS CHECKED EVERY CALL, NOT CACHED AS A BOOL: the definition itself is what is
	// cached (so a fit letter keeps returning the SAME object), and FitsItsLetter is cheap -
	// two comparisons and a table lookup - so there is nothing worth a second cached field for.
	return UEntityDefinition::FitsItsLetter(*LetterStandDefinitions[Ordinal], Letter)
		? LetterStandDefinitions[Ordinal].Get()
		: nullptr;
}

int32 UStandDefinitionCache::RebindStandDefinitions(URoadNetwork* Network)
{
	if (Network == nullptr)
	{
		return 0;
	}

	int32 Rebound = 0;
	const TArray<FEntityInstance>& Entities = Network->GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Entity = Entities[Index];
		// ONE LINE, BOTH NAMES (Check-Architecture's is-plotted-not-depot rule): a stand with
		// an outline. One without is a legacy stand EnsureStandOutlines has not reached yet -
		// the callers run that first - and has no letter to read.
		if (!Entity.bAlive || !(Entity.IsStand() && Entity.IsPlotted()))
		{
			continue;
		}

		// THE OUTLINE'S LETTER, not DesignWingspan's: the outline is what the player drew and
		// what the definition was chosen from at commit (PlaceStandInPlot), so reading it back
		// here resolves the SAME definition the commit did.
		const TOptional<EIcaoCode> Letter = StandBox::LetterOf(Entity.Outline);
		UEntityDefinition* Definition = Letter.IsSet() ? ResolveStandDefinitionFor(*Letter) : nullptr;
		if (Definition == nullptr)
		{
			// LEFT UNTOUCHED, not cleared: whatever it has is no worse than nothing, and a
			// stand whose Definition is null already says "NOT reachable" in the Inspector.
			UE_LOG(LogRoadMesh, Warning,
				TEXT("RebindStandDefinitions: stand %d's outline reads as %s, which has no buildable "
					 "definition - left as it was."),
				Index, Letter.IsSet() ? *FString::Printf(TEXT("Code %s"), IcaoCode::ToLetter(*Letter)) : TEXT("no letter"));
			continue;
		}
		const FEntityInstanceId EntityId = Network->EntityIdAt(Index);
		if (Entity.Definition != Definition && Network->SetEntityDefinition(EntityId, Definition))
		{
			++Rebound;
		}

		// THE POSE, RE-DERIVED FROM THE OUTLINE (spec §1; final review 2026-09-27). The outline is
		// what the player drew and what survives a change of geometry; the pose is derived from
		// it, and a save made before 2026-09-26 holds a pose derived by the OLD rule - the stop
		// mark Depth - MaxNoseFwd in from the entrance. A Code C drawn at the old 55 m floor reads
		// as Code B now, and B's bays laid off that old pose sat 1100 uu beyond the drawn far edge.
		// HERE because this is where the letter is re-read, and the pose depends on it. Entity is
		// not re-read after SetEntityDefinition: that writes the pointer alone, and what is read
		// below is the outline and the pose.
		// ENFORCED BY: Airside.Present.StandPlot.OldPoseRederivedOnLoad
		// THE SPAN FOLLOWS THE LETTER (re-review, 2026-09-27), re-posed or not: every admission,
		// label and marking reads DesignWingspan, so an outline that reads as a new letter has to
		// carry that letter's span. Keyed on the LETTER the span reads as, not on the value, so a
		// stand whose span is some other figure inside its own letter's band keeps it. Zero is
		// "never measured" (a raw fixture) and is left alone, as StandMarkingBuilder leaves it.
		// The log reads the old span off Entity (a reference) before the write replaces it.
		// ENFORCED BY: Airside.Present.StandPlot.OldPoseRederivedOnLoad
		if (Entity.DesignWingspan > 0.0 && IcaoCode::CodeForWingspan(Entity.DesignWingspan) != *Letter)
		{
			const double Span = IcaoCode::DesignSpanForLetter(*Letter);
			UE_LOG(LogRoadMesh, Log,
				TEXT("RebindStandDefinitions: stand %d's outline reads as Code %s; its design span %.0f -> %.0f."),
				Index, IcaoCode::ToLetter(*Letter), Entity.DesignWingspan, Span);
			Network->SetStandDesignWingspan(EntityId, Span);
		}

		const StandBox::FStandPose Pose = StandDefinitionCacheLocal::PoseFromOutline(Entity, *Letter);
		if (!StandDefinitionCacheLocal::StandMatchesPose(*Network, EntityId, Entity, Pose, *Definition))
		{
			UE_LOG(LogRoadMesh, Log,
				TEXT("RebindStandDefinitions: stand %d re-posed as Code %s off its outline - stop mark (%.0f, %.0f) -> (%.0f, %.0f)."),
				Index, IcaoCode::ToLetter(*Letter), Entity.Position.X, Entity.Position.Y, Pose.Position.X, Pose.Position.Y);
			Network->RePoseStand(EntityId, Pose.Position, RoadGeom::Bearing(Pose.Facing), Definition->Anchors);
		}
	}

	if (Rebound > 0)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("RebindStandDefinitions: %d stand(s) re-pointed at their letter's definition."), Rebound);
	}
	return Rebound;
}
