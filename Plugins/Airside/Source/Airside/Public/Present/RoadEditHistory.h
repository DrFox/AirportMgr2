#pragma once

#include "CoreMinimal.h"
#include "Model/BuildPurse.h"
#include "UObject/Object.h"
#include "UObject/StrongObjectPtr.h"
#include "RoadEditHistory.generated.h"

class URoadNetwork;

/** One remembered state of the graph, and the name of the edit that left it behind. */
USTRUCT()
struct FRoadEditSnapshot
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<URoadNetwork> Network = nullptr;

	/** Shown to the player: "Undo split segment". */
	UPROPERTY() FString Label;

	/**
	 * The ledger entry the edit this snapshot precedes paid for, or INDEX_NONE.
	 *
	 * ON THE SNAPSHOT, because the snapshot IS the edit as far as undo is concerned: stepping
	 * past it reverses that edit, so the charge to reverse has to travel with it. Reversing by
	 * ID rather than re-pricing the geometry is what puts back exactly what was taken - see
	 * IBuildPurse::Reverse.
	 *
	 * NOT a UPROPERTY and not saved, like the history itself: an undo stack is a session's, and
	 * a charge id restored against a ledger that had moved on would reverse somebody else's
	 * entry.
	 */
	int32 ChargeId = INDEX_NONE;

	/**
	 * What that edit was worth, so a REDO can charge for it again.
	 *
	 * WITHOUT THIS, UNDO IS A MONEY PRINTER. Undo refunds what the build took; if redo then
	 * re-applied the edit for nothing, the player would end up holding both the taxiway and
	 * its refund, and could repeat it. Re-charging on redo is what closes that, and the quote
	 * has to travel with the snapshot because Travel has no idea what edit it is replaying.
	 */
	FBuildQuote Quote;
};

/**
 * Undo and redo for the road graph, as a Memento: each entry is a whole copy of the
 * network as it stood before an edit.
 *
 * A NAMED deviation from design spec 7.3, which specifies Command. Command is the right
 * pattern when state is large or expensive to copy; here it is neither, and it has one
 * requirement Memento satisfies for free and Command does not. Spec 7.3's own warning is
 * that "Revert must restore handles identically, generation counter included" - and the
 * command layer built to that spec failed exactly there, twice: FCreateSegment::Revert
 * un-created with Remove, bumping generations, while its siblings restored slots. Undo
 * jammed permanently, and redo of any two-segment chain died on a stale handle.
 *
 * Restoring a whole snapshot cannot make that mistake. Generations, free lists and the
 * solver's stored cut vertices come back bitwise because they are not re-derived at all.
 *
 * What it costs: one duplicate of the graph per edit. A thousand nodes is on the order of
 * 250 KB, so a full stack is a few megabytes, and DuplicateObject on this exact type is
 * already proven at sixty times a second by the ghost preview. If the network ever grows
 * past what is comfortable to copy, Command returns behind this same interface - callers
 * see Undo, Redo and an edit scope, none of which name a mechanism.
 *
 * The snapshots are UPROPERTY-held because they are UObjects: a raw pointer to one is
 * collected out from under the stack at the next GC.
 */
