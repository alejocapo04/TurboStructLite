/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Biblioteca Blueprint de TurboStructLite. Expone funciones asincronas para guardar, cargar,
 * copiar, renombrar y consultar subslots de structs/arrays/mapas/sets con compresion y
 * cifrado opcional, ademas de utilidades de cola y metadata.
 *
 * Description (EN):
 * TurboStructLite Blueprint library. Exposes async functions to save, load, copy, rename, and
 * query struct/array/map/set subslots with optional compression and encryption, plus queue
 * and metadata utilities.
 */

#include "CoreMinimal.h"
#include "LatentActions.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "TurboStructLiteTypes.h"
#include "TurboStructLiteBPLibrary.generated.h"

class STurboStructLiteDatabaseWidget;
class FTurboStructLiteDatabaseParser;
enum class EAsyncExecution;
class IConsoleVariable;
class UTurboStructLiteQueryLibrary;

DECLARE_LOG_CATEGORY_EXTERN(LogTurboStructLite, Log, All);

// === Delegates ===
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FTurboStructLiteSaveComplete, bool, bSuccess, FString, FilePath, int32, SubSlotIndex);
DECLARE_DYNAMIC_DELEGATE_OneParam(FTurboStructLiteLoadComplete, bool, bSuccess);
DECLARE_DYNAMIC_DELEGATE_OneParam(FTurboStructLiteDeleteComplete, bool, bSuccess);
DECLARE_DYNAMIC_DELEGATE_OneParam(FTurboStructLiteExistComplete, bool, bSuccess);
DECLARE_DYNAMIC_DELEGATE_RetVal(FString, FTurboStructLiteKeyProviderDelegate);

// Category: Serialization.
// Scoped helper to restore the thread-local parallel limit.
class TURBOSTRUCTLITE_API FScopedParallelLimitLite
{
public:
	// Category: Serialization.
	explicit FScopedParallelLimitLite(int32 NewLimit);
	// Category: Serialization.
	~FScopedParallelLimitLite();

	FScopedParallelLimitLite(const FScopedParallelLimitLite&) = delete;
	FScopedParallelLimitLite& operator=(const FScopedParallelLimitLite&) = delete;

private:
	int32 PrevLimit;
};

UCLASS()
class TURBOSTRUCTLITE_API UTurboStructLiteBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

	friend class UTurboStructLiteQueryLibrary;
	friend class FScopedParallelLimitLite;

public:
	// Save wildcard struct data into a slot/subslot.
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "TurboStructLite Basic Operations", meta = (CustomStructureParam = "Data", AutoCreateRefTerm = "bUseWriteAheadLog,bSaveOnlyMarked,Data,OnComplete,QueuePriority,MaxParallelThreads,EncryptionKey,CompressionBatching,Compression", AdvancedDisplay = "bUseWriteAheadLog,bSaveOnlyMarked,QueuePriority,MaxParallelThreads,EncryptionKey,Encryption,Compression,CompressionBatching"))
	static void TurboStructSaveLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const int32& Data, const FTurboStructLiteSaveComplete& OnComplete, bool bUseWriteAheadLog = false, bool bSaveOnlyMarked = false, int32 QueuePriority = 10, int32 MaxParallelThreads = 4, const FString& EncryptionKey = TEXT(""), ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::ProjectDefault, ETurboStructLiteCompression Compression = ETurboStructLiteCompression::ProjectDefault, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault);

	// Load wildcard struct data from a slot/subslot.
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "TurboStructLite Basic Operations", meta = (CustomStructureParam = "Data", AutoCreateRefTerm = "bUseWriteAheadLog,OnComplete,QueuePriority,MaxParallelThreads,EncryptionKey,CompressionBatching", AdvancedDisplay = "bUseWriteAheadLog,QueuePriority,MaxParallelThreads,EncryptionKey,Encryption,CompressionBatching"))
	static void TurboStructLoadLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const int32& Data, const FTurboStructLiteLoadComplete& OnComplete, bool bUseWriteAheadLog = false, int32 QueuePriority = 10, int32 MaxParallelThreads = 4, const FString& EncryptionKey = TEXT(""), ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::ProjectDefault, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault);

	// Delete a stored subslot from a slot.
	UFUNCTION(BlueprintCallable, Category = "TurboStructLite Basic Operations", meta = (AdvancedDisplay = "QueuePriority"))
	static void TurboStructDeleteLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority = 10);

	// Check if a subslot exists.
	UFUNCTION(BlueprintCallable, Category = "TurboStructLite Basic Operations", meta = (AdvancedDisplay = "QueuePriority"))
	static void TurboStructExistLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, bool bCheckFileOnly, const FTurboStructLiteExistComplete& OnComplete, int32 QueuePriority = 10);

