#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLite.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "TurboStructLiteTypes.h"
#include "Misc/ScopeLock.h"
#include "UObject/UnrealType.h"
#include "Runtime/Launch/Resources/Version.h"
#include "TurboStructLiteDebugMacros.h"

FCriticalSection UTurboStructLiteBPLibrary::QueuesMutex;
TMap<FString, TSharedPtr<FTurboStructLiteTaskQueue>> UTurboStructLiteBPLibrary::QueuesBySlot;
FCriticalSection UTurboStructLiteBPLibrary::SlotOperationMutex;
TMap<FString, TSharedPtr<FCriticalSection>> UTurboStructLiteBPLibrary::SlotOperationLocks;
FCriticalSection UTurboStructLiteBPLibrary::ActiveSlotOpsMutex;
TMap<FString, int32> UTurboStructLiteBPLibrary::ActiveSlotOps;
int32 UTurboStructLiteBPLibrary::ActiveSlotOpsTotal = 0;

TSharedPtr<FTurboStructLiteTaskQueue> UTurboStructLiteBPLibrary::GetQueueForSlot(const FString& SlotName)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	FScopeLock Lock(&QueuesMutex);
	if (TSharedPtr<FTurboStructLiteTaskQueue>* Found = QueuesBySlot.Find(SanitizedName))
	{
		return *Found;
	}
	TSharedPtr<FTurboStructLiteTaskQueue> NewQueue = MakeShared<FTurboStructLiteTaskQueue>();
	QueuesBySlot.Add(SanitizedName, NewQueue);
	return NewQueue;
}

TSharedPtr<FCriticalSection> UTurboStructLiteBPLibrary::GetSlotOperationLock(const FString& SlotName)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	FScopeLock Lock(&SlotOperationMutex);
	if (TSharedPtr<FCriticalSection>* Found = SlotOperationLocks.Find(SanitizedName))
	{
		return *Found;
	}
	TSharedPtr<FCriticalSection> NewLock = MakeShared<FCriticalSection>();
	SlotOperationLocks.Add(SanitizedName, NewLock);
	return NewLock;
}

void UTurboStructLiteBPLibrary::BeginSlotOperation(const FString& SlotName)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	if (SanitizedName.IsEmpty())
	{
		return;
	}
	FScopeLock Lock(&ActiveSlotOpsMutex);
	int32& Count = ActiveSlotOps.FindOrAdd(SanitizedName);
	++Count;
	++ActiveSlotOpsTotal;
}

void UTurboStructLiteBPLibrary::EndSlotOperation(const FString& SlotName)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	if (SanitizedName.IsEmpty())
	{
		return;
	}
	FScopeLock Lock(&ActiveSlotOpsMutex);
	if (int32* Count = ActiveSlotOps.Find(SanitizedName))
	{
		--(*Count);
		if (*Count <= 0)
		{
			ActiveSlotOps.Remove(SanitizedName);
		}
	}
	if (ActiveSlotOpsTotal > 0)
	{
		--ActiveSlotOpsTotal;
	}
}

bool UTurboStructLiteBPLibrary::HasActiveSlotOperation(const FString& SlotName)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	if (SanitizedName.IsEmpty())
	{
		return false;
	}
	FScopeLock Lock(&ActiveSlotOpsMutex);
	if (const int32* Count = ActiveSlotOps.Find(SanitizedName))
	{
		return *Count > 0;
	}
	return false;
}

bool UTurboStructLiteBPLibrary::HasAnyActiveSlotOperation()
{
	FScopeLock Lock(&ActiveSlotOpsMutex);
	return ActiveSlotOpsTotal > 0;
}

bool UTurboStructLiteBPLibrary::HasActiveGameWorld()
{
	if (!GEngine)
	{
		return false;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::GamePreview) && !World->bIsTearingDown)
		{
			return true;
		}
	}
	return false;
}