UCLASS()
class AIRSIDE_API URoadEditHistory : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * How many edits can be taken back. Older snapshots are dropped from the bottom.
	 *
	 * A cap rather than unbounded growth: every entry is a whole graph, so an unbounded
	 * stack grows without limit over a long session.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside", meta = (ClampMin = "1"))
	int32 MaxDepth = 50;

	// --- Edit lifecycle ---------------------------------------------------------------
	//
	// Two-phase on purpose. The snapshot has to be taken BEFORE the mutation - by the time
	// an edit knows it succeeded, the state it would have preserved is already gone - but
	// it must only reach the stack if the edit actually happened. An edit that refuses and
	// still pushes gives the player an undo step that visibly does nothing, and they have
	// to press it twice to get past.
	//
	// Prefer FRoadEditScope to calling these by hand; it cannot forget to end an edit.

	/** Copy Network aside as the state to come back to. */
	void BeginEdit(const URoadNetwork& Network, const FString& Label);

	/** The edit happened: push the pending snapshot, and drop any redo future. */
	void CommitEdit();

	/** The edit refused: discard the pending snapshot, leaving the stacks untouched. */
	void AbandonEdit();

	/**
	 * The edit must be UNDONE: put Live back to the state it started from, IN PLACE, and clear
	 * the pending edit. False when no edit was in progress, touching nothing.
	 *
	 * NOT AbandonEdit, WHICH REVERTS NOTHING. Abandon drops the snapshot and leaves the model
	 * exactly as the edit left it - right for a mutator that refuses before touching anything,
	 * and wrong for an interactive drag, which has already moved the node on every frame it
	 * was held. The caller notifies afterwards (URoadEditFacade::RollBackOpenEdit does): this
	 * restores the model and nothing that derives from it.
	 *
	 * IN PLACE, where it used to hand the snapshot back for the caller to adopt (RevertEdit,
	 * replaced for issue #437): the two ways of getting a network back - this, and the local
	 * snapshot a history-less scope holds - are now ONE, URoadNetwork::RestoreFrom, so a
	 * restore in the editor world and a restore in a game world cannot disagree about which
	 * fields come back or how the revision clocks move. The stacks are untouched: a rolled-back
	 * edit is not an undo step, because as far as the player is concerned it never happened.
	 */
	bool RollbackEdit(URoadNetwork& Live);

	/**
	 * Record what the edit in progress paid, so undoing past it can put the money back.
	 *
	 * ON THE PENDING SNAPSHOT and therefore only between BeginEdit and CommitEdit. A no-op when
	 * nothing is being edited, which is the editor-world case: there HistoryForEdit is null,
	 * the transaction system owns undo, and nothing was charged anyway.
	 */
	void SetPendingCharge(int32 ChargeId, const FBuildQuote& Quote);

	/**
	 * The charge id of the edit Undo would step past, or INDEX_NONE.
	 *
	 * READ BEFORE TRAVELLING, never after: Undo moves that snapshot onto the redo stack, so by
	 * the time it returns the entry this names is no longer on top.
	 */
	int32 PeekUndoChargeId() const;

	/** What redoing the next step would cost again, or a free quote if there is nothing to redo. */
	const FBuildQuote& PeekRedoQuote() const;

	/**
	 * Record the charge a REDO just made, onto the step it re-applied.
	 *
	 * The redone edit is on top of the undo stack by the time this is called, and it carries a
	 * NEW ledger id - the old one names an entry that has already been reversed, and reversing
	 * it a second time is refused by the ledger on purpose.
	 */
	void SetUndoTopCharge(int32 ChargeId);

	bool IsEditing() const { return PendingSnapshot != nullptr; }

	// --- Travel -----------------------------------------------------------------------

	/**
	 * Step back. Returns the graph to adopt as live, or null when there is nothing to undo.
	 *
	 * Current is copied onto the redo stack first, so the step is reversible. The returned
	 * network is owned by this history's outer chain and is no longer referenced here - the
	 * caller adopts it outright rather than copying it again.
	 */
	URoadNetwork* Undo(const URoadNetwork& Current);

	/** Step forward. Returns the graph to adopt as live, or null. */
	URoadNetwork* Redo(const URoadNetwork& Current);

	bool CanUndo() const { return UndoStack.Num() > 0; }
	bool CanRedo() const { return RedoStack.Num() > 0; }

	int32 UndoDepth() const { return UndoStack.Num(); }
	int32 RedoDepth() const { return RedoStack.Num(); }

	/** Name of the edit the next Undo would take back, or empty. */
	FString PeekUndoLabel() const;
	FString PeekRedoLabel() const;

	/**
	 * Forget everything. For a new level, or a network replaced wholesale.
	 *
	 * CALLS AbandonEdit() FIRST (issue #299) rather than dropping PendingSnapshot/PendingLabel
	 * by hand: a pending edit still open when Clear runs left PendingCharge/PendingQuote behind
	 * before this, which the next edit's CommitEdit would then attach itself to.
	 */
	void Clear();

