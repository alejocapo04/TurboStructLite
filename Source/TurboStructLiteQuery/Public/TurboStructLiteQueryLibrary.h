/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Biblioteca Blueprint de TurboStructLiteQuery. Expone consultas y filtros logicos
 * sobre slots mediante nodos CustomThunk.
 *
 * Description (EN):
 * TurboStructLiteQuery Blueprint library. Exposes logical queries and filtering
 * over slots via CustomThunk nodes.
 */

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "TurboStructLiteTypesQuery.h"
#include "TurboStructLiteQueryLibrary.generated.h"

DECLARE_DYNAMIC_DELEGATE_SixParams(FTurboStructLiteLogicLoadComplete, bool, bSuccess, FString, OutErrorMessage, FString, OutMetadata, FDateTime, OutSaveDate, int32, OutSaveVersion, FString, OutStats);

UCLASS()
class TURBOSTRUCTLITEQUERY_API UTurboStructLiteQueryLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Load an array using a logic query (async).
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "TurboStructLite Logic Query", meta = (ArrayParm = "Data", ArrayTypeDependentParams = "Data", AutoCreateRefTerm = "QueryString,bUseWriteAheadLog,OnComplete,QueuePriority,MaxParallelThreads,EncryptionKey,CompressionBatching", AdvancedDisplay = "bUseWriteAheadLog,QueuePriority,MaxParallelThreads,EncryptionKey,Encryption,CompressionBatching", CPP_Default_SubSlotIndex = "-1"))
	static void TurboStructLoadArrayLogicLite(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FString& QueryString, UPARAM(ref) TArray<int32>& Data, const FTurboStructLiteLogicLoadComplete& OnComplete, bool bUseWriteAheadLog = false, int32 QueuePriority = 10, int32 MaxParallelThreads = 4, const FString& EncryptionKey = TEXT(""), ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::ProjectDefault, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault);

