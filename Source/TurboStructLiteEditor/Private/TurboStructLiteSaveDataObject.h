/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * UObject auxiliar para mostrar datos de subslots en el editor. Almacena metadata, tamanos,
 * cifrado/compresion y payload editable via PropertyBag.
 *
 * Description (EN):
 * Helper UObject to display subslot data in the editor. Stores metadata, sizes, encryption/
 * compression info, and an editable payload via PropertyBag.
 */

#if !defined(TURBOSTRUCT_HAS_EDITOR_UI)
#define TURBOSTRUCT_HAS_EDITOR_UI 0
#endif

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "CoreMinimal.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "StructUtils/PropertyBag.h"
#else
#include "PropertyBag.h"
#endif
#include "TurboStructLiteSaveDataObject.generated.h"

UCLASS(Transient)
class TURBOSTRUCTLITEEDITOR_API UTurboStructLiteSaveDataObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FString SlotName;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	int32 SubSlotIndex = 0;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FString SourceFile;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FDateTime FileTimestamp;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	int32 DataSize = 0;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	int32 UncompressedSize = 0;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FString Compression;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FString Encryption;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FString DebugMetadata;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	bool bParsed = false;

	UPROPERTY(VisibleAnywhere, Category = "Metadata")
	FString ParseError;

	UPROPERTY(VisibleAnywhere, Transient, Category = "TurboStructLite Data", meta = (ShowOnlyInnerProperties))
	FInstancedPropertyBag Payload;
};

#endif
