/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Parser de metadata de TurboStruct. Analiza cadenas de metadata de subslots, construye property
 * bags, serializa/deserializa bytes y describe configuraciones de compresion/cifrado.
 *
 * Description (EN):
 * TurboStruct metadata parser. Parses subslot metadata strings, builds property bags, serializes/
 * deserializes bytes, and describes compression/encryption settings.
 */

#if !defined(TURBOSTRUCT_HAS_EDITOR_UI)
#define TURBOSTRUCT_HAS_EDITOR_UI 0
#endif

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"
#include "TurboStructLiteTypes.h"
#include "TurboStructLiteEditorTypes.h"

class FProperty;

class FTurboStructLiteDatabaseParser
{
public:
	static bool ParseMetadata(const FString& InMetadata, FTurboStructLiteParsedProperty& OutMeta, FString& OutError);
	static bool BuildBag(const FTurboStructLiteParsedProperty& Meta, FInstancedPropertyBag& Bag);
	static bool DeserializeIntoBag(const FTurboStructLiteParsedProperty& Meta, FInstancedPropertyBag& Bag, const TArray<uint8>& Bytes, FString& OutError);
	static bool SerializeFromBag(const FTurboStructLiteParsedProperty& Meta, const FInstancedPropertyBag& Bag, TArray<uint8>& OutBytes, FString& OutError);
	static FString DescribeCompression(ETurboStructLiteCompression Compression);
	static FString DescribeEncryption(ETurboStructLiteEncryption Encryption);
	static FString BuildMetadataString(const FTurboStructLiteParsedProperty& Meta);
	static FString FormatMetadataForDisplay(const FString& RawMetadata, const FTurboStructLiteParsedProperty& Parsed);
	static FString GetLastParseError();
	static void SetLastParseError(const FString& Error);
	static bool IsAllDigits(const FString& Text);
	static void GetStructFieldNames(const UStruct* Struct, TArray<FString>& OutNames, int32 Depth = 0);

	// Compress raw bytes using runtime implementation. Category = TurboStruct|Editor.
	static bool CompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out, int32 MaxParallelThreads = -1, int32 ChunkBatchSizeMB = -1);

	// Decompress raw bytes using runtime implementation. Category = TurboStruct|Editor.
	static bool DecompressBuffer(ETurboStructLiteCompression Method, const TArray<uint8>& In, TArray<uint8>& Out);

	// Encrypt a payload using runtime implementation. Category = TurboStruct|Editor.
	static bool EncryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData);

	// Decrypt a payload using runtime implementation. Category = TurboStruct|Editor.
	static bool DecryptDataBuffer(ETurboStructLiteEncryption Method, const FString& Key, TArray<uint8>& InOutData);

	// Serialize a property with metadata using runtime implementation. Category = TurboStruct|Editor.
	static bool SerializePropertyWithMeta(FProperty* Property, void* Address, TArray<uint8>& OutBytes, FString& OutDebugMeta, bool bSaveOnlyMarked = false);

	// Deserialize a property with metadata using runtime implementation. Category = TurboStruct|Editor.
	static bool DeserializePropertyWithMeta(FProperty* Property, void* Address, const TArray<uint8>& InBytes, int32 OverrideMaxThreads = -1, bool bSaveOnlyMarked = false);

private:
	// Category: Internal.
	static FString LastParseError;
	// Category: Internal.
	static UObject* FindAssetByName(const FString& Name, const TArray<FTopLevelAssetPath>& ClassPaths);
	// Category: Internal.
	static bool ResolveEnumType(const FString& InType, const UObject*& OutTypeObject);
	// Category: Internal.
	static bool ResolveStructType(const FString& InType, const UObject*& OutTypeObject);
	// Category: Internal.
	static FString StripWhitespace(const FString& In);
	// Category: Internal.
	static bool ParseContainerAndType(const FString& InType, FString& OutElementType, bool& bOutArray, bool& bOutSet, bool& bOutMap);
	// Category: Internal.
	static bool ResolveValueType(const FString& InType, EPropertyBagPropertyType& OutType, const UObject*& OutTypeObject);
	// Category: Internal.
	static bool IsLikelyHex(const FString& Text);
	// Category: Internal.
	static FString SanitizeFieldName(const FString& InName);
	// Category: Internal.
	static FString DescribeStructFields(const UStruct* Struct, int32 Depth);
	// Category: Internal.
	static FString DescribePropertyReadable(const FProperty* Prop, int32 Depth);
	// Category: Internal.
	static FProperty* ResolveBagProperty(const FInstancedPropertyBag& Bag, const FString& Name);
};

#endif
