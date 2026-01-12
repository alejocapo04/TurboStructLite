#include "TurboStructLiteBPLibrary.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/CriticalSection.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#if WITH_EDITOR
#include "Trace/Trace.inl"
#endif

bool UTurboStructLiteBPLibrary::EstimateWildcardSize(FProperty* DataProp, void* DataPtr, int64& OutSizeBytes)
{
	OutSizeBytes = 0;
	if (!DataProp || !DataPtr)
	{
		return false;
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, DataPtr);
		OutSizeBytes = static_cast<int64>(ArrayHelper.Num()) * ArrayProp->Inner->GetSize();
		return OutSizeBytes > 0;
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(DataProp))
	{
		FScriptSetHelper SetHelper(SetProp, DataPtr);
		OutSizeBytes = static_cast<int64>(SetHelper.Num()) * SetProp->ElementProp->GetSize();
		return OutSizeBytes > 0;
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(DataProp))
	{
		FScriptMapHelper MapHelper(MapProp, DataPtr);
		const int64 ElementSize = MapProp->KeyProp->GetSize() + MapProp->ValueProp->GetSize();
		OutSizeBytes = static_cast<int64>(MapHelper.Num()) * ElementSize;
		return OutSizeBytes > 0;
	}
	OutSizeBytes = DataProp->GetSize();
	return OutSizeBytes > 0;
}

void UTurboStructLiteBPLibrary::HandleWildcardLoad(FProperty* DataProp, void* DataPtr, const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FTurboStructLiteLoadComplete& LoadDelegate, bool bUseWriteAheadLog, int32 QueuePriority, int32 MaxParallelThreads, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, ETurboStructLiteBatchingSetting CompressionBatching, const TCHAR* OperationName, const TCHAR* WildcardLabelLower, const TCHAR* WildcardLabelUpper, const TCHAR* LoadLabel)
{
	if (!DataProp || !DataPtr)
	{
		ensureMsgf(false, TEXT("%s: invalid wildcard %s"), OperationName, WildcardLabelLower);
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: Failed to resolve Wildcard %s"), OperationName, WildcardLabelUpper), ELogVerbosity::Error);
		LoadDelegate.ExecuteIfBound(false);
		return;
	}

	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: Invalid slot parameters"), OperationName), ELogVerbosity::Error);
		LoadDelegate.ExecuteIfBound(false);
		return;
	}

	BeginMemoryOpMessage(MainSlotName, SubSlotIndex, false, false);
	const ETurboStructLiteEncryption SelectedEncryption = Encryption;
	const ETurboStructLiteEncryption ResolvedEncryption = SelectedEncryption == ETurboStructLiteEncryption::ProjectDefault ? GetActiveEncryptionMode() : SelectedEncryption;
	FString ResolvedKey = EncryptionKey;
	if (ResolvedEncryption == ETurboStructLiteEncryption::AES && ResolvedKey.IsEmpty())
	{
		ResolvedKey = GetActiveEncryptionKey();
		if (ResolvedKey.IsEmpty())
		{
			FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: AES selected but EncryptionKey is empty"), OperationName), ELogVerbosity::Error);
			EndMemoryOpMessage(MainSlotName, SubSlotIndex, false, false);
			LoadDelegate.ExecuteIfBound(false);
			return;
		}
	}
	FString WALPath;
	if (bUseWriteAheadLog)
	{
		const FString LoadOpLabel = FString::Printf(TEXT("%s_Enc%d"), LoadLabel, static_cast<int32>(ResolvedEncryption));
		WALPath = GenerateWALPath(MainSlotName, SubSlotIndex, LoadOpLabel);
		WriteWALEntry(WALPath, FString::Printf(TEXT("Queued %s Slot=%s SubSlot=%d Async=%s Encryption=%d"), LoadLabel, *MainSlotName, SubSlotIndex, bAsync ? TEXT("true") : TEXT("false"), static_cast<int32>(ResolvedEncryption)));
	}

	FTurboStructLiteLoadRequest Request;
	Request.SlotName = MainSlotName;
	Request.SubSlotIndex = SubSlotIndex;
	Request.bAsync = bAsync;
	Request.DefaultEncryption = ResolvedEncryption;
	Request.EncryptionKey = ResolvedKey;
	Request.DataProp = DataProp;
	Request.DataPtr = DataPtr;
	Request.QueuePriority = FMath::Clamp(QueuePriority, 0, 100);
	Request.MaxParallelThreads = FMath::Clamp(MaxParallelThreads, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	Request.CompressionBatching = CompressionBatching;
	Request.bUseWriteAheadLog = bUseWriteAheadLog;
	Request.WALPath = WALPath;
	Request.Callback = [LoadDelegate](bool bSuccess) mutable
	{
		FTurboStructLiteLoadComplete Local = LoadDelegate;
		Local.ExecuteIfBound(bSuccess);
	};
	EnqueueLoadRequest(MoveTemp(Request));
}