private:
	/**
	 * Held here, not in the scope guard, so it is GC-rooted for the whole of an edit. A
	 * snapshot referenced only by a stack-local raw pointer is collectable the moment
	 * anything triggers a collection mid-edit.
	 */
	UPROPERTY() TObjectPtr<URoadNetwork> PendingSnapshot = nullptr;

	UPROPERTY() FString PendingLabel;

	/** What the edit in progress paid, and for what. Both move onto the snapshot at CommitEdit. */
	int32 PendingCharge = INDEX_NONE;
	FBuildQuote PendingQuote;

	// One array of pairs, never two parallel arrays keyed by index. An index-parallel
	// invariant with nothing enforcing it is what put an out-of-bounds read into the
	// entity anchors, through ordinary authoring rather than an edge case.
	UPROPERTY() TArray<FRoadEditSnapshot> UndoStack;
	UPROPERTY() TArray<FRoadEditSnapshot> RedoStack;
};

/**
 * Scope guard around one edit. Snapshots on the way in, and on the way out either keeps
 * that snapshot or throws it away depending on whether the edit said it succeeded.
 *
 * The point is that forgetting to record an edit becomes a missing line inside a mutator
 * rather than a silent hole in undo. A mutation that reaches URoadNetwork without passing
 * through one of these is a mutation undo cannot reverse, and nothing reports that.
 *
 * Must not nest: the inner scope would snapshot a half-finished graph. Nothing nests today
 * and an ensure fires if that changes.
 *
 * A SCOPE THAT IS NOT COMMITTED STILL ROLLS NOTHING BACK ON ITS OWN - Rollback() is the explicit
 * way to say "this edit failed after it wrote". That is deliberate (RoadEditHistoryTest pins it):
 * an uncommitted scope is also what a no-op that succeeded looks like, and an implicit restore
 * would spend a whole-network copy and move every revision clock on each of those.
 *
 * ROLLBACK DOES NOT DEPEND ON THE UNDO HISTORY (issue #437). The snapshot a scope takes for undo
 * only exists where there is a history - a game world - and the editor world has none by design
 * (the transaction system owns undo). The scope used to be a Memento with no restore, so
 * "refused after it wrote" was unrepresentable there, and every mutator that could hit it
 * copied the model's own refusal rules ABOVE the scope instead. Now a scope with no history
 * takes a local snapshot of its own, so Rollback() works in both worlds and a mutator may ask the
 * model once and undo on a no.
 */
class AIRSIDE_API FRoadEditScope
{
public:
	/** Network is non-const because Rollback writes it. A null Network makes the scope inert. */
	FRoadEditScope(URoadEditHistory* InHistory, URoadNetwork* InNetwork, const TCHAR* InLabel);
	~FRoadEditScope();

	FRoadEditScope(const FRoadEditScope&) = delete;
	FRoadEditScope& operator=(const FRoadEditScope&) = delete;

	/** Call on the success path, and only there. */
	void Commit() { bCommitted = true; }

	/**
	 * The edit failed after it wrote: put the network back to how it was when this scope began,
	 * bitwise, IN PLACE (URoadNetwork::RestoreFrom), and push no undo step. True when it did.
	 *
	 * NO NOTIFY, deliberately: a plain scope has broadcast nothing before its commit, so no
	 * cache has seen the failed edit and none is stale - and the revision clocks RestoreFrom
	 * moves forward make sure nothing stamped DURING the edit can mistake the restored state
	 * for its own. (A drag is different, it notified per frame - see
	 * URoadEditFacade::RollBackOpenEdit, which does notify.)
	 *
	 * Ends the edit: afterwards the destructor has nothing to commit or abandon. False, changing
	 * nothing, for an inert scope or a second call after the history's edit ended.
	 */
	bool Rollback();

	/**
	 * A copy of Net a rollback can restore from. A scope's local snapshot and the facade's
	 * editor-world drag point both take theirs from here, so a restore point means one thing
	 * however it was taken. Owned by the transient package, never the level.
	 */
	static URoadNetwork* SnapshotForRollback(const URoadNetwork& Net);

private:
	URoadEditHistory* History = nullptr;
	URoadNetwork* Network = nullptr;

	/**
	 * Held only when there is NO history, so a game world does not copy the network twice per
	 * edit (its snapshot is the history's pending one). A strong pointer because the scope is
	 * plain C++ - a raw one is collectable the moment anything triggers a collection mid-edit -
	 * and it lives on the stack, so it is released on the game thread that took it.
	 */
	TStrongObjectPtr<URoadNetwork> LocalSnapshot;

	bool bCommitted = false;
	bool bBegan = false;
};
