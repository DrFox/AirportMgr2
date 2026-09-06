#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "UObject/Object.h"
#include "OpsCatalog.generated.h"

/**
 * Every loaded definition, by class and by name. The one place content defaults are
 * resolved from (CLAUDE.md "Content/": a literal asset path at a second site is a second
 * source of truth).
 *
 * World-free: tests Add() NewObject'd definitions. In play, UOpsRuntime calls
 * LoadFromAssetManager once, which needs DefaultGame.ini's PrimaryAssetTypesToScan.
 */
UCLASS()
class AIRPORTOPS_API UOpsCatalog : public UObject
{
	GENERATED_BODY()

public:
	/** Ignores null and duplicates. */
	void Add(UOpsDefinition* Definition);

	template<class T>
	TArray<T*> All() const
	{
		TArray<T*> Out;
		for (const TObjectPtr<UOpsDefinition>& D : Definitions)
		{
			if (T* Typed = Cast<T>(D.Get())) { Out.Add(Typed); }
		}
		return Out;
	}

	template<class T>
	T* Find(FName AssetName) const
	{
		for (const TObjectPtr<UOpsDefinition>& D : Definitions)
		{
			if (D != nullptr && D->GetFName() == AssetName)
			{
				if (T* Typed = Cast<T>(D.Get())) { return Typed; }
			}
		}
		return nullptr;
	}

	int32 Num() const { return Definitions.Num(); }

	/**
	 * Loads every primary asset of every concrete UOpsDefinition subclass synchronously and
	 * Adds it. Returns how many were added. Logs the per-type count: "0 Scenario(s)" in the
	 * log is the answer to "why does the game use the fallback numbers".
	 */
	int32 LoadFromAssetManager();

private:
	/** Transient: definitions are content, re-found from the Asset Manager on every start, never saved. */
	UPROPERTY(Transient) TArray<TObjectPtr<UOpsDefinition>> Definitions;
};