void UTurboStructLiteBPLibrary::HandleWildcardSave(FProperty* DataProp, void* DataPtr, const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FTurboStructLiteSaveComplete& SaveDelegate, bool bUseWriteAheadLog, bool bSaveOnlyMarked, int32 QueuePriority, int32 MaxParallelThreads, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, ETurboStructLiteCompression Compression, ETurboStructLiteBatchingSetting CompressionBatching, const TCHAR* OperationName, const TCHAR* WildcardLabelLower, const TCHAR* WildcardLabelUpper, const TCHAR* SaveLabel, bool bEmitDebugPropInfo)
{
	if (!DataProp || !DataPtr)
	{
		ensureMsgf(false, TEXT("%s: invalid wildcard %s"), OperationName, WildcardLabelLower);
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: Failed to resolve Wildcard %s"), OperationName, WildcardLabelUpper), ELogVerbosity::Error);
		SaveDelegate.ExecuteIfBound(false, FString(), SubSlotIndex);
		return;
	}

	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: Invalid slot parameters"), OperationName), ELogVerbosity::Error);
		SaveDelegate.ExecuteIfBound(false, FString(), SubSlotIndex);
		return;
	}

	const ETurboStructLiteCompression ResolvedCompression = ResolveCompression(Compression);
	const ETurboStructLiteBatchingSetting ResolvedBatchingSetting = CompressionBatching;
	const FString FilePath = BuildSavePath(MainSlotName);
	const int32 ClampedParallel = FMath::Clamp(MaxParallelThreads, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	const ETurboStructLiteEncryption SelectedEncryption = Encryption;
	const ETurboStructLiteEncryption ResolvedEncryption = SelectedEncryption == ETurboStructLiteEncryption::ProjectDefault ? GetActiveEncryptionMode() : SelectedEncryption;
	const FString ResolvedKey = EncryptionKey.IsEmpty() ? GetActiveEncryptionKey() : EncryptionKey;
	const bool bHasEncryptionKey = !ResolvedKey.IsEmpty();
	FString WALPath;
	if (bUseWriteAheadLog)
	{
		const FString SaveOpLabel = FString::Printf(TEXT("%s_Comp%d_Enc%d"), SaveLabel, static_cast<int32>(ResolvedCompression), static_cast<int32>(ResolvedEncryption));
		WALPath = GenerateWALPath(MainSlotName, SubSlotIndex, SaveOpLabel);
		WriteWALEntry(WALPath, FString::Printf(TEXT("Queued %s Slot=%s SubSlot=%d Async=%s Compression=%d Batching=%d Encryption=%d KeyProvided=%s"), SaveLabel, *MainSlotName, SubSlotIndex, bAsync ? TEXT("true") : TEXT("false"), static_cast<int32>(ResolvedCompression), static_cast<int32>(ResolvedBatchingSetting), static_cast<int32>(ResolvedEncryption), bHasEncryptionKey ? TEXT("true") : TEXT("false")));
	}
	BeginMemoryOpMessage(MainSlotName, SubSlotIndex, true, false);

	if (bAsync)
	{
#if WITH_EDITOR
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SaveWildcard_CaptureSnapshot"));
#endif
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Serialize snapshot start"));
		}
		TArray<uint8> Snapshot;
		const int32 ValueSize = DataProp->GetSize();
		if (ValueSize > 0)
		{
			Snapshot.SetNumUninitialized(ValueSize);
			DataProp->InitializeValue(Snapshot.GetData());
			DataProp->CopyCompleteValue(Snapshot.GetData(), DataPtr);
		}
		const bool bHasSnapshot = Snapshot.Num() > 0;

		if (ResolvedEncryption == ETurboStructLiteEncryption::AES && ResolvedKey.IsEmpty())
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("AES key missing"));
			}
			FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: AES selected but EncryptionKey is empty"), OperationName), ELogVerbosity::Error);
			EndMemoryOpMessage(MainSlotName, SubSlotIndex, true, false);
			SaveDelegate.ExecuteIfBound(false, FilePath, SubSlotIndex);
			return;
		}

		const FString SlotCopy = MainSlotName;
		const int32 SubSlotCopy = SubSlotIndex;
		const int32 PriorityCopy = FMath::Clamp(QueuePriority, 0, 100);
		const bool bUseLog = bUseWriteAheadLog;
		const FString WALPathCopy = WALPath;

		Async(EAsyncExecution::ThreadPool, [DataProp, Snapshot = MoveTemp(Snapshot), SaveDelegate, SlotCopy, SubSlotCopy, FilePath, ResolvedCompression, ResolvedEncryption, ResolvedKey, PriorityCopy, bHasSnapshot, ClampedParallel, ResolvedBatchingSetting, bUseLog, WALPathCopy, bSaveOnlyMarked]() mutable
		{
#if WITH_EDITOR
			TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SaveWildcard_AsyncTask"));
#endif
			int64 EstimatedBytes = 0;
			if (EstimateWildcardSize(DataProp, Snapshot.GetData(), EstimatedBytes))
			{
				UpdateMemoryPressureWarning(SlotCopy, SubSlotCopy, EstimatedBytes, true, false);
			}
			if (bUseLog)
			{
				WriteWALEntry(WALPathCopy, TEXT("Serialize start"));
			}
			TArray<uint8> RawBytes;
			{
				FScopedParallelLimitLite ThreadGuard(ClampedParallel);
				if (!SerializeWildcard(DataProp, Snapshot.GetData(), RawBytes, bSaveOnlyMarked))
				{
					if (bHasSnapshot)
					{
						DataProp->DestroyValue(Snapshot.GetData());
					}
					if (bUseLog)
					{
						WriteWALEntry(WALPathCopy, TEXT("Serialize failed"));
					}
					EndMemoryOpMessage(SlotCopy, SubSlotCopy, true, false);
					AsyncTask(ENamedThreads::GameThread, [SaveDelegate, FilePath, SubSlotCopy]()
					{
						FTurboStructLiteSaveComplete Local = SaveDelegate;
						Local.ExecuteIfBound(false, FilePath, SubSlotCopy);
					});
					return;
				}
			}
			if (bHasSnapshot)
			{
				DataProp->DestroyValue(Snapshot.GetData());
			}
			if (bUseLog)
			{
				WriteWALEntry(WALPathCopy, FString::Printf(TEXT("Serialize success Bytes=%d"), RawBytes.Num()));
			}
			UpdateMemoryPressureWarning(SlotCopy, SubSlotCopy, RawBytes.Num(), true, false);

			FTurboStructLiteSaveRequest Request;
			Request.SlotName = SlotCopy;
			Request.SubSlotIndex = SubSlotCopy;
			Request.Compression = ResolvedCompression;
			Request.Encryption = ResolvedEncryption;
			Request.EncryptionKey = ResolvedKey;
			Request.bAsync = true;
			Request.QueuePriority = PriorityCopy;
			Request.MaxParallelThreads = ClampedParallel;
			Request.CompressionBatching = ResolvedBatchingSetting;
			Request.bUseWriteAheadLog = bUseLog;
			Request.WALPath = WALPathCopy;
			Request.bSaveOnlyMarked = bSaveOnlyMarked;
			Request.RawBytes = MoveTemp(RawBytes);
			if (!BuildDebugMetadataFromBytes(Request.RawBytes, Request.DebugMetadata))
			{
				Request.DebugMetadata = BuildDebugMetadata(DataProp);
			}
			Request.Callback = [SaveDelegate](bool bSaved, FString CallbackFilePath, int32 CallbackSubSlot) mutable
			{
				FTurboStructLiteSaveComplete Local = SaveDelegate;
				Local.ExecuteIfBound(bSaved, CallbackFilePath, CallbackSubSlot);
			};

			AsyncTask(ENamedThreads::GameThread, [Request = MoveTemp(Request)]() mutable
			{
				EnqueueSaveRequest(MoveTemp(Request));
			});
		});
		return;
	}

	TArray<uint8> RawBytes;
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("TurboStructLite_SaveWildcard_SyncSerialize"));
#endif
	int64 EstimatedBytes = 0;
	if (EstimateWildcardSize(DataProp, DataPtr, EstimatedBytes))
	{
		UpdateMemoryPressureWarning(MainSlotName, SubSlotIndex, EstimatedBytes, true, false);
	}
	{
		FScopedParallelLimitLite ThreadGuard(ClampedParallel);
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Serialize start"));
		}
		if (!SerializeWildcard(DataProp, DataPtr, RawBytes, bSaveOnlyMarked))
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Serialize failed"));
			}
			FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: Serialization failed"), OperationName), ELogVerbosity::Warning);
			EndMemoryOpMessage(MainSlotName, SubSlotIndex, true, false);
			SaveDelegate.ExecuteIfBound(false, FilePath, SubSlotIndex);
			return;
		}
	}
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, FString::Printf(TEXT("Serialize success Bytes=%d"), RawBytes.Num()));
	}
	UpdateMemoryPressureWarning(MainSlotName, SubSlotIndex, RawBytes.Num(), true, false);

	if (bEmitDebugPropInfo)
	{
		const FString PropDesc = FString::Printf(TEXT("%s: Prop=%s Type=%s Size=%d RawBytes=%d"), OperationName, *DataProp->GetName(), *DataProp->GetCPPType(), DataProp->GetSize(), RawBytes.Num());
		FFrame::KismetExecutionMessage(*PropDesc, ELogVerbosity::Warning);
	}

	FTurboStructLiteSaveRequest Request;
	Request.SlotName = MainSlotName;
	Request.SubSlotIndex = SubSlotIndex;
	Request.Compression = ResolvedCompression;
	Request.bAsync = bAsync;
	Request.RawBytes = MoveTemp(RawBytes);
	Request.MaxParallelThreads = ClampedParallel;
	Request.CompressionBatching = ResolvedBatchingSetting;
	Request.QueuePriority = FMath::Clamp(QueuePriority, 0, 100);
	Request.bUseWriteAheadLog = bUseWriteAheadLog;
	Request.WALPath = WALPath;
	Request.bSaveOnlyMarked = bSaveOnlyMarked;
	if (!BuildDebugMetadataFromBytes(Request.RawBytes, Request.DebugMetadata))
	{
		Request.DebugMetadata = BuildDebugMetadata(DataProp);
	}
	if (ResolvedEncryption == ETurboStructLiteEncryption::AES && ResolvedKey.IsEmpty())
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("AES key missing"));
		}
		FFrame::KismetExecutionMessage(*FString::Printf(TEXT("%s: AES selected but EncryptionKey is empty"), OperationName), ELogVerbosity::Error);
		EndMemoryOpMessage(MainSlotName, SubSlotIndex, true, false);
		SaveDelegate.ExecuteIfBound(false, FilePath, SubSlotIndex);
		return;
	}
	Request.Encryption = ResolvedEncryption;
	Request.EncryptionKey = ResolvedKey;
	Request.Callback = [SaveDelegate](bool bSaved, FString CallbackFilePath, int32 CallbackSubSlot) mutable
	{
		FTurboStructLiteSaveComplete Local = SaveDelegate;
		Local.ExecuteIfBound(bSaved, CallbackFilePath, CallbackSubSlot);
	};
	EnqueueSaveRequest(MoveTemp(Request));
}

