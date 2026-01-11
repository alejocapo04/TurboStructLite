#include "TurboStructLiteQueryLibrary.h"
#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"
#include "TurboStructLiteConstantsQuery.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ConfigCacheIni.h"
#include "Math/NumericLimits.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchive.h"
#if __has_include("Serialization/StructuredArchiveAdapters.h")
#include "Serialization/StructuredArchiveAdapters.h"
#endif
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeBool.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/Compression.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Guid.h"
#if WITH_EDITOR
#include "Trace/Trace.inl"
#endif

bool UTurboStructLiteQueryLibrary::EvaluateLogicQueryNode(const FTurboStructLiteQueryNode& Root, const uint8* RootPtr, const uint8* KeyPtr, const uint8* ValuePtr)
{
	auto ResolvePtr = [&](const FTurboStructLiteQueryBoundProperty& Bound, const uint8*& OutPtr, const FProperty*& OutProp) -> bool
	{
		const uint8* BasePtr = RootPtr;
		if (Bound.Root == ETurboStructLiteQueryValueRoot::MapKey)
		{
			BasePtr = KeyPtr;
		}
		else if (Bound.Root == ETurboStructLiteQueryValueRoot::MapValue)
		{
			BasePtr = ValuePtr;
		}
		if (!BasePtr)
		{
			return false;
		}
		const uint8* CurrentPtr = BasePtr;
		for (FProperty* Prop : Bound.PropertyChain)
		{
			if (!Prop)
			{
				return false;
			}
			CurrentPtr = Prop->ContainerPtrToValuePtr<uint8>(CurrentPtr);
		}
		OutPtr = CurrentPtr;
		OutProp = Bound.LeafProperty;
		return OutProp != nullptr;
	};

	TFunction<bool(const FTurboStructLiteQueryNode&)> EvalNode;
	EvalNode = [&](const FTurboStructLiteQueryNode& Node) -> bool
	{
		switch (Node.Type)
		{
			case ETurboStructLiteQueryNodeType::And:
				return EvalNode(*Node.Left) && EvalNode(*Node.Right);
			case ETurboStructLiteQueryNodeType::Or:
				return EvalNode(*Node.Left) || EvalNode(*Node.Right);
			case ETurboStructLiteQueryNodeType::Not:
				return !EvalNode(*Node.Left);
			case ETurboStructLiteQueryNodeType::Comparison:
				break;
			default:
				return false;
		}

		const FTurboStructLiteQueryComparison& Comparison = Node.Comparison;
		const uint8* ValuePtrResolved = nullptr;
		const FProperty* LeafProp = nullptr;
		if (!ResolvePtr(Comparison.Lhs, ValuePtrResolved, LeafProp))
		{
			return false;
		}

		auto CompareNumeric = [&](double LhsValue, double RhsValue) -> bool
		{
			switch (Comparison.Op)
			{
				case ETurboStructLiteQueryCompareOp::Equal:
					return LhsValue == RhsValue;
				case ETurboStructLiteQueryCompareOp::NotEqual:
					return LhsValue != RhsValue;
				case ETurboStructLiteQueryCompareOp::Greater:
					return LhsValue > RhsValue;
				case ETurboStructLiteQueryCompareOp::Less:
					return LhsValue < RhsValue;
				case ETurboStructLiteQueryCompareOp::GreaterEqual:
					return LhsValue >= RhsValue;
				case ETurboStructLiteQueryCompareOp::LessEqual:
					return LhsValue <= RhsValue;
				default:
					return false;
			}
		};

		auto CompareEqualLiteral = [&](const FProperty* Prop, const void* Ptr) -> bool
		{
			const FTurboStructLiteQueryLiteral& Literal = Comparison.Rhs;
			if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			{
				const bool Value = BoolProp->GetPropertyValue(Ptr);
				return Literal.Type == ETurboStructLiteQueryLiteralType::Boolean && Value == Literal.BoolValue;
			}
			if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
			{
				double Value = NumProp->IsInteger() ? static_cast<double>(NumProp->GetSignedIntPropertyValue(Ptr)) : NumProp->GetFloatingPointPropertyValue(Ptr);
				const double RhsValue = (Literal.Type == ETurboStructLiteQueryLiteralType::Integer) ? static_cast<double>(Literal.IntValue) : Literal.FloatValue;
				return Value == RhsValue;
			}
			if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(Ptr);
				return Literal.Type == ETurboStructLiteQueryLiteralType::Integer && Value == Literal.IntValue;
			}
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				const int64 Value = ByteProp->GetSignedIntPropertyValue(Ptr);
				return Literal.Type == ETurboStructLiteQueryLiteralType::Integer && Value == Literal.IntValue;
			}
			if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			{
				const FString* ValuePtr = StrProp->GetPropertyValuePtr(Ptr);
				if (!ValuePtr)
				{
					return false;
				}
				return Literal.Type == ETurboStructLiteQueryLiteralType::String && *ValuePtr == Literal.StringValue;
			}
			if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
			{
				const FName Value = NameProp->GetPropertyValue(Ptr);
				if (Literal.Type == ETurboStructLiteQueryLiteralType::Name)
				{
					return Value == Literal.NameValue;
				}
				if (Literal.Type == ETurboStructLiteQueryLiteralType::String)
				{
					return Value.ToString() == Literal.StringValue;
				}
			}
			return false;
		};

		if (Comparison.Lhs.bIsContainer)
		{
			if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(LeafProp))
			{
				FScriptArrayHelper Helper(ArrayProp, const_cast<uint8*>(ValuePtrResolved));
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					if (CompareEqualLiteral(ArrayProp->Inner, Helper.GetRawPtr(Index)))
					{
						return true;
					}
				}
				return false;
			}
			if (const FSetProperty* SetProp = CastField<FSetProperty>(LeafProp))
			{
				FScriptSetHelper Helper(SetProp, const_cast<uint8*>(ValuePtrResolved));
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					if (CompareEqualLiteral(SetProp->ElementProp, Helper.GetElementPtr(Index)))
					{
						return true;
					}
				}
				return false;
			}
			if (const FMapProperty* MapProp = CastField<FMapProperty>(LeafProp))
			{
				FScriptMapHelper Helper(MapProp, const_cast<uint8*>(ValuePtrResolved));
				for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					if (CompareEqualLiteral(MapProp->ValueProp, Helper.GetValuePtr(Index)))
					{
						return true;
					}
				}
				return false;
			}
		}

		if (const FStrProperty* StrProp = CastField<FStrProperty>(LeafProp))
		{
			const FString* ValuePtr = StrProp->GetPropertyValuePtr(ValuePtrResolved);
			if (!ValuePtr)
			{
				return false;
			}
			const FString& Value = *ValuePtr;
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::Contains)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::String && Value.Contains(Comparison.Rhs.StringValue, ESearchCase::CaseSensitive);
			}
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::Equal)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::String && Value == Comparison.Rhs.StringValue;
			}
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::NotEqual)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::String && Value != Comparison.Rhs.StringValue;
			}
			return false;
		}
		if (const FNameProperty* NameProp = CastField<FNameProperty>(LeafProp))
		{
			const FName Value = NameProp->GetPropertyValue(ValuePtrResolved);
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::Equal)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Name && Value == Comparison.Rhs.NameValue;
			}
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::NotEqual)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Name && Value != Comparison.Rhs.NameValue;
			}
			return false;
		}
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(LeafProp))
		{
			const bool Value = BoolProp->GetPropertyValue(ValuePtrResolved);
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::Equal)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Boolean && Value == Comparison.Rhs.BoolValue;
			}
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::NotEqual)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Boolean && Value != Comparison.Rhs.BoolValue;
			}
			return false;
		}
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(LeafProp))
		{
			const double Value = NumProp->IsInteger() ? static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtrResolved)) : NumProp->GetFloatingPointPropertyValue(ValuePtrResolved);
			const double RhsValue = (Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Integer) ? static_cast<double>(Comparison.Rhs.IntValue) : Comparison.Rhs.FloatValue;
			return CompareNumeric(Value, RhsValue);
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(LeafProp))
		{
			const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtrResolved);
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::Equal)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Integer && Value == Comparison.Rhs.IntValue;
			}
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::NotEqual)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Integer && Value != Comparison.Rhs.IntValue;
			}
			return false;
		}
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(LeafProp))
		{
			const int64 Value = ByteProp->GetSignedIntPropertyValue(ValuePtrResolved);
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::Equal)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Integer && Value == Comparison.Rhs.IntValue;
			}
			if (Comparison.Op == ETurboStructLiteQueryCompareOp::NotEqual)
			{
				return Comparison.Rhs.Type == ETurboStructLiteQueryLiteralType::Integer && Value != Comparison.Rhs.IntValue;
			}
			return false;
		}
		return false;
	};

	return EvalNode(Root);
}