void UTurboStructLiteBPLibrary::ClearAllQueues()
{
	TArray<TFunction<void()>> CancelCallbacks;
	TArray<FString> QueuesToRemove;
	{
		FScopeLock GlobalLock(&QueuesMutex);
		for (auto& Pair : QueuesBySlot)
		{
			if (TSharedPtr<FTurboStructLiteTaskQueue> Queue = Pair.Value)
			{
				FScopeLock QueueLock(&Queue->Mutex);
				for (FTurboStructLiteQueuedTask& Task : Queue->PendingTasks)
				{
					if (Task.CancelCallback)
					{
						CancelCallbacks.Add(MoveTemp(Task.CancelCallback));
					}
				}
				Queue->PendingTasks.Reset();
				if (!Queue->bTaskInProgress)
				{
					QueuesToRemove.Add(Pair.Key);
				}
			}
		}
		for (const FString& Key : QueuesToRemove)
		{
			QueuesBySlot.Remove(Key);
		}
	}
	DispatchCancelCallbacks(MoveTemp(CancelCallbacks));
}

void UTurboStructLiteBPLibrary::DispatchCancelCallbacks(TArray<TFunction<void()>>&& Callbacks)
{
	if (Callbacks.Num() == 0)
	{
		return;
	}
	if (IsInGameThread())
	{
		for (TFunction<void()>& Callback : Callbacks)
		{
			if (Callback)
			{
				Callback();
			}
		}
		return;
	}
	AsyncTask(ENamedThreads::GameThread, [Callbacks = MoveTemp(Callbacks)]() mutable
	{
		for (TFunction<void()>& Callback : Callbacks)
		{
			if (Callback)
			{
				Callback();
			}
		}
	});
}