void UTurboStructLiteBPLibrary::TurboStructDeleteLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority)
{
	FTurboStructLiteDeleteComplete DelegateCopy = OnComplete;
	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructDeleteLite: Invalid slot parameters"), ELogVerbosity::Error);
		DelegateCopy.ExecuteIfBound(false);
		return;
	}

	const int32 ClampedPriority = FMath::Clamp(QueuePriority, 0, 100);

	auto Task = [Slot = MainSlotName, SubSlotIndex, bAsync, DelegateCopy]() mutable
	{
		auto Work = [Slot, SubSlotIndex]() -> bool
		{
			BeginSlotOperation(Slot);
			TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(Slot);
			bool bRemoved = false;
			{
				FScopeLock Lock(OpLock.Get());
				bRemoved = UTurboStructLiteBPLibrary::RemoveEntry(Slot, SubSlotIndex);
			}
			EndSlotOperation(Slot);
			return bRemoved;
		};

		if (bAsync)
		{
			Async(EAsyncExecution::ThreadPool, [Work, DelegateCopy, Slot]() mutable
			{
				const bool bRemoved = Work();
				AsyncTask(ENamedThreads::GameThread, [DelegateCopy, bRemoved, Slot]() mutable
				{
					if (!HasActiveGameWorld())
					{
						UTurboStructLiteBPLibrary::ProcessNextTask(Slot);
						return;
					}
					FTurboStructLiteDeleteComplete Local = DelegateCopy;
					Local.ExecuteIfBound(bRemoved);
					UTurboStructLiteBPLibrary::ProcessNextTask(Slot);
				});
			});
			return;
		}

		const bool bRemovedSync = Work();
		if (!HasActiveGameWorld())
		{
			UTurboStructLiteBPLibrary::ProcessNextTask(Slot);
			return;
		}
		DelegateCopy.ExecuteIfBound(bRemovedSync);
		UTurboStructLiteBPLibrary::ProcessNextTask(Slot);
	};

	TFunction<void()> CancelCallback = [DelegateCopy]() mutable
	{
		FTurboStructLiteDeleteComplete Local = DelegateCopy;
		Local.ExecuteIfBound(false);
	};
	EnqueueTask(MainSlotName, MoveTemp(Task), ClampedPriority, MoveTemp(CancelCallback));
}

