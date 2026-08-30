#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
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
class ROADNET_API URoadEditHistory : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * How many edits can be taken back. Older snapshots are dropped from the bottom.
	 *
	 * A cap rather than unbounded growth: every entry is a whole graph, so an unbounded
	 * stack grows without limit over a long session.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet", meta = (ClampMin = "1"))
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

	/** Forget everything. For a new level, or a network replaced wholesale. */
	void Clear();

private:
	/**
	 * Held here, not in the scope guard, so it is GC-rooted for the whole of an edit. A
	 * snapshot referenced only by a stack-local raw pointer is collectable the moment
	 * anything triggers a collection mid-edit.
	 */
	UPROPERTY() TObjectPtr<URoadNetwork> PendingSnapshot = nullptr;

	UPROPERTY() FString PendingLabel;

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
 */
class ROADNET_API FRoadEditScope
{
public:
	FRoadEditScope(URoadEditHistory* InHistory, const URoadNetwork* InNetwork, const TCHAR* InLabel);
	~FRoadEditScope();

	FRoadEditScope(const FRoadEditScope&) = delete;
	FRoadEditScope& operator=(const FRoadEditScope&) = delete;

	/** Call on the success path, and only there. */
	void Commit() { bCommitted = true; }

private:
	URoadEditHistory* History = nullptr;
	bool bCommitted = false;
	bool bBegan = false;
};