private:
	// === Custom thunks ===
	// Thunk to save wildcard data.
	DECLARE_FUNCTION(execTurboStructSaveLite);
	// Thunk to load wildcard data.
	DECLARE_FUNCTION(execTurboStructLoadLite);
	// Thunk to save wildcard arrays.
	DECLARE_FUNCTION(execTurboStructSaveLiteArray);
	// Thunk to load wildcard arrays.
	DECLARE_FUNCTION(execTurboStructLoadLiteArray);
	// Thunk to save wildcard maps.
	DECLARE_FUNCTION(execTurboStructSaveLiteMap);
	// Thunk to load wildcard maps.
	DECLARE_FUNCTION(execTurboStructLoadLiteMap);
	// Thunk to save wildcard sets.
	DECLARE_FUNCTION(execTurboStructSaveLiteSet);
	// Thunk to load wildcard sets.
	DECLARE_FUNCTION(execTurboStructLoadLiteSet);

	// Category: Utilities.
	static int32 TurboStructLiteGetPendingCount(const FString& SlotName);
	// Category: Utilities.
	static bool TurboStructLiteIsSlotBusy(const FString& SlotName);
	// Category: Utilities.
	static bool TurboStructLiteIsSystemBusy();
	// Category: Utilities.
	static void TurboStructLiteClearQueues(bool bClearAll = true, const FString& SlotName = TEXT(""), int32 Priority = -1);
	// Category: Utilities.
	static TArray<int32> TurboStructLiteGetSubSlots(const FString& MainSlotName);
	// Category: Utilities.
	static FTurboStructLiteSubSlotIndexResult TurboStructLiteGetSubSlotsResult(const FString& MainSlotName);
	// Category: Utilities.
	static bool TurboStructLiteGetSlotInfo(const FString& MainSlotName, FTurboStructLiteSlotInfo& OutInfo);
	// Category: Utilities.
	static TArray<FTurboStructLiteSubSlotInfo> TurboStructLiteGetSubSlotInfos(const FString& MainSlotName);
	// Category: Utilities.
	static FTurboStructLiteSubSlotInfoResult TurboStructLiteGetSubSlotInfosResult(const FString& MainSlotName);
	// Category: Utilities.
	static bool TurboStructLiteLoadSubSlotBytes(const FString& MainSlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, TArray<uint8>& OutBytes);
	// Category: Utilities.
	static bool TurboStructLiteSaveSubSlotBytes(const FString& MainSlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, ETurboStructLiteCompression Compression, const FString& DebugMetadata, const TArray<uint8>& RawBytes);
	// Category: Utilities.
	static bool TurboStructLiteRemoveSubSlotImmediate(const FString& MainSlotName, int32 SubSlotIndex);
	// Category: Utilities.
	static void TurboStructLiteDeleteSlot(const FString& MainSlotName, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority = 10);
	// Category: Utilities.
	static void TurboStructLiteCopySlot(const FString& SourceSlotName, const FString& TargetSlotName, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority = 10);
	// Category: Utilities.
	static void TurboStructLiteRenameSlot(const FString& SourceSlotName, const FString& TargetSlotName, bool bAsync, const FTurboStructLiteDeleteComplete& OnComplete, int32 QueuePriority = 10);
	// Category: Security.
	static void RegisterEncryptionKeyProvider(FTurboStructLiteKeyProviderDelegate NewProvider);

	// Category: Basic Operations.
	static void HandleWildcardSave(FProperty* DataProp, void* DataPtr, const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FTurboStructLiteSaveComplete& SaveDelegate, bool bUseWriteAheadLog, bool bSaveOnlyMarked, int32 QueuePriority, int32 MaxParallelThreads, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, ETurboStructLiteCompression Compression, ETurboStructLiteBatchingSetting CompressionBatching, const TCHAR* OperationName, const TCHAR* WildcardLabelLower, const TCHAR* WildcardLabelUpper, const TCHAR* SaveLabel, bool bEmitDebugPropInfo);
	// Category: Basic Operations.
	static void HandleWildcardLoad(FProperty* DataProp, void* DataPtr, const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FTurboStructLiteLoadComplete& LoadDelegate, bool bUseWriteAheadLog, int32 QueuePriority, int32 MaxParallelThreads, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, ETurboStructLiteBatchingSetting CompressionBatching, const TCHAR* OperationName, const TCHAR* WildcardLabelLower, const TCHAR* WildcardLabelUpper, const TCHAR* LoadLabel);

	// === Compression helpers ===
	// Compress raw bytes with the selected method.
	bool TurboStructLiteCompress(ETurboStructLiteCompression Method, const TArray<uint8>& InBytes, TArray<uint8>& OutCompressedBytes);
	
	// Decompress bytes with the selected method.
	void TurboStructLiteDecompress(ETurboStructLiteCompression Method, const TArray<uint8>& InCompressedBytes, TArray<uint8>& OutRawBytes);

	// Convenience: Compress with LZ4.
	void TurboStructLiteCompressLZ4(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Decompress with LZ4.
	void TurboStructLiteDecompressLZ4(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Compress with Zlib.
	void TurboStructLiteCompressZlib(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Decompress with Zlib.
	void TurboStructLiteDecompressZlib(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Compress with Gzip.
	void TurboStructLiteCompressGzip(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Decompress with Gzip.
	void TurboStructLiteDecompressGzip(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Compress with Oodle.
	void TurboStructLiteCompressOodle(const TArray<uint8>& In, TArray<uint8>& Out);

	// Convenience: Decompress with Oodle.
	void TurboStructLiteDecompressOodle(const TArray<uint8>& In, TArray<uint8>& Out);

	// === Config ===
	static inline FCriticalSection EncryptionSettingsMutex;
	static inline ETurboStructLiteEncryption ActiveEncryptionMode = ETurboStructLiteEncryption::None;
	static inline FString ActiveEncryptionKey;
	static inline FString CachedProviderKey;
	static inline bool bHasCachedProviderKey = false;
	static inline ETurboStructLiteCompression ActiveDefaultCompression = ETurboStructLiteCompression::Oodle;
	static inline ETurboStructLiteEncryption ConfigEncryptionMode = ETurboStructLiteEncryption::None;
	static inline FString ConfigEncryptionKey;
	static inline ETurboStructLiteCompression ConfigDefaultCompression = ETurboStructLiteCompression::Oodle;
	static inline int32 ConfigDefaultBatchingMB = 4;
	static inline int32 ActiveDefaultBatchingMB = 4;
	static inline bool bTurboStructLiteSettingsLoaded = false;
	static inline const TCHAR* TurboStructLiteSettingsSection = TEXT("/Script/TurboStructLiteProjectSettings.TurboStructLiteProjectSettings");

	// Magic number to identify save files.
	UPROPERTY()
	int32 TurboStructLiteMagic = 0x53534653; //Struct Serialization Fast System 

	// Version number to validate save files.
	UPROPERTY()
	int32 TurboStructLiteVersion = 4;

	// === Encryption helpers ===
	// Get active encryption selection.
	static ETurboStructLiteEncryption GetActiveEncryptionMode();
	// Get active encryption key string.
	static FString GetActiveEncryptionKey();
	// Ensure defaults from config are loaded.
	static void EnsureSettingsLoaded();
	// Load legacy redirects from config (Category: Serialization).
	static bool LoadLegacyRedirects(TMap<FString, FString>& OutRedirects);
	// Resolve ProjectDefault compression to configured value.
	static ETurboStructLiteCompression ResolveCompression(ETurboStructLiteCompression Method);
	// Get configured default compression.
	static ETurboStructLiteCompression GetDefaultCompression();
	// Resolve batching size in MB (ProjectDefault falls back to settings).
	static int32 ResolveBatchingMB(ETurboStructLiteBatchingSetting Batching);
	// Encrypt buffer with selected method.
	static bool EncryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData);
	// Decrypt buffer with selected method.
	static bool DecryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData);
	// Derive AES key material (64 bytes) from string.
	static bool DeriveAESKeyFromString(const FString& Key, const TArray<uint8>& Salt, uint8 OutKey[64]);
	// Encrypt buffer with AES-256-GCM fallback (FAES CTR + GHASH).
	static bool EncryptAesGcmFallback(const uint8* Key, const uint8* IV, int32 IVLen, const uint8* Plaintext, int32 PlaintextLen, TArray<uint8>& OutCiphertext, TArray<uint8>& OutTag);
	// Decrypt buffer with AES-256-GCM fallback (FAES CTR + GHASH).
	static bool DecryptAesGcmFallback(const uint8* Key, const uint8* IV, int32 IVLen, const uint8* Ciphertext, int32 CiphertextLen, const uint8* Tag, int32 TagLen, TArray<uint8>& OutPlaintext);
	// Encrypt a single AES block with a 256-bit key.
	static void EncryptAesBlock(const uint8* Key, const uint8 InBlock[16], uint8 OutBlock[16]);
	// Increment the 32-bit counter portion of a GCM block.
	static void GcmIncrement32(uint8 Counter[16]);
	// Right shift a 128-bit block by one bit (big-endian).
	static void GcmRightShift(uint8 Block[16]);
	// Multiply two 128-bit values in GF(2^128).
	static void GcmMultiply(const uint8 X[16], const uint8 Y[16], uint8 Out[16]);
	// Update GHASH state with data.
	static void GcmUpdate(uint8 Xi[16], const uint8* Data, int32 DataLen, const uint8 H[16]);
	// Finalize GHASH with length block.
	static void GcmFinalize(uint8 Xi[16], uint64 AadBits, uint64 CipherBits, const uint8 H[16]);
	// Build the J0 block from an IV.
	static void GcmBuildJ0(const uint8* IV, int32 IVLen, const uint8 H[16], uint8 OutJ0[16]);
	// Apply CTR transform for GCM.
	static void GcmCtrCrypt(const uint8* Key, const uint8 J0[16], const uint8* In, int32 InLen, TArray<uint8>& Out);
	// Constant-time compare for authentication tags.
	static bool GcmConstantTimeEqual(const uint8* A, const uint8* B, int32 Len);

	// === Serialization helpers ===
	// Map compression enum to the engine compression name.
	static FName GetCompressionName(ETurboStructLiteCompression Method);
	// Core buffer compression entry point.
	static bool CompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out, int32 MaxParallelThreads = -1, int32 ChunkBatchSizeMB = -1);
	// Core buffer decompression entry point.
	static bool DecompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out);
	// Serialize property with embedded metadata.
	static bool SerializePropertyWithMeta(FProperty* Property, void* Address, TArray<uint8>& OutBytes, FString& OutDebugMeta, bool bSaveOnlyMarked = false);
	// Deserialize property using embedded metadata.
	static bool DeserializePropertyWithMeta(FProperty* Property, void* Address, const TArray<uint8>& InBytes, int32 OverrideMaxThreads = -1, bool bSaveOnlyMarked = false);
	// Build debug metadata string from serialized bytes.
	static bool BuildDebugMetadataFromBytes(const TArray<uint8>& InBytes, FString& OutDebugMeta);
	// Read the root metadata type from serialized bytes.
	static bool GetRootMetaTypeFromBytes(const TArray<uint8>& InBytes, FString& OutType);
	// Normalize a type name for comparisons.
	static FString NormalizeTypeName(const FString& InType);
	// Normalize metadata field names for matching. Category: Serialization.
	static FString NormalizeMetaFieldName(const FString& InName);
	// Compare metadata names against a property name/authored name. Category: Serialization.
	static bool NamesMatchForMigration(const FString& MetaName, const FProperty* Property);
	// Read metadata and data pointers from serialized bytes.
	static bool ReadMetaFromBytes(const TArray<uint8>& InBytes, TArray<FTurboStructLiteFieldMeta>& OutFields, const uint8*& OutDataPtr, int32& OutDataLen, FString& OutErrorMessage);
	// Resolve the metadata field list for a struct root.
	static const TArray<FTurboStructLiteFieldMeta>* ResolveStructMetaFields(const TArray<FTurboStructLiteFieldMeta>& Fields, const FStructProperty* StructProp);
	// Find a metadata entry and offset for a property chain.
	static bool FindMetaByPropertyChain(const TArray<FTurboStructLiteFieldMeta>& MetaFields, const TArray<FProperty*>& PropertyChain, int32& OutOffset, const FTurboStructLiteFieldMeta*& OutMeta);
	// Copy archive version data into another archive.
	static void CopyArchiveVersions(FArchive& TargetAr, const FArchive& SourceAr);
	// Deserialize a property from a byte slice.
	static bool DeserializePropertyFromSlice(FProperty* Property, void* Address, const uint8* DataPtr, int32 DataSize, bool bSaveOnlyMarked, const FArchive& VersionSource);
	// Read a variant value from stored metadata bytes.
	static bool TryReadVariantFromMeta(const FTurboStructLiteFieldMeta& Meta, const uint8* DataPtr, int32 DataSize, bool bSaveOnlyMarked, const FArchive& VersionSource, FTurboStructLiteVariant& OutVariant, bool& bOutReaderError);
	// Try to migrate a property value when the stored type mismatches.
	static bool TryMigratePropertyValue(const FTurboStructLiteFieldMeta& Meta, FProperty* Property, void* Address, const uint8* DataPtr, int32 DataSize, bool bSaveOnlyMarked, const FArchive& VersionSource, bool& bOutReaderError);
	// Apply a variant value to a property.
	static bool TryApplyVariantToProperty(FProperty* Property, void* Address, const FTurboStructLiteVariant& Variant);
	// Convert a property value into a variant.
	static bool BuildVariantFromProperty(const FProperty* Property, const void* ValuePtr, FTurboStructLiteVariant& OutVariant);
	// Check if a property is safe to process in parallel.
	static bool IsPropertySafeForParallel(const FProperty* Property);
	// Set maximum parallel threads for array serialization (returns previous).
	static int32 SetParallelThreadLimit(int32 MaxThreads);
	// Get current parallel thread limit for serialization.
	static int32 GetParallelThreadLimit();
	// Serialize a wildcard property into bytes (optionally SaveGame-only).
	static bool SerializeWildcard(FProperty* Property, void* Address, TArray<uint8>& OutBytes, bool bSaveOnlyMarked = false);
	// Deserialize bytes into a wildcard property (optionally SaveGame-only).
	static bool DeserializeWildcard(FProperty* Property, void* Address, const TArray<uint8>& InBytes, int32 OverrideMaxThreads = -1, bool bSaveOnlyMarked = false);
	// Build debug metadata string for a property (name/type/fields).
	static FString BuildDebugMetadata(FProperty* Property);
	// Serialize large arrays in parallel to accelerate saves.
	static bool SerializeArrayParallel(FArrayProperty* ArrayProp, void* Address, TArray<uint8>& OutData, FTurboStructLiteFieldMeta& OutMeta, bool bSaveOnlyMarked = false);
	// Access saved magic value.
	static int32 GetMagic();
	// Access saved version value.
	static int32 GetVersion();
	// Read subslot info (metadata) using an explicit encryption key (editor helper).
	static bool TurboStructLiteGetSubSlotInfoWithKey(const FString& MainSlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, FTurboStructLiteSubSlotInfo& OutInfo);
	// Serialize a property for editor display, skipping transient fields.
	static bool TurboStructLiteSerializeProperty(FProperty* Property, void* Address, TArray<uint8>& OutBytes);
	// Deserialize a property for editor display, skipping transient fields.
	static bool TurboStructLiteDeserializeProperty(FProperty* Property, void* Address, const TArray<uint8>& InBytes);
	// Validate struct layout against recorded field names (editor safety).
	static bool TurboStructLiteValidateStructLayout(const FProperty* Property, const TArray<FString>& FieldNames);
	// Internal read of subslot metadata (used by editor helpers).
	static bool ReadSubSlotInfoInternal(const FString& SlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption Encryption, FTurboStructLiteSubSlotInfo& OutInfo);
	// Serialize field metadata tree into an archive.
	static void WriteFieldMeta(FArchive& Ar, const FTurboStructLiteFieldMeta& Meta);
	// Deserialize field metadata tree from an archive.
	static bool ReadFieldMeta(FArchive& Ar, FTurboStructLiteFieldMeta& OutMeta);
	// Build the debug metadata string (Name/Type/Fields) from field metadata.
	static bool BuildDebugString(const TArray<FTurboStructLiteFieldMeta>& Fields, FString& Out);
	// Detect unsupported properties for serialization.
	static bool IsUnsupportedProperty(const FProperty* Property);
	// Recursively serialize a property into data and metadata.
	static bool SerializePropertyRecursive(FProperty* Property, void* Address, TArray<uint8>& OutData, FTurboStructLiteFieldMeta& OutMeta, bool bSaveOnlyMarked = false);
	// Apply metadata back into a struct instance.
	static bool ApplyMetaToStruct(const TArray<FTurboStructLiteFieldMeta>& MetaFields, const UStruct* Struct, uint8* BasePtr, const uint8* Data, int32 DataLen, int32& Offset, int32 MaxThreads, bool bSaveOnlyMarked, const FString& PathPrefix, const FArchive& VersionSource);
	// Build a comma separated list of struct field names.
	static FString BuildStructFieldList(const UStruct* Struct);
	// Compare struct fields against a recorded list (order-sensitive).
	static bool StructMatchesFields(const UStruct* Struct, const TArray<FString>& FieldNames);

	// Map blueprint async execution to engine async execution.
	static EAsyncExecution ResolveAsyncExecution(ETurboStructLiteAsyncExecution Execution);

	// === File helpers ===
	// Read header and entry count from an archive.
	static bool ReadHeaderAndEntryCount(FArchive& Reader, int32 ExpectedMagic, int32& OutVersion, int32& OutEntryCount);
	// Skip forward a data segment safely.
	static bool SkipData(FArchive& Reader, int32 DataSize);
	// Validate that a buffer size fits in the remaining archive.
	static bool IsValidBufferSize(FArchive& Reader, int32 SizeToCheck);
	// Calculate streaming buffer size for file IO based on file size.
	static int32 CalcStreamingBufferSize(int64 FileSize);
	// Write a single entry to an archive.
	static void WriteEntry(FArchive& Writer, int32 SubSlot, const FTurboStructLiteEntry& Entry);
	// List subslot indices in a slot file.
	static bool ListSubSlotIndices(const FString& SlotName, TArray<int32>& OutSubSlots);
	// List subslot infos (with metadata) in a slot file.
	static bool ListSubSlotInfos(const FString& SlotName, TArray<FTurboStructLiteSubSlotInfo>& OutInfos);
	// Query subslot indices with status for a slot.
	static ETurboStructLiteSlotQueryStatus QuerySubSlotIndices(const FString& SlotName, TArray<int32>& OutSubSlots);
	// Query subslot infos with status for a slot.
	static ETurboStructLiteSlotQueryStatus QuerySubSlotInfos(const FString& SlotName, TArray<FTurboStructLiteSubSlotInfo>& OutInfos);
	// Fetch slot metadata.
	static bool GetSlotInfoInternal(const FString& SlotName, FTurboStructLiteSlotInfo& OutInfo);
	// Sanitize slot name for on-disk usage. Category: File helpers.
	static FString SanitizeSlotName(const FString& InSlotName);
	// Build absolute path to a slot file.
	static FString BuildSavePath(const FString& SlotName);
	// Build WAL absolute path for an operation.
	static FString GenerateWALPath(const FString& SlotName, int32 SubSlotIndex, const FString& OpLabel);
	// Calculate a 256-bit hash (Blake3 on 5.1+, SHA-1 fallback on 5.0).
	static void CalculateTurboHash(const uint8* Data, int32 Size, uint8 OutHash[32]);
	// Append a WAL entry with timestamp.
	static void WriteWALEntry(const FString& WalPath, const FString& Message);
	// Delete a WAL file safely.
	static void DeleteWALFile(const FString& WalPath);
	// Read the full turbo struct file into a map.
	static bool ReadTurboStructLiteFile(const FString& FilePath, TMap<int32, FTurboStructLiteEntry>& OutEntries);
	// Write the full turbo struct file from a map.
	static bool WriteTurboStructLiteFile(const FString& FilePath, const TMap<int32, FTurboStructLiteEntry>& Entries);
	// Save a single subslot to disk (streaming-friendly).
	static bool SaveEntry(const FString& SlotName, int32 SubSlotIndex, ETurboStructLiteCompression Compression, ETurboStructLiteEncryption Encryption, const FString& EncryptionKey, const TArray<uint8>& RawBytes, const FString& DebugMeta, int32 MaxParallelThreads = -1, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault, bool bUseWriteAheadLog = false, const FString& WALPath = TEXT(""));
	// Load a single subslot from disk.
	static bool LoadEntry(const FString& SlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption DefaultEncryption, TArray<uint8>& OutRawBytes, bool bUseWriteAheadLog = false, const FString& WALPath = TEXT(""));
	// Check for existence of a subslot.
	static bool ExistsEntry(const FString& SlotName, int32 SubSlotIndex);
	// Remove a subslot from disk.
	static bool RemoveEntry(const FString& SlotName, int32 SubSlotIndex);
	// Build a slot index cache for fast seeks.
	static bool BuildSlotIndex(const FString& SlotName, FTurboStructLiteSlotIndex& OutIndex);
	// Get or rebuild the cached slot index.
	static bool GetSlotIndex(const FString& SlotName, FTurboStructLiteSlotIndex& OutIndex);
	// Get cached offsets for a subslot.
	static bool GetCachedEntry(const FString& SlotName, int32 SubSlotIndex, FTurboStructLiteCachedEntry& OutEntry);
	// Check if on-screen memory warnings are enabled.
	static bool ShouldShowOnScreenWarnings();
	// Ensure the memory warning CVar is registered.
	static void EnsureMemoryWarningCvar();
	// Build a stable key for memory warnings.
	static uint32 BuildMemoryWarningKey(const FString& SlotName, int32 SubSlotIndex, bool bIsSave, bool bIsLogic);
	// Show the initial on-screen operation message.
	static void BeginMemoryOpMessage(const FString& SlotName, int32 SubSlotIndex, bool bIsSave, bool bIsLogic);
	// Update the on-screen/log warning for large payloads.
	static void UpdateMemoryPressureWarning(const FString& SlotName, int32 SubSlotIndex, int64 RawSizeBytes, bool bIsSave, bool bIsLogic);
	// Clear the on-screen operation message.
	static void EndMemoryOpMessage(const FString& SlotName, int32 SubSlotIndex, bool bIsSave, bool bIsLogic);
	// Estimate raw size from a wildcard property and pointer.
	static bool EstimateWildcardSize(FProperty* DataProp, void* DataPtr, int64& OutSizeBytes);
	// Resolve expected uncompressed size for a slot/subslot.
	static bool GetExpectedRawSize(const FString& SlotName, int32 SubSlotIndex, int64& OutSizeBytes);
	// Invalidate the cached slot index.
	static void InvalidateSlotIndex(const FString& SlotName);
	// Invalidate all cached slot indexes.
	static void InvalidateAllSlotIndexes();

	// === Task queue ===
	// Enqueue a per-slot task.
	static void EnqueueTask(const FString& SlotName, TFunction<void()> Task, int32 Priority, TFunction<void()> CancelCallback = TFunction<void()>());
	// Run the next task for a slot.
	static void ProcessNextTask(const FString& SlotName);
	// Queue a save request.
	static void EnqueueSaveRequest(FTurboStructLiteSaveRequest&& Request);
	// Execute a save request (sync or async).
	static void ExecuteSaveRequest(FTurboStructLiteSaveRequest&& Request);
	// Category: Task queue.
	// Execute the core save work for a slot/subslot.
	static bool ExecuteSaveWork(const FString& SlotName, int32 SubSlotIndex, ETurboStructLiteCompression Compression, ETurboStructLiteEncryption Encryption, const FString& EncryptionKey, const TArray<uint8>& RawBytes, const FString& DebugMeta, int32 MaxParallelThreads, ETurboStructLiteBatchingSetting CompressionBatching, bool bUseWriteAheadLog, const FString& WALPath);
	// Category: Task queue.
	// Execute a save request on the thread pool.
	static void ExecuteSaveAsync(TArray<uint8>&& RawBytes, const FString& SlotName, int32 SubSlotIndex, ETurboStructLiteCompression Compression, ETurboStructLiteEncryption Encryption, const FString& EncryptionKey, const FString& DebugMeta, int32 MaxParallelThreads, ETurboStructLiteBatchingSetting CompressionBatching, TFunction<void(bool, FString, int32)>&& Callback, bool bUseWriteAheadLog, const FString& WALPath);
	// Category: Task queue.
	// Finalize an async save on the game thread.
	static void FinalizeSaveRequestAsync(const FString& SlotName, int32 SubSlotIndex, bool bSaved, const FString& FilePath, TFunction<void(bool, FString, int32)>&& Callback, bool bUseWriteAheadLog, const FString& WALPath);
	// Category: Task queue.
	// Finalize a sync save on the calling thread.
	static void FinalizeSaveRequestSync(const FString& SlotName, int32 SubSlotIndex, bool bSaved, const FString& FilePath, TFunction<void(bool, FString, int32)>&& Callback, bool bUseWriteAheadLog, const FString& WALPath);
	// Mark save queue as done for a slot.
	static void FinishQueuedSave(const FString& SlotName);
	// Queue a load request.
	static void EnqueueLoadRequest(FTurboStructLiteLoadRequest&& Request);
	// Execute a load request (sync or async).
	static void ExecuteLoadRequest(FTurboStructLiteLoadRequest&& Request);
	// Mark load queue as done for a slot.
	static void FinishQueuedLoad(const FString& SlotName);
	// Access or create per-slot task queue.
	static TSharedPtr<FTurboStructLiteTaskQueue> GetQueueForSlot(const FString& SlotName);
	// Clear all queues when no game world is active.
	static void ClearAllQueues();
	// Check if an active game world exists.
	static bool HasActiveGameWorld();
	// Dispatch cancel callbacks on the game thread.
	static void DispatchCancelCallbacks(TArray<TFunction<void()>>&& Callbacks);
	// Get or create a per-slot operation lock.
	static TSharedPtr<FCriticalSection> GetSlotOperationLock(const FString& SlotName);
	// Mark a slot operation as started.
	static void BeginSlotOperation(const FString& SlotName);
	// Mark a slot operation as finished.
	static void EndSlotOperation(const FString& SlotName);
	// Check if a slot has an active operation.
	static bool HasActiveSlotOperation(const FString& SlotName);
	// Check if any slot has an active operation.
	static bool HasAnyActiveSlotOperation();
	// Global encryption key provider.
	static FTurboStructLiteKeyProviderDelegate GlobalKeyProvider;
	// Shared task queue storage.
	static FCriticalSection QueuesMutex;
	static TMap<FString, TSharedPtr<FTurboStructLiteTaskQueue>> QueuesBySlot;
	static FCriticalSection SlotOperationMutex;
	static TMap<FString, TSharedPtr<FCriticalSection>> SlotOperationLocks;
	static FCriticalSection ActiveSlotOpsMutex;
	static TMap<FString, int32> ActiveSlotOps;
	static int32 ActiveSlotOpsTotal;
	static FCriticalSection SlotIndexMutex;
	static TMap<FString, FTurboStructLiteSlotIndex> CachedSlotIndexes;
	static FCriticalSection MemoryWarningMutex;
	static TSet<uint32> WarnedMemorySlots;
	static bool bMemoryWarningsCvarRegistered;
	static IConsoleVariable* MemoryWarningsOnScreenInShippingCvar;

	friend class STurboStructLiteDatabaseWidget;
	friend class FTurboStructLiteDatabaseParser;
};