void UTurboStructLiteBPLibrary::TurboStructExistLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, bool bCheckFileOnly, const FTurboStructLiteExistComplete& OnComplete, int32 QueuePriority)
{
	FTurboStructLiteExistComplete DelegateCopy = OnComplete;
	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructExistLite: Invalid slot parameters"), ELogVerbosity::Error);
		DelegateCopy.ExecuteIfBound(false);
		return;
	}

	const int32 ClampedPriority = FMath::Clamp(QueuePriority, 0, 100);

	auto Task = [Slot = MainSlotName, SubSlotIndex, bAsync, bCheckFileOnly, DelegateCopy]() mutable
	{
		auto Work = [Slot, SubSlotIndex, bCheckFileOnly]() -> bool
		{
			BeginSlotOperation(Slot);
			TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(Slot);
			bool bExists = false;
			{
				FScopeLock Lock(OpLock.Get());
				if (bCheckFileOnly)
				{
					const FString FilePath = BuildSavePath(Slot);
					bExists = FPaths::FileExists(FilePath);
				}
				else
				{
					bExists = ExistsEntry(Slot, SubSlotIndex);
				}
			}
			EndSlotOperation(Slot);
			return bExists;
		};

		if (bAsync)
		{
			Async(EAsyncExecution::ThreadPool, [Work, DelegateCopy, Slot]() mutable
			{
				const bool bExists = Work();
				AsyncTask(ENamedThreads::GameThread, [DelegateCopy, bExists, Slot]() mutable
				{
					if (!HasActiveGameWorld())
					{
						ProcessNextTask(Slot);
						return;
					}
					FTurboStructLiteExistComplete Local = DelegateCopy;
					Local.ExecuteIfBound(bExists);
					ProcessNextTask(Slot);
				});
			});
			return;
		}

		const bool bExistsSync = Work();
		if (!HasActiveGameWorld())
		{
			ProcessNextTask(Slot);
			return;
		}
		DelegateCopy.ExecuteIfBound(bExistsSync);
		ProcessNextTask(Slot);
	};

	TFunction<void()> CancelCallback = [DelegateCopy]() mutable
	{
		FTurboStructLiteExistComplete Local = DelegateCopy;
		Local.ExecuteIfBound(false);
	};
	EnqueueTask(MainSlotName, MoveTemp(Task), ClampedPriority, MoveTemp(CancelCallback));
}

