#include "Present/StandDefinitionCache.h"

#include "AirsideLog.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/IcaoCode.h"
#include "Solve/StandBox.h"

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
		if (Entity.Definition != Definition && Network->SetEntityDefinition(Network->EntityIdAt(Index), Definition))
		{
			++Rebound;
		}
	}

	if (Rebound > 0)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("RebindStandDefinitions: %d stand(s) re-pointed at their letter's definition."), Rebound);
	}
	return Rebound;
}