int32 UTurboStructLiteBPLibrary::TurboStructLiteGetPendingCount(const FString& SlotName)
{
	if (SlotName.IsEmpty())
	{
		return 0;
	}

	const FString SanitizedName = SanitizeSlotName(SlotName);
	FScopeLock GlobalLock(&QueuesMutex);
	if (const TSharedPtr<FTurboStructLiteTaskQueue>* Found = QueuesBySlot.Find(SanitizedName))
	{
		const TSharedPtr<FTurboStructLiteTaskQueue> Queue = *Found;
		if (Queue.IsValid())
		{
			FScopeLock QueueLock(&Queue->Mutex);
			int32 Total = Queue->PendingTasks.Num();
			if (Queue->bTaskInProgress)
			{
				++Total;
			}
			return Total;
		}
	}
	return 0;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteIsSlotBusy(const FString& SlotName)
{
	if (SlotName.IsEmpty())
	{
		return false;
	}
	if (HasActiveSlotOperation(SlotName))
	{
		return true;
	}
	return TurboStructLiteGetPendingCount(SlotName) > 0;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteIsSystemBusy()
{
	if (HasAnyActiveSlotOperation())
	{
		return true;
	}
	FScopeLock GlobalLock(&QueuesMutex);
	for (const auto& Pair : QueuesBySlot)
	{
		const TSharedPtr<FTurboStructLiteTaskQueue> Queue = Pair.Value;
		if (Queue.IsValid())
		{
			FScopeLock QueueLock(&Queue->Mutex);
			if (Queue->bTaskInProgress || Queue->PendingTasks.Num() > 0)
			{
				return true;
			}
		}
	}
	return false;
}

void UTurboStructLiteBPLibrary::TurboStructLiteClearQueues(bool bClearAll, const FString& SlotName, int32 Priority)
{
	if (bClearAll)
	{
		ClearAllQueues();
		return;
	}

	const bool bFilterPriority = Priority >= 0 && Priority <= 100;
	if (SlotName.IsEmpty() && !bFilterPriority)
	{
		return;
	}

	TArray<TFunction<void()>> CancelCallbacks;
	TArray<FString> QueuesToRemove;
	{
		FScopeLock GlobalLock(&QueuesMutex);
		if (SlotName.IsEmpty())
		{
			for (auto& Pair : QueuesBySlot)
			{
				if (TSharedPtr<FTurboStructLiteTaskQueue> Queue = Pair.Value)
				{
					FScopeLock QueueLock(&Queue->Mutex);
					TArray<FTurboStructLiteQueuedTask> Remaining;
					Remaining.Reserve(Queue->PendingTasks.Num());
					for (FTurboStructLiteQueuedTask& Task : Queue->PendingTasks)
					{
						if (Task.Priority == Priority)
						{
							if (Task.CancelCallback)
							{
								CancelCallbacks.Add(MoveTemp(Task.CancelCallback));
							}
						}
						else
						{
							Remaining.Add(MoveTemp(Task));
						}
					}
					Queue->PendingTasks = MoveTemp(Remaining);
					if (Queue->PendingTasks.Num() == 0 && !Queue->bTaskInProgress)
					{
						QueuesToRemove.Add(Pair.Key);
					}
				}
			}
		}
		else
		{
			const FString SanitizedName = SanitizeSlotName(SlotName);
			if (TSharedPtr<FTurboStructLiteTaskQueue>* FoundQueue = QueuesBySlot.Find(SanitizedName))
			{
				TSharedPtr<FTurboStructLiteTaskQueue> Queue = *FoundQueue;
				if (Queue.IsValid())
				{
					FScopeLock QueueLock(&Queue->Mutex);
					if (bFilterPriority)
					{
						TArray<FTurboStructLiteQueuedTask> Remaining;
						Remaining.Reserve(Queue->PendingTasks.Num());
						for (FTurboStructLiteQueuedTask& Task : Queue->PendingTasks)
						{
							if (Task.Priority == Priority)
							{
								if (Task.CancelCallback)
								{
									CancelCallbacks.Add(MoveTemp(Task.CancelCallback));
								}
							}
							else
							{
								Remaining.Add(MoveTemp(Task));
							}
						}
						Queue->PendingTasks = MoveTemp(Remaining);
					}
					else
					{
						for (FTurboStructLiteQueuedTask& Task : Queue->PendingTasks)
						{
							if (Task.CancelCallback)
							{
								CancelCallbacks.Add(MoveTemp(Task.CancelCallback));
							}
						}
						Queue->PendingTasks.Reset();
					}
					if (Queue->PendingTasks.Num() == 0 && !Queue->bTaskInProgress)
					{
						QueuesToRemove.Add(SanitizedName);
					}
				}
			}
		}
		for (const FString& Key : QueuesToRemove)
		{
			QueuesBySlot.Remove(Key);
		}
	}
	DispatchCancelCallbacks(MoveTemp(CancelCallbacks));
}

void UTurboStructLiteBPLibrary::EnqueueTask(const FString& SlotName, TFunction<void()> Task, int32 Priority, TFunction<void()> CancelCallback)
{
	const FString SanitizedName = SanitizeSlotName(SlotName);
	const TSharedPtr<FTurboStructLiteTaskQueue> Queue = GetQueueForSlot(SanitizedName);
	bool bShouldStart = false;
	if (Queue.IsValid())
	{
		FScopeLock Lock(&Queue->Mutex);

		FTurboStructLiteQueuedTask NewTask;
		NewTask.Payload = MoveTemp(Task);
		NewTask.CancelCallback = MoveTemp(CancelCallback);
		NewTask.Priority = Priority;

		int32 InsertIndex = Queue->PendingTasks.Num();
		for (int32 i = 0; i < Queue->PendingTasks.Num(); ++i)
		{
			if (Queue->PendingTasks[i].Priority > Priority)
			{
				InsertIndex = i;
				break;
			}
		}
		Queue->PendingTasks.Insert(MoveTemp(NewTask), InsertIndex);

		if (!Queue->bTaskInProgress)
		{
			Queue->bTaskInProgress = true;
			bShouldStart = true;
		}
	}
	if (bShouldStart)
	{
		ProcessNextTask(SanitizedName);
	}
}

void UTurboStructLiteBPLibrary::ProcessNextTask(const FString& SlotName)
{
	if (!HasActiveGameWorld())
	{
		ClearAllQueues();
		return;
	}

	const TSharedPtr<FTurboStructLiteTaskQueue> Queue = GetQueueForSlot(SlotName);
	TFunction<void()> Task;
	if (Queue.IsValid())
	{
		FScopeLock Lock(&Queue->Mutex);
		if (Queue->PendingTasks.Num() == 0)
		{
			Queue->bTaskInProgress = false;
			return;
		}
		Task = MoveTemp(Queue->PendingTasks[0].Payload);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		Queue->PendingTasks.RemoveAt(0, 1, EAllowShrinking::Yes);
#else
		Queue->PendingTasks.RemoveAt(0, 1, true);
#endif
	}
	if (Task)
	{
		Task();
	}
}

void UTurboStructLiteBPLibrary::EnqueueSaveRequest(FTurboStructLiteSaveRequest&& Request)
{
	const FString SlotCopy = Request.SlotName;
	const int32 SubSlotCopy = Request.SubSlotIndex;
	const int32 Priority = FMath::Clamp(Request.QueuePriority, 0, 100);
	const bool bUseWriteAheadLog = Request.bUseWriteAheadLog;
	const FString WALPath = Request.WALPath;
	TSharedRef<TFunction<void(bool, FString, int32)>> SharedCallback = MakeShared<TFunction<void(bool, FString, int32)>>(MoveTemp(Request.Callback));
	Request.Callback = [SharedCallback](bool bSuccess, FString FilePath, int32 CallbackSubSlot) mutable
	{
		if (*SharedCallback)
		{
			(*SharedCallback)(bSuccess, FilePath, CallbackSubSlot);
		}
	};
	TFunction<void()> CancelCallback = [SharedCallback, SlotCopy, SubSlotCopy, bUseWriteAheadLog, WALPath]() mutable
	{
		EndMemoryOpMessage(SlotCopy, SubSlotCopy, true, false);
		if (bUseWriteAheadLog && !WALPath.IsEmpty())
		{
			WriteWALEntry(WALPath, TEXT("Save cancelled"));
			DeleteWALFile(WALPath);
		}
		if (*SharedCallback)
		{
			const FString FilePathCopy = BuildSavePath(SlotCopy);
			(*SharedCallback)(false, FilePathCopy, SubSlotCopy);
		}
	};
	EnqueueTask(SlotCopy, [Request = MoveTemp(Request)]() mutable { ExecuteSaveRequest(MoveTemp(Request)); }, Priority, MoveTemp(CancelCallback));
}

void UTurboStructLiteBPLibrary::ExecuteSaveRequest(FTurboStructLiteSaveRequest&& Request)
{
	const FString SlotCopy = MoveTemp(Request.SlotName);
	const int32 SubSlotCopy = Request.SubSlotIndex;
	const ETurboStructLiteCompression CompressionCopy = Request.Compression;
	const ETurboStructLiteEncryption EncryptionCopy = Request.Encryption;
	const FString EncryptionKeyCopy = Request.EncryptionKey;
	const bool bAsync = Request.bAsync;
	TArray<uint8> RawBytes = MoveTemp(Request.RawBytes);
	TFunction<void(bool, FString, int32)> Callback = MoveTemp(Request.Callback);
	const FString DebugMetaCopy = Request.DebugMetadata;
	const int32 MaxParallelThreadsCopy = Request.MaxParallelThreads;
	const ETurboStructLiteBatchingSetting BatchingCopy = Request.CompressionBatching;
	const bool bUseWriteAheadLog = Request.bUseWriteAheadLog;
	const FString WALPath = Request.WALPath;

	auto SaveWork = [SlotCopy, SubSlotCopy, CompressionCopy, EncryptionCopy, EncryptionKeyCopy, DebugMetaCopy, MaxParallelThreadsCopy, BatchingCopy, bUseWriteAheadLog, WALPath](const TArray<uint8>& Bytes) -> bool
	{
		BeginSlotOperation(SlotCopy);
		TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(SlotCopy);
		bool bSaved = false;
		{
			FScopeLock Lock(OpLock.Get());
			bSaved = SaveEntry(SlotCopy, SubSlotCopy, CompressionCopy, EncryptionCopy, EncryptionKeyCopy, Bytes, DebugMetaCopy, MaxParallelThreadsCopy, BatchingCopy, bUseWriteAheadLog, WALPath);
		}
		EndSlotOperation(SlotCopy);
		return bSaved;
	};

	if (bAsync)
	{
		const FString FilePathCopy = BuildSavePath(SlotCopy);
		Async(EAsyncExecution::ThreadPool, [RawBytes = MoveTemp(RawBytes), SaveWork, Callback = MoveTemp(Callback), SlotCopy, SubSlotCopy, FilePathCopy, bUseWriteAheadLog, WALPath]() mutable
		{
			TURBOSTRUCTLITE_TRACE_SCOPE(TEXT("TurboStructLite_SaveAsync"));
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Async save task start"));
			}
			const bool bSaved = SaveWork(RawBytes);
			AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), bSaved, SlotCopy, SubSlotCopy, FilePathCopy, bUseWriteAheadLog, WALPath]() mutable
			{
				EndMemoryOpMessage(SlotCopy, SubSlotCopy, true, false);
				if (!HasActiveGameWorld())
				{
					if (bUseWriteAheadLog)
					{
						DeleteWALFile(WALPath);
					}
					FinishQueuedSave(SlotCopy);
					return;
				}
				if (bUseWriteAheadLog)
				{
					WriteWALEntry(WALPath, bSaved ? TEXT("Save completed") : TEXT("Save failed"));
					if (bSaved)
					{
						DeleteWALFile(WALPath);
					}
				}
				if (Callback)
				{
					Callback(bSaved, FilePathCopy, SubSlotCopy);
				}
				FinishQueuedSave(SlotCopy);
			});
		});
		return;
	}

	TURBOSTRUCTLITE_TRACE_SCOPE(TEXT("TurboStructLite_SaveSync"));
	const bool bSavedSync = SaveWork(RawBytes);
	EndMemoryOpMessage(SlotCopy, SubSlotCopy, true, false);
	if (!HasActiveGameWorld())
	{
		if (bUseWriteAheadLog)
		{
			if (bSavedSync)
			{
				DeleteWALFile(WALPath);
			}
		}
		FinishQueuedSave(SlotCopy);
		return;
	}
	if (Callback)
	{
		const FString FilePathCopy = BuildSavePath(SlotCopy);
		Callback(bSavedSync, FilePathCopy, SubSlotCopy);
	}
	if (bUseWriteAheadLog)
	{
		WriteWALEntry(WALPath, bSavedSync ? TEXT("Save completed") : TEXT("Save failed"));
		if (bSavedSync)
		{
			DeleteWALFile(WALPath);
		}
	}
	FinishQueuedSave(SlotCopy);
}