void UTurboStructLiteBPLibrary::TurboStructLiteDeleteSlot(const FString& MainSlotName, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority)
{
	FTurboStructLiteDeleteComplete DelegateCopy = OnComplete;
	if (MainSlotName.IsEmpty())
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructLiteDeleteSlot: Invalid slot parameters"), ELogVerbosity::Error);
		DelegateCopy.ExecuteIfBound(false);
		return;
	}

	const int32 ClampedPriority = FMath::Clamp(QueuePriority, 0, 100);

	auto Task = [Slot = MainSlotName, bAsync, DelegateCopy]() mutable
	{
		auto Work = [Slot]() -> bool
		{
			BeginSlotOperation(Slot);
			TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(Slot);
			bool bDeleted = false;
			{
				FScopeLock Lock(OpLock.Get());
				const FString FilePath = BuildSavePath(Slot);
				bDeleted = IFileManager::Get().Delete(*FilePath, false, true);
				if (bDeleted)
				{
					InvalidateSlotIndex(Slot);
				}
			}
			EndSlotOperation(Slot);
			return bDeleted;
		};

		if (bAsync)
		{
			Async(EAsyncExecution::ThreadPool, [Work, DelegateCopy, Slot]() mutable
			{
				const bool bDeleted = Work();
				AsyncTask(ENamedThreads::GameThread, [DelegateCopy, bDeleted, Slot]() mutable
				{
					if (!HasActiveGameWorld())
					{
						ProcessNextTask(Slot);
						return;
					}
					FTurboStructLiteDeleteComplete Local = DelegateCopy;
					Local.ExecuteIfBound(bDeleted);
					ProcessNextTask(Slot);
				});
			});
			return;
		}

		const bool bDeletedSync = Work();
		if (!HasActiveGameWorld())
		{
			ProcessNextTask(Slot);
			return;
		}
		DelegateCopy.ExecuteIfBound(bDeletedSync);
		ProcessNextTask(Slot);
	};

	TFunction<void()> CancelCallback = [DelegateCopy]() mutable
	{
		FTurboStructLiteDeleteComplete Local = DelegateCopy;
		Local.ExecuteIfBound(false);
	};
	EnqueueTask(MainSlotName, MoveTemp(Task), ClampedPriority, MoveTemp(CancelCallback));
}