private:
	// === Custom thunks ===
	DECLARE_FUNCTION(execTurboStructLiteLoadStructLogic);
	DECLARE_FUNCTION(execTurboStructLoadArrayLogicLite);
	DECLARE_FUNCTION(execTurboStructLiteLoadSetLogic);
	DECLARE_FUNCTION(execTurboStructLiteLoadMapLogic);
	DECLARE_FUNCTION(execTurboStructLiteValidateQuery);

	// Category: Logic Query.
	static void TurboStructLiteLoadStructLogic(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FString& QueryString, const int32& Data, const FTurboStructLiteLogicLoadComplete& OnComplete, bool bUseWriteAheadLog = false, int32 QueuePriority = 10, int32 MaxParallelThreads = 4, const FString& EncryptionKey = TEXT(""), ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::ProjectDefault, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault);
	// Category: Logic Query.
	static void TurboStructLiteLoadSetLogic(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FString& QueryString, UPARAM(ref) TSet<int32>& Data, const FTurboStructLiteLogicLoadComplete& OnComplete, bool bUseWriteAheadLog = false, int32 QueuePriority = 10, int32 MaxParallelThreads = 4, const FString& EncryptionKey = TEXT(""), ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::ProjectDefault, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault);
	// Category: Logic Query.
	static void TurboStructLiteLoadMapLogic(const FString& MainSlotName, int32 SubSlotIndex, bool bAsync, const FString& QueryString, UPARAM(ref) TMap<int32, int32>& Data, const FTurboStructLiteLogicLoadComplete& OnComplete, bool bUseWriteAheadLog = false, int32 QueuePriority = 10, int32 MaxParallelThreads = 4, const FString& EncryptionKey = TEXT(""), ETurboStructLiteEncryption Encryption = ETurboStructLiteEncryption::ProjectDefault, ETurboStructLiteBatchingSetting CompressionBatching = ETurboStructLiteBatchingSetting::ProjectDefault);
	// Category: Logic Query.
	static void TurboStructLiteValidateQuery(const FString& QueryString, const int32& Context, bool& IsValid, FString& ErrorMessage);

	// === Logic query helpers ===
	// Resolve the first available subslot for a slot.
	static bool ResolveLogicSubSlot(const FString& SlotName, int32& OutSubSlotIndex, FString& OutErrorMessage);
	// Parse SELECT/WHERE syntax into query text, fields, and SQL clauses.
	static bool ParseSelectQueryString(const FString& InQueryString, const TArray<FString>& InSelectFields, FString& OutQueryString, TArray<FString>& OutSelectFields, int32& OutLimit, int32& OutOffset, FString& OutOrderByField, bool& bOutOrderDescending, TArray<ETurboStructLiteAggregateOp>& OutAggregateOps, TArray<FString>& OutAggregateFields, TArray<FName>& OutAggregateColumns, FString& OutErrorMessage);
	// Build a logic query context from a struct type.
	static bool BuildLogicQueryContextFromStruct(UStruct* RootStruct, FTurboStructLiteLogicQueryContext& OutContext, FString& OutErrorMessage);
	// Resolve a context struct from a slot's metadata.
	static bool ResolveContextStructFromSlot(const FString& SlotName, int32 SubSlotIndex, const FString& EncryptionKey, ETurboStructLiteEncryption SelectedEncryption, UStruct*& OutStruct, FString& OutErrorMessage);
	// Find a struct by normalized type name.
	static UStruct* FindStructByTypeName(const FString& TypeName);
	// Execute a SELECT/aggregate query and return rows.
	static bool ExecuteSelectQuery(const FString& SlotName, int32 SubSlotIndex, const FString& QueryString, const FString& EncryptionKey, ETurboStructLiteEncryption SelectedEncryption, int32 MaxParallelThreads, bool bUseWriteAheadLog, const FString& WALPath, UStruct* ContextStruct, bool& bOutHasAggregates, TArray<FTurboStructLiteRow>& OutRows, FString& OutMetadata, FDateTime& OutSaveDate, FString& OutStatsText, FString& OutErrorMessage);
	// Execute a SELECT/aggregate query and build an output value buffer.
	static bool ExecuteSelectQueryToValue(const FString& SlotName, int32 SubSlotIndex, const FString& QueryString, const FString& EncryptionKey, ETurboStructLiteEncryption SelectedEncryption, int32 MaxParallelThreads, bool bUseWriteAheadLog, const FString& WALPath, FProperty* OutputProp, TArray<uint8>& OutValue, FString& OutMetadata, FDateTime& OutSaveDate, FString& OutStatsText, FString& OutErrorMessage);
	// Apply aggregate results to an output property.
	static bool ApplyAggregateToOutput(const TArray<FTurboStructLiteRow>& Rows, FProperty* OutputProp, void* OutputPtr, FString& OutErrorMessage);
	// Apply row results to an output property.
	static bool ApplyRowsToOutput(const TArray<FTurboStructLiteRow>& Rows, FProperty* OutputProp, void* OutputPtr, FString& OutErrorMessage);
	// Apply a row to a struct instance.
	static bool ApplyRowToStruct(const FTurboStructLiteRow& Row, UStruct* Struct, void* StructPtr, FString& OutErrorMessage);
	// Build a logic query context from a property.
	static bool BuildLogicQueryContext(FProperty* DataProp, FTurboStructLiteLogicQueryContext& OutContext, FString& OutErrorMessage);
	// Tokenize a logic query string with optional SQL keyword support.
	static bool TokenizeLogicQuery(const FString& QueryString, TArray<FTurboStructLiteQueryToken>& OutTokens, FString& OutErrorMessage, int32& OutErrorPos, bool bAllowSqlKeywords = false);
	// Parse a logic query into an AST.
	static bool ParseLogicQuery(const TArray<FTurboStructLiteQueryToken>& Tokens, TSharedPtr<FTurboStructLiteQueryNode>& OutRoot, FString& OutErrorMessage, int32& OutErrorPos);
	// Bind a logic query AST to a context.
	static bool BindLogicQuery(TSharedPtr<FTurboStructLiteQueryNode>& Root, const FTurboStructLiteLogicQueryContext& Context, FString& OutErrorMessage, int32& OutErrorPos);
	// Evaluate a logic query AST.
	static bool EvaluateLogicQueryNode(const FTurboStructLiteQueryNode& Root, const uint8* RootPtr, const uint8* KeyPtr, const uint8* ValuePtr);
	// Collect bound properties from a logic query AST.
	static void CollectQueryBoundProperties(const TSharedPtr<FTurboStructLiteQueryNode>& Root, TArray<const FTurboStructLiteQueryBoundProperty*>& OutProperties);
	// Build select field infos from field paths.
	static bool BuildSelectFieldInfos(const TArray<FString>& SelectFields, const UStruct* RootStruct, TArray<FTurboStructLiteSelectFieldInfo>& OutFields, FString& OutErrorMessage);
	// Apply a logic query over loaded data.
	static bool ApplyLogicFilter(FProperty* DataProp, const TSharedPtr<FTurboStructLiteQueryNode>& Root, const TArray<uint8>& SourceValueBuffer, TArray<uint8>& OutValueBuffer, FTurboStructLiteLogicQueryStats& OutStats, FString& OutErrorMessage);
	// Deserialize bytes into a value buffer for processing.
	static bool DeserializeLogicValue(FProperty* DataProp, const TArray<uint8>& RawBytes, int32 MaxThreads, TArray<uint8>& OutValueBuffer, FString& OutErrorMessage);
	// Format logic query stats output.
	static FString FormatLogicStats(const FTurboStructLiteLogicQueryStats& Stats);
};
