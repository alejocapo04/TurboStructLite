/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Tipos basicos de TurboStructLite. Define enums de compresion/cifrado, estructuras de info de
 * slots/subslots y contenedores internos usados por las colas y serializacion.
 *
 * Description (EN):
 * Core TurboStructLite types. Defines compression/encryption enums, slot/subslot info structs,
 * and internal containers used by task queues and serialization.
 */

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/Field.h"
#include "TurboStructLiteTypes.generated.h"

UENUM(BlueprintType)
enum class ETurboStructLiteCompression : uint8
{
	ProjectDefault = 0 UMETA(DisplayName = "Use Project Settings"),
	None        = 1 UMETA(DisplayName = "No Compression (Raw)"),
	LZ4         = 2 UMETA(DisplayName = "LZ4 (Ultra Fast)"),
	Zlib        = 3 UMETA(DisplayName = "Zlib (Balanced)"),
	Gzip        = 4 UMETA(DisplayName = "Gzip (Web Standard)"),
	Oodle       = 5 UMETA(DisplayName = "Oodle (Best Ratio)")
};

UENUM(BlueprintType)
enum class ETurboStructLiteCompressionSettings : uint8
{
	None        = 0 UMETA(DisplayName = "No Compression (Raw)"),
	LZ4         = 1 UMETA(DisplayName = "LZ4 (Ultra Fast)"),
	Zlib        = 2 UMETA(DisplayName = "Zlib (Balanced)"),
	Gzip        = 3 UMETA(DisplayName = "Gzip (Web Standard)"),
	Oodle       = 4 UMETA(DisplayName = "Oodle (Best Ratio)")
};

UENUM(BlueprintType)
enum class ETurboStructLiteBatching : uint8
{
	Default    = 0 UMETA(DisplayName = "4 MB (Default)"),
	Two       = 2 UMETA(DisplayName = "2 MB"),
	Four      = 4 UMETA(DisplayName = "4 MB"),
	Eight     = 8 UMETA(DisplayName = "8 MB"),
	Sixteen   = 16 UMETA(DisplayName = "16 MB"),
	ThirtyTwo = 32 UMETA(DisplayName = "32 MB")
};

UENUM(BlueprintType)
enum class ETurboStructLiteBatchingSetting : uint8
{
	ProjectDefault = 0 UMETA(DisplayName = "Use Project Settings"),
	Two       = 2 UMETA(DisplayName = "2 MB"),
	Four      = 4 UMETA(DisplayName = "4 MB"),
	Eight     = 8 UMETA(DisplayName = "8 MB"),
	Sixteen   = 16 UMETA(DisplayName = "16 MB"),
	ThirtyTwo = 32 UMETA(DisplayName = "32 MB")
};

UENUM(BlueprintType)
enum class ETurboStructLiteEncryption : uint8
{
	ProjectDefault UMETA(DisplayName = "Project Default"),
	None           UMETA(DisplayName = "None"),
	AES            UMETA(DisplayName = "AES (Advanced Encryption Standard)")
};

UENUM(BlueprintType)
enum class ETurboStructLiteEncryptionSettings : uint8
{
	None UMETA(DisplayName = "None"),
	AES  UMETA(DisplayName = "AES (Advanced Encryption Standard)")
};

UENUM(BlueprintType)
enum class ETurboStructLiteAsyncExecution : uint8
{
	TaskGraph UMETA(DisplayName = "Task Graph"),
	TaskGraphMainThread UMETA(DisplayName = "Task Graph (Main Thread)"),
	Thread UMETA(DisplayName = "Thread"),
	ThreadIfForkSafe UMETA(DisplayName = "Thread If Fork Safe"),
	ThreadPool UMETA(DisplayName = "Thread Pool"),
	LargeThreadPool UMETA(DisplayName = "Large Thread Pool")
};

struct FTurboStructLiteEntry
{
	ETurboStructLiteCompression Compression = ETurboStructLiteCompression::None;
	ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::None;
	TArray<uint8> Data;
	int32 UncompressedSize = 0;
};

USTRUCT(BlueprintType)
struct TURBOSTRUCTLITE_API FTurboStructLiteSlotInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	int64 FileSizeBytes = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	int32 EntryCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	FDateTime Timestamp = FDateTime(0);
};