bool UTurboStructLiteQueryLibrary::ApplyLogicFilter(FProperty* DataProp, const TSharedPtr<FTurboStructLiteQueryNode>& Root, const TArray<uint8>& SourceValueBuffer, TArray<uint8>& OutValueBuffer, FTurboStructLiteLogicQueryStats& OutStats, FString& OutErrorMessage)
{
	OutStats = FTurboStructLiteLogicQueryStats();
	OutErrorMessage.Reset();
	if (!DataProp || !Root.IsValid())
	{
		OutErrorMessage = TEXT("Type Error: Invalid data context");
		return false;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(DataProp))
	{
		OutStats.Scanned = 1;
		const uint8* BasePtr = SourceValueBuffer.GetData();
		const bool bMatch = EvaluateLogicQueryNode(*Root, BasePtr, nullptr, nullptr);
		OutStats.Matched = bMatch ? 1 : 0;
		const int32 ValueSize = StructProp->GetSize();
		OutValueBuffer.SetNumUninitialized(ValueSize);
		StructProp->InitializeValue(OutValueBuffer.GetData());
		if (bMatch)
		{
			StructProp->CopyCompleteValue(OutValueBuffer.GetData(), BasePtr);
		}
		return true;
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp))
	{
		FScriptArrayHelper SourceHelper(ArrayProp, const_cast<uint8*>(SourceValueBuffer.GetData()));
		const int32 Num = SourceHelper.Num();
		OutStats.Scanned = Num;
		const int32 ValueSize = ArrayProp->GetSize();
		OutValueBuffer.SetNumUninitialized(ValueSize);
		ArrayProp->InitializeValue(OutValueBuffer.GetData());
		FScriptArrayHelper OutHelper(ArrayProp, OutValueBuffer.GetData());

		const int32 MaxThreads = FMath::Clamp(UTurboStructLiteBPLibrary::GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		const int32 ElementSize = ArrayProp->Inner->GetSize();
		const int64 EstimatedBytes = static_cast<int64>(Num) * ElementSize;
		const bool bParallel = EstimatedBytes >= TurboStructLiteQueryParallelMinBytes && Num > 1 && MaxThreads > 1;
		if (bParallel)
		{
			const int32 TaskCount = FMath::Min(MaxThreads, Num);
			TArray<FTurboStructLiteThreadResultBucket> Buckets;
			Buckets.SetNum(TaskCount);
			const int32 ItemsPerBucket = FMath::DivideAndRoundUp(Num, TaskCount);
			for (int32 Index = 0; Index < TaskCount; ++Index)
			{
				Buckets[Index].Indices.Reserve(ItemsPerBucket);
			}

			ParallelFor(TaskCount, [&](int32 TaskIndex)
			{
				const int32 Start = TaskIndex * ItemsPerBucket;
				const int32 End = FMath::Min(Start + ItemsPerBucket, Num);
				for (int32 Index = Start; Index < End; ++Index)
				{
					const uint8* ElemPtr = SourceHelper.GetRawPtr(Index);
					if (EvaluateLogicQueryNode(*Root, ElemPtr, nullptr, nullptr))
					{
						Buckets[TaskIndex].Indices.Add(Index);
					}
				}
			}, EParallelForFlags::Unbalanced);

			for (const FTurboStructLiteThreadResultBucket& Bucket : Buckets)
			{
				for (int32 Index : Bucket.Indices)
				{
					const int32 NewIndex = OutHelper.AddValue();
					ArrayProp->Inner->CopyCompleteValue(OutHelper.GetRawPtr(NewIndex), SourceHelper.GetRawPtr(Index));
					OutStats.Matched++;
				}
			}
			return true;
		}

		for (int32 Index = 0; Index < Num; ++Index)
		{
			const uint8* ElemPtr = SourceHelper.GetRawPtr(Index);
			if (EvaluateLogicQueryNode(*Root, ElemPtr, nullptr, nullptr))
			{
				const int32 NewIndex = OutHelper.AddValue();
				ArrayProp->Inner->CopyCompleteValue(OutHelper.GetRawPtr(NewIndex), SourceHelper.GetRawPtr(Index));
				OutStats.Matched++;
			}
		}
		return true;
	}

	if (FSetProperty* SetProp = CastField<FSetProperty>(DataProp))
	{
		FScriptSetHelper SourceHelper(SetProp, const_cast<uint8*>(SourceValueBuffer.GetData()));
		const int32 ValueSize = SetProp->GetSize();
		OutValueBuffer.SetNumUninitialized(ValueSize);
		SetProp->InitializeValue(OutValueBuffer.GetData());
		FScriptSetHelper OutHelper(SetProp, OutValueBuffer.GetData());

		TArray<int32> ValidIndices;
		ValidIndices.Reserve(SourceHelper.Num());
		for (int32 Index = 0; Index < SourceHelper.GetMaxIndex(); ++Index)
		{
			if (SourceHelper.IsValidIndex(Index))
			{
				ValidIndices.Add(Index);
			}
		}
		OutStats.Scanned = ValidIndices.Num();

		const int32 MaxThreads = FMath::Clamp(UTurboStructLiteBPLibrary::GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		const int32 ElementSize = SetProp->ElementProp->GetSize();
		const int64 EstimatedBytes = static_cast<int64>(ValidIndices.Num()) * ElementSize;
		const bool bParallel = EstimatedBytes >= TurboStructLiteQueryParallelMinBytes && ValidIndices.Num() > 1 && MaxThreads > 1;
		if (bParallel)
		{
			const int32 TaskCount = FMath::Min(MaxThreads, ValidIndices.Num());
			TArray<FTurboStructLiteThreadResultBucket> Buckets;
			Buckets.SetNum(TaskCount);
			const int32 ItemsPerBucket = FMath::DivideAndRoundUp(ValidIndices.Num(), TaskCount);
			for (int32 Index = 0; Index < TaskCount; ++Index)
			{
				Buckets[Index].Indices.Reserve(ItemsPerBucket);
			}

			ParallelFor(TaskCount, [&](int32 TaskIndex)
			{
				const int32 Start = TaskIndex * ItemsPerBucket;
				const int32 End = FMath::Min(Start + ItemsPerBucket, ValidIndices.Num());
				for (int32 LocalIndex = Start; LocalIndex < End; ++LocalIndex)
				{
					const int32 SourceIndex = ValidIndices[LocalIndex];
					const uint8* ElemPtr = SourceHelper.GetElementPtr(SourceIndex);
					if (EvaluateLogicQueryNode(*Root, ElemPtr, nullptr, nullptr))
					{
						Buckets[TaskIndex].Indices.Add(SourceIndex);
					}
				}
			}, EParallelForFlags::Unbalanced);

			for (const FTurboStructLiteThreadResultBucket& Bucket : Buckets)
			{
				for (int32 SourceIndex : Bucket.Indices)
				{
					OutHelper.AddElement(SourceHelper.GetElementPtr(SourceIndex));
					OutStats.Matched++;
				}
			}
			return true;
		}

		for (int32 Index : ValidIndices)
		{
			const uint8* ElemPtr = SourceHelper.GetElementPtr(Index);
			if (EvaluateLogicQueryNode(*Root, ElemPtr, nullptr, nullptr))
			{
				OutHelper.AddElement(SourceHelper.GetElementPtr(Index));
				OutStats.Matched++;
			}
		}
		return true;
	}

	if (FMapProperty* MapProp = CastField<FMapProperty>(DataProp))
	{
		FScriptMapHelper SourceHelper(MapProp, const_cast<uint8*>(SourceValueBuffer.GetData()));
		const int32 ValueSize = MapProp->GetSize();
		OutValueBuffer.SetNumUninitialized(ValueSize);
		MapProp->InitializeValue(OutValueBuffer.GetData());
		FScriptMapHelper OutHelper(MapProp, OutValueBuffer.GetData());

		TArray<int32> ValidIndices;
		ValidIndices.Reserve(SourceHelper.Num());
		for (int32 Index = 0; Index < SourceHelper.GetMaxIndex(); ++Index)
		{
			if (SourceHelper.IsValidIndex(Index))
			{
				ValidIndices.Add(Index);
			}
		}
		OutStats.Scanned = ValidIndices.Num();

		const int32 MaxThreads = FMath::Clamp(UTurboStructLiteBPLibrary::GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		const int32 ElementSize = MapProp->KeyProp->GetSize() + MapProp->ValueProp->GetSize();
		const int64 EstimatedBytes = static_cast<int64>(ValidIndices.Num()) * ElementSize;
		const bool bParallel = EstimatedBytes >= TurboStructLiteQueryParallelMinBytes && ValidIndices.Num() > 1 && MaxThreads > 1;
		if (bParallel)
		{
			const int32 TaskCount = FMath::Min(MaxThreads, ValidIndices.Num());
			TArray<FTurboStructLiteThreadResultBucket> Buckets;
			Buckets.SetNum(TaskCount);
			const int32 ItemsPerBucket = FMath::DivideAndRoundUp(ValidIndices.Num(), TaskCount);
			for (int32 Index = 0; Index < TaskCount; ++Index)
			{
				Buckets[Index].Indices.Reserve(ItemsPerBucket);
			}

			ParallelFor(TaskCount, [&](int32 TaskIndex)
			{
				const int32 Start = TaskIndex * ItemsPerBucket;
				const int32 End = FMath::Min(Start + ItemsPerBucket, ValidIndices.Num());
				for (int32 LocalIndex = Start; LocalIndex < End; ++LocalIndex)
				{
					const int32 SourceIndex = ValidIndices[LocalIndex];
					const uint8* KeyPtr = SourceHelper.GetKeyPtr(SourceIndex);
					const uint8* ValuePtr = SourceHelper.GetValuePtr(SourceIndex);
					const uint8* RootPtr = ValuePtr;
					if (EvaluateLogicQueryNode(*Root, RootPtr, KeyPtr, ValuePtr))
					{
						Buckets[TaskIndex].Indices.Add(SourceIndex);
					}
				}
			}, EParallelForFlags::Unbalanced);

			for (const FTurboStructLiteThreadResultBucket& Bucket : Buckets)
			{
				for (int32 SourceIndex : Bucket.Indices)
				{
					OutHelper.AddPair(SourceHelper.GetKeyPtr(SourceIndex), SourceHelper.GetValuePtr(SourceIndex));
					OutStats.Matched++;
				}
			}
			return true;
		}

		for (int32 Index : ValidIndices)
		{
			const uint8* KeyPtr = SourceHelper.GetKeyPtr(Index);
			const uint8* ValuePtr = SourceHelper.GetValuePtr(Index);
			const uint8* RootPtr = ValuePtr;
			if (EvaluateLogicQueryNode(*Root, RootPtr, KeyPtr, ValuePtr))
			{
				OutHelper.AddPair(SourceHelper.GetKeyPtr(Index), SourceHelper.GetValuePtr(Index));
				OutStats.Matched++;
			}
		}
		return true;
	}

	OutErrorMessage = TEXT("Type Error: Unsupported data container for logic query");
	return false;
}

bool UTurboStructLiteQueryLibrary::DeserializeLogicValue(FProperty* DataProp, const TArray<uint8>& RawBytes, int32 MaxThreads, TArray<uint8>& OutValueBuffer, FString& OutErrorMessage)
{
	OutValueBuffer.Reset();
	OutErrorMessage.Reset();
	if (!DataProp)
	{
		OutErrorMessage = TEXT("Type Error: Invalid data property");
		return false;
	}
	const int32 ValueSize = DataProp->GetSize();
	if (ValueSize <= 0)
	{
		OutErrorMessage = TEXT("Type Error: Invalid data size");
		return false;
	}
	OutValueBuffer.SetNumUninitialized(ValueSize);
	DataProp->InitializeValue(OutValueBuffer.GetData());
	if (!UTurboStructLiteBPLibrary::DeserializePropertyWithMeta(DataProp, OutValueBuffer.GetData(), RawBytes, MaxThreads))
	{
		DataProp->DestroyValue(OutValueBuffer.GetData());
		OutValueBuffer.Reset();
		OutErrorMessage = TEXT("IO Error: Failed to deserialize data");
		return false;
	}
	return true;
}

FString UTurboStructLiteQueryLibrary::FormatLogicStats(const FTurboStructLiteLogicQueryStats& Stats)
{
	return FString::Printf(TEXT("Scanned: %d, Matched: %d, Time: %.2fms"), Stats.Scanned, Stats.Matched, Stats.ElapsedMs);
}

bool UTurboStructLiteQueryLibrary::ApplyRowToStruct(const FTurboStructLiteRow& Row, UStruct* Struct, void* StructPtr, FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (!Struct || !StructPtr)
	{
		OutErrorMessage = TEXT("Type Error: Invalid struct output");
		return false;
	}
	auto FindPropertyByName = [&](UStruct* InStruct, const FString& Segment) -> FProperty*
	{
		if (!InStruct)
		{
			return nullptr;
		}
		FProperty* Found = FindFProperty<FProperty>(InStruct, *Segment);
		if (Found)
		{
			return Found;
		}
		FProperty* Match = nullptr;
		for (TFieldIterator<FProperty> It(InStruct); It; ++It)
		{
			FProperty* Prop = *It;
			const FString DisplayName = Prop->GetDisplayNameText().ToString();
			if (DisplayName.Equals(Segment, ESearchCase::IgnoreCase))
			{
				if (Match)
				{
					return nullptr;
				}
				Match = Prop;
			}
		}
		return Match;
	};
	for (const TPair<FName, FTurboStructLiteVariant>& Pair : Row.Columns)
	{
		FString ColumnName = Pair.Key.ToString();
		ColumnName.TrimStartAndEndInline();
		if (ColumnName.IsEmpty())
		{
			continue;
		}
		TArray<FString> Segments;
		ColumnName.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			continue;
		}
		UStruct* CurrentStruct = Struct;
		uint8* CurrentPtr = static_cast<uint8*>(StructPtr);
		FProperty* TargetProp = nullptr;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Found = FindPropertyByName(CurrentStruct, Segments[Index]);
			if (!Found)
			{
				TargetProp = nullptr;
				break;
			}
			CurrentPtr = Found->ContainerPtrToValuePtr<uint8>(CurrentPtr);
			if (Index < Segments.Num() - 1)
			{
				FStructProperty* NextStruct = CastField<FStructProperty>(Found);
				if (!NextStruct)
				{
					TargetProp = nullptr;
					break;
				}
				CurrentStruct = NextStruct->Struct;
			}
			else
			{
				TargetProp = Found;
			}
		}
		if (!TargetProp || !CurrentPtr)
		{
			continue;
		}
		if (!UTurboStructLiteBPLibrary::TryApplyVariantToProperty(TargetProp, CurrentPtr, Pair.Value))
		{
			OutErrorMessage = TEXT("Type Error: Failed to apply query result");
			return false;
		}
	}
	return true;
}
bool UTurboStructLiteQueryLibrary::ApplyRowsToOutput(const TArray<FTurboStructLiteRow>& Rows, FProperty* OutputProp, void* OutputPtr, FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (!OutputProp || !OutputPtr)
	{
		OutErrorMessage = TEXT("Type Error: Invalid output");
		return false;
	}
	if (FStructProperty* StructProp = CastField<FStructProperty>(OutputProp))
	{
		TArray<uint8> TempValue;
		TempValue.SetNumUninitialized(StructProp->GetSize());
		StructProp->InitializeValue(TempValue.GetData());
		if (Rows.Num() > 0)
		{
			if (!ApplyRowToStruct(Rows[0], StructProp->Struct, TempValue.GetData(), OutErrorMessage))
			{
				StructProp->DestroyValue(TempValue.GetData());
				return false;
			}
		}
		StructProp->CopyCompleteValue(OutputPtr, TempValue.GetData());
		StructProp->DestroyValue(TempValue.GetData());
		return true;
	}
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(OutputProp))
	{
		FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);
		if (!InnerStruct)
		{
			OutErrorMessage = TEXT("Type Error: Array output must be struct");
			return false;
		}
		ArrayProp->DestroyValue(OutputPtr);
		ArrayProp->InitializeValue(OutputPtr);
		FScriptArrayHelper Helper(ArrayProp, OutputPtr);
		for (const FTurboStructLiteRow& Row : Rows)
		{
			TArray<uint8> TempValue;
			TempValue.SetNumUninitialized(InnerStruct->GetSize());
			InnerStruct->InitializeValue(TempValue.GetData());
			if (!ApplyRowToStruct(Row, InnerStruct->Struct, TempValue.GetData(), OutErrorMessage))
			{
				InnerStruct->DestroyValue(TempValue.GetData());
				return false;
			}
			const int32 NewIndex = Helper.AddValue();
			ArrayProp->Inner->CopyCompleteValue(Helper.GetRawPtr(NewIndex), TempValue.GetData());
			InnerStruct->DestroyValue(TempValue.GetData());
		}
		return true;
	}
	if (FSetProperty* SetProp = CastField<FSetProperty>(OutputProp))
	{
		FStructProperty* InnerStruct = CastField<FStructProperty>(SetProp->ElementProp);
		if (!InnerStruct)
		{
			OutErrorMessage = TEXT("Type Error: Set output must be struct");
			return false;
		}
		SetProp->DestroyValue(OutputPtr);
		SetProp->InitializeValue(OutputPtr);
		FScriptSetHelper Helper(SetProp, OutputPtr);
		for (const FTurboStructLiteRow& Row : Rows)
		{
			TArray<uint8> TempValue;
			TempValue.SetNumUninitialized(InnerStruct->GetSize());
			InnerStruct->InitializeValue(TempValue.GetData());
			if (!ApplyRowToStruct(Row, InnerStruct->Struct, TempValue.GetData(), OutErrorMessage))
			{
				InnerStruct->DestroyValue(TempValue.GetData());
				return false;
			}
			Helper.AddElement(TempValue.GetData());
			InnerStruct->DestroyValue(TempValue.GetData());
		}
		return true;
	}
	if (CastField<FMapProperty>(OutputProp))
	{
		OutErrorMessage = TEXT("Type Error: Select projection is not supported for map outputs");
		return false;
	}
	OutErrorMessage = TEXT("Type Error: Invalid output type for select query");
	return false;
}
bool UTurboStructLiteQueryLibrary::ApplyAggregateToOutput(const TArray<FTurboStructLiteRow>& Rows, FProperty* OutputProp, void* OutputPtr, FString& OutErrorMessage)
{
	OutErrorMessage.Reset();
	if (!OutputProp || !OutputPtr)
	{
		OutErrorMessage = TEXT("Type Error: Invalid output");
		return false;
	}
	const bool bNumeric = CastField<FNumericProperty>(OutputProp) || CastField<FEnumProperty>(OutputProp);
	if (!bNumeric)
	{
		OutErrorMessage = TEXT("Type Error: Aggregate output must be numeric");
		return false;
	}
	if (Rows.Num() == 0)
	{
		OutErrorMessage = TEXT("IO Error: Aggregate result is empty");
		return false;
	}
	const FTurboStructLiteRow& Row = Rows[0];
	if (Row.Columns.Num() != 1)
	{
		OutErrorMessage = TEXT("Type Error: Aggregate output requires a single value");
		return false;
	}
	const FTurboStructLiteVariant& Variant = Row.Columns.CreateConstIterator().Value();
	if (!UTurboStructLiteBPLibrary::TryApplyVariantToProperty(OutputProp, OutputPtr, Variant))
	{
		OutErrorMessage = TEXT("Type Error: Failed to apply aggregate output");
		return false;
	}
	return true;
}

