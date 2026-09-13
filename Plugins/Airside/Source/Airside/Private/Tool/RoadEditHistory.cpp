#include "Tool/RoadEditHistory.h"

#include "Model/RoadNetwork.h"

void URoadEditHistory::BeginEdit(const URoadNetwork& Network, const FString& Label)
{
	// Nesting would snapshot a graph that is halfway through an edit, and the outer scope
	// would then commit the inner one's state as though it were the "before". Nothing
	// nests today; this fires loudly rather than quietly recording the wrong thing.
	if (!ensureMsgf(PendingSnapshot == nullptr,
		TEXT("FRoadEditScope nested: '%s' began while '%s' was still open"),
		*Label, *PendingLabel))
	{
		return;
	}

	PendingSnapshot = DuplicateObject<URoadNetwork>(&Network, this);
	PendingLabel = Label;
}

void URoadEditHistory::SetPendingCharge(int32 ChargeId, const FBuildQuote& Quote)
{
	// Only meaningful between BeginEdit and CommitEdit; silently ignored otherwise, which is
	// the editor-world case where undo belongs to the transaction system and nothing was paid.
	if (PendingSnapshot != nullptr)
	{
		PendingCharge = ChargeId;
		PendingQuote = Quote;
	}
}

const FBuildQuote& URoadEditHistory::PeekRedoQuote() const
{
	static const FBuildQuote Free;
	return RedoStack.Num() > 0 ? RedoStack.Last().Quote : Free;
}

void URoadEditHistory::SetUndoTopCharge(int32 ChargeId)
{
	if (UndoStack.Num() > 0)
	{
		UndoStack.Last().ChargeId = ChargeId;
	}
}

int32 URoadEditHistory::PeekUndoChargeId() const
{
	return UndoStack.Num() > 0 ? UndoStack.Last().ChargeId : INDEX_NONE;
}

void URoadEditHistory::CommitEdit()
{
	if (PendingSnapshot == nullptr)
	{
		return;
	}

	// A new edit invalidates the future. Keeping the redo stack across one would let a
	// player redo their way into a graph that was built on a state no longer underneath it.
	RedoStack.Reset();

	FRoadEditSnapshot Entry;
	Entry.Network = PendingSnapshot;
	Entry.Label = PendingLabel;
	Entry.ChargeId = PendingCharge;
	Entry.Quote = PendingQuote;
	UndoStack.Add(Entry);

	PendingSnapshot = nullptr;
	PendingLabel.Reset();
	PendingCharge = INDEX_NONE;

	// Dropped from the bottom: the oldest states are the ones nobody is coming back to.
	const int32 Cap = FMath::Max(MaxDepth, 1);
	if (UndoStack.Num() > Cap)
	{
		UndoStack.RemoveAt(0, UndoStack.Num() - Cap);
	}
}

void URoadEditHistory::AbandonEdit()
{
	// Left unreferenced, so the collector takes it. The stacks are untouched, which is the
	// whole point: a refused edit must not become an undo step that does nothing.
	PendingSnapshot = nullptr;
	PendingLabel.Reset();

	// The charge goes with it. A refused edit was never charged - CanAfford runs before the
	// mutation, not after - so a pending id surviving here could only attach itself to the
	// NEXT edit and let an undo reverse a charge that edit never made.
	PendingCharge = INDEX_NONE;
	PendingQuote = FBuildQuote();
}

URoadNetwork* URoadEditHistory::RevertEdit()
{
	if (PendingSnapshot == nullptr)
	{
		return nullptr;
	}

	// Handed over outright, exactly as Undo does: this history stops referencing it, so nothing
	// later mutates a graph the stacks still believe in. The stacks themselves are untouched -
	// a reverted edit is not an undo step, because as far as the player is concerned it never
	// happened.
	URoadNetwork* Reverted = PendingSnapshot;
	PendingSnapshot = nullptr;
	PendingLabel.Reset();
	PendingCharge = INDEX_NONE;
	PendingQuote = FBuildQuote();
	return Reverted;
}

URoadNetwork* URoadEditHistory::Undo(const URoadNetwork& Current)
{
	if (UndoStack.Num() == 0)
	{
		return nullptr;
	}

	FRoadEditSnapshot Entry = UndoStack.Pop();

	// The state being left becomes the way back. Copied rather than adopted, because the
	// caller still owns Current until it swaps in what this returns.
	FRoadEditSnapshot Forward;
	Forward.Network = DuplicateObject<URoadNetwork>(&Current, this);
	Forward.Label = Entry.Label;
	// The money travels with the step. Redoing this edit charges for it again - see
	// FRoadEditSnapshot::Quote for why leaving it behind would be a money printer.
	Forward.ChargeId = Entry.ChargeId;
	Forward.Quote = Entry.Quote;
	RedoStack.Add(Forward);

	// Handed over outright. This history no longer references it, so nothing later
	// mutates a graph the stacks still believe in.
	return Entry.Network;
}

URoadNetwork* URoadEditHistory::Redo(const URoadNetwork& Current)
{
	if (RedoStack.Num() == 0)
	{
		return nullptr;
	}

	FRoadEditSnapshot Entry = RedoStack.Pop();

	FRoadEditSnapshot Backward;
	Backward.Network = DuplicateObject<URoadNetwork>(&Current, this);
	Backward.Label = Entry.Label;
	// ChargeId is left alone here and written by URoadEditFacade::Redo once it has actually
	// charged: the id on Entry names a ledger entry that has already been reversed.
	Backward.Quote = Entry.Quote;
	UndoStack.Add(Backward);

	return Entry.Network;
}

FString URoadEditHistory::PeekUndoLabel() const
{
	return UndoStack.Num() > 0 ? UndoStack.Last().Label : FString();
}

FString URoadEditHistory::PeekRedoLabel() const
{
	return RedoStack.Num() > 0 ? RedoStack.Last().Label : FString();
}

void URoadEditHistory::Clear()
{
	UndoStack.Reset();
	RedoStack.Reset();
	PendingSnapshot = nullptr;
	PendingLabel.Reset();
}

FRoadEditScope::FRoadEditScope(URoadEditHistory* InHistory, const URoadNetwork* InNetwork, const TCHAR* InLabel)
	: History(InHistory)
{
	if (History != nullptr && InNetwork != nullptr)
	{
		History->BeginEdit(*InNetwork, FString(InLabel));
		bBegan = true;
	}
}

FRoadEditScope::~FRoadEditScope()
{
	if (History == nullptr || !bBegan)
	{
		return;
	}

	if (bCommitted)
	{
		History->CommitEdit();
	}
	else
	{
		History->AbandonEdit();
	}
}
