#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"
#include "Misc/Compression.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/Guid.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchive.h"
#include "UObject/UObjectGlobals.h"
#if __has_include("Serialization/StructuredArchiveAdapters.h")
#include "Serialization/StructuredArchiveAdapters.h"
#endif
#if WITH_EDITOR
#include "Trace/Trace.inl"
#endif
#include "Async/ParallelFor.h"
#include "HAL/ThreadSafeBool.h"
#include "Runtime/Launch/Resources/Version.h"


bool UTurboStructLiteBPLibrary::IsUnsupportedProperty(const FProperty* Property)
{
	if (!Property)
	{
		return true;
	}

	if (Property->IsA<FObjectPropertyBase>()
		|| Property->IsA<FInterfaceProperty>()
		|| Property->IsA<FDelegateProperty>()
		|| Property->IsA<FMulticastDelegateProperty>()
		|| Property->IsA<FFieldPathProperty>())
	{
		return true;
	}

	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		return IsUnsupportedProperty(ArrayProp->Inner);
	}

	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		return IsUnsupportedProperty(SetProp->ElementProp);
	}

	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		return IsUnsupportedProperty(MapProp->KeyProp) || IsUnsupportedProperty(MapProp->ValueProp);
	}

	return false;
}bool UTurboStructLiteBPLibrary::SerializePropertyRecursive(FProperty* Property, void* Address, TArray<uint8>& OutData, FTurboStructLiteFieldMeta& OutMeta, bool bSaveOnlyMarked)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SerializePropertyRecursive"));
#endif
	if (!ensureMsgf(Property && Address, TEXT("TurboStructLite SerializePropertyRecursive: invalid input")))
	{
		return false;
	}
	const FString AuthoredName = Property->GetAuthoredName();
	OutMeta.Name = AuthoredName.IsEmpty() ? Property->GetName() : AuthoredName;
	OutMeta.Type = Property->GetCPPType(nullptr, 0);
	const int32 StartOffset = OutData.Num();

	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		uint8* StructPtr = static_cast<uint8*>(Address);
		if (!ensureMsgf(StructPtr, TEXT("TurboStructLite SerializePropertyRecursive: null struct ptr")))
		{
			return false;
		}

		int32 BeforeChildren = OutData.Num();
		TArray<FProperty*> ChildProps;
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			ChildProps.Add(*It);
		}

		const int32 MaxThreads = FMath::Clamp(GetParallelThreadLimit(), 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		const int32 StructParallelThreshold = 8;

		bool bAllSafe = true;
		if (ChildProps.Num() >= StructParallelThreshold && MaxThreads > 1)
		{
			for (FProperty* ChildProp : ChildProps)
			{
				if (!ChildProp || !IsPropertySafeForParallel(ChildProp))
				{
					bAllSafe = false;
					break;
				}
			}
		}
		const bool bDoParallel = bAllSafe && ChildProps.Num() >= StructParallelThreshold && MaxThreads > 1;

		if (bDoParallel)
		{
			const int32 NumProps = ChildProps.Num();

			OutMeta.Children.SetNum(NumProps);
			TArray<uint8*> ChildAddresses;
			ChildAddresses.SetNum(NumProps);
			for (int32 Index = 0; Index < NumProps; ++Index)
			{
				ChildAddresses[Index] = ChildProps[Index] ? ChildProps[Index]->ContainerPtrToValuePtr<uint8>(StructPtr) : nullptr;
				if (!ensureMsgf(ChildAddresses[Index], TEXT("TurboStructLite SerializePropertyRecursive: null child address")))
				{
					return false;
				}
			}

			TArray<int32> WorkPropIndex;
			TArray<int32> WorkArrayOffset;
			TArray<int32> WorkArrayCount;
			WorkPropIndex.Reserve(NumProps * 2);
			WorkArrayOffset.Reserve(NumProps * 2);
			WorkArrayCount.Reserve(NumProps * 2);

			TArray<bool> bPropIsSplit;
			bPropIsSplit.Init(false, NumProps);

			const int32 ArraySplitThreshold = 1;
			const int32 ArrayChunkSize = 1;

			for (int32 PropIdx = 0; PropIdx < NumProps; ++PropIdx)
			{
				FProperty* Child = ChildProps[PropIdx];
				bool bSplit = false;
				if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Child))
				{
					if (IsPropertySafeForParallel(ArrayProp))
					{
						FScriptArrayHelper Helper(ArrayProp, ChildAddresses[PropIdx]);
						const int32 NumElems = Helper.Num();
						if (NumElems > ArraySplitThreshold)
						{
							int32 NumChunks = FMath::DivideAndRoundUp(NumElems, ArrayChunkSize);
							if (NumChunks < MaxThreads * 2)
							{
								NumChunks = MaxThreads * 4;
							}
							const int32 RealBatchSize = FMath::Max(1, FMath::DivideAndRoundUp(NumElems, NumChunks));
							for (int32 ChunkIdx = 0; ChunkIdx < NumChunks; ++ChunkIdx)
							{
								WorkPropIndex.Add(PropIdx);
								WorkArrayOffset.Add(ChunkIdx * RealBatchSize);
								WorkArrayCount.Add(FMath::Min(RealBatchSize, NumElems - WorkArrayOffset.Last()));
							}
							bSplit = true;
							bPropIsSplit[PropIdx] = true;
						}
					}
				}
				if (!bSplit)
				{
					WorkPropIndex.Add(PropIdx);
					WorkArrayOffset.Add(-1);
					WorkArrayCount.Add(0);
				}
			}

			const int32 WorkCount = WorkPropIndex.Num();
			TArray<TArray<uint8>> TaskBuffers;
			TaskBuffers.SetNum(WorkCount);
			TArray<TArray<int32>> PropTaskIds;
			PropTaskIds.SetNum(NumProps);
			for (int32 TaskIdx = 0; TaskIdx < WorkCount; ++TaskIdx)
			{
				PropTaskIds[WorkPropIndex[TaskIdx]].Add(TaskIdx);
			}

			FThreadSafeCounter NextTaskIndex(0);
			FThreadSafeBool bHasError(false);
			TArray<bool> PropOk;
			PropOk.Init(false, NumProps);

			const int32 NumActiveWorkers = FMath::Max(1, FMath::Min(MaxThreads, WorkCount));