USTRUCT(BlueprintType)
struct TURBOSTRUCTLITE_API FTurboStructLiteSubSlotInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	int32 SubSlotIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	int32 DataSizeBytes = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	int32 UncompressedSizeBytes = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	ETurboStructLiteCompression Compression = ETurboStructLiteCompression::None;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::None;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	FString DebugMetadata;
};

UENUM(BlueprintType)
enum class ETurboStructLiteSlotQueryStatus : uint8
{
	Ok UMETA(DisplayName = "Ok"),
	OkEmpty UMETA(DisplayName = "Ok (Empty)"),
	SlotMissing UMETA(DisplayName = "Slot Missing"),
	SlotInvalid UMETA(DisplayName = "Slot Invalid")
};

USTRUCT(BlueprintType)
struct TURBOSTRUCTLITE_API FTurboStructLiteSubSlotIndexResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	ETurboStructLiteSlotQueryStatus Status = ETurboStructLiteSlotQueryStatus::SlotMissing;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	TArray<int32> SubSlots;
};

USTRUCT(BlueprintType)
struct TURBOSTRUCTLITE_API FTurboStructLiteSubSlotInfoResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	ETurboStructLiteSlotQueryStatus Status = ETurboStructLiteSlotQueryStatus::SlotMissing;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	TArray<FTurboStructLiteSubSlotInfo> SubSlotInfos;
};

struct FTurboStructLiteQueuedTask
{
	TFunction<void()> Payload;
	TFunction<void()> CancelCallback;
	int32 Priority = 10;

	bool operator<(const FTurboStructLiteQueuedTask& Other) const
	{
		return Priority < Other.Priority;
	}
};

struct FTurboStructLiteSaveRequest
{
	FString SlotName;
	int32 SubSlotIndex = 0;
	ETurboStructLiteCompression Compression = ETurboStructLiteCompression::None;
	ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::None;
	bool bAsync = false;
	bool bUseWriteAheadLog = false;
	bool bSaveOnlyMarked = false;
	TArray<uint8> RawBytes;
	FString DebugMetadata;
	FString EncryptionKey;
	FString WALPath;
	int32 QueuePriority = 10;
	int32 MaxParallelThreads = 4;
	ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault;
	TFunction<void(bool, FString, int32)> Callback;
};

struct FTurboStructLiteLoadRequest
{
	FString SlotName;
	int32 SubSlotIndex = 0;
	bool bAsync = false;
	bool bUseWriteAheadLog = false;
	ETurboStructLiteEncryption DefaultEncryption = ETurboStructLiteEncryption::ProjectDefault;
	FString EncryptionKey;
	FString WALPath;
	FProperty* DataProp = nullptr;
	void* DataPtr = nullptr;
	int32 QueuePriority = 10;
	int32 MaxParallelThreads = 4;
	ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault;
	TFunction<void(bool)> Callback;
};

struct FTurboStructLiteTaskQueue
{
	FCriticalSection Mutex;
	TArray<FTurboStructLiteQueuedTask> PendingTasks;
	bool bTaskInProgress = false;
};

struct FTurboStructLiteFieldMeta
{
	FString Name;
	FString Type;
	int32 Size = 0;
	TArray<FTurboStructLiteFieldMeta> Children;
};

struct FTurboStructLiteLoadWorkUnit
{
	int32 MetaIndex = 0;
	int32 ArrayOffset = -1;
	int32 ArrayCount = 0;
};

struct FTurboStructLiteCachedEntry
{
	int64 DataOffset = 0;
	int64 MetaOffset = 0;
	int32 DataSize = 0;
	int32 MetaSize = 0;
	int32 UncompressedSize = 0;
	ETurboStructLiteCompression Compression = ETurboStructLiteCompression::None;
	ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::None;
};

struct FTurboStructLiteSlotIndex
{
	FDateTime Timestamp;
	int64 FileSizeBytes = 0;
	int32 EntryCount = 0;
	TMap<int32, FTurboStructLiteCachedEntry> Entries;
	TArray<int32> OrderedSubSlots;
};

UENUM(BlueprintType)
enum class ETurboStructLiteVariantType : uint8
{
	Empty,
	String,
	Int,
	Float,
	Bool,
	Name,
	Struct
};

USTRUCT(BlueprintType)
struct TURBOSTRUCTLITE_API FTurboStructLiteVariant
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	ETurboStructLiteVariantType Type = ETurboStructLiteVariantType::Empty;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	FString StringValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	int64 IntValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	double FloatValue = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	bool BoolValue = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TurboStructLite")
	FName NameValue;
};