bool UTurboStructLiteQueryLibrary::ExecuteSelectQueryToValue(const FString& SlotName, int32 SubSlotIndex, const FString& QueryString, const FString& EncryptionKey, ETurboStructLiteEncryption SelectedEncryption, int32 MaxParallelThreads, bool bUseWriteAheadLog, const FString& WALPath, FProperty* OutputProp, TArray<uint8>& OutValue, FString& OutMetadata, FDateTime& OutSaveDate, FString& OutStatsText, FString& OutErrorMessage)
{
	OutValue.Reset();
	OutMetadata.Reset();
	OutSaveDate = FDateTime(0);
	OutStatsText.Reset();
	OutErrorMessage.Reset();
	if (!OutputProp)
	{
		OutErrorMessage = TEXT("Type Error: Invalid output");
		return false;
	}
	UStruct* ContextStruct = nullptr;
	if (!ResolveContextStructFromSlot(SlotName, SubSlotIndex, EncryptionKey, SelectedEncryption, ContextStruct, OutErrorMessage))
	{
		return false;
	}
	bool bHasAggregates = false;
	TArray<FTurboStructLiteRow> Rows;
	if (!ExecuteSelectQuery(SlotName, SubSlotIndex, QueryString, EncryptionKey, SelectedEncryption, MaxParallelThreads, bUseWriteAheadLog, WALPath, ContextStruct, bHasAggregates, Rows, OutMetadata, OutSaveDate, OutStatsText, OutErrorMessage))
	{
		return false;
	}
	const int32 ValueSize = OutputProp->GetSize();
	if (ValueSize <= 0)
	{
		OutErrorMessage = TEXT("Type Error: Invalid output size");
		return false;
	}
	OutValue.SetNumUninitialized(ValueSize);
	OutputProp->InitializeValue(OutValue.GetData());
	const bool bApplied = bHasAggregates ? ApplyAggregateToOutput(Rows, OutputProp, OutValue.GetData(), OutErrorMessage) : ApplyRowsToOutput(Rows, OutputProp, OutValue.GetData(), OutErrorMessage);
	if (!bApplied)
	{
		OutputProp->DestroyValue(OutValue.GetData());
		OutValue.Reset();
		return false;
	}
	return true;
}