void UTurboStructLiteBPLibrary::TurboStructLiteCopySlot(const FString& SourceSlotName, const FString& TargetSlotName, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority)
{
	FTurboStructLiteDeleteComplete DelegateCopy = OnComplete;
	if (SourceSlotName.IsEmpty() || TargetSlotName.IsEmpty())
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructLiteCopySlot: Invalid slot parameters"), ELogVerbosity::Error);
		DelegateCopy.ExecuteIfBound(false);
		return;
	}

	const int32 ClampedPriority = FMath::Clamp(QueuePriority, 0, 100);

	auto Task = [Source = SourceSlotName, Target = TargetSlotName, bAsync, DelegateCopy]() mutable
	{
		auto Work = [Source, Target]() -> bool
		{
			const FString SourceSanitized = SanitizeSlotName(Source);
			const FString TargetSanitized = SanitizeSlotName(Target);
			const bool bSameSlot = SourceSanitized == TargetSanitized;
			BeginSlotOperation(SourceSanitized);
			if (!bSameSlot)
			{
				BeginSlotOperation(TargetSanitized);
			}
			const FString FirstName = SourceSanitized < TargetSanitized ? SourceSanitized : TargetSanitized;
			const FString SecondName = SourceSanitized < TargetSanitized ? TargetSanitized : SourceSanitized;
			TSharedPtr<FCriticalSection> FirstLock = GetSlotOperationLock(FirstName);
			TSharedPtr<FCriticalSection> SecondLock = GetSlotOperationLock(SecondName);
			bool bCopied = false;
			if (bSameSlot)
			{
				FScopeLock LockA(FirstLock.Get());
				const FString SourcePath = BuildSavePath(Source);
				const FString TargetPath = BuildSavePath(Target);
				bCopied = IFileManager::Get().Copy(*TargetPath, *SourcePath, true, true) == COPY_OK;
			}
			else
			{
				FScopeLock LockA(FirstLock.Get());
				FScopeLock LockB(SecondLock.Get());
				const FString SourcePath = BuildSavePath(Source);
				const FString TargetPath = BuildSavePath(Target);
				bCopied = IFileManager::Get().Copy(*TargetPath, *SourcePath, true, true) == COPY_OK;
			}
			if (bCopied)
			{
				InvalidateSlotIndex(Target);
			}
			EndSlotOperation(SourceSanitized);
			if (!bSameSlot)
			{
				EndSlotOperation(TargetSanitized);
			}
			return bCopied;
		};

		if (bAsync)
		{
			Async(EAsyncExecution::ThreadPool, [Work, DelegateCopy, Source]() mutable
			{
				const bool bCopied = Work();
				AsyncTask(ENamedThreads::GameThread, [DelegateCopy, bCopied, Source]() mutable
				{
					if (!HasActiveGameWorld())
					{
						ProcessNextTask(Source);
						return;
					}
					FTurboStructLiteDeleteComplete Local = DelegateCopy;
					Local.ExecuteIfBound(bCopied);
					ProcessNextTask(Source);
				});
			});
			return;
		}

		const bool bCopiedSync = Work();
		if (!HasActiveGameWorld())
		{
			ProcessNextTask(Source);
			return;
		}
		DelegateCopy.ExecuteIfBound(bCopiedSync);
		ProcessNextTask(Source);
	};

	TFunction<void()> CancelCallback = [DelegateCopy]() mutable
	{
		FTurboStructLiteDeleteComplete Local = DelegateCopy;
		Local.ExecuteIfBound(false);
	};
	EnqueueTask(SourceSlotName, MoveTemp(Task), ClampedPriority, MoveTemp(CancelCallback));
}

