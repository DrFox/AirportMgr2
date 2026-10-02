#pragma once

// TEST-ONLY. Reflection-driven answers to two questions about URoadNetwork that a hand-typed list
// cannot answer for itself:
//
//   DifferingProperties - "are these two networks the same MODEL?", bitwise, over every UPROPERTY.
//   MakeEveryPropertyNonDefault - "is there a field this copy would have to remember?", by giving
//   every one a value a fresh network does not have.
//
// WHY REFLECTION, NOT A LIST (issue #318, and the same lesson as CLAUDE.md's "Check where a list is
// CONSUMED"): URoadNetwork::CopyFrom is a hand-maintained list of its fields, and a test that named
// the fields it checked would be a second hand-maintained list, agreeing with the first by luck.
// Walking TFieldIterator<FProperty> follows the class as it grows. A field of a kind the mutator
// below does not know is reported, so a new type of member forces a decision here instead of being
// skipped in silence - a completeness test that skips what it cannot handle is exactly the "green
// test that measures nothing" this project has shipped before.
//
// Identical_InContainer, not operator==: URoadNetwork has none, and per-property identity is what
// the surface model's bitwise-weld contract (CLAUDE.md invariant 1) is stated in - a restored
// network must hand the solver the same values, not values within a tolerance.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Model/LandGrid.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "UObject/UnrealType.h"

namespace NetworkIdentity
{
	/**
	 * The properties the guideline builder RE-DERIVES from the road graph on every Topology notify,
	 * and hands FRESH handles to each time (a rebuilt node is a new slot with a new generation - see
	 * HoldingPositionMarkTest's "the old node handle is dead"). Two networks with identical roads
	 * differ here after any rebuild, so a test that restores a model and then lets the facade notify
	 * compares everything EXCEPT these. A test that restores with no notify in between compares all.
	 */
	inline const TArray<FName>& RederivedOnNotify()
	{
		static const TArray<FName> Derived = {
			TEXT("GuidelineNodes"), TEXT("GuidelineNodeFreeList"), TEXT("GuidelineEdges"),
			TEXT("GuidelineEdgeFreeList"), TEXT("ReverseTurnEnds") };
		return Derived;
	}

	/** Names of every reflected property of URoadNetwork whose value differs between A and B,
	 *  bar those in Ignore. */
	inline TArray<FString> DifferingProperties(const URoadNetwork& A, const URoadNetwork& B,
		const TArray<FName>& Ignore = TArray<FName>())
	{
		TArray<FString> Differing;
		for (TFieldIterator<FProperty> It(URoadNetwork::StaticClass()); It; ++It)
		{
			if (!Ignore.Contains(It->GetFName()) && !It->Identical_InContainer(&A, &B))
			{
				Differing.Add(It->GetName());
			}
		}
		return Differing;
	}

	/** How many reflected properties a network has - the floor a walk that found nothing fails. */
	inline int32 PropertyCount()
	{
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(URoadNetwork::StaticClass()); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	/**
	 * Gives every reflected property of Net a value a fresh network does not hold: one more
	 * (default-built) element on an array, a flipped bool, a number moved by 7, a pointer to a
	 * profile. Returns the properties it has NO rule for - the caller fails on any, because one
	 * left at its default would pass a copy that never touched it.
	 *
	 * Whatever it writes is meaningless as a road network (an element with no handle in a slot
	 * map, a node with generation 0). That is fine here and only here: nothing solves, draws or
	 * searches the result, it is copied and compared.
	 */
	inline TArray<FString> MakeEveryPropertyNonDefault(URoadNetwork& Net)
	{
		TArray<FString> NoRule;
		for (TFieldIterator<FProperty> It(URoadNetwork::StaticClass()); It; ++It)
		{
			FProperty* Property = *It;
			void* Value = Property->ContainerPtrToValuePtr<void>(&Net);
			if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(Array, Value);
				Helper.AddValue();
			}
			else if (FBoolProperty* Flag = CastField<FBoolProperty>(Property))
			{
				Flag->SetPropertyValue(Value, !Flag->GetPropertyValue(Value));
			}
			else if (FEnumProperty* Enum = CastField<FEnumProperty>(Property))
			{
				FNumericProperty* Underlying = Enum->GetUnderlyingProperty();
				Underlying->SetIntPropertyValue(Value, Underlying->GetSignedIntPropertyValue(Value) + 1);
			}
			else if (FNumericProperty* Number = CastField<FNumericProperty>(Property))
			{
				if (Number->IsFloatingPoint())
				{
					Number->SetFloatingPointPropertyValue(Value, Number->GetFloatingPointPropertyValue(Value) + 7.0);
				}
				else
				{
					Number->SetIntPropertyValue(Value, Number->GetSignedIntPropertyValue(Value) + 7);
				}
			}
			else if (FObjectProperty* Object = CastField<FObjectProperty>(Property))
			{
				if (Object->PropertyClass != nullptr && URoadProfile::StaticClass()->IsChildOf(Object->PropertyClass))
				{
					Object->SetObjectPropertyValue(Value, URoadProfile::MakeTransient(1000.0, 500.0));
				}
				else
				{
					NoRule.Add(Property->GetName());
				}
			}
			else if (FStructProperty* Struct = CastField<FStructProperty>(Property); Struct != nullptr && Struct->Struct == FLandGrid::StaticStruct())
			{
				// Owned land (2026-10-02): one more tile is a value no default grid has.
				static_cast<FLandGrid*>(Value)->Owned ^= 1ull;
			}
			else
			{
				NoRule.Add(Property->GetName());
			}
		}
		return NoRule;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