bool UTurboStructLiteQueryLibrary::ExecuteSelectQuery(const FString& SlotName, int32 SubSlotIndex, const FString& QueryString, const FString& EncryptionKey, ETurboStructLiteEncryption SelectedEncryption, int32 MaxParallelThreads, bool bUseWriteAheadLog, const FString& WALPath, UStruct* ContextStruct, bool& bOutHasAggregates, TArray<FTurboStructLiteRow>& OutRows, FString& OutMetadata, FDateTime& OutSaveDate, FString& OutStatsText, FString& OutErrorMessage)
{
	OutRows.Reset();
	OutMetadata.Reset();
	OutSaveDate = FDateTime(0);
	OutStatsText.Reset();
	OutErrorMessage.Reset();
	bOutHasAggregates = false;
	if (!ContextStruct)
	{
		OutErrorMessage = TEXT("Type Error: Invalid query context");
		return false;
	}
	TArray<FString> EmptySelectFields;
	FString ParsedQueryString;
	TArray<FString> ParsedSelectFields;
	int32 ParsedLimit = 0;
	int32 ParsedOffset = 0;
	FString ParsedOrderBy;
	bool bParsedOrderDesc = false;
	TArray<ETurboStructLiteAggregateOp> ParsedAggregateOps;
	TArray<FString> ParsedAggregateFields;
	TArray<FName> ParsedAggregateColumns;
	if (!ParseSelectQueryString(QueryString, EmptySelectFields, ParsedQueryString, ParsedSelectFields, ParsedLimit, ParsedOffset, ParsedOrderBy, bParsedOrderDesc, ParsedAggregateOps, ParsedAggregateFields, ParsedAggregateColumns, OutErrorMessage))
	{
		return false;
	}
	const bool bHasAggregates = ParsedAggregateOps.Num() > 0;
	bOutHasAggregates = bHasAggregates;
	const bool bHasOrderBy = !ParsedOrderBy.IsEmpty();
	FString TrimmedQuery = ParsedQueryString;
	TrimmedQuery.TrimStartAndEndInline();
	const bool bQueryIsTrue = TrimmedQuery.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	bool bAggregateCountOnly = bHasAggregates;
	if (bAggregateCountOnly)
	{
		for (const ETurboStructLiteAggregateOp Op : ParsedAggregateOps)
		{
			if (Op != ETurboStructLiteAggregateOp::Count)
			{
				bAggregateCountOnly = false;
				break;
			}
		}
	}
	TUniquePtr<FStructProperty> ContextStructProp;
	UScriptStruct* ScriptStruct = Cast<UScriptStruct>(ContextStruct);
	if (!ScriptStruct)
	{
		OutErrorMessage = TEXT("Type Error: Context struct is invalid");
		return false;
	}
	ContextStructProp = MakeUnique<FStructProperty>(FFieldVariant(), NAME_None, RF_NoFlags);
	ContextStructProp->Struct = ScriptStruct;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	ContextStructProp->SetElementSize(ScriptStruct->GetStructureSize());
#else
	ContextStructProp->ElementSize = ScriptStruct->GetStructureSize();
#endif
	FTurboStructLiteLogicQueryContext QueryContext;
	if (!BuildLogicQueryContext(ContextStructProp.Get(), QueryContext, OutErrorMessage))
	{
		return false;
	}
	TArray<FTurboStructLiteQueryToken> Tokens;
	int32 ErrorPos = 0;
	if (!TokenizeLogicQuery(ParsedQueryString, Tokens, OutErrorMessage, ErrorPos))
	{
		return false;
	}
	TSharedPtr<FTurboStructLiteQueryNode> Root;
	if (!ParseLogicQuery(Tokens, Root, OutErrorMessage, ErrorPos))
	{
		return false;
	}
	if (!BindLogicQuery(Root, QueryContext, OutErrorMessage, ErrorPos))
	{
		return false;
	}
	if (ParsedSelectFields.IsEmpty() && !bHasAggregates)
	{
		if (QueryContext.RootStruct)
		{
			for (TFieldIterator<FProperty> It(QueryContext.RootStruct); It; ++It)
			{
				ParsedSelectFields.Add(It->GetName());
			}
		}
	}
	TArray<FTurboStructLiteSelectFieldInfo> SelectFieldInfos;
	if (!bHasAggregates)
	{
		if (!BuildSelectFieldInfos(ParsedSelectFields, QueryContext.RootStruct, SelectFieldInfos, OutErrorMessage))
		{
			return false;
		}
		if (SelectFieldInfos.Num() == 0)
		{
			OutErrorMessage = TEXT("Type Error: No valid select fields");
			return false;
		}
	}
	FTurboStructLiteSelectFieldInfo OrderFieldInfo;
	if (bHasOrderBy)
	{
		TArray<FString> OrderFields;
		OrderFields.Add(ParsedOrderBy);
		TArray<FTurboStructLiteSelectFieldInfo> OrderFieldInfos;
		if (!BuildSelectFieldInfos(OrderFields, QueryContext.RootStruct, OrderFieldInfos, OutErrorMessage))
		{
			return false;
		}
		if (OrderFieldInfos.Num() == 0)
		{
			OutErrorMessage = TEXT("Type Error: Invalid ORDER BY field");
			return false;
		}
		OrderFieldInfo = MoveTemp(OrderFieldInfos[0]);
	}
	TArray<FTurboStructLiteSelectFieldInfo> AggregateFieldInfos;
	TArray<int32> AggregateFieldIndices;
	if (bHasAggregates)
	{
		TArray<FString> AggregatePaths;
		for (int32 Index = 0; Index < ParsedAggregateOps.Num(); ++Index)
		{
			if (ParsedAggregateOps[Index] != ETurboStructLiteAggregateOp::Count)
			{
				const FString& FieldPath = ParsedAggregateFields[Index];
				if (!FieldPath.IsEmpty())
				{
					AggregatePaths.AddUnique(FieldPath);
				}
			}
		}
		if (AggregatePaths.Num() > 0)
		{
			if (!BuildSelectFieldInfos(AggregatePaths, QueryContext.RootStruct, AggregateFieldInfos, OutErrorMessage))
			{
				return false;
			}
		}
		TMap<FName, int32> AggregateIndexMap;
		for (int32 Index = 0; Index < AggregateFieldInfos.Num(); ++Index)
		{
			AggregateIndexMap.Add(AggregateFieldInfos[Index].ColumnName, Index);
		}
		AggregateFieldIndices.Init(INDEX_NONE, ParsedAggregateOps.Num());
		for (int32 Index = 0; Index < ParsedAggregateOps.Num(); ++Index)
		{
			if (ParsedAggregateOps[Index] != ETurboStructLiteAggregateOp::Count)
			{
				const FName FieldName = FName(*ParsedAggregateFields[Index]);
				const int32* FoundIndex = AggregateIndexMap.Find(FieldName);
				if (!FoundIndex)
				{
					OutErrorMessage = TEXT("Type Error: Invalid aggregate field");
					return false;
				}
				AggregateFieldIndices[Index] = *FoundIndex;
			}
		}
	}
	const int32 ClampedParallel = FMath::Clamp(MaxParallelThreads, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	const double StartSeconds = FPlatformTime::Seconds();
	FTurboStructLiteLogicQueryStats Stats;
	TArray<FTurboStructLiteRow> Results;
	TArray<double> SortKeys;
	TArray<int64> AggregateCounts;
	TArray<double> AggregateSums;
	const bool bHasLimit = ParsedLimit > 0;
	const bool bHasOffset = ParsedOffset > 0;
	const bool bForceSingleThread = (bHasLimit || bHasOffset) && !bHasAggregates;
	const bool bAllowEarlyExit = bForceSingleThread && bHasLimit && !bHasOrderBy;
	const int32 MaxMatchIndex = bHasLimit ? ParsedOffset + ParsedLimit : 0;
	bool bOffsetAppliedInLoop = false;
	bool bLimitAppliedInLoop = false;
	if (bHasAggregates)
	{
		AggregateCounts.Init(0, ParsedAggregateOps.Num());
		AggregateSums.Init(0.0, ParsedAggregateOps.Num());
	}
	int64 FastCountValue = 0;
	auto ResolvePropertyPtr = [](const TArray<FProperty*>& Chain, uint8* BasePtr) -> uint8*
	{
		uint8* CurrentPtr = BasePtr;
		for (FProperty* Prop : Chain)
		{
			if (!Prop || !CurrentPtr)
			{
				return nullptr;
			}
			CurrentPtr = Prop->ContainerPtrToValuePtr<uint8>(CurrentPtr);
		}
		return CurrentPtr;
	};
	auto TryGetNumericValue = [](FProperty* Property, uint8* ValuePtr, bool bCountOnly, bool bAllowString, double& OutValue) -> bool
	{
		if (bCountOnly)
		{
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property);
			if (!ArrayProp)
			{
				return false;
			}
			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			OutValue = static_cast<double>(Helper.Num());
			return true;
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
			if (!Underlying)
			{
				return false;
			}
			if (Underlying->IsFloatingPoint())
			{
				OutValue = Underlying->GetFloatingPointPropertyValue(ValuePtr);
			}
			else if (CastField<FByteProperty>(Underlying) || CastField<FUInt16Property>(Underlying) || CastField<FUInt32Property>(Underlying) || CastField<FUInt64Property>(Underlying))
			{
				OutValue = static_cast<double>(Underlying->GetUnsignedIntPropertyValue(ValuePtr));
			}
			else
			{
				OutValue = static_cast<double>(Underlying->GetSignedIntPropertyValue(ValuePtr));
			}
			return true;
		}
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
		{
			if (NumProp->IsFloatingPoint())
			{
				OutValue = NumProp->GetFloatingPointPropertyValue(ValuePtr);
			}
			else if (CastField<FByteProperty>(NumProp) || CastField<FUInt16Property>(NumProp) || CastField<FUInt32Property>(NumProp) || CastField<FUInt64Property>(NumProp))
			{
				OutValue = static_cast<double>(NumProp->GetUnsignedIntPropertyValue(ValuePtr));
			}
			else
			{
				OutValue = static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr));
			}
			return true;
		}
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			OutValue = BoolProp->GetPropertyValue(ValuePtr) ? 1.0 : 0.0;
			return true;
		}
		if (bAllowString)
		{
			if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
			{
				const FString Value = StrProp->GetPropertyValue(ValuePtr);
				return LexTryParseString(OutValue, *Value);
			}
			if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
			{
				const FString Value = NameProp->GetPropertyValue(ValuePtr).ToString();
				return LexTryParseString(OutValue, *Value);
			}
			if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
			{
				const FString Value = TextProp->GetPropertyValue(ValuePtr).ToString();
				return LexTryParseString(OutValue, *Value);
			}
		}
		return false;
	};
	const bool bFastCount = bAggregateCountOnly && bQueryIsTrue;
	TArray<int32> SubSlots;
	if (!bFastCount)
	{
		if (SubSlotIndex == -1)
		{
			if (!UTurboStructLiteBPLibrary::ListSubSlotIndices(SlotName, SubSlots) || SubSlots.Num() == 0)
			{
				OutErrorMessage = TEXT("IO Error: No subslots found");
				return false;
			}
		}
		else
		{
			SubSlots.Add(SubSlotIndex);
		}
	}
	else
	{
		if (SubSlotIndex == -1)
		{
			const FString FilePath = UTurboStructLiteBPLibrary::BuildSavePath(SlotName);
			TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
			int32 FileVersion = 0;
			int32 EntryCount = 0;
			if (!Reader || !UTurboStructLiteBPLibrary::ReadHeaderAndEntryCount(*Reader, UTurboStructLiteBPLibrary::GetMagic(), FileVersion, EntryCount))
			{
				OutErrorMessage = TEXT("IO Error: Load failed");
				return false;
			}
			FastCountValue = EntryCount;
		}
		else
		{
			FastCountValue = UTurboStructLiteBPLibrary::ExistsEntry(SlotName, SubSlotIndex) ? 1 : 0;
		}
		if (FastCountValue > 0)
		{
			for (int32 AggIndex = 0; AggIndex < ParsedAggregateOps.Num(); ++AggIndex)
			{
				AggregateCounts[AggIndex] = FastCountValue;
			}
			Stats.Scanned = FastCountValue;
			Stats.Matched = FastCountValue;
		}
	}
	const int32 PrevParallel = UTurboStructLiteBPLibrary::SetParallelThreadLimit(ClampedParallel);
	if (!bFastCount)
	{
		if (SubSlots.Num() > 0)
		{
			FTurboStructLiteSlotIndex SlotIndex;
			if (!UTurboStructLiteBPLibrary::GetSlotIndex(SlotName, SlotIndex))
			{
				UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevParallel);
				OutErrorMessage = TEXT("IO Error: Load failed");
				return false;
			}
			TArray<FTurboStructLiteCachedEntry> CachedEntries;
			CachedEntries.SetNum(SubSlots.Num());
			bool bCacheReady = true;
			for (int32 CacheIndex = 0; CacheIndex < SubSlots.Num(); ++CacheIndex)
			{
				const FTurboStructLiteCachedEntry* Found = SlotIndex.Entries.Find(SubSlots[CacheIndex]);
				if (!Found)
				{
					bCacheReady = false;
					break;
				}
				CachedEntries[CacheIndex] = *Found;
			}
			if (!bCacheReady)
			{
				UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevParallel);
				OutErrorMessage = TEXT("IO Error: Load failed");
				return false;
			}
			UTurboStructLiteBPLibrary::EnsureSettingsLoaded();
			const FString FilePath = UTurboStructLiteBPLibrary::BuildSavePath(SlotName);
			auto LoadEntryFromCache = [EncryptionKey, SelectedEncryption](FArchive& Reader, const FTurboStructLiteCachedEntry& Cached, TArray<uint8>& OutRawBytes) -> bool
			{
				OutRawBytes.Reset();
				Reader.Seek(Cached.DataOffset);
				if (!UTurboStructLiteBPLibrary::IsValidBufferSize(Reader, Cached.DataSize))
				{
					return false;
				}
				FTurboStructLiteEntry Entry;
				Entry.Compression = Cached.Compression;
				Entry.Encryption = Cached.Encryption;
				Entry.UncompressedSize = Cached.UncompressedSize;
				Entry.Data.SetNum(Cached.DataSize);
				if (Cached.DataSize > 0)
				{
					Reader.Serialize(Entry.Data.GetData(), Cached.DataSize);
				}
				if (Cached.MetaSize > 0)
				{
					Reader.Seek(Cached.MetaOffset);
					if (!UTurboStructLiteBPLibrary::IsValidBufferSize(Reader, Cached.MetaSize))
					{
						return false;
					}
				}
				ETurboStructLiteEncryption EffectiveEncryption = Entry.Encryption;
				if (EffectiveEncryption == ETurboStructLiteEncryption::ProjectDefault)
				{
					EffectiveEncryption = SelectedEncryption == ETurboStructLiteEncryption::ProjectDefault ? UTurboStructLiteBPLibrary::GetActiveEncryptionMode() : SelectedEncryption;
				}
				if (EffectiveEncryption == ETurboStructLiteEncryption::AES)
				{
					FString KeyToUse = EncryptionKey;
					if (KeyToUse.IsEmpty())
					{
						KeyToUse = UTurboStructLiteBPLibrary::GetActiveEncryptionKey();
					}
					if (KeyToUse.IsEmpty())
					{
						return false;
					}
					if (!UTurboStructLiteBPLibrary::DecryptDataBuffer(ETurboStructLiteEncryption::AES, KeyToUse, Entry.Data))
					{
						return false;
					}
				}
				return UTurboStructLiteBPLibrary::DecompressBuffer(Entry.Compression, Entry.Data, OutRawBytes);
			};
			const int32 TaskCount = (bUseWriteAheadLog || bForceSingleThread) ? 1 : FMath::Min(ClampedParallel, SubSlots.Num());
			const int32 PerTaskThreads = FMath::Max(1, ClampedParallel / TaskCount);
			TArray<FTurboStructLiteRow> SubRows;
			TArray<bool> SubMatched;
			TArray<double> SubSortKeys;
			TArray<FTurboStructLiteLogicQueryStats> SubStats;
			TArray<FString> SubErrors;
			SubRows.SetNum(SubSlots.Num());
			SubMatched.Init(false, SubSlots.Num());
			if (bHasOrderBy)
			{
				SubSortKeys.SetNum(SubSlots.Num());
			}
			SubStats.SetNum(SubSlots.Num());
			SubErrors.SetNum(SubSlots.Num());
			const int32 ItemsPerTask = FMath::DivideAndRoundUp(SubSlots.Num(), TaskCount);
			TArray<TArray<int64>> TaskAggregateCounts;
			TArray<TArray<double>> TaskAggregateSums;
			if (bHasAggregates)
			{
				TaskAggregateCounts.SetNum(TaskCount);
				TaskAggregateSums.SetNum(TaskCount);
				for (int32 TaskIndex = 0; TaskIndex < TaskCount; ++TaskIndex)
				{
					TaskAggregateCounts[TaskIndex].Init(0, ParsedAggregateOps.Num());
					TaskAggregateSums[TaskIndex].Init(0.0, ParsedAggregateOps.Num());
				}
			}
			int32 MatchesFound = 0;
			const bool bApplyOffsetInLoop = TaskCount == 1 && bForceSingleThread && !bHasOrderBy && !bHasAggregates;
			if (bApplyOffsetInLoop && bHasOffset)
			{
				bOffsetAppliedInLoop = true;
			}
			if (bAllowEarlyExit && bApplyOffsetInLoop)
			{
				bLimitAppliedInLoop = true;
			}
			ParallelFor(TaskCount, [&](int32 TaskIndex)
			{
				const int32 PrevTaskParallel = UTurboStructLiteBPLibrary::SetParallelThreadLimit(PerTaskThreads);
				const int32 Start = TaskIndex * ItemsPerTask;
				const int32 End = FMath::Min(Start + ItemsPerTask, SubSlots.Num());
				TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
				if (!Reader)
				{
					for (int32 SubSlotIdx = Start; SubSlotIdx < End; ++SubSlotIdx)
					{
						SubErrors[SubSlotIdx] = TEXT("IO Error: Load failed");
					}
					UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevTaskParallel);
					return;
				}
				TArray<int64>* LocalAggCounts = bHasAggregates ? &TaskAggregateCounts[TaskIndex] : nullptr;
				TArray<double>* LocalAggSums = bHasAggregates ? &TaskAggregateSums[TaskIndex] : nullptr;
				for (int32 SubSlotIdx = Start; SubSlotIdx < End; ++SubSlotIdx)
				{
					FTurboStructLiteLogicQueryStats LocalStats;
					FString LocalError;
					bool bLocalSuccess = true;
					bool bLocalMatch = false;
					bool bStoreRow = false;
					double LocalSortValue = 0.0;
					bool bStopEarly = false;
					FTurboStructLiteRow LocalRow;
					const int32 CurrentSubSlot = SubSlots[SubSlotIdx];
					if (bUseWriteAheadLog)
					{
						UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, FString::Printf(TEXT("SelectLogic SubSlot=%d"), CurrentSubSlot));
					}
					TArray<uint8> RawBytes;
					if (!LoadEntryFromCache(*Reader, CachedEntries[SubSlotIdx], RawBytes))
					{
						LocalError = TEXT("IO Error: Load failed");
						bLocalSuccess = false;
					}
					else
					{
						FString RootType;
						if (UTurboStructLiteBPLibrary::GetRootMetaTypeFromBytes(RawBytes, RootType))
						{
							const FString NormalRoot = UTurboStructLiteBPLibrary::NormalizeTypeName(RootType);
							const FString NormalData = UTurboStructLiteBPLibrary::NormalizeTypeName(ContextStructProp->GetCPPType(nullptr, 0));
							if (NormalRoot != NormalData)
							{
								LocalError = TEXT("Type Error: Stored data type mismatch");
								bLocalSuccess = false;
							}
						}
						if (bLocalSuccess)
						{
							TArray<uint8> FullValue;
							FString DeserializeError;
							if (!DeserializeLogicValue(ContextStructProp.Get(), RawBytes, PerTaskThreads, FullValue, DeserializeError))
							{
								LocalError = DeserializeError;
								bLocalSuccess = false;
							}
							else
							{
								LocalStats.Scanned = 1;
								bLocalMatch = EvaluateLogicQueryNode(*Root, FullValue.GetData(), nullptr, nullptr);
								LocalStats.Matched = bLocalMatch ? 1 : 0;
								if (bLocalMatch)
								{
									bool bShouldStore = true;
									if (bApplyOffsetInLoop)
									{
										MatchesFound++;
										if (MatchesFound <= ParsedOffset)
										{
											bShouldStore = false;
										}
									}
									if (bHasAggregates)
									{
										if (LocalAggCounts && LocalAggSums)
										{
											for (int32 AggIndex = 0; AggIndex < ParsedAggregateOps.Num(); ++AggIndex)
											{
												if (ParsedAggregateOps[AggIndex] == ETurboStructLiteAggregateOp::Count)
												{
													(*LocalAggCounts)[AggIndex] += 1;
													continue;
												}
												const int32 FieldIndex = AggregateFieldIndices[AggIndex];
												if (!AggregateFieldInfos.IsValidIndex(FieldIndex))
												{
													LocalError = TEXT("Type Error: Invalid aggregate field");
													bLocalSuccess = false;
													break;
												}
												const FTurboStructLiteSelectFieldInfo& FieldInfo = AggregateFieldInfos[FieldIndex];
												uint8* FieldPtr = ResolvePropertyPtr(FieldInfo.PropertyChain, FullValue.GetData());
												if (!FieldPtr)
												{
													LocalError = TEXT("Type Error: Invalid field pointer");
													bLocalSuccess = false;
													break;
												}
												double NumericValue = 0.0;
												if (!TryGetNumericValue(FieldInfo.LeafProperty, FieldPtr, FieldInfo.bCountOnly, false, NumericValue))
												{
													LocalError = TEXT("Type Error: Aggregate field must be numeric");
													bLocalSuccess = false;
													break;
												}
												(*LocalAggSums)[AggIndex] += NumericValue;
												(*LocalAggCounts)[AggIndex] += 1;
											}
										}
									}
									else if (bShouldStore)
									{
										for (const FTurboStructLiteSelectFieldInfo& FieldInfo : SelectFieldInfos)
										{
											if (FieldInfo.bCountOnly)
											{
												int32 CountValue = 0;
												FArrayProperty* ArrayProp = CastField<FArrayProperty>(FieldInfo.LeafProperty);
												if (!ArrayProp)
												{
													LocalError = TEXT("Type Error: Invalid array field");
													bLocalSuccess = false;
													break;
												}
												uint8* FieldPtr = ResolvePropertyPtr(FieldInfo.PropertyChain, FullValue.GetData());
												if (!FieldPtr)
												{
													LocalError = TEXT("Type Error: Invalid field pointer");
													bLocalSuccess = false;
													break;
												}
												FScriptArrayHelper Helper(ArrayProp, FieldPtr);
												CountValue = Helper.Num();
												FTurboStructLiteVariant Variant;
												Variant.Type = ETurboStructLiteVariantType::Int;
												Variant.IntValue = CountValue;
												Variant.FloatValue = static_cast<double>(CountValue);
												Variant.StringValue = LexToString(CountValue);
												LocalRow.Columns.Add(FieldInfo.ColumnName, Variant);
											}
											else
											{
												uint8* FieldPtr = ResolvePropertyPtr(FieldInfo.PropertyChain, FullValue.GetData());
												if (!FieldPtr)
												{
													LocalError = TEXT("Type Error: Invalid field pointer");
													bLocalSuccess = false;
													break;
												}
												FTurboStructLiteVariant Variant;
												if (!UTurboStructLiteBPLibrary::BuildVariantFromProperty(FieldInfo.LeafProperty, FieldPtr, Variant))
												{
													LocalError = TEXT("Type Error: Failed to build variant");
													bLocalSuccess = false;
													break;
												}
												LocalRow.Columns.Add(FieldInfo.ColumnName, Variant);
											}
										}
										if (bLocalSuccess && bHasOrderBy)
										{
											uint8* SortPtr = ResolvePropertyPtr(OrderFieldInfo.PropertyChain, FullValue.GetData());
											if (!SortPtr)
											{
												LocalError = TEXT("Type Error: Invalid ORDER BY field pointer");
												bLocalSuccess = false;
											}
											else
											{
												double NumericValue = 0.0;
												const bool bSortOk = TryGetNumericValue(OrderFieldInfo.LeafProperty, SortPtr, OrderFieldInfo.bCountOnly, true, NumericValue);
												const double Fallback = bParsedOrderDesc ? -TNumericLimits<double>::Max() : TNumericLimits<double>::Max();
												LocalSortValue = bSortOk ? NumericValue : Fallback;
											}
										}
										if (bLocalSuccess)
										{
											bStoreRow = true;
										}
									}
									if (bAllowEarlyExit && bApplyOffsetInLoop && MatchesFound >= MaxMatchIndex)
									{
										bStopEarly = true;
									}
								}
								ContextStructProp->DestroyValue(FullValue.GetData());
							}
						}
					}
					if (bLocalSuccess)
					{
						SubStats[SubSlotIdx] = LocalStats;
						if (bStoreRow)
						{
							SubMatched[SubSlotIdx] = true;
							SubRows[SubSlotIdx] = MoveTemp(LocalRow);
							if (bHasOrderBy)
							{
								SubSortKeys[SubSlotIdx] = LocalSortValue;
							}
						}
					}
					else
					{
						SubErrors[SubSlotIdx] = LocalError;
					}
					if (bStopEarly)
					{
						break;
					}
				}
				UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevTaskParallel);
			}, EParallelForFlags::Unbalanced);
			if (bHasAggregates)
			{
				for (int32 TaskIndex = 0; TaskIndex < TaskAggregateCounts.Num(); ++TaskIndex)
				{
					const TArray<int64>& TaskCounts = TaskAggregateCounts[TaskIndex];
					const TArray<double>& TaskSums = TaskAggregateSums[TaskIndex];
					for (int32 AggIndex = 0; AggIndex < ParsedAggregateOps.Num(); ++AggIndex)
					{
						if (TaskCounts.IsValidIndex(AggIndex))
						{
							AggregateCounts[AggIndex] += TaskCounts[AggIndex];
						}
						if (TaskSums.IsValidIndex(AggIndex))
						{
							AggregateSums[AggIndex] += TaskSums[AggIndex];
						}
					}
				}
			}
			for (int32 MergeIndex = 0; MergeIndex < SubSlots.Num(); ++MergeIndex)
			{
				if (!SubErrors[MergeIndex].IsEmpty())
				{
					OutErrorMessage = SubErrors[MergeIndex];
					break;
				}
				FTurboStructLiteLogicQueryStats LocalStats = SubStats[MergeIndex];
				Stats.Scanned += LocalStats.Scanned;
				Stats.Matched += LocalStats.Matched;
				if (OutMetadata.IsEmpty())
				{
					FTurboStructLiteSubSlotInfo SubInfo;
					if (UTurboStructLiteBPLibrary::ReadSubSlotInfoInternal(SlotName, SubSlots[MergeIndex], EncryptionKey, SelectedEncryption, SubInfo))
					{
						OutMetadata = SubInfo.DebugMetadata;
					}
				}
				if (!bHasAggregates && SubMatched[MergeIndex])
				{
					Results.Add(MoveTemp(SubRows[MergeIndex]));
					if (bHasOrderBy)
					{
						SortKeys.Add(SubSortKeys[MergeIndex]);
					}
				}
			}
		}
	}
	UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevParallel);
	if (!OutErrorMessage.IsEmpty())
	{
		return false;
	}
	if (bHasAggregates)
	{
		FTurboStructLiteRow AggregateRow;
		for (int32 AggIndex = 0; AggIndex < ParsedAggregateOps.Num(); ++AggIndex)
		{
			const ETurboStructLiteAggregateOp Op = ParsedAggregateOps[AggIndex];
			FTurboStructLiteVariant Variant;
			if (Op == ETurboStructLiteAggregateOp::Count)
			{
				const int64 CountValue = AggregateCounts.IsValidIndex(AggIndex) ? AggregateCounts[AggIndex] : 0;
				Variant.Type = ETurboStructLiteVariantType::Int;
				Variant.IntValue = CountValue;
				Variant.FloatValue = static_cast<double>(CountValue);
				Variant.StringValue = LexToString(CountValue);
			}
			else if (Op == ETurboStructLiteAggregateOp::Sum)
			{
				const double SumValue = AggregateSums.IsValidIndex(AggIndex) ? AggregateSums[AggIndex] : 0.0;
				Variant.Type = ETurboStructLiteVariantType::Float;
				Variant.FloatValue = SumValue;
				Variant.StringValue = LexToString(SumValue);
			}
			else
			{
				const double SumValue = AggregateSums.IsValidIndex(AggIndex) ? AggregateSums[AggIndex] : 0.0;
				const int64 CountValue = AggregateCounts.IsValidIndex(AggIndex) ? AggregateCounts[AggIndex] : 0;
				const double AvgValue = CountValue > 0 ? (SumValue / static_cast<double>(CountValue)) : 0.0;
				Variant.Type = ETurboStructLiteVariantType::Float;
				Variant.FloatValue = AvgValue;
				Variant.StringValue = LexToString(AvgValue);
			}
			if (ParsedAggregateColumns.IsValidIndex(AggIndex))
			{
				AggregateRow.Columns.Add(ParsedAggregateColumns[AggIndex], Variant);
			}
		}
		Results.Reset();
		Results.Add(MoveTemp(AggregateRow));
	}
	else
	{
		if (bHasOrderBy && SortKeys.Num() == Results.Num() && Results.Num() > 1)
		{
			TArray<int32> Order;
			Order.SetNum(Results.Num());
			for (int32 Index = 0; Index < Results.Num(); ++Index)
			{
				Order[Index] = Index;
			}
			Order.Sort([&](int32 A, int32 B)
			{
				return bParsedOrderDesc ? (SortKeys[A] > SortKeys[B]) : (SortKeys[A] < SortKeys[B]);
			});
			TArray<FTurboStructLiteRow> SortedResults;
			SortedResults.Reserve(Results.Num());
			for (int32 Index : Order)
			{
				SortedResults.Add(MoveTemp(Results[Index]));
			}
			Results = MoveTemp(SortedResults);
		}
		if (bHasOffset && !bOffsetAppliedInLoop)
		{
			if (ParsedOffset >= Results.Num())
			{
				Results.Reset();
			}
			else if (ParsedOffset > 0)
			{
				Results.RemoveAt(0, ParsedOffset);
			}
		}
		if (bHasLimit && !bLimitAppliedInLoop && Results.Num() > ParsedLimit)
		{
			Results.SetNum(ParsedLimit);
		}
	}
	Stats.ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	OutStatsText = FormatLogicStats(Stats);
	FTurboStructLiteSlotInfo SlotInfo;
	if (UTurboStructLiteBPLibrary::GetSlotInfoInternal(SlotName, SlotInfo))
	{
		OutSaveDate = SlotInfo.Timestamp;
	}
	OutRows = MoveTemp(Results);
	return true;
}
DEFINE_FUNCTION(UTurboStructLiteQueryLibrary::execTurboStructLoadArrayLogicLite)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);
	P_GET_PROPERTY(FStrProperty, QueryString);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FArrayProperty>(nullptr);
	FProperty* DataProp = Stack.MostRecentProperty;
	void* DataPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteLogicLoadComplete LoadDelegate;
	if (OnComplete.IsBound())
	{
		LoadDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}