void UTurboStructLiteBPLibrary::TurboStructLiteRenameSlot(const FString& SourceSlotName, const FString& TargetSlotName, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority)
{
	FTurboStructLiteDeleteComplete DelegateCopy = OnComplete;
	if (SourceSlotName.IsEmpty() || TargetSlotName.IsEmpty())
	{
		FFrame::KismetExecutionMessage(TEXT("TurboStructLiteRenameSlot: Invalid slot parameters"), ELogVerbosity::Error);
		DelegateCopy.ExecuteIfBound(false);
		return;
	}

	const int32 ClampedPriority = FMath::Clamp(QueuePriority, 0, 100);

	auto Task = [Source = SourceSlotName, Target = TargetSlotName, bAsync, DelegateCopy]() mutable
	{
		auto Work = [Source, Target]() -> bool
		{
			const FString SourceSanitized = SanitizeSlotName(Source);
			const FString TargetSanitized = SanitizeSlotName(Target);
			const bool bSameSlot = SourceSanitized == TargetSanitized;
			BeginSlotOperation(SourceSanitized);
			if (!bSameSlot)
			{
				BeginSlotOperation(TargetSanitized);
			}
			const FString FirstName = SourceSanitized < TargetSanitized ? SourceSanitized : TargetSanitized;
			const FString SecondName = SourceSanitized < TargetSanitized ? TargetSanitized : SourceSanitized;
			TSharedPtr<FCriticalSection> FirstLock = GetSlotOperationLock(FirstName);
			TSharedPtr<FCriticalSection> SecondLock = GetSlotOperationLock(SecondName);
			bool bMoved = false;
			if (bSameSlot)
			{
				FScopeLock LockA(FirstLock.Get());
				const FString SourcePath = BuildSavePath(Source);
				const FString TargetPath = BuildSavePath(Target);
				bMoved = IFileManager::Get().Move(*TargetPath, *SourcePath, true, true, false, true);
			}
			else
			{
				FScopeLock LockA(FirstLock.Get());
				FScopeLock LockB(SecondLock.Get());
				const FString SourcePath = BuildSavePath(Source);
				const FString TargetPath = BuildSavePath(Target);
				bMoved = IFileManager::Get().Move(*TargetPath, *SourcePath, true, true, false, true);
			}
			if (bMoved)
			{
				InvalidateSlotIndex(Source);
				InvalidateSlotIndex(Target);
			}
			EndSlotOperation(SourceSanitized);
			if (!bSameSlot)
			{
				EndSlotOperation(TargetSanitized);
			}
			return bMoved;
		};

		if (bAsync)
		{
			Async(EAsyncExecution::ThreadPool, [Work, DelegateCopy, Source]() mutable
			{
				const bool bMoved = Work();
				AsyncTask(ENamedThreads::GameThread, [DelegateCopy, bMoved, Source]() mutable
				{
					if (!HasActiveGameWorld())
					{
						ProcessNextTask(Source);
						return;
					}
					FTurboStructLiteDeleteComplete Local = DelegateCopy;
					Local.ExecuteIfBound(bMoved);
					ProcessNextTask(Source);
				});
			});
			return;
		}

		const bool bMovedSync = Work();
		if (!HasActiveGameWorld())
		{
			ProcessNextTask(Source);
			return;
		}
		DelegateCopy.ExecuteIfBound(bMovedSync);
		ProcessNextTask(Source);
	};

	TFunction<void()> CancelCallback = [DelegateCopy]() mutable
	{
		FTurboStructLiteDeleteComplete Local = DelegateCopy;
		Local.ExecuteIfBound(false);
	};
	EnqueueTask(SourceSlotName, MoveTemp(Task), ClampedPriority, MoveTemp(CancelCallback));
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructLoadLite)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
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

	FTurboStructLiteLoadComplete LoadDelegate;
	if (OnComplete.IsBound())
	{
		LoadDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardLoad(DataProp, DataPtr, MainSlotName, SubSlotIndex, bAsync, LoadDelegate, bUseWriteAheadLog, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructLoadLite"), TEXT("data"), TEXT("Data"), TEXT("Load"));
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructSaveLite)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	FProperty* DataProp = Stack.MostRecentProperty;
	void* DataPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_UBOOL(bSaveOnlyMarked);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteCompression, Compression);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteSaveComplete SaveDelegate;
	if (OnComplete.IsBound())
	{
		SaveDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardSave(DataProp, DataPtr, MainSlotName, SubSlotIndex, bAsync, SaveDelegate, bUseWriteAheadLog, bSaveOnlyMarked, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteCompression>(Compression), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructSaveLite"), TEXT("data"), TEXT("Data"), TEXT("Save"), true);
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructSaveLiteArray)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FArrayProperty>(nullptr);
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Stack.MostRecentProperty);
	void* ArrayPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_UBOOL(bSaveOnlyMarked);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteCompression, Compression);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteSaveComplete SaveDelegate;
	if (OnComplete.IsBound())
	{
		SaveDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardSave(ArrayProp, ArrayPtr, MainSlotName, SubSlotIndex, bAsync, SaveDelegate, bUseWriteAheadLog, bSaveOnlyMarked, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteCompression>(Compression), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructSaveLiteArray"), TEXT("array"), TEXT("Array"), TEXT("SaveArray"), false);
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructLoadLiteArray)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FArrayProperty>(nullptr);
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Stack.MostRecentProperty);
	void* ArrayPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteLoadComplete LoadDelegate;
	if (OnComplete.IsBound())
	{
		LoadDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardLoad(ArrayProp, ArrayPtr, MainSlotName, SubSlotIndex, bAsync, LoadDelegate, bUseWriteAheadLog, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructLoadLiteArray"), TEXT("array"), TEXT("Array"), TEXT("LoadArray"));
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructSaveLiteMap)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FMapProperty>(nullptr);
	FMapProperty* MapProp = CastField<FMapProperty>(Stack.MostRecentProperty);
	void* MapPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_UBOOL(bSaveOnlyMarked);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteCompression, Compression);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteSaveComplete SaveDelegate;
	if (OnComplete.IsBound())
	{
		SaveDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardSave(MapProp, MapPtr, MainSlotName, SubSlotIndex, bAsync, SaveDelegate, bUseWriteAheadLog, bSaveOnlyMarked, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteCompression>(Compression), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructSaveLiteMap"), TEXT("map"), TEXT("Map"), TEXT("SaveMap"), false);
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructLoadLiteMap)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FMapProperty>(nullptr);
	FMapProperty* MapProp = CastField<FMapProperty>(Stack.MostRecentProperty);
	void* MapPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteLoadComplete LoadDelegate;
	if (OnComplete.IsBound())
	{
		LoadDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardLoad(MapProp, MapPtr, MainSlotName, SubSlotIndex, bAsync, LoadDelegate, bUseWriteAheadLog, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructLoadLiteMap"), TEXT("map"), TEXT("Map"), TEXT("LoadMap"));
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructSaveLiteSet)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FSetProperty>(nullptr);
	FSetProperty* SetProp = CastField<FSetProperty>(Stack.MostRecentProperty);
	void* SetPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_UBOOL(bSaveOnlyMarked);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteCompression, Compression);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteSaveComplete SaveDelegate;
	if (OnComplete.IsBound())
	{
		SaveDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardSave(SetProp, SetPtr, MainSlotName, SubSlotIndex, bAsync, SaveDelegate, bUseWriteAheadLog, bSaveOnlyMarked, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteCompression>(Compression), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructSaveLiteSet"), TEXT("set"), TEXT("Set"), TEXT("SaveSet"), false);
}

DEFINE_FUNCTION(UTurboStructLiteBPLibrary::execTurboStructLoadLiteSet)
{
	P_GET_PROPERTY(FStrProperty, MainSlotName);
	P_GET_PROPERTY(FIntProperty, SubSlotIndex);
	P_GET_UBOOL(bAsync);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FSetProperty>(nullptr);
	FSetProperty* SetProp = CastField<FSetProperty>(Stack.MostRecentProperty);
	void* SetPtr = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, OnComplete);
	P_GET_UBOOL(bUseWriteAheadLog);
	P_GET_PROPERTY(FIntProperty, QueuePriority);
	P_GET_PROPERTY(FIntProperty, MaxParallelThreads);
	P_GET_PROPERTY(FStrProperty, EncryptionKey);
	P_GET_ENUM(ETurboStructLiteEncryption, Encryption);
	P_GET_ENUM(ETurboStructLiteBatchingSetting, CompressionBatching);

	P_FINISH;

	FTurboStructLiteLoadComplete LoadDelegate;
	if (OnComplete.IsBound())
	{
		LoadDelegate.BindUFunction(OnComplete.GetUObject(), OnComplete.GetFunctionName());
	}
	HandleWildcardLoad(SetProp, SetPtr, MainSlotName, SubSlotIndex, bAsync, LoadDelegate, bUseWriteAheadLog, QueuePriority, MaxParallelThreads, EncryptionKey, static_cast<ETurboStructLiteEncryption>(Encryption), static_cast<ETurboStructLiteBatchingSetting>(CompressionBatching), TEXT("TurboStructLoadLiteSet"), TEXT("set"), TEXT("Set"), TEXT("LoadSet"));
}





