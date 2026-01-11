/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Description (EN):
 * Helper types for the TurboStructLite editor. Defines save entries and UI tree nodes.
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
#include "TurboStructLiteTypes.h"
#include "TurboStructLiteSaveDataObject.h"

enum class ESaveEntryKind : uint8
{
	TurboStructBase
};

struct FTurboStructLiteParsedProperty
{
	FString PropertyName;
	FString TypeName;
	bool bIsArray = false;
	bool bIsSet = false;
	EPropertyBagPropertyType ValueType = EPropertyBagPropertyType::None;
	TWeakObjectPtr<const UObject> TypeObject;
	TArray<FString> FieldNames;
};

struct FSaveEntry
{
	FString SlotName;
	int32 UserIndex = 0;
	FString FilePath;
	FDateTime Timestamp;
	ESaveEntryKind Kind = ESaveEntryKind::TurboStructBase;
	FTurboStructLiteSubSlotInfo Info;
	FString MetaString;
	FTurboStructLiteParsedProperty ParsedMeta;
	bool bLoaded = false;
	bool bDirty = false;
	FString ParseError;
	FInstancedPropertyBag DataBag;
	TStrongObjectPtr<UTurboStructLiteSaveDataObject> DataObject;
	FString EncryptionKey;
	bool bKeyPromptCancelled = false;
	bool bReadOnly = false;

	~FSaveEntry()
	{
		FMemory::Memzero(&DataBag, sizeof(FInstancedPropertyBag));
		if (DataObject.IsValid() && DataObject.Get() && DataObject->IsValidLowLevelFast())
		{
			FMemory::Memzero(&DataObject->Payload, sizeof(FInstancedPropertyBag));
		}
	}
};

struct FSaveTreeNode
{
	enum class ENodeType : uint8 { Slot, SubSlotGroup, SubSlot };

	ENodeType Type = ENodeType::Slot;
	FString SlotName;
	ESaveEntryKind Kind = ESaveEntryKind::TurboStructBase;
	int32 UserIndex = 0;
	int32 SubSlotIndex = 0;
	TArray<TSharedPtr<FSaveTreeNode>> Children;
	TSharedPtr<FSaveEntry> Entry;
};

#endif