auto EmitResult = [LoadDelegate, MainSlotName, SubSlotIndex](bool bSuccess, const FString& ErrorMessage, const FString& Metadata, const FDateTime& SaveDate, int32 SaveVersion, const FString& StatsText) mutable
{
	FTurboStructLiteLogicLoadComplete Local = LoadDelegate;
	Local.ExecuteIfBound(bSuccess, ErrorMessage, Metadata, SaveDate, SaveVersion, StatsText);
	UTurboStructLiteBPLibrary::EndMemoryOpMessage(MainSlotName, SubSlotIndex, false, true);
};

	const int32 SaveVersion = UTurboStructLiteBPLibrary::GetVersion();

	if (!DataProp || !DataPtr)
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructLoadArrayLogicLite: Invalid data type"), ELogVerbosity::Error);
		EmitResult(false, TEXT("Type Error: Data must be an array"), FString(), FDateTime(0), SaveVersion, FString());
		return;
	}
	FString TrimmedQuery = QueryString;
	TrimmedQuery.TrimStartAndEndInline();
	const bool bSelectMode = TrimmedQuery.StartsWith(TEXT("SELECT"), ESearchCase::IgnoreCase);
	if (!bSelectMode && !CastField<FArrayProperty>(DataProp))
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructLoadArrayLogicLite: Invalid data type"), ELogVerbosity::Error);
		EmitResult(false, TEXT("Type Error: Data must be an array"), FString(), FDateTime(0), SaveVersion, FString());
		return;
	}