void UTurboStructLiteBPLibrary::FinishQueuedSave(const FString& SlotName)
{
	ProcessNextTask(SlotName);
}

void UTurboStructLiteBPLibrary::EnqueueLoadRequest(FTurboStructLiteLoadRequest&& Request)
{
	const FString SlotCopy = Request.SlotName;
	const int32 SubSlotCopy = Request.SubSlotIndex;
	const int32 Priority = FMath::Clamp(Request.QueuePriority, 0, 100);
	const bool bUseWriteAheadLog = Request.bUseWriteAheadLog;
	const FString WALPath = Request.WALPath;
	TSharedRef<TFunction<void(bool)>> SharedCallback = MakeShared<TFunction<void(bool)>>(MoveTemp(Request.Callback));
	Request.Callback = [SharedCallback](bool bSuccess) mutable
	{
		if (*SharedCallback)
		{
			(*SharedCallback)(bSuccess);
		}
	};
	TFunction<void()> CancelCallback = [SharedCallback, SlotCopy, SubSlotCopy, bUseWriteAheadLog, WALPath]() mutable
	{
		EndMemoryOpMessage(SlotCopy, SubSlotCopy, false, false);
		if (bUseWriteAheadLog && !WALPath.IsEmpty())
		{
			WriteWALEntry(WALPath, TEXT("Load cancelled"));
			DeleteWALFile(WALPath);
		}
		if (*SharedCallback)
		{
			(*SharedCallback)(false);
		}
	};
	EnqueueTask(SlotCopy, [Request = MoveTemp(Request)]() mutable { ExecuteLoadRequest(MoveTemp(Request)); }, Priority, MoveTemp(CancelCallback));
}

