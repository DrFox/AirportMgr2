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
	UndoStack.Add(Entry);

	PendingSnapshot = nullptr;
	PendingLabel.Reset();

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