if (MainSlotName.IsEmpty() || SubSlotIndex < -1)
{
	FFrame::KismetExecutionMessage(TEXT("TurboStructLoadArrayLogicLite: Invalid slot parameters"), ELogVerbosity::Error);
	EmitResult(false, TEXT("IO Error: Invalid slot parameters"), FString(), FDateTime(0), SaveVersion, FString());
	return;
}

UTurboStructLiteBPLibrary::BeginMemoryOpMessage(MainSlotName, SubSlotIndex, false, true);
if (bSelectMode)
{
	const ETurboStructLiteEncryption SelectedEncryption = static_cast<ETurboStructLiteEncryption>(Encryption);
	const ETurboStructLiteEncryption ResolvedEncryption = SelectedEncryption == ETurboStructLiteEncryption::ProjectDefault ? UTurboStructLiteBPLibrary::GetActiveEncryptionMode() : SelectedEncryption;
	const int32 ClampedPriority = FMath::Clamp(QueuePriority, TurboStructLiteQueryQueuePriorityMin, TurboStructLiteQueryQueuePriorityMax);
	const int32 ClampedParallel = FMath::Clamp(MaxParallelThreads, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	FString WALPath;
	if (bUseWriteAheadLog)
	{
		const FString LoadOpLabel = FString::Printf(TEXT("LoadArrayLogic_Enc%d"), static_cast<int32>(ResolvedEncryption));
		WALPath = UTurboStructLiteBPLibrary::GenerateWALPath(MainSlotName, SubSlotIndex, LoadOpLabel);
		UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, FString::Printf(TEXT("Queued LoadArrayLogic Slot=%s SubSlot=%d Async=%s Encryption=%d Queue=%d Threads=%d Batching=%d"), *MainSlotName, SubSlotIndex, bAsync ? TEXT("true") : TEXT("false"), static_cast<int32>(ResolvedEncryption), ClampedPriority, ClampedParallel, static_cast<int32>(CompressionBatching)));
		UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, FString::Printf(TEXT("Query=%s"), *QueryString));
	}

	const FString SlotCopy = MainSlotName;
	const FString EncryptionKeyCopy = EncryptionKey;
	const FString QueryCopy = QueryString;

	auto RunSelectLoad = [SlotCopy, SubSlotIndex, QueryCopy, DataProp, DataPtr, LoadDelegate, bUseWriteAheadLog, WALPath, EncryptionKeyCopy, SelectedEncryption, ClampedParallel, SaveVersion](bool bApplyOnGameThread) mutable
	{
		int64 ExpectedBytes = 0;
		if (UTurboStructLiteBPLibrary::GetExpectedRawSize(SlotCopy, SubSlotIndex, ExpectedBytes))
		{
			UTurboStructLiteBPLibrary::UpdateMemoryPressureWarning(SlotCopy, SubSlotIndex, ExpectedBytes, false, true);
		}
		bool bSuccess = false;
		FString ErrorMessage;
		FString Metadata;
		FDateTime SaveDate;
		FString StatsText;
		TArray<uint8> CombinedValue;
		if (ExecuteSelectQueryToValue(SlotCopy, SubSlotIndex, QueryCopy, EncryptionKeyCopy, SelectedEncryption, ClampedParallel, bUseWriteAheadLog, WALPath, DataProp, CombinedValue, Metadata, SaveDate, StatsText, ErrorMessage))
		{
			bSuccess = true;
		}
		if (bUseWriteAheadLog && !ErrorMessage.IsEmpty())
		{
			UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, ErrorMessage);
		}
		auto ApplyResults = [SlotCopy, DataProp, DataPtr, LoadDelegate, bUseWriteAheadLog, WALPath, bSuccess, ErrorMessage, Metadata, SaveDate, SaveVersion, StatsText, CombinedValue = MoveTemp(CombinedValue)]() mutable
		{
			if (!UTurboStructLiteBPLibrary::HasActiveGameWorld())
			{
				if (bUseWriteAheadLog)
				{
					UTurboStructLiteBPLibrary::DeleteWALFile(WALPath);
				}
				UTurboStructLiteBPLibrary::FinishQueuedLoad(SlotCopy);
				return;
			}
			if (!DataProp || !DataPtr)
			{
				if (bUseWriteAheadLog)
				{
					UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, TEXT("LoadArrayLogic skipped: invalid target pointer"));
				}
				FTurboStructLiteLogicLoadComplete Local = LoadDelegate;
				Local.ExecuteIfBound(false, TEXT("Type Error: Invalid target pointer"), FString(), FDateTime(0), SaveVersion, FString());
				UTurboStructLiteBPLibrary::FinishQueuedLoad(SlotCopy);
				return;
			}
			bool bApplied = false;
			if (bSuccess && CombinedValue.Num() > 0)
			{
				bool bSwapped = false;
				if (CastField<FArrayProperty>(DataProp))
				{
					FScriptArray* DestArray = static_cast<FScriptArray*>(DataPtr);
					FScriptArray* SrcArray = reinterpret_cast<FScriptArray*>(CombinedValue.GetData());
					FMemory::Memswap(DestArray, SrcArray, sizeof(FScriptArray));
					bSwapped = true;
				}
				else if (CastField<FMapProperty>(DataProp))
				{
					FScriptMap* DestMap = static_cast<FScriptMap*>(DataPtr);
					FScriptMap* SrcMap = reinterpret_cast<FScriptMap*>(CombinedValue.GetData());
					FMemory::Memswap(DestMap, SrcMap, sizeof(FScriptMap));
					bSwapped = true;
				}
				else if (CastField<FSetProperty>(DataProp))
				{
					FScriptSet* DestSet = static_cast<FScriptSet*>(DataPtr);
					FScriptSet* SrcSet = reinterpret_cast<FScriptSet*>(CombinedValue.GetData());
					FMemory::Memswap(DestSet, SrcSet, sizeof(FScriptSet));
					bSwapped = true;
				}
				else if (CastField<FStructProperty>(DataProp))
				{
					FMemory::Memswap(DataPtr, CombinedValue.GetData(), DataProp->GetSize());
					bSwapped = true;
				}
				if (!bSwapped)
				{
					DataProp->CopyCompleteValue(DataPtr, CombinedValue.GetData());
				}
				DataProp->DestroyValue(CombinedValue.GetData());
				bApplied = true;
			}
			else if (CombinedValue.Num() > 0 && DataProp)
			{
				DataProp->DestroyValue(CombinedValue.GetData());
			}
			if (bUseWriteAheadLog)
			{
				UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, bApplied && bSuccess ? TEXT("LoadArrayLogic completed") : TEXT("LoadArrayLogic failed"));
				if (bApplied && bSuccess)
				{
					UTurboStructLiteBPLibrary::DeleteWALFile(WALPath);
				}
			}
			FTurboStructLiteLogicLoadComplete Local = LoadDelegate;
			Local.ExecuteIfBound(bApplied && bSuccess, ErrorMessage, Metadata, SaveDate, SaveVersion, StatsText);
			UTurboStructLiteBPLibrary::FinishQueuedLoad(SlotCopy);
		};

		if (bApplyOnGameThread)
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(ApplyResults));
		}
		else
		{
			ApplyResults();
		}
	};

	TArray<const FStructProperty*> DummyStructProps;
	const bool bCanBackground = DataProp && !DataProp->ContainsObjectReference(DummyStructProps, EPropertyObjectReferenceType::Strong);

	auto Task = [RunSelectLoad, bAsync, bCanBackground]() mutable
	{
		if (bAsync)
		{
			if (bCanBackground)
			{
				Async(EAsyncExecution::ThreadPool, [RunSelectLoad]() mutable
				{
#if WITH_EDITOR
					TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadArrayLogic_AsyncTask"));
#endif
					RunSelectLoad(true);
				});
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, [RunSelectLoad]() mutable
				{
#if WITH_EDITOR
					TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadArrayLogic_AsyncTask"));
#endif
					RunSelectLoad(true);
				});
			}
			return;
		}