void UTurboStructLiteBPLibrary::ExecuteLoadRequest(FTurboStructLiteLoadRequest&& Request)
{
	FProperty* DataProp = Request.DataProp;
	void* DataPtr = Request.DataPtr;
	TFunction<void(bool)> LoadCallback = MoveTemp(Request.Callback);
	const FString EncryptionKeyCopy = Request.EncryptionKey;
	const ETurboStructLiteEncryption DefaultEncryptionCopy = Request.DefaultEncryption;
	const int32 MaxParallelThreadsCopy = Request.MaxParallelThreads;
	const ETurboStructLiteBatchingSetting BatchingCopy = Request.CompressionBatching;
	const bool bUseWriteAheadLog = Request.bUseWriteAheadLog;
	const FString WALPath = Request.WALPath;

	if (Request.bAsync)
	{
		const FString SlotCopy = Request.SlotName;
		const int32 SubSlotCopy = Request.SubSlotIndex;
		const FString EncryptionKey = EncryptionKeyCopy;
		const ETurboStructLiteEncryption DefaultEncryption = DefaultEncryptionCopy;
		const int32 MaxThreads = FMath::Clamp(MaxParallelThreadsCopy, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		TArray<const FStructProperty*> DummyStructProps;
		const bool bCanBackgroundDeserialize = DataProp && !DataProp->ContainsObjectReference(DummyStructProps, EPropertyObjectReferenceType::Strong);

		Async(EAsyncExecution::ThreadPool, [SlotCopy, SubSlotCopy, EncryptionKey, DefaultEncryption, DataProp, DataPtr, bCanBackgroundDeserialize, LoadCallback = MoveTemp(LoadCallback), MaxThreads, bUseWriteAheadLog, WALPath]() mutable
		{
			TArray<uint8> RawBytes;
			bool bLoaded = false;
			{
				FScopedParallelLimitLite ThreadGuard(MaxThreads);
				int64 ExpectedBytes = 0;
				if (GetExpectedRawSize(SlotCopy, SubSlotCopy, ExpectedBytes))
				{
					UpdateMemoryPressureWarning(SlotCopy, SubSlotCopy, ExpectedBytes, false, false);
				}
				BeginSlotOperation(SlotCopy);
				TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(SlotCopy);
				{
					FScopeLock Lock(OpLock.Get());
					bLoaded = UTurboStructLiteBPLibrary::LoadEntry(SlotCopy, SubSlotCopy, EncryptionKey, DefaultEncryption, RawBytes, bUseWriteAheadLog, WALPath);
				}
				EndSlotOperation(SlotCopy);
			}
			if (!bLoaded)
			{
				AsyncTask(ENamedThreads::GameThread, [LoadCallback, SlotCopy, SubSlotCopy, bUseWriteAheadLog, WALPath]() mutable
				{
					EndMemoryOpMessage(SlotCopy, SubSlotCopy, false, false);
					if (bUseWriteAheadLog)
					{
						WriteWALEntry(WALPath, TEXT("Load failed"));
					}
					if (LoadCallback)
					{
						LoadCallback(false);
					}
					FinishQueuedLoad(SlotCopy);
				});
				return;
			}

			if (bCanBackgroundDeserialize)
			{
				TArray<uint8> Snapshot;
				bool bDeserialized = false;
				if (DataProp && DataPtr)
				{
						const int32 ValueSize = DataProp->GetSize();
						if (ValueSize > 0)
						{
							Snapshot.SetNumUninitialized(ValueSize);
							DataProp->InitializeValue(Snapshot.GetData());
							bDeserialized = UTurboStructLiteBPLibrary::DeserializeWildcard(DataProp, Snapshot.GetData(), RawBytes, MaxThreads);
						}
					}

				AsyncTask(ENamedThreads::GameThread, [LoadCallback, SlotCopy, SubSlotCopy, DataProp, DataPtr, Snapshot = MoveTemp(Snapshot), bDeserialized, bUseWriteAheadLog, WALPath]() mutable
				{
					EndMemoryOpMessage(SlotCopy, SubSlotCopy, false, false);
					if (!HasActiveGameWorld())
					{
						if (bUseWriteAheadLog)
						{
							DeleteWALFile(WALPath);
						}
						FinishQueuedLoad(SlotCopy);
						return;
					}
					if (!DataProp || !DataPtr || Snapshot.Num() <= 0 || !Snapshot.GetData())
					{
						if (bUseWriteAheadLog)
						{
							WriteWALEntry(WALPath, TEXT("Load skipped: invalid target pointer"));
						}
						if (LoadCallback)
						{
							LoadCallback(false);
						}
						FinishQueuedLoad(SlotCopy);
						return;
					}
					bool bApplied = false;
					if (bDeserialized)
					{
						TURBOSTRUCTLITE_TRACE_SCOPE(TEXT("TurboStructLite_InstantSwap"));
						bool bSwapped = false;
						if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(DataProp))
						{
							FScriptArray* DestArray = static_cast<FScriptArray*>(DataPtr);
							FScriptArray* SrcArray = reinterpret_cast<FScriptArray*>(Snapshot.GetData());
							FMemory::Memswap(DestArray, SrcArray, sizeof(FScriptArray));
							bSwapped = true;
						}
						else if (FMapProperty* MapProp = CastField<FMapProperty>(DataProp))
						{
							FScriptMap* DestMap = static_cast<FScriptMap*>(DataPtr);
							FScriptMap* SrcMap = reinterpret_cast<FScriptMap*>(Snapshot.GetData());
							FMemory::Memswap(DestMap, SrcMap, sizeof(FScriptMap));
							bSwapped = true;
						}
						else if (FSetProperty* SetProp = CastField<FSetProperty>(DataProp))
						{
							FScriptSet* DestSet = static_cast<FScriptSet*>(DataPtr);
							FScriptSet* SrcSet = reinterpret_cast<FScriptSet*>(Snapshot.GetData());
							FMemory::Memswap(DestSet, SrcSet, sizeof(FScriptSet));
							bSwapped = true;
						}
						else if (FStructProperty* StructProp = CastField<FStructProperty>(DataProp))
						{
							FMemory::Memswap(DataPtr, Snapshot.GetData(), DataProp->GetSize());
							bSwapped = true;
						}
						if (!bSwapped)
						{
							DataProp->CopyCompleteValue(DataPtr, Snapshot.GetData());
						}
						DataProp->DestroyValue(Snapshot.GetData());
						bApplied = true;
					}
					else if (Snapshot.Num() > 0 && DataProp)
					{
						DataProp->DestroyValue(Snapshot.GetData());
					}
					if (bUseWriteAheadLog)
					{
						WriteWALEntry(WALPath, bApplied ? TEXT("Load completed") : TEXT("Load failed"));
						if (bApplied)
						{
							DeleteWALFile(WALPath);
						}
					}
					if (LoadCallback)
					{
						LoadCallback(bApplied);
					}
					FinishQueuedLoad(SlotCopy);
				});
				return;
			}

			AsyncTask(ENamedThreads::GameThread, [RawBytes = MoveTemp(RawBytes), LoadCallback, SlotCopy, SubSlotCopy, DataProp, DataPtr, MaxThreads, bUseWriteAheadLog, WALPath]() mutable
			{
				EndMemoryOpMessage(SlotCopy, SubSlotCopy, false, false);
				if (!HasActiveGameWorld())
				{
					if (bUseWriteAheadLog)
					{
						DeleteWALFile(WALPath);
					}
					FinishQueuedLoad(SlotCopy);
					return;
				}
				if (!DataProp || !DataPtr)
				{
					if (bUseWriteAheadLog)
					{
						WriteWALEntry(WALPath, TEXT("Load skipped: invalid target pointer"));
					}
					if (LoadCallback)
					{
						LoadCallback(false);
					}
					FinishQueuedLoad(SlotCopy);
					return;
				}
				bool bApplied = false;
				if (DataProp && DataPtr)
				{
					void* NonConstPtr = const_cast<void*>(DataPtr);
					bApplied = UTurboStructLiteBPLibrary::DeserializeWildcard(DataProp, NonConstPtr, RawBytes, MaxThreads);
				}
				if (bUseWriteAheadLog)
				{
					WriteWALEntry(WALPath, bApplied ? TEXT("Load completed") : TEXT("Load failed"));
					if (bApplied)
					{
						DeleteWALFile(WALPath);
					}
				}
				if (LoadCallback)
				{
					LoadCallback(bApplied);
				}
				FinishQueuedLoad(SlotCopy);
			});
		});
		return;
	}

	TArray<uint8> RawBytes;
	const int32 MaxThreads = FMath::Clamp(MaxParallelThreadsCopy, 1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
	int64 ExpectedBytes = 0;
	bool bLoadedSync = false;
	{
		FScopedParallelLimitLite ThreadGuard(MaxThreads);
		if (GetExpectedRawSize(Request.SlotName, Request.SubSlotIndex, ExpectedBytes))
		{
			UpdateMemoryPressureWarning(Request.SlotName, Request.SubSlotIndex, ExpectedBytes, false, false);
		}
		BeginSlotOperation(Request.SlotName);
		TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(Request.SlotName);
		{
			FScopeLock Lock(OpLock.Get());
			bLoadedSync = UTurboStructLiteBPLibrary::LoadEntry(Request.SlotName, Request.SubSlotIndex, EncryptionKeyCopy, DefaultEncryptionCopy, RawBytes, bUseWriteAheadLog, WALPath);
		}
		EndSlotOperation(Request.SlotName);
	}
	if (bLoadedSync)
	{
		if (!HasActiveGameWorld())
		{
			if (bUseWriteAheadLog)
			{
				DeleteWALFile(WALPath);
			}
			EndMemoryOpMessage(Request.SlotName, Request.SubSlotIndex, false, false);
			FinishQueuedLoad(Request.SlotName);
			return;
		}
		if (!DataProp || !DataPtr)
		{
			if (bUseWriteAheadLog)
			{
				WriteWALEntry(WALPath, TEXT("Load skipped: invalid target pointer"));
			}
			EndMemoryOpMessage(Request.SlotName, Request.SubSlotIndex, false, false);
			FinishQueuedLoad(Request.SlotName);
			return;
		}
		void* NonConstPtr = const_cast<void*>(DataPtr);
		const bool bApplied = UTurboStructLiteBPLibrary::DeserializeWildcard(DataProp, NonConstPtr, RawBytes, MaxThreads);
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, bApplied ? TEXT("Load completed") : TEXT("Load failed"));
			if (bApplied)
			{
				DeleteWALFile(WALPath);
			}
		}
		if (LoadCallback)
		{
			LoadCallback(bApplied);
		}
		EndMemoryOpMessage(Request.SlotName, Request.SubSlotIndex, false, false);
		FinishQueuedLoad(Request.SlotName);
	}
	else
	{
		if (bUseWriteAheadLog)
		{
			WriteWALEntry(WALPath, TEXT("Load failed"));
		}
		if (LoadCallback)
		{
			LoadCallback(false);
		}
		EndMemoryOpMessage(Request.SlotName, Request.SubSlotIndex, false, false);
		FinishQueuedLoad(Request.SlotName);
	}
}

void UTurboStructLiteBPLibrary::FinishQueuedLoad(const FString& SlotName)
{
	ProcessNextTask(SlotName);
}



