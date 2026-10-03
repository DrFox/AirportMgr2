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
 * NEEDS NO OWNER: until 2026-10-03 it reached the actor through Outer for one thing - Code C's
 * definition was the actor's authored StandDefinition or DA_Stand_CodeC by content default. Both
 * are retired (owner ruling: a stand layout is derived data, and the saved copy went stale), so
 * Code C is built here like every other letter and the cache asks its owner nothing.
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
	 * EVERY LETTER ALIKE, CODE C INCLUDED since 2026-10-03: C used to forward to the actor's
	 * authored DA_Stand_CodeC, which went stale once (re-authored by hand for the tow's settle
	 * straights) - so it is built here now, and ARoadNetworkActor::ResolveStandDefinition (the
	 * point-placed stand) asks THIS for C, so a drawn C and a placed C are the SAME object, as they
	 * were when both read the asset. Each letter is built once with UEntityDefinition::MakeStandTransient
	 * (Letter, this), flagged RF_Transient - see LetterStandDefinitions for why it is never
	 * saved - and cached in LetterStandDefinitions[ordinal] so a second stand of the same letter
	 * reuses it rather than building a second Flyweight. OUTERED TO THIS CACHE, not the actor:
	 * this object's own lifetime already tracks the actor's (it is a CreateDefaultSubobject of
	 * one), so outering the template one level further in costs nothing and keeps every object
	 * this cache ever creates findable from the cache alone.
	 *
	 * NULL, LOGGED ONCE PER LETTER, when the built template does not fit its own letter's floor -
	 * UEntityDefinition::FitsItsLetter. A GUARD, not a live case: the first measurement found A and B NOT
	 * fitting, and the far-side-entry work (2026-09-26) gave every letter its own design
	 * vehicle so all six fit now - this null stays reachable for a future template or floor
	 * regression rather than being removed with the bug it once caught. A caller refuses on
	 * null; WhyStandRefused is what turns that into "Code X stands cannot be built yet" for the
	 * player.
	 * ENFORCED BY: Airside.Present.StandPlot.PlacesOtherLetters (B, D, E, F), Airside.Present.StandPlot.OldPoseRederivedOnLoad (C, B)
	 */
	UEntityDefinition* ResolveStandDefinitionFor(EIcaoCode Letter);

	/**
	 * Re-point every live, plotted stand in Network at the one its OUTLINE's letter resolves to
	 * (ResolveStandDefinitionFor(StandBox::LetterOf(Outline))) - Code C to the actor's authored
	 * asset, every other letter (A, B, D, E, F all build as of 2026-09-26) to this
	 * cache. Returns how many changed; logs the count, and a Warning naming each stand whose
	 * outline reads as no letter, or as one with no buildable definition - a template or floor
	 * regression, per ResolveStandDefinitionFor's own guard, not a letter that is expected to be
	 * missing today - which is left exactly as it was.
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
	 *
	 * AND THE POSE IS RE-DERIVED FROM THE OUTLINE (spec §1, since 2026-09-27): StandBox::PoseFor
	 * for the letter the outline reads as now, at the fleet-resolved envelope - the rule
	 * PlaceStandInPlot commits by. A stand whose stop mark or anchors disagree with that, beyond
	 * a centimetre, is moved through URoadNetwork::RePoseStand and logged by name; a save made
	 * before the 2026-09-26 geometry change is the case (its stop mark sat Depth - MaxNoseFwd in
	 * from the entrance), and a stand placed today re-derives its own pose and is left alone. Not
	 * counted in the return, which stays "how many re-pointed".
	 * ENFORCED BY: Airside.Present.StandPlot.OldPoseRederivedOnLoad
	 */
	int32 RebindStandDefinitions(URoadNetwork* Network);

private:
	/**
	 * A drawn stand's definition, per letter (A-F, indexed by EIcaoCode's own ordinal) - lazily
	 * built by ResolveStandDefinitionFor, Code C included since 2026-10-03.
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
