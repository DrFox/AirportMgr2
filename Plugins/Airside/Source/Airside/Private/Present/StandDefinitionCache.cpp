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
	 * THE ENTRANCE EDGE IS READ, NOT SEARCHED FOR (#450's leftover): FEntityInstance::FrontageEdge stores it, written by PlaceStandInPlot from the
	 * edge it was GIVEN and by StandBox::EntranceEdgeOf for a stand given none (PlaceEntity, and EnsureStandFrontages for one saved before the field).
	 * This used to run that search itself on every load - the edge whose midpoint lies furthest BEHIND the current stop mark along its facing, which
	 * is how it found the entrance without assuming it was Outline[0..1] (PlaceStandInPlot reverses a clockwise outline before storing it, which puts the
	 * drawn FAR edge at 0->1; see StandMarkingBuilder.h's own note). The stored edge is in the stored order, so a stand placed today re-derives exactly
	 * the A, B and Inward its commit used. A SECOND SEARCH HERE would be the pose and the paint agreeing only while a comment said they did.
	 * ENFORCED BY: Airside.Model.StandFrontage.ReadersReadTheStoredEdge, Check-Architecture rule 80 (nothing but URoadNetwork asks EntranceEdgeOf)
	 *
	 * UNSET when the stand stores no entrance (an outline the migration has not reached, or an index that names no edge): the caller leaves the stand as
	 * it is and says so - a pose derived off a guessed edge would MOVE a stand the player placed, which is the one thing a repair must not do.
	 */
	TOptional<StandBox::FStandPose> PoseFromOutline(const FEntityInstance& Stand, EIcaoCode Letter)
	{
		FVector2D A, B;
		if (!Stand.GetFrontage(A, B))
		{
			return TOptional<StandBox::FStandPose>();
		}
		return StandBox::PoseFor(A, B, PlotYard::InwardOf(Stand.Outline, A, B), Stand.Outline, Letter,
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

		const TOptional<StandBox::FStandPose> Pose = StandDefinitionCacheLocal::PoseFromOutline(Entity, *Letter);
		if (!Pose.IsSet())
		{
			// LEFT AS IT WAS, like a stand with no buildable definition above: the entrance is read off the STORED edge (PoseFromOutline), and one that
			// is not there is the migration not having run - EnsureStandFrontages comes before this in both load paths - which is a bug to read in the log, not a
			// reason to move a stand the player placed by a guess.
			// ENFORCED BY: Airside.Model.StandFrontage.MigrationStoresTheEntranceOnce (the outline-less stand each load half gives a box and THEN an entrance),
			// Airside.Present.StandPlot.OldPoseRederivedOnLoad (the migration before this rebind - swapped, the stand is not re-posed)
			// ONCE, NOT PER STAND: this runs on every re-registration of the actor (an editor re-register, a PIE start) for every stand with no edge, so a
			// level that missed the migration would print one line per stand per load; the first line says what is wrong, and the rest is the same sentence.
			static bool bWarnedNoEntrance = false;
			if (!bWarnedNoEntrance)
			{
				bWarnedNoEntrance = true;
				UE_LOG(LogRoadMesh, Warning,
					TEXT("RebindStandDefinitions: stand %d has an outline but no stored entrance edge (EnsureStandFrontages did not run?) - not re-posed. Logged once."), Index);
			}
			continue;
		}
		if (!StandDefinitionCacheLocal::StandMatchesPose(*Network, EntityId, Entity, *Pose, *Definition))
		{
			UE_LOG(LogRoadMesh, Log,
				TEXT("RebindStandDefinitions: stand %d re-posed as Code %s off its outline - stop mark (%.0f, %.0f) -> (%.0f, %.0f)."),
				Index, IcaoCode::ToLetter(*Letter), Entity.Position.X, Entity.Position.Y, Pose->Position.X, Pose->Position.Y);
			Network->RePoseStand(EntityId, Pose->Position, RoadGeom::Bearing(Pose->Facing), Definition->Anchors);
		}
	}

	if (Rebound > 0)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("RebindStandDefinitions: %d stand(s) re-pointed at their letter's definition."), Rebound);
	}
	return Rebound;
}
