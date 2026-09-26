#pragma once

#include "CoreMinimal.h"
#include "Solve/IcaoCode.h"
#include "StandDefinitionCache.generated.h"

class ARoadNetworkActor;
class UEntityDefinition;
class URoadNetwork;

/**
 * Per-letter stand Flyweights (D/E/F, built the first time a letter is drawn) plus the rebind
 * step that re-points a loaded network's stands at them - split out of ARoadNetworkActor by
 * issue #298. ResolveStandDefinitionFor's lazily-built cache and RebindStandDefinitions' whole-
 * network walk are LOGIC, not a level-authored UPROPERTY, a component, or a thing only an AActor
 * can do - the actor's own class comment reserves itself for exactly those plus forwarding, and
 * this pair of members (with the const_cast the first one needed to earn its keep) is what made
 * it "a working class again".
 *
 * A UCLASS(UObject), CreateDefaultSubobject and Transient - the same pattern and the same reason
 * as URoadEditFacade/UAirsideTraffic: it holds UObject pointers the collector must trace
 * (LetterStandDefinitions below), and every one of them is derived, rebuildable state - see that
 * field's own comment for why none of it is ever saved.
 *
 * REACHES ITS OWNER THROUGH Outer, the same idiom as URoadEditFacade's private Actor() (see that
 * class's own comment) - CreateDefaultSubobject sets it, so a second stored pointer would only be
 * a second thing that could disagree with the first. Needed for exactly one thing: Code C's
 * definition is StandDefinition (an authored, saved UPROPERTY) or the content default, and that
 * resolution stays on the actor with every other Resolve* (CLAUDE.md's "Content/ resolves every
 * content default in exactly one function") - this cache asks the actor for the answer rather
 * than duplicating it.
 *
 * NO const_cast, UNLIKE THE ACTOR'S OLD ResolveStandDefinitionFor. That existed only to let a
 * CONST actor method lazily fill a cache - the shape ResolveProfile's RuntimeProfile would need
 * if that method were const too. A subobject reached through a TObjectPtr member is never itself
 * const merely because its owner's own accessor is (see ARoadNetworkActor::GetTraffic()/
 * GetPresenter() for the same shallow-const pattern already in use to read a subobject from a
 * const method), so the lazy build below is a plain mutation on a plain non-const method - the
 * const_cast was never protecting anything this move could not simply drop.
 */
UCLASS()
class AIRSIDE_API UStandDefinitionCache : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * A stand template for Letter, resolved and cached - the drawn-stand commit path's one place
	 * to ask "what does a Code X stand look like".
	 *
	 * CODE C FORWARDS TO THE ACTOR'S ResolveStandDefinition() UNCHANGED: it alone has an
	 * authored asset (StandDefinition, else DA_Stand_CodeC by content default), and a drawn
	 * Code C stand must place the SAME object a legacy PlaceStand drops, or the two could
	 * disagree about anchors, trucks or the envelope. Every other letter has no authored asset
	 * (Task 1's own finding), so it is built once with UEntityDefinition::MakeStandTransient
	 * (Letter, this), flagged RF_Transient - see LetterStandDefinitions for why it is never
	 * saved - and cached in LetterStandDefinitions[ordinal] so a second stand of the same letter
	 * reuses it rather than building a second Flyweight. OUTERED TO THIS CACHE, not the actor:
	 * this object's own lifetime already tracks the actor's (it is a CreateDefaultSubobject of
	 * one), so outering the template one level further in costs nothing and keeps every object
	 * this cache ever creates findable from the cache alone.
	 *
	 * NULL, LOGGED ONCE PER LETTER, when the built template does not fit its own letter's floor -
	 * UEntityDefinition::FitsItsLetter, and Task 1's measured table: A and B do not fit today, C
	 * through F do. A caller refuses on null; WhyStandRefused is what turns that into "Code X
	 * stands cannot be built yet" for the player.
	 */
	UEntityDefinition* ResolveStandDefinitionFor(EIcaoCode Letter);

	/**
	 * Re-point every live, plotted stand in Network at the one its OUTLINE's letter resolves to
	 * (ResolveStandDefinitionFor(StandBox::LetterOf(Outline))) - Code C to the actor's authored
	 * asset, D/E/F to this cache. Returns how many changed; logs the count, and a Warning naming
	 * each stand whose outline reads as no letter, or as one with no buildable definition (A/B
	 * today), which is left exactly as it was.
	 *
	 * CALLED WHEREVER A NETWORK IS (RE)LOADED, because the per-letter definitions are never
	 * saved (LetterStandDefinitions): from ARoadNetworkActor::PostRegisterAllComponents, which
	 * runs after serialisation for a level load AND a PIE duplicate (see its own comment on why
	 * neither PostLoad nor BeginPlay covers both), and from the ops layer's save-game load after
	 * it restores a snapshot, which runs Serialize and never PostLoad. Idempotent: a stand
	 * already on its letter's definition is not touched and not counted.
	 *
	 * ON THIS CACHE, not the facade or the model: it is the second reader of the definition
	 * cache this class owns (ResolveStandDefinitionFor is the first), and Model/ may not reach
	 * Entities/ to resolve anything. The write itself goes through URoadNetwork::
	 * SetEntityDefinition, a pure pointer write.
	 * ENFORCED BY: AirportOps.Present.RuntimeLoad.DrawnStandSurvivesLoad,
	 * Airside.Present.StandPlot.RebindsAfterLevelLoad.
	 */
	int32 RebindStandDefinitions(URoadNetwork* Network);

private:
	/**
	 * The owning actor, found through Outer rather than stored a second time - see the class
	 * comment. Needed for exactly one call: Actor().ResolveStandDefinition() for Code C.
	 */
	ARoadNetworkActor& Actor() const;

	/**
	 * A drawn stand's definition, per letter (A-F, indexed by EIcaoCode's own ordinal) - lazily
	 * built by ResolveStandDefinitionFor. Code C keeps using the actor's StandDefinition/
	 * ResolveStandDefinition instead - see ResolveStandDefinitionFor - so this array is never
	 * touched for C and index 2 (EIcaoCode::C) stays unset.
	 *
	 * TRANSIENT, moved verbatim from ARoadNetworkActor (issue #298), where it was REVERSED by
	 * the final review on the original task (C2/I7) from being saved: it used to be saved, on
	 * the argument that a stand's saved Definition must find its object again on load. That held
	 * for a LEVEL and for nothing else: OpsSave writes Definition as a PATH, and a new session
	 * has nothing at that path, so a loaded D stand came back with a null Definition and "NOT
	 * reachable". And saving the cache froze its templates into the level - A/B stayed
	 * unbuildable, and a D/E/F template change never reached a level that already had one. So
	 * the cache is derived state again, rebuilt on demand, and RebindStandDefinitions is what
	 * re-points every stand at it after any load - level or save game. The objects themselves
	 * carry RF_Transient, so a level save writes a stand's reference to one as null rather than
	 * serialising a stale copy beside it; the rebind is what fills it back in.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UEntityDefinition>> LetterStandDefinitions;
};