#if WITH_EDITOR
			TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SerializePropertyRecursive_HybridParallel"));
#endif
			ParallelFor(NumActiveWorkers, [&](int32 WorkerID)
			{
				FScopedParallelLimitLite ThreadGuard(1);

				while (true)
				{
					const int32 TaskID = NextTaskIndex.Increment() - 1;
					if (TaskID >= WorkCount)
					{
						break;
					}
					if (bHasError)
					{
						break;
					}

					const int32 PropIdx = WorkPropIndex[TaskID];
					const int32 Offset = WorkArrayOffset[TaskID];
					const int32 Count = WorkArrayCount[TaskID];
					FProperty* Prop = ChildProps[PropIdx];
					uint8* Addr = ChildAddresses[PropIdx];

					if (Offset >= 0)
					{
						if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
						{
							FScriptArrayHelper Helper(ArrayProp, Addr);
							if (Offset + Count > Helper.Num())
							{
								bHasError = true;
								continue;
							}
							TArray<uint8>& Buffer = TaskBuffers[TaskID];
							FMemoryWriter Writer(Buffer, true);
							FObjectAndNameAsStringProxyArchive Ar(Writer, true);
							Ar.ArIsSaveGame = bSaveOnlyMarked;
							Ar.ArNoDelta = true;
							for (int32 LocalIdx = 0; LocalIdx < Count; ++LocalIdx)
							{
								const int32 ElemIdx = Offset + LocalIdx;
								uint8* ElemPtr = Helper.GetRawPtr(ElemIdx);
								checkSlow(ElemPtr);
								FStructuredArchiveFromArchive Structured(Ar);
								FStructuredArchive::FSlot Slot = Structured.GetSlot();
								ArrayProp->Inner->SerializeItem(Slot, ElemPtr, nullptr);
							}
						}
						else
						{
							bHasError = true;
						}
					}
					else
					{
						FTurboStructLiteFieldMeta LocalMeta;
						if (IsUnsupportedProperty(Prop) || !Addr)
						{
							bHasError = true;
						}
						else
						{
							if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
							{
								if (SerializeArrayParallel(ArrayProp, Addr, TaskBuffers[TaskID], LocalMeta, bSaveOnlyMarked))
								{
									OutMeta.Children[PropIdx] = MoveTemp(LocalMeta);
									PropOk[PropIdx] = true;
								}
								else
								{
								if (SerializePropertyRecursive(Prop, Addr, TaskBuffers[TaskID], LocalMeta, bSaveOnlyMarked))
									{
										OutMeta.Children[PropIdx] = MoveTemp(LocalMeta);
										PropOk[PropIdx] = true;
									}
									else
									{
										bHasError = true;
									}
								}
							}
							else
							{
								if (SerializePropertyRecursive(Prop, Addr, TaskBuffers[TaskID], LocalMeta, bSaveOnlyMarked))
								{
									OutMeta.Children[PropIdx] = MoveTemp(LocalMeta);
									PropOk[PropIdx] = true;
								}
								else
								{
									bHasError = true;
								}
							}
						}
					}
				}
			}, EParallelForFlags::Unbalanced);

			if (bHasError)
			{
				return false;
			}

			int32 TotalAddedSize = 0;
			for (const TArray<uint8>& Buff : TaskBuffers)
			{
				TotalAddedSize += Buff.Num();
			}
			for (int32 PropIdx = 0; PropIdx < NumProps; ++PropIdx)
			{
				if (bPropIsSplit[PropIdx])
				{
					TotalAddedSize += sizeof(int32);
				}
			}
			OutData.Reserve(OutData.Num() + TotalAddedSize);

