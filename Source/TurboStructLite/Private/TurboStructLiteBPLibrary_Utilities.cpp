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
TArray<int32> UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlots(const FString& MainSlotName)
{
	TArray<int32> SubSlots;
	QuerySubSlotIndices(MainSlotName, SubSlots);
	return SubSlots;
}

FTurboStructLiteSubSlotIndexResult UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlotsResult(const FString& MainSlotName)
{
	FTurboStructLiteSubSlotIndexResult Result;
	Result.Status = QuerySubSlotIndices(MainSlotName, Result.SubSlots);
	return Result;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteGetSlotInfo(const FString& MainSlotName, FTurboStructLiteSlotInfo& OutInfo)
{
	return GetSlotInfoInternal(MainSlotName, OutInfo);
}

TArray<FTurboStructLiteSubSlotInfo> UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlotInfos(const FString& MainSlotName)
{
	TArray<FTurboStructLiteSubSlotInfo> Infos;
	QuerySubSlotInfos(MainSlotName, Infos);
	return Infos;
}

FTurboStructLiteSubSlotInfoResult UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlotInfosResult(const FString& MainSlotName)
{
	FTurboStructLiteSubSlotInfoResult Result;
	Result.Status = QuerySubSlotInfos(MainSlotName, Result.SubSlotInfos);
	return Result;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteLoadSubSlotBytes(const FString& MainSlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, TArray<uint8>& OutBytes)
{
	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		return false;
	}
	const ETurboStructLiteEncryption ResolvedEncryption = Encryption == ETurboStructLiteEncryption::ProjectDefault ? GetActiveEncryptionMode() : Encryption;
	FString KeyToUse = EncryptionKey;
	if (ResolvedEncryption == ETurboStructLiteEncryption::AES && KeyToUse.IsEmpty())
	{
		KeyToUse = GetActiveEncryptionKey();
		if (KeyToUse.IsEmpty())
		{
			return false;
		}
	}
	BeginSlotOperation(MainSlotName);
	TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(MainSlotName);
	bool bLoaded = false;
	{
		FScopeLock Lock(OpLock.Get());
		bLoaded = LoadEntry(MainSlotName, SubSlotIndex, KeyToUse, ResolvedEncryption, OutBytes);
	}
	EndSlotOperation(MainSlotName);
	return bLoaded;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteSaveSubSlotBytes(const FString& MainSlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, ETurboStructLiteCompression Compression, const FString& DebugMetadata, const TArray<uint8>& RawBytes)
{
	if (MainSlotName.IsEmpty() || SubSlotIndex < 0 || RawBytes.Num() == 0)
	{
		return false;
	}
	EnsureSettingsLoaded();
	const ETurboStructLiteCompression ResolvedCompression = ResolveCompression(Compression);
	ETurboStructLiteEncryption ResolvedEncryption = Encryption == ETurboStructLiteEncryption::ProjectDefault ? GetActiveEncryptionMode() : Encryption;
	FString KeyToUse = EncryptionKey;
	if (ResolvedEncryption == ETurboStructLiteEncryption::AES && KeyToUse.IsEmpty())
	{
		KeyToUse = GetActiveEncryptionKey();
		if (KeyToUse.IsEmpty())
		{
			return false;
		}
	}
	BeginSlotOperation(MainSlotName);
	TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(MainSlotName);
	bool bSaved = false;
	{
		FScopeLock Lock(OpLock.Get());
		bSaved = SaveEntry(MainSlotName, SubSlotIndex, ResolvedCompression, ResolvedEncryption, KeyToUse, RawBytes, DebugMetadata, GetParallelThreadLimit(), ETurboStructLiteBatchingSetting::ProjectDefault);
	}
	EndSlotOperation(MainSlotName);
	return bSaved;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteRemoveSubSlotImmediate(const FString& MainSlotName, int32 SubSlotIndex)
{
	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		return false;
	}
	BeginSlotOperation(MainSlotName);
	TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(MainSlotName);
	bool bRemoved = false;
	{
		FScopeLock Lock(OpLock.Get());
		bRemoved = RemoveEntry(MainSlotName, SubSlotIndex);
	}
	EndSlotOperation(MainSlotName);
	return bRemoved;
}

bool UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlotInfoWithKey(const FString& MainSlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, FTurboStructLiteSubSlotInfo& OutInfo)
{
	OutInfo = FTurboStructLiteSubSlotInfo();
	if (MainSlotName.IsEmpty() || SubSlotIndex < 0)
	{
		return false;
	}
	BeginSlotOperation(MainSlotName);
	TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(MainSlotName);
	bool bRead = false;
	{
		FScopeLock Lock(OpLock.Get());
		bRead = ReadSubSlotInfoInternal(MainSlotName, SubSlotIndex, EncryptionKey, Encryption, OutInfo);
	}
	EndSlotOperation(MainSlotName);
	return bRead;
}

ETurboStructLiteSlotQueryStatus UTurboStructLiteBPLibrary::QuerySubSlotIndices(const FString& SlotName, TArray<int32>& OutSubSlots)
{
	OutSubSlots.Reset();
	if (SlotName.IsEmpty())
	{
		return ETurboStructLiteSlotQueryStatus::SlotInvalid;
	}
	BeginSlotOperation(SlotName);
	TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(SlotName);
	ETurboStructLiteSlotQueryStatus Status = ETurboStructLiteSlotQueryStatus::SlotInvalid;
	{
		FScopeLock Lock(OpLock.Get());
		const FString FilePath = BuildSavePath(SlotName);
		if (!FPaths::FileExists(FilePath))
		{
			Status = ETurboStructLiteSlotQueryStatus::SlotMissing;
		}
		else if (!ListSubSlotIndices(SlotName, OutSubSlots))
		{
			Status = ETurboStructLiteSlotQueryStatus::SlotInvalid;
		}
		else
		{
			Status = OutSubSlots.Num() == 0 ? ETurboStructLiteSlotQueryStatus::OkEmpty : ETurboStructLiteSlotQueryStatus::Ok;
		}
	}
	EndSlotOperation(SlotName);
	return Status;
}

ETurboStructLiteSlotQueryStatus UTurboStructLiteBPLibrary::QuerySubSlotInfos(const FString& SlotName, TArray<FTurboStructLiteSubSlotInfo>& OutInfos)
{
	OutInfos.Reset();
	if (SlotName.IsEmpty())
	{
		return ETurboStructLiteSlotQueryStatus::SlotInvalid;
	}
	BeginSlotOperation(SlotName);
	TSharedPtr<FCriticalSection> OpLock = GetSlotOperationLock(SlotName);
	ETurboStructLiteSlotQueryStatus Status = ETurboStructLiteSlotQueryStatus::SlotInvalid;
	{
		FScopeLock Lock(OpLock.Get());
		const FString FilePath = BuildSavePath(SlotName);
		if (!FPaths::FileExists(FilePath))
		{
			Status = ETurboStructLiteSlotQueryStatus::SlotMissing;
		}
		else if (!ListSubSlotInfos(SlotName, OutInfos))
		{
			Status = ETurboStructLiteSlotQueryStatus::SlotInvalid;
		}
		else
		{
			Status = OutInfos.Num() == 0 ? ETurboStructLiteSlotQueryStatus::OkEmpty : ETurboStructLiteSlotQueryStatus::Ok;
		}
	}
	EndSlotOperation(SlotName);
	return Status;
}


