#include "UI/UiLayoutStore.h"

TOptional<FUiWindowPlacement> FMemoryUiLayoutStore::Read(FName Id) const
{
	const FUiWindowPlacement* Found = Values.Find(Id);
	return Found != nullptr ? TOptional<FUiWindowPlacement>(*Found) : TOptional<FUiWindowPlacement>();
}

void FMemoryUiLayoutStore::Write(FName Id, const FUiWindowPlacement& Placement)
{
	Values.Add(Id, Placement);
	++WriteCount;
}

void FMemoryUiLayoutStore::Clear()
{
	Values.Reset();
}