#if WITH_EDITOR
			TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SerializePropertyRecursive_MergeBuffers"));
#endif
			for (int32 PropIdx = 0; PropIdx < NumProps; ++PropIdx)
			{
				const TArray<int32>& Tasks = PropTaskIds[PropIdx];
				if (Tasks.Num() == 0)
				{
					return false;
				}

				const int32 StartOffsetProp = OutData.Num();

				if (bPropIsSplit[PropIdx])
				{
					FProperty* Prop = ChildProps[PropIdx];
					FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
					if (!ArrayProp)
					{
						return false;
					}
					FScriptArrayHelper Helper(ArrayProp, ChildAddresses[PropIdx]);
					const int32 NumElems = Helper.Num();
					OutData.Append(reinterpret_cast<const uint8*>(&NumElems), sizeof(int32));

					for (int32 TaskId : Tasks)
					{
						OutData.Append(TaskBuffers[TaskId]);
					}

					FTurboStructLiteFieldMeta& Meta = OutMeta.Children[PropIdx];
					Meta.Name = ArrayProp->GetName();
					Meta.Type = ArrayProp->GetCPPType(nullptr, 0);
					Meta.Size = OutData.Num() - StartOffsetProp;
					PropOk[PropIdx] = true;
				}
				else
				{
					for (int32 TaskId : Tasks)
					{
						OutData.Append(TaskBuffers[TaskId]);
					}
				}
			}

			for (bool bOk : PropOk)
			{
				if (!bOk)
				{
					return false;
				}
			}
		}
		else
		{
			for (FProperty* ChildProp : ChildProps)
			{
				FTurboStructLiteFieldMeta ChildMeta;
				void* ChildAddr = ChildProp ? ChildProp->ContainerPtrToValuePtr<void>(StructPtr) : nullptr;
				if (!IsUnsupportedProperty(ChildProp) && ChildAddr && SerializePropertyRecursive(ChildProp, ChildAddr, OutData, ChildMeta, bSaveOnlyMarked))
				{
					OutMeta.Children.Add(MoveTemp(ChildMeta));
				}
			}
		}
		OutMeta.Size = OutData.Num() - BeforeChildren;
		return true;
	}
	else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		if (SerializeArrayParallel(ArrayProp, Address, OutData, OutMeta, bSaveOnlyMarked))
		{
			return true;
		}
		TArray<uint8> Local;
		FMemoryWriter Writer(Local, true);
		FObjectAndNameAsStringProxyArchive Ar(Writer, true);
		Ar.ArIsSaveGame = bSaveOnlyMarked;
		Ar.ArNoDelta = true;
		FStructuredArchiveFromArchive Structured(Ar);
		FStructuredArchive::FSlot Slot = Structured.GetSlot();
		ArrayProp->SerializeItem(Slot, Address, nullptr);
		OutData.Append(Local);
	}
	else
	{
		if (IsUnsupportedProperty(Property))
		{
			return false;
		}

		TArray<uint8> Local;
		FMemoryWriter Writer(Local, true);
		FObjectAndNameAsStringProxyArchive Ar(Writer, true);
		Ar.ArIsSaveGame = bSaveOnlyMarked;
		Ar.ArNoDelta = true;
		FStructuredArchiveFromArchive Structured(Ar);
		FStructuredArchive::FSlot Slot = Structured.GetSlot();
		Property->SerializeItem(Slot, Address, nullptr);
		OutData.Append(Local);
	}

	OutMeta.Size = OutData.Num() - StartOffset;
	return true;
}bool UTurboStructLiteBPLibrary::ApplyMetaToStruct(const TArray<FTurboStructLiteFieldMeta>& MetaFields, const UStruct* Struct, uint8* BasePtr, const uint8* Data, int32 DataLen, int32& Offset, int32 MaxThreads, bool bSaveOnlyMarked, const FString& PathPrefix, const FArchive& VersionSource)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_ApplyMetaToStruct"));
#endif
	if (!ensureMsgf(Struct && BasePtr && Data, TEXT("TurboStructLite ApplyMetaToStruct: invalid input")))
	{
		return false;
	}
	TArray<FProperty*> DestProps;
	TMap<FName, FProperty*> NameToProp;
	DestProps.Reserve(32);
	NameToProp.Reserve(32);
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* P = *It;
		DestProps.Add(P);
		NameToProp.Add(P->GetFName(), P);
		const FString AuthoredName = P->GetAuthoredName();
		if (!AuthoredName.IsEmpty())
		{
			NameToProp.Add(*AuthoredName, P);
			const FString NormalAuthored = NormalizeMetaFieldName(AuthoredName);
			if (NormalAuthored != AuthoredName)
			{
				NameToProp.Add(*NormalAuthored, P);
			}
		}
	}
	TMap<FString, FString> Redirects;
	LoadLegacyRedirects(Redirects);
	const TMap<FString, FString>* RedirectsPtr = Redirects.Num() > 0 ? &Redirects : nullptr;

	TArray<int32> MetaOffsets;
	MetaOffsets.SetNum(MetaFields.Num());
	int32 RunningOffset = Offset;
	for (int32 Index = 0; Index < MetaFields.Num(); ++Index)
	{
		const FTurboStructLiteFieldMeta& Meta = MetaFields[Index];
		const int64 NextOffset = static_cast<int64>(RunningOffset) + static_cast<int64>(Meta.Size);
		if (Meta.Size < 0 || NextOffset > DataLen)
		{
			Offset = DataLen;
			return false;
		}
		MetaOffsets[Index] = RunningOffset;
		RunningOffset = static_cast<int32>(NextOffset);
	}
	const int32 FinalOffset = RunningOffset;

	TArray<FTurboStructLiteLoadWorkUnit> WorkList;
	WorkList.Reserve(MetaFields.Num() * 2);

	TArray<FProperty*> ResolvedProps;
	ResolvedProps.SetNum(MetaFields.Num());
	TArray<bool> ResolvedTypeMatches;
	ResolvedTypeMatches.Init(false, MetaFields.Num());

	const int32 ArraySplitThreshold = 100;
	const int32 MaxSlices = MaxThreads * 32;

	for (int32 MetaIdx = 0; MetaIdx < MetaFields.Num(); ++MetaIdx)
	{
		const FTurboStructLiteFieldMeta& Meta = MetaFields[MetaIdx];
		FProperty* TargetProp = nullptr;
		bool bTypeMatch = false;
		const FString MetaBaseName = NormalizeMetaFieldName(Meta.Name);
		FString ResolvedName = MetaBaseName;
		if (RedirectsPtr)
		{
			const FString ScopedPathBase = PathPrefix.IsEmpty() ? MetaBaseName : PathPrefix + TEXT(".") + MetaBaseName;
			const FString ScopedPathRaw = PathPrefix.IsEmpty() ? Meta.Name : PathPrefix + TEXT(".") + Meta.Name;
			if (const FString* Found = RedirectsPtr->Find(ScopedPathBase))
			{
				ResolvedName = *Found;
			}
			else if (const FString* FoundRaw = RedirectsPtr->Find(ScopedPathRaw))
			{
				ResolvedName = *FoundRaw;
			}
			else if (const FString* FoundGlobal = RedirectsPtr->Find(MetaBaseName))
			{
				ResolvedName = *FoundGlobal;
			}
			else if (const FString* FoundGlobalRaw = RedirectsPtr->Find(Meta.Name))
			{
				ResolvedName = *FoundGlobalRaw;
			}
		}

		const FString ResolvedNormalized = NormalizeMetaFieldName(ResolvedName);
		if (FProperty** Found = NameToProp.Find(*ResolvedNormalized))
		{
			TargetProp = *Found;
			bTypeMatch = NormalizeTypeName(TargetProp->GetCPPType(nullptr, 0)) == NormalizeTypeName(Meta.Type);
		}
		if (!TargetProp && ResolvedNormalized != MetaBaseName)
		{
			if (FProperty** Found = NameToProp.Find(*MetaBaseName))
			{
				TargetProp = *Found;
				bTypeMatch = NormalizeTypeName(TargetProp->GetCPPType(nullptr, 0)) == NormalizeTypeName(Meta.Type);
			}
		}

		if (!TargetProp && DestProps.IsValidIndex(MetaIdx))
		{
			FProperty* Candidate = DestProps[MetaIdx];
			if (NormalizeTypeName(Candidate->GetCPPType(nullptr, 0)) == NormalizeTypeName(Meta.Type))
			{
				TargetProp = Candidate;
				bTypeMatch = true;
			}
		}

		if (TargetProp)
		{
			const FString MetaType = NormalizeTypeName(Meta.Type);
			const bool bMetaIsContainer = MetaType.StartsWith(TEXT("tarray<")) || MetaType.StartsWith(TEXT("tset<")) || MetaType.StartsWith(TEXT("tmap<"));
			const bool bTargetIsContainer = TargetProp->IsA<FArrayProperty>() || TargetProp->IsA<FSetProperty>() || TargetProp->IsA<FMapProperty>();
			if (!bTypeMatch && (bMetaIsContainer || bTargetIsContainer))
			{
				TargetProp = nullptr;
				bTypeMatch = false;
			}
		}

		ResolvedProps[MetaIdx] = TargetProp;
		ResolvedTypeMatches[MetaIdx] = TargetProp && bTypeMatch;

		if (!TargetProp)
		{
			continue;
		}

		bool bSplitted = false;

		if (ResolvedTypeMatches[MetaIdx])
		{
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(TargetProp))
			{
				bool bIsFixedSize = ArrayProp->Inner->HasAnyPropertyFlags(CPF_IsPlainOldData);
				if (!bIsFixedSize)
				{
					if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
					{
						if (InnerStruct->Struct->StructFlags & STRUCT_IsPlainOldData)
						{
							bIsFixedSize = true;
						}
					}
				}

				if (bIsFixedSize && Meta.Size >= 4)
				{
					int32 NumStored = 0;
					const uint8* PropData = Data + MetaOffsets[MetaIdx];
					checkSlow(PropData);
					checkSlow(MetaOffsets[MetaIdx] >= 0);
					checkSlow(MetaOffsets[MetaIdx] + static_cast<int32>(sizeof(int32)) <= DataLen);
					FMemory::Memcpy(&NumStored, PropData, sizeof(int32));

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
					const int32 ElementSize = ArrayProp->Inner->GetElementSize();
#else
					const int32 ElementSize = ArrayProp->Inner->GetSize();
#endif
					const int64 ExpectedSize = static_cast<int64>(NumStored) * ElementSize + 4;

					if (ExpectedSize == Meta.Size && NumStored > ArraySplitThreshold)
					{
						void* ArrayAddr = ArrayProp->ContainerPtrToValuePtr<void>(BasePtr);
						FScriptArrayHelper Helper(ArrayProp, ArrayAddr);
						Helper.Resize(NumStored);

						int32 NumChunks = FMath::Clamp(NumStored / 1000, MaxThreads, MaxSlices);
						const int32 BatchSize = FMath::Max(1, FMath::DivideAndRoundUp(NumStored, NumChunks));

						for (int32 ChunkIdx = 0; ChunkIdx < NumChunks; ++ChunkIdx)
						{
							FTurboStructLiteLoadWorkUnit Unit;
							Unit.MetaIndex = MetaIdx;
							Unit.ArrayOffset = ChunkIdx * BatchSize;
							Unit.ArrayCount = FMath::Min(BatchSize, NumStored - Unit.ArrayOffset);
							if (Unit.ArrayCount > 0)
							{
								WorkList.Add(Unit);
							}
						}
						bSplitted = true;
					}
				}
			}
		}

		if (!bSplitted)
		{
			FTurboStructLiteLoadWorkUnit Unit;
			Unit.MetaIndex = MetaIdx;
			Unit.ArrayOffset = -1;
			Unit.ArrayCount = 0;
			WorkList.Add(Unit);
		}
	}

	const int32 MaxThreadsClamped = FMath::Clamp(MaxThreads, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	const int32 WorkCount = WorkList.Num();
	const int32 NumActiveWorkers = FMath::Max(1, FMath::Min(MaxThreadsClamped, WorkCount));

	FThreadSafeCounter NextTaskIndex(0);
	FThreadSafeBool bHasError(false);

	ParallelFor(NumActiveWorkers, [&](int32 WorkerID)
	{
		FScopedParallelLimitLite ThreadGuard(MaxThreadsClamped);

		while (true)
		{
			const int32 TaskID = NextTaskIndex.Increment() - 1;
			if (TaskID >= WorkCount)
			{
				break;
			}
			if (bHasError)
			{
				break;
			}

			const FTurboStructLiteLoadWorkUnit& Unit = WorkList[TaskID];
			const FTurboStructLiteFieldMeta& Meta = MetaFields[Unit.MetaIndex];
			FProperty* TargetProp = ResolvedProps[Unit.MetaIndex];
			const int32 PropOffset = MetaOffsets[Unit.MetaIndex];
			const bool bTypeMatch = ResolvedTypeMatches[Unit.MetaIndex];
			if (!TargetProp)
			{
				continue;
			}
			uint8* TargetPtr = TargetProp->ContainerPtrToValuePtr<uint8>(BasePtr);
			checkSlow(TargetPtr);

			if (Unit.ArrayOffset != -1)
			{
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(TargetProp);
				checkSlow(ArrayProp);
				FScriptArrayHelper Helper(ArrayProp, TargetPtr);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				const int32 ElementSize = ArrayProp->Inner->GetElementSize();
#else
				const int32 ElementSize = ArrayProp->Inner->GetSize();
#endif

				const uint8* SrcData = Data + PropOffset + sizeof(int32) + (Unit.ArrayOffset * ElementSize);
				uint8* DestData = Helper.GetRawPtr(Unit.ArrayOffset);
				checkSlow(DestData);
				checkSlow(PropOffset >= 0);
				checkSlow(Unit.ArrayOffset >= 0);
				checkSlow(Unit.ArrayCount >= 0);
				checkSlow(static_cast<int64>(PropOffset) + sizeof(int32) + (static_cast<int64>(Unit.ArrayOffset) + static_cast<int64>(Unit.ArrayCount)) * ElementSize <= DataLen);

				FMemory::Memcpy(DestData, SrcData, Unit.ArrayCount * ElementSize);
			}
			else
			{
				checkSlow(PropOffset >= 0);
				checkSlow(static_cast<int64>(PropOffset) + Meta.Size <= DataLen);
				if (FStructProperty* StructProp = CastField<FStructProperty>(TargetProp))
				{
					if (Meta.Children.Num() > 0)
					{
						const FString ChildPath = PathPrefix.IsEmpty() ? Meta.Name : PathPrefix + TEXT(".") + Meta.Name;
						int32 ChildOffset = 0;
						if (!ApplyMetaToStruct(Meta.Children, StructProp->Struct, TargetPtr, Data + PropOffset, Meta.Size, ChildOffset, MaxThreadsClamped, bSaveOnlyMarked, ChildPath, VersionSource))
						{
							bHasError = true;
						}
					}
					else if (bTypeMatch)
					{
						if (!DeserializePropertyFromSlice(TargetProp, TargetPtr, Data + PropOffset, Meta.Size, bSaveOnlyMarked, VersionSource))
						{
							bHasError = true;
						}
					}
				}
				else
				{
					if (bTypeMatch)
					{
						if (!DeserializePropertyFromSlice(TargetProp, TargetPtr, Data + PropOffset, Meta.Size, bSaveOnlyMarked, VersionSource))
						{
							bHasError = true;
						}
					}
					else
					{
						bool bReaderError = false;
						if (!TryMigratePropertyValue(Meta, TargetProp, TargetPtr, Data + PropOffset, Meta.Size, bSaveOnlyMarked, VersionSource, bReaderError))
						{
							if (bReaderError)
							{
								bHasError = true;
							}
						}
					}
				}
			}
		}

	}, EParallelForFlags::Unbalanced);

	if (bHasError)
	{
		Offset = DataLen;
		return false;
	}

	Offset = FinalOffset;
	return true;
}bool UTurboStructLiteBPLibrary::ReadMetaFromBytes(const TArray<uint8>& InBytes, TArray<FTurboStructLiteFieldMeta>& OutFields, const uint8*& OutDataPtr, int32& OutDataLen, FString& OutErrorMessage)
{
	OutFields.Reset();
	OutErrorMessage.Reset();
	OutDataPtr = nullptr;
	OutDataLen = 0;

	if (InBytes.Num() < static_cast<int32>(sizeof(int32) * 2))
	{
		OutErrorMessage = TEXT("IO Error: Invalid data header");
		return false;
	}

	FMemoryReader Reader(InBytes, true);
	int32 FormatVersion = 0;
	Reader << FormatVersion;
	if (FormatVersion != 1)
	{
		OutErrorMessage = TEXT("IO Error: Unsupported data format");
		return false;
	}

	int32 MetaSize = 0;
	Reader << MetaSize;
	if (MetaSize < 0 || Reader.TotalSize() < Reader.Tell() + MetaSize)
	{
		OutErrorMessage = TEXT("IO Error: Invalid metadata size");
		return false;
	}

	TArray<uint8> MetaBytes;
	MetaBytes.SetNum(MetaSize);
	if (MetaSize > 0)
	{
		Reader.Serialize(MetaBytes.GetData(), MetaSize);
	}

	OutDataPtr = InBytes.GetData() + Reader.Tell();
	OutDataLen = InBytes.Num() - Reader.Tell();

	FMemoryReader MetaReader(MetaBytes, true);
	int32 Count = 0;
	MetaReader << Count;
	if (Count < 0)
	{
		OutErrorMessage = TEXT("IO Error: Invalid metadata count");
		return false;
	}
	OutFields.SetNum(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!ReadFieldMeta(MetaReader, OutFields[Index]))
		{
			OutErrorMessage = TEXT("IO Error: Failed to read metadata");
			return false;
		}
	}
	return true;
}const TArray<FTurboStructLiteFieldMeta>* UTurboStructLiteBPLibrary::ResolveStructMetaFields(const TArray<FTurboStructLiteFieldMeta>& Fields, const FStructProperty* StructProp)
{
	if (!StructProp || Fields.Num() == 0)
	{
		return nullptr;
	}
	if (Fields.Num() == 1)
	{
		const FString RootType = NormalizeTypeName(Fields[0].Type);
		const FString StructType = NormalizeTypeName(StructProp->GetCPPType(nullptr, 0));
		if (RootType == StructType)
		{
			return &Fields[0].Children;
		}
	}
	return &Fields;
}bool UTurboStructLiteBPLibrary::FindMetaByPropertyChain(const TArray<FTurboStructLiteFieldMeta>& MetaFields, const TArray<FProperty*>& PropertyChain, int32& OutOffset, const FTurboStructLiteFieldMeta*& OutMeta)
{
	OutOffset = 0;
	OutMeta = nullptr;
	if (MetaFields.Num() == 0 || PropertyChain.Num() == 0)
	{
		return false;
	}

	const TArray<FTurboStructLiteFieldMeta>* CurrentFields = &MetaFields;
	int32 TotalOffset = 0;

	for (int32 ChainIndex = 0; ChainIndex < PropertyChain.Num(); ++ChainIndex)
	{
		FProperty* Prop = PropertyChain[ChainIndex];
		if (!Prop)
		{
			return false;
		}
		int32 LocalOffset = 0;
		const FTurboStructLiteFieldMeta* FoundMeta = nullptr;
		for (int32 MetaIndex = 0; MetaIndex < CurrentFields->Num(); ++MetaIndex)
		{
			const FTurboStructLiteFieldMeta& Meta = (*CurrentFields)[MetaIndex];
			if (NamesMatchForMigration(Meta.Name, Prop))
			{
				FoundMeta = &Meta;
				break;
			}
			LocalOffset += Meta.Size;
		}
		if (!FoundMeta)
		{
			return false;
		}
		const FString MetaType = NormalizeTypeName(FoundMeta->Type);
		const FString PropType = NormalizeTypeName(Prop->GetCPPType(nullptr, 0));
		if (!MetaType.IsEmpty() && MetaType != PropType)
		{
			return false;
		}
		TotalOffset += LocalOffset;
		if (ChainIndex == PropertyChain.Num() - 1)
		{
			OutOffset = TotalOffset;
			OutMeta = FoundMeta;
			return true;
		}
		if (FoundMeta->Children.Num() == 0)
		{
			return false;
		}
		CurrentFields = &FoundMeta->Children;
	}
	return false;
}bool UTurboStructLiteBPLibrary::DeserializePropertyFromSlice(FProperty* Property, void* Address, const uint8* DataPtr, int32 DataSize, bool bSaveOnlyMarked, const FArchive& VersionSource)
{
	if (!Property || !Address || !DataPtr || DataSize <= 0)
	{
		return false;
	}
	FMemoryReaderView ReaderView(MakeArrayView(DataPtr, DataSize));
	FObjectAndNameAsStringProxyArchive Ar(ReaderView, true);
	CopyArchiveVersions(Ar, VersionSource);
	Ar.ArIsSaveGame = bSaveOnlyMarked;
	Ar.ArNoDelta = true;
	FStructuredArchiveFromArchive Structured(Ar);
	FStructuredArchive::FSlot Slot = Structured.GetSlot();
	Property->SerializeItem(Slot, Address, nullptr);
	return !ReaderView.IsError();
}