#if WITH_EDITOR
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadArrayLogic_Sync"));
#endif
		RunSelectLoad(false);
	};

	UTurboStructLiteBPLibrary::EnqueueTask(SlotCopy, MoveTemp(Task), ClampedPriority);
	return;
}

FTurboStructLiteLogicQueryContext QueryContext;
FString LocalError;
	if (!BuildLogicQueryContext(DataProp, QueryContext, LocalError))
	{
		EmitResult(false, LocalError, FString(), FDateTime(0), SaveVersion, FString());
		return;
	}

	TArray<FTurboStructLiteQueryToken> Tokens;
	int32 ErrorPos = 0;
	if (!TokenizeLogicQuery(QueryString, Tokens, LocalError, ErrorPos))
	{
		EmitResult(false, LocalError, FString(), FDateTime(0), SaveVersion, FString());
		return;
	}

	TSharedPtr<FTurboStructLiteQueryNode> Root;
	if (!ParseLogicQuery(Tokens, Root, LocalError, ErrorPos))
	{
		EmitResult(false, LocalError, FString(), FDateTime(0), SaveVersion, FString());
		return;
	}

	if (!BindLogicQuery(Root, QueryContext, LocalError, ErrorPos))
	{
		EmitResult(false, LocalError, FString(), FDateTime(0), SaveVersion, FString());
		return;
	}

	const ETurboStructLiteEncryption SelectedEncryption = static_cast<ETurboStructLiteEncryption>(Encryption);
	const ETurboStructLiteEncryption ResolvedEncryption = SelectedEncryption == ETurboStructLiteEncryption::ProjectDefault ? UTurboStructLiteBPLibrary::GetActiveEncryptionMode() : SelectedEncryption;
	const int32 ClampedPriority = FMath::Clamp(QueuePriority, TurboStructLiteQueryQueuePriorityMin, TurboStructLiteQueryQueuePriorityMax);
	const int32 ClampedParallel = FMath::Clamp(MaxParallelThreads, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	FString WALPath;
	if (bUseWriteAheadLog)
	{
		const FString LoadOpLabel = FString::Printf(TEXT("LoadArrayLogic_Enc%d"), static_cast<int32>(ResolvedEncryption));
		WALPath = UTurboStructLiteBPLibrary::GenerateWALPath(MainSlotName, SubSlotIndex, LoadOpLabel);
		UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, FString::Printf(TEXT("Queued LoadArrayLogic Slot=%s SubSlot=%d Async=%s Encryption=%d Queue=%d Threads=%d Batching=%d"), *MainSlotName, SubSlotIndex, bAsync ? TEXT("true") : TEXT("false"), static_cast<int32>(ResolvedEncryption), ClampedPriority, ClampedParallel, static_cast<int32>(CompressionBatching)));
		UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, FString::Printf(TEXT("Query=%s"), *QueryString));
	}

	const FString SlotCopy = MainSlotName;
	const FString EncryptionKeyCopy = EncryptionKey;

	auto RunLogicLoad = [SlotCopy, SubSlotIndex, Root, DataProp, DataPtr, LoadDelegate, bUseWriteAheadLog, WALPath, EncryptionKeyCopy, SelectedEncryption, ClampedParallel, SaveVersion](bool bApplyOnGameThread) mutable
	{
		int64 ExpectedBytes = 0;
		if (UTurboStructLiteBPLibrary::GetExpectedRawSize(SlotCopy, SubSlotIndex, ExpectedBytes))
		{
			UTurboStructLiteBPLibrary::UpdateMemoryPressureWarning(SlotCopy, SubSlotIndex, ExpectedBytes, false, true);
		}
		const double StartSeconds = FPlatformTime::Seconds();
		bool bSuccess = false;
		FString ErrorMessage;
		FString Metadata;
		FDateTime SaveDate;
		FString StatsText;
		FTurboStructLiteLogicQueryStats Stats;
		TArray<uint8> CombinedValue;
		bool bHasCombined = false;
		bool bStructMatched = false;
		auto NormalizeType = [](const FString& InType)
		{
			FString Out = InType;
			Out.TrimStartAndEndInline();
			if (Out.StartsWith(TEXT("struct ")))
			{
				Out = Out.Mid(7);
			}
			else if (Out.StartsWith(TEXT("class ")))
			{
				Out = Out.Mid(6);
			}
			Out.ReplaceInline(TEXT(" "), TEXT(""));
			return Out.ToLower();
		};

		TArray<int32> SubSlots;
		if (SubSlotIndex == -1)
		{
			if (!UTurboStructLiteBPLibrary::ListSubSlotIndices(SlotCopy, SubSlots) || SubSlots.Num() == 0)
			{
				ErrorMessage = TEXT("IO Error: No subslots found");
			}
		}
		else
		{
			SubSlots.Add(SubSlotIndex);
		}

		const int32 PrevParallel = UTurboStructLiteBPLibrary::SetParallelThreadLimit(ClampedParallel);
		if (ErrorMessage.IsEmpty())
		{
			if (SubSlots.Num() > 0)
			{
				FTurboStructLiteSlotIndex SlotIndex;
				if (!UTurboStructLiteBPLibrary::GetSlotIndex(SlotCopy, SlotIndex))
				{
					ErrorMessage = TEXT("IO Error: Load failed");
				}
				else
				{
					TArray<FTurboStructLiteCachedEntry> CachedEntries;
					CachedEntries.SetNum(SubSlots.Num());
					bool bCacheReady = true;
					for (int32 CacheIndex = 0; CacheIndex < SubSlots.Num(); ++CacheIndex)
					{
						const FTurboStructLiteCachedEntry* Found = SlotIndex.Entries.Find(SubSlots[CacheIndex]);
						if (!Found)
						{
							bCacheReady = false;
							break;
						}
						CachedEntries[CacheIndex] = *Found;
					}
					if (!bCacheReady)
					{
						ErrorMessage = TEXT("IO Error: Load failed");
					}
					else
					{
						UTurboStructLiteBPLibrary::EnsureSettingsLoaded();
						const FString FilePath = UTurboStructLiteBPLibrary::BuildSavePath(SlotCopy);
						auto LoadEntryFromCache = [EncryptionKeyCopy, SelectedEncryption](FArchive& Reader, const FTurboStructLiteCachedEntry& Cached, TArray<uint8>& OutRawBytes) -> bool
						{
							OutRawBytes.Reset();
							Reader.Seek(Cached.DataOffset);
							if (!UTurboStructLiteBPLibrary::IsValidBufferSize(Reader, Cached.DataSize))
							{
								return false;
							}
							FTurboStructLiteEntry Entry;
							Entry.Compression = Cached.Compression;
							Entry.Encryption = Cached.Encryption;
							Entry.UncompressedSize = Cached.UncompressedSize;
							Entry.Data.SetNum(Cached.DataSize);
							if (Cached.DataSize > 0)
							{
								Reader.Serialize(Entry.Data.GetData(), Cached.DataSize);
							}
							if (Cached.MetaSize > 0)
							{
								Reader.Seek(Cached.MetaOffset);
								if (!UTurboStructLiteBPLibrary::IsValidBufferSize(Reader, Cached.MetaSize))
								{
									return false;
								}
							}
							ETurboStructLiteEncryption EffectiveEncryption = Entry.Encryption;
							if (EffectiveEncryption == ETurboStructLiteEncryption::ProjectDefault)
							{
								EffectiveEncryption = SelectedEncryption == ETurboStructLiteEncryption::ProjectDefault ? UTurboStructLiteBPLibrary::GetActiveEncryptionMode() : SelectedEncryption;
							}
							if (EffectiveEncryption == ETurboStructLiteEncryption::AES)
							{
								FString KeyToUse = EncryptionKeyCopy;
								if (KeyToUse.IsEmpty())
								{
									KeyToUse = UTurboStructLiteBPLibrary::GetActiveEncryptionKey();
								}
								if (KeyToUse.IsEmpty())
								{
									return false;
								}
								if (!UTurboStructLiteBPLibrary::DecryptDataBuffer(ETurboStructLiteEncryption::AES, KeyToUse, Entry.Data))
								{
									return false;
								}
							}
							return UTurboStructLiteBPLibrary::DecompressBuffer(Entry.Compression, Entry.Data, OutRawBytes);
						};
						const int32 TaskCount = bUseWriteAheadLog ? 1 : FMath::Min(ClampedParallel, SubSlots.Num());
						const int32 PerTaskThreads = FMath::Max(1, ClampedParallel / TaskCount);
						TArray<TArray<uint8>> SubValues;
						TArray<FTurboStructLiteLogicQueryStats> SubStats;
						TArray<FString> SubErrors;
						SubValues.SetNum(SubSlots.Num());
						SubStats.SetNum(SubSlots.Num());
						SubErrors.SetNum(SubSlots.Num());
						const int32 ItemsPerTask = FMath::DivideAndRoundUp(SubSlots.Num(), TaskCount);

						ParallelFor(TaskCount, [&](int32 TaskIndex)
						{
							const int32 PrevTaskParallel = UTurboStructLiteBPLibrary::SetParallelThreadLimit(PerTaskThreads);
							const int32 Start = TaskIndex * ItemsPerTask;
							const int32 End = FMath::Min(Start + ItemsPerTask, SubSlots.Num());
							TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath));
							if (!Reader)
							{
								for (int32 SubSlotIdx = Start; SubSlotIdx < End; ++SubSlotIdx)
								{
									SubErrors[SubSlotIdx] = TEXT("IO Error: Load failed");
								}
								UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevTaskParallel);
								return;
							}
							for (int32 SubSlotIdx = Start; SubSlotIdx < End; ++SubSlotIdx)
							{
								FTurboStructLiteLogicQueryStats LocalStats;
								FString LocalError;
								TArray<uint8> LocalValue;
								bool bLocalSuccess = true;
								const int32 CurrentSubSlot = SubSlots[SubSlotIdx];
								if (bUseWriteAheadLog)
								{
									UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, FString::Printf(TEXT("LoadArrayLogic SubSlot=%d"), CurrentSubSlot));
								}


								TArray<uint8> RawBytes;
								if (!LoadEntryFromCache(*Reader, CachedEntries[SubSlotIdx], RawBytes))
								{
									LocalError = TEXT("IO Error: Load failed");
									bLocalSuccess = false;
								}
								else
								{
									bool bSingleElement = false;
									FProperty* ElementProp = nullptr;
									FString RootType;
									if (UTurboStructLiteBPLibrary::GetRootMetaTypeFromBytes(RawBytes, RootType))
									{
										const FString NormalRoot = NormalizeType(RootType);
										const FString NormalData = NormalizeType(DataProp->GetCPPType(nullptr, 0));
										if (NormalRoot != NormalData)
										{
											if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp))
											{
												const FString NormalInner = NormalizeType(ArrayProp->Inner->GetCPPType(nullptr, 0));
												if (NormalRoot == NormalInner)
												{
													bSingleElement = true;
													ElementProp = ArrayProp->Inner;
												}
												else
												{
													LocalError = TEXT("Type Error: Stored data type mismatch");
													bLocalSuccess = false;
												}
											}
											else
											{
												LocalError = TEXT("Type Error: Stored data type mismatch");
												bLocalSuccess = false;
											}
										}
									}
									if (bLocalSuccess)
									{
										if (bSingleElement && ElementProp)
										{
											TArray<uint8> ElementValue;
											FString DeserializeError;
											if (!DeserializeLogicValue(ElementProp, RawBytes, PerTaskThreads, ElementValue, DeserializeError))
											{
												LocalError = DeserializeError;
												bLocalSuccess = false;
											}
											else
											{
												LocalStats.Scanned = 1;
												const bool bMatch = EvaluateLogicQueryNode(*Root, ElementValue.GetData(), nullptr, nullptr);
												LocalStats.Matched = bMatch ? 1 : 0;
												const int32 ValueSize = DataProp->GetSize();
												LocalValue.SetNumUninitialized(ValueSize);
												DataProp->InitializeValue(LocalValue.GetData());
												if (bMatch)
												{
													FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp);
													if (ArrayProp)
													{
														FScriptArrayHelper SubHelper(ArrayProp, LocalValue.GetData());
														const int32 NewIndex = SubHelper.AddValue();
														ArrayProp->Inner->CopyCompleteValue(SubHelper.GetRawPtr(NewIndex), ElementValue.GetData());
													}
												}
												ElementProp->DestroyValue(ElementValue.GetData());
											}
										}
										else
										{
											TArray<uint8> SourceValue;
											FString DeserializeError;
											if (!DeserializeLogicValue(DataProp, RawBytes, PerTaskThreads, SourceValue, DeserializeError))
											{
												LocalError = DeserializeError;
												bLocalSuccess = false;
											}
											else
											{
												FString FilterError;
												if (!ApplyLogicFilter(DataProp, Root, SourceValue, LocalValue, LocalStats, FilterError))
												{
													LocalError = FilterError;
													bLocalSuccess = false;
												}
												DataProp->DestroyValue(SourceValue.GetData());
											}
										}
									}
								}

								if (bLocalSuccess)
								{
									SubStats[SubSlotIdx] = LocalStats;
									SubValues[SubSlotIdx] = MoveTemp(LocalValue);
								}
								else
								{
									SubErrors[SubSlotIdx] = LocalError;
									if (LocalValue.Num() > 0 && DataProp)
									{
										DataProp->DestroyValue(LocalValue.GetData());
									}
								}
							}
							UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevTaskParallel);
						}, EParallelForFlags::Unbalanced);

						for (int32 MergeIndex = 0; MergeIndex < SubSlots.Num(); ++MergeIndex)
						{
							if (!SubErrors[MergeIndex].IsEmpty())
							{
								ErrorMessage = SubErrors[MergeIndex];
								for (int32 CleanupIndex = MergeIndex; CleanupIndex < SubSlots.Num(); ++CleanupIndex)
								{
									if (SubValues[CleanupIndex].Num() > 0 && DataProp)
									{
										DataProp->DestroyValue(SubValues[CleanupIndex].GetData());
									}
								}
								break;
							}
		
							FTurboStructLiteLogicQueryStats LocalStats = SubStats[MergeIndex];
							Stats.Scanned += LocalStats.Scanned;
							Stats.Matched += LocalStats.Matched;
		
							if (Metadata.IsEmpty())
							{
								FTurboStructLiteSubSlotInfo SubInfo;
								if (UTurboStructLiteBPLibrary::ReadSubSlotInfoInternal(SlotCopy, SubSlots[MergeIndex], EncryptionKeyCopy, SelectedEncryption, SubInfo))
								{
									Metadata = SubInfo.DebugMetadata;
								}
							}
		
							TArray<uint8>& SubValue = SubValues[MergeIndex];
							if (FStructProperty* StructProp = CastField<FStructProperty>(DataProp))
							{
								if (!bHasCombined)
								{
									CombinedValue = MoveTemp(SubValue);
									bHasCombined = true;
									bStructMatched = LocalStats.Matched > 0;
								}
								else if (LocalStats.Matched > 0 && !bStructMatched)
								{
									if (CombinedValue.Num() > 0)
									{
										DataProp->DestroyValue(CombinedValue.GetData());
									}
									CombinedValue = MoveTemp(SubValue);
									bStructMatched = true;
								}
								else
								{
									if (SubValue.Num() > 0)
									{
										DataProp->DestroyValue(SubValue.GetData());
									}
								}
							}
							else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp))
							{
								if (!bHasCombined)
								{
									CombinedValue = MoveTemp(SubValue);
									bHasCombined = true;
								}
								else
								{
									FScriptArrayHelper CombinedHelper(ArrayProp, CombinedValue.GetData());
									FScriptArrayHelper SubHelper(ArrayProp, SubValue.GetData());
									const int32 SubNum = SubHelper.Num();
									for (int32 Index = 0; Index < SubNum; ++Index)
									{
										const int32 NewIndex = CombinedHelper.AddValue();
										ArrayProp->Inner->CopyCompleteValue(CombinedHelper.GetRawPtr(NewIndex), SubHelper.GetRawPtr(Index));
									}
									DataProp->DestroyValue(SubValue.GetData());
								}
							}
							else if (FSetProperty* SetProp = CastField<FSetProperty>(DataProp))
							{
								if (!bHasCombined)
								{
									CombinedValue = MoveTemp(SubValue);
									bHasCombined = true;
								}
								else
								{
									FScriptSetHelper CombinedHelper(SetProp, CombinedValue.GetData());
									FScriptSetHelper SubHelper(SetProp, SubValue.GetData());
									for (int32 Index = 0; Index < SubHelper.GetMaxIndex(); ++Index)
									{
										if (SubHelper.IsValidIndex(Index))
										{
											CombinedHelper.AddElement(SubHelper.GetElementPtr(Index));
										}
									}
									DataProp->DestroyValue(SubValue.GetData());
								}
							}
							else if (FMapProperty* MapProp = CastField<FMapProperty>(DataProp))
							{
								if (!bHasCombined)
								{
									CombinedValue = MoveTemp(SubValue);
									bHasCombined = true;
								}
								else
								{
									FScriptMapHelper CombinedHelper(MapProp, CombinedValue.GetData());
									FScriptMapHelper SubHelper(MapProp, SubValue.GetData());
									for (int32 Index = 0; Index < SubHelper.GetMaxIndex(); ++Index)
									{
										if (SubHelper.IsValidIndex(Index))
										{
											CombinedHelper.AddPair(SubHelper.GetKeyPtr(Index), SubHelper.GetValuePtr(Index));
										}
									}
									DataProp->DestroyValue(SubValue.GetData());
								}
							}
							else
							{
								ErrorMessage = TEXT("Type Error: Unsupported data container for logic query");
								if (SubValue.Num() > 0)
								{
									DataProp->DestroyValue(SubValue.GetData());
								}
								for (int32 CleanupIndex = MergeIndex + 1; CleanupIndex < SubSlots.Num(); ++CleanupIndex)
								{
									if (SubValues[CleanupIndex].Num() > 0 && DataProp)
									{
										DataProp->DestroyValue(SubValues[CleanupIndex].GetData());
									}
								}
								break;
							}
						}
					}
				}
			}
		}
		UTurboStructLiteBPLibrary::SetParallelThreadLimit(PrevParallel);

		if (ErrorMessage.IsEmpty())
		{
			Stats.ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
			StatsText = FormatLogicStats(Stats);
			FTurboStructLiteSlotInfo SlotInfo;
			if (UTurboStructLiteBPLibrary::GetSlotInfoInternal(SlotCopy, SlotInfo))
			{
				SaveDate = SlotInfo.Timestamp;
			}
			bSuccess = true;
		}
		else if (CombinedValue.Num() > 0 && DataProp)
		{
			DataProp->DestroyValue(CombinedValue.GetData());
			CombinedValue.Reset();
		}

		if (bUseWriteAheadLog && !ErrorMessage.IsEmpty())
		{
			UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, ErrorMessage);
		}

		auto ApplyResults = [SlotCopy, DataProp, DataPtr, LoadDelegate, bUseWriteAheadLog, WALPath, bSuccess, ErrorMessage, Metadata, SaveDate, SaveVersion, StatsText, CombinedValue = MoveTemp(CombinedValue)]() mutable
		{
			if (!UTurboStructLiteBPLibrary::HasActiveGameWorld())
			{
				if (bUseWriteAheadLog)
				{
					UTurboStructLiteBPLibrary::DeleteWALFile(WALPath);
				}
				UTurboStructLiteBPLibrary::FinishQueuedLoad(SlotCopy);
				return;
			}
			if (!DataProp || !DataPtr)
			{
				if (bUseWriteAheadLog)
				{
					UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, TEXT("LoadArrayLogic skipped: invalid target pointer"));
				}
				FTurboStructLiteLogicLoadComplete Local = LoadDelegate;
				Local.ExecuteIfBound(false, TEXT("Type Error: Invalid target pointer"), FString(), FDateTime(0), SaveVersion, FString());
				UTurboStructLiteBPLibrary::FinishQueuedLoad(SlotCopy);
				return;
			}
			bool bApplied = false;
			if (bSuccess && CombinedValue.Num() > 0)
			{
				bool bSwapped = false;
				if (CastField<FArrayProperty>(DataProp))
				{
					FScriptArray* DestArray = static_cast<FScriptArray*>(DataPtr);
					FScriptArray* SrcArray = reinterpret_cast<FScriptArray*>(CombinedValue.GetData());
					FMemory::Memswap(DestArray, SrcArray, sizeof(FScriptArray));
					bSwapped = true;
				}
				else if (CastField<FMapProperty>(DataProp))
				{
					FScriptMap* DestMap = static_cast<FScriptMap*>(DataPtr);
					FScriptMap* SrcMap = reinterpret_cast<FScriptMap*>(CombinedValue.GetData());
					FMemory::Memswap(DestMap, SrcMap, sizeof(FScriptMap));
					bSwapped = true;
				}
				else if (CastField<FSetProperty>(DataProp))
				{
					FScriptSet* DestSet = static_cast<FScriptSet*>(DataPtr);
					FScriptSet* SrcSet = reinterpret_cast<FScriptSet*>(CombinedValue.GetData());
					FMemory::Memswap(DestSet, SrcSet, sizeof(FScriptSet));
					bSwapped = true;
				}
				else if (CastField<FStructProperty>(DataProp))
				{
					FMemory::Memswap(DataPtr, CombinedValue.GetData(), DataProp->GetSize());
					bSwapped = true;
				}
				if (!bSwapped)
				{
					DataProp->CopyCompleteValue(DataPtr, CombinedValue.GetData());
				}
				DataProp->DestroyValue(CombinedValue.GetData());
				bApplied = true;
			}
			else if (CombinedValue.Num() > 0 && DataProp)
			{
				DataProp->DestroyValue(CombinedValue.GetData());
			}
			if (bUseWriteAheadLog)
			{
				UTurboStructLiteBPLibrary::WriteWALEntry(WALPath, bApplied && bSuccess ? TEXT("LoadArrayLogic completed") : TEXT("LoadArrayLogic failed"));
				if (bApplied && bSuccess)
				{
					UTurboStructLiteBPLibrary::DeleteWALFile(WALPath);
				}
			}
			FTurboStructLiteLogicLoadComplete Local = LoadDelegate;
			Local.ExecuteIfBound(bApplied && bSuccess, ErrorMessage, Metadata, SaveDate, SaveVersion, StatsText);
			UTurboStructLiteBPLibrary::FinishQueuedLoad(SlotCopy);
		};

		if (bApplyOnGameThread)
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(ApplyResults));
		}
		else
		{
			ApplyResults();
		}
	};

	TArray<const FStructProperty*> DummyStructProps;
	const bool bCanBackground = DataProp && !DataProp->ContainsObjectReference(DummyStructProps, EPropertyObjectReferenceType::Strong);

	auto Task = [RunLogicLoad, bAsync, bCanBackground]() mutable
	{
		if (bAsync)
		{
			if (bCanBackground)
			{
				Async(EAsyncExecution::ThreadPool, [RunLogicLoad]() mutable
				{
#if WITH_EDITOR
					TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadArrayLogic_AsyncTask"));
#endif
					RunLogicLoad(true);
				});
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, [RunLogicLoad]() mutable
				{
#if WITH_EDITOR
					TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadArrayLogic_AsyncTask"));
#endif
					RunLogicLoad(true);
				});
			}
			return;
		}
#if WITH_EDITOR
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_LoadArrayLogic_Sync"));
#endif
		RunLogicLoad(false);
	};

	UTurboStructLiteBPLibrary::EnqueueTask(SlotCopy, MoveTemp(Task), ClampedPriority);
}

