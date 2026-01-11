#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLiteDatabaseParser.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Animation/CurveSequence.h"
#include "Widgets/Layout/SWidgetSwitcher.h"

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"

bool STurboStructLiteDatabaseWidget::IsStructSafeRecursive(const UStruct* InStruct, TSet<const UStruct*>& Visited) const
{
	if (!InStruct || Visited.Contains(InStruct))
	{
		return true;
	}
	Visited.Add(InStruct);

	const FString Name = InStruct->GetName();
	if (Name.StartsWith(TEXT("REINST_")) ||
		Name.StartsWith(TEXT("TRASH_")) ||
		Name.StartsWith(TEXT("SKEL_")) ||
		Name.StartsWith(TEXT("PLACEHOLDER_")))
	{
		return false;
	}

	for (TFieldIterator<FProperty> It(InStruct); It; ++It)
	{
		FProperty* Prop = *It;
		const UStruct* ChildStruct = nullptr;
		const UClass* ChildClass = nullptr;

		if (const FStructProperty* SProp = CastField<FStructProperty>(Prop))
		{
			ChildStruct = SProp->Struct;
		}
		else if (const FArrayProperty* AProp = CastField<FArrayProperty>(Prop))
		{
			if (const FStructProperty* InnerS = CastField<FStructProperty>(AProp->Inner))
			{
				ChildStruct = InnerS->Struct;
			}
			else if (const FObjectProperty* InnerO = CastField<FObjectProperty>(AProp->Inner))
			{
				ChildClass = InnerO->PropertyClass;
			}
		}
		else if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			if (const FStructProperty* InnerS = CastField<FStructProperty>(SetProp->ElementProp))
			{
				ChildStruct = InnerS->Struct;
			}
		}
		else if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			if (const FStructProperty* KeyS = CastField<FStructProperty>(MapProp->KeyProp))
			{
				if (!IsStructSafeRecursive(KeyS->Struct, Visited))
				{
					return false;
				}
			}
			if (const FStructProperty* ValS = CastField<FStructProperty>(MapProp->ValueProp))
			{
				if (!IsStructSafeRecursive(ValS->Struct, Visited))
				{
					return false;
				}
			}
		}
		else if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			ChildClass = ObjProp->PropertyClass;
		}

		if (ChildStruct && !IsStructSafeRecursive(ChildStruct, Visited))
		{
			return false;
		}

		if (ChildClass)
		{
			const FString ClassName = ChildClass->GetName();
			if (ClassName.StartsWith(TEXT("REINST_")) || ClassName.StartsWith(TEXT("TRASH_")) || ClassName.StartsWith(TEXT("SKEL_")))
			{
				return false;
			}
		}
	}

	return true;
}

FString STurboStructLiteDatabaseWidget::BuildSlotKey(const FString& SlotName, ESaveEntryKind Kind, int32 UserIndex) const
{
	return FString::Printf(TEXT("%s|%d|%d"), *SlotName, static_cast<int32>(Kind), UserIndex);
}

void STurboStructLiteDatabaseWidget::RefreshData()
{
	SelectedNode.Reset();
	RootNodes.Reset();
	FilteredRootNodes.Reset();
	AllSlotNodes.Reset();
	ClearAllInspector();

	const FString SaveDir = GetSaveDirectory();
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFiles(FoundFiles, *SaveDir, TEXT("*.ssfs"));

	for (const FString& FileName : FoundFiles)
	{
		const FString SlotName = FPaths::GetBaseFilename(FileName);
		const FString FilePath = FPaths::Combine(SaveDir, FileName);
		FTurboStructLiteSlotInfo SlotInfo;
		UTurboStructLiteBPLibrary::TurboStructLiteGetSlotInfo(SlotName, SlotInfo);
		SlotInfo.Timestamp = IFileManager::Get().GetTimeStamp(*FilePath);

		TArray<FTurboStructLiteSubSlotInfo> Infos = UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlotInfos(SlotName);
		if (Infos.Num() == 0)
		{
			continue;
		}

		TSharedPtr<FSaveTreeNode> SlotNode = MakeShared<FSaveTreeNode>();
		SlotNode->Type = FSaveTreeNode::ENodeType::Slot;
		SlotNode->SlotName = SlotName;
		SlotNode->Kind = ESaveEntryKind::TurboStructBase;
		SlotNode->UserIndex = 0;

		for (const FTurboStructLiteSubSlotInfo& Info : Infos)
		{
			TSharedPtr<FSaveEntry> Entry = MakeShared<FSaveEntry>();
			Entry->SlotName = SlotName;
			Entry->Kind = ESaveEntryKind::TurboStructBase;
			Entry->UserIndex = 0;
			Entry->FilePath = FilePath;
			Entry->Timestamp = SlotInfo.Timestamp;
			Entry->Info = Info;
			Entry->MetaString = Info.DebugMetadata;

			TSharedPtr<FSaveTreeNode> ChildNode = MakeShared<FSaveTreeNode>();
			ChildNode->Type = FSaveTreeNode::ENodeType::SubSlot;
			ChildNode->SlotName = SlotName;
			ChildNode->Kind = ESaveEntryKind::TurboStructBase;
			ChildNode->UserIndex = 0;
			ChildNode->SubSlotIndex = Info.SubSlotIndex;
			ChildNode->Entry = Entry;

			SlotNode->Children.Add(ChildNode);
		}

		SlotNode->Children.Sort([](const TSharedPtr<FSaveTreeNode>& A, const TSharedPtr<FSaveTreeNode>& B)
		{
			return A->SubSlotIndex < B->SubSlotIndex;
		});

		RootNodes.Add(SlotNode);
		AllSlotNodes.Add(BuildSlotKey(SlotName, ESaveEntryKind::TurboStructBase, 0), SlotNode);
	}

	RootNodes.Sort([](const TSharedPtr<FSaveTreeNode>& A, const TSharedPtr<FSaveTreeNode>& B)
	{
		return A->SlotName < B->SlotName;
	});

	FilterTree();
	UpdateTabLabel();
}

bool STurboStructLiteDatabaseWidget::LoadEntryData(FSaveEntry& Entry)
{
	if ((Entry.bDirty || Entry.bLoaded) && Entry.DataObject.IsValid())
	{
		return true;
	}

	Entry.ParseError.Reset();
	Entry.bReadOnly = true;

	if (Entry.Info.Encryption == ETurboStructLiteEncryption::AES && Entry.EncryptionKey.IsEmpty())
	{
		if (!EnsureEncryptionKey(Entry))
		{
			Entry.ParseError = TEXT("Encryption key required");
			if (Entry.DataObject.IsValid())
			{
				Entry.DataObject->ParseError = Entry.ParseError;
				Entry.DataObject->bParsed = false;
			}
			return false;
		}
	}

	if (Entry.Info.Encryption == ETurboStructLiteEncryption::AES)
	{
		FTurboStructLiteSubSlotInfo InfoWithMeta;
		if (UTurboStructLiteBPLibrary::TurboStructLiteGetSubSlotInfoWithKey(Entry.SlotName, Entry.Info.SubSlotIndex, Entry.EncryptionKey, Entry.Info.Encryption, InfoWithMeta))
		{
			if (!InfoWithMeta.DebugMetadata.IsEmpty())
			{
				Entry.MetaString = InfoWithMeta.DebugMetadata;
			}
		}
	}

	TArray<uint8> Bytes;
	const FString KeyToUse = Entry.Info.Encryption == ETurboStructLiteEncryption::AES ? Entry.EncryptionKey : FString();
	const bool bLoaded = UTurboStructLiteBPLibrary::TurboStructLiteLoadSubSlotBytes(Entry.SlotName, Entry.Info.SubSlotIndex, KeyToUse, Entry.Info.Encryption, Bytes);
	if (!bLoaded)
	{
		Entry.ParseError = TEXT("File Corrupted / Load Failed");
		Entry.bLoaded = false;
		return false;
	}

	TWeakObjectPtr<const UObject> TypeHint = Entry.ParsedMeta.TypeObject;
	const FString HintName = Entry.ParsedMeta.TypeName;
	FString MetaError;
	if (!FTurboStructLiteDatabaseParser::ParseMetadata(Entry.MetaString, Entry.ParsedMeta, MetaError))
	{
		Entry.ParseError = MetaError;
		FTurboStructLiteDatabaseParser::SetLastParseError(MetaError);
	}
	else
	{
		if (TypeHint.IsValid() && (Entry.ParsedMeta.TypeName.Equals(TypeHint->GetName()) || Entry.ParsedMeta.TypeName.Equals(HintName)))
		{
			Entry.ParsedMeta.TypeObject = TypeHint;
		}

		if (Entry.ParsedMeta.TypeObject.IsValid())
		{
			const UObject* TypeObj = Entry.ParsedMeta.TypeObject.Get();
			if (!IsValid(TypeObj) || TypeObj->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				Entry.ParseError = TEXT("Struct Type is invalid/destroying");
				Entry.bLoaded = false;
				return false;
			}

			if (const UStruct* StructDef = Cast<UStruct>(TypeObj))
			{
				TSet<const UStruct*> Visited;
				if (!IsStructSafeRecursive(StructDef, Visited))
				{
					Entry.ParseError = TEXT("Struct hierarchy contains unstable types (REINST). Compile/Refresh.");
					Entry.bLoaded = false;
					return false;
				}
			}
		}

		if (const UStruct* StructDefinition = Cast<UStruct>(Entry.ParsedMeta.TypeObject.Get()))
		{
			if (const FProperty* RealProp = StructDefinition->FindPropertyByName(*Entry.ParsedMeta.PropertyName))
			{
				FString RealType = RealProp->GetCPPType();
				if (RealType == TEXT("uint8"))
				{
					RealType = TEXT("byte");
				}
				const bool bTypeMismatch = !Entry.ParsedMeta.TypeName.Equals(RealType, ESearchCase::IgnoreCase) && !Entry.ParsedMeta.TypeName.Contains(RealType);
				if (bTypeMismatch)
				{
					Entry.ParseError = FString::Printf(TEXT("Type Mismatch: Disk=%s != Struct=%s"), *Entry.ParsedMeta.TypeName, *RealType);
					FTurboStructLiteDatabaseParser::SetLastParseError(Entry.ParseError);
					if (!Entry.DataObject.IsValid())
					{
						Entry.DataObject = TStrongObjectPtr<UTurboStructLiteSaveDataObject>(NewObject<UTurboStructLiteSaveDataObject>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional));
					}
					Entry.DataObject->ClearFlags(RF_Transactional);
					Entry.DataObject->Payload.Reset();
					Entry.DataObject->bParsed = false;
					Entry.DataObject->ParseError = Entry.ParseError;
					Entry.bLoaded = true;
					return false;
				}
			}
		}

		FInstancedPropertyBag Bag;
		if (FTurboStructLiteDatabaseParser::BuildBag(Entry.ParsedMeta, Bag))
		{
			bool bLayoutChanged = false;
			if (Entry.ParsedMeta.ValueType == EPropertyBagPropertyType::Struct && Entry.ParsedMeta.FieldNames.Num() > 0)
			{
				if (const UStruct* Struct = Cast<UStruct>(Entry.ParsedMeta.TypeObject.Get()))
				{
					TArray<FString> CurrentFields;
					FTurboStructLiteDatabaseParser::GetStructFieldNames(Struct, CurrentFields, 0);
					CurrentFields.Sort();
					TArray<FString> SavedFields = Entry.ParsedMeta.FieldNames;
					SavedFields.Sort();
					if (CurrentFields != SavedFields)
					{
						Entry.ParsedMeta.FieldNames = CurrentFields;
						Entry.MetaString = FTurboStructLiteDatabaseParser::BuildMetadataString(Entry.ParsedMeta);
						Entry.DataBag = Bag;
						Entry.ParseError.Reset();
						FTurboStructLiteDatabaseParser::SetLastParseError(TEXT(""));
						bLayoutChanged = true;
					}
				}
			}

			FString DeserializeError;
			if (FTurboStructLiteDatabaseParser::DeserializeIntoBag(Entry.ParsedMeta, Bag, Bytes, DeserializeError))
			{
				Entry.DataBag = Bag;
				if (!bLayoutChanged)
				{
					Entry.ParseError.Reset();
					FTurboStructLiteDatabaseParser::SetLastParseError(TEXT(""));
				}
			}
			else
			{
				Entry.ParseError = DeserializeError.IsEmpty() ? TEXT("Deserialize failed") : DeserializeError;
				FTurboStructLiteDatabaseParser::SetLastParseError(Entry.ParseError);
				Entry.DataBag = Bag;
			}
		}
		else
		{
			Entry.ParseError = TEXT("Cannot build UI for this type");
			FTurboStructLiteDatabaseParser::SetLastParseError(Entry.ParseError);
		}
	}

	const int64 LargeThreshold = 64 * 1024 * 1024;
	if (!bAllowLargeLoads && Bytes.Num() > LargeThreshold)
	{
		if (!Entry.DataObject.IsValid())
		{
			Entry.DataObject = TStrongObjectPtr<UTurboStructLiteSaveDataObject>(NewObject<UTurboStructLiteSaveDataObject>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional));
		}
		Entry.DataObject->ClearFlags(RF_Transactional);
		Entry.DataObject->SlotName = Entry.SlotName;
		Entry.DataObject->SubSlotIndex = Entry.Info.SubSlotIndex;
		Entry.DataObject->SourceFile = Entry.FilePath;
		Entry.DataObject->FileTimestamp = Entry.Timestamp;
		Entry.DataObject->DataSize = Entry.Info.DataSizeBytes;
		Entry.DataObject->UncompressedSize = Entry.Info.UncompressedSizeBytes;
		Entry.DataObject->Compression = FTurboStructLiteDatabaseParser::DescribeCompression(Entry.Info.Compression);
		Entry.DataObject->Encryption = FTurboStructLiteDatabaseParser::DescribeEncryption(Entry.Info.Encryption);
		Entry.DataObject->DebugMetadata = FTurboStructLiteDatabaseParser::FormatMetadataForDisplay(Entry.MetaString, Entry.ParsedMeta);
		Entry.DataObject->bParsed = false;
		Entry.DataObject->ParseError = FString::Printf(TEXT("Payload is %.2f MB. Enable 'Allow loading payloads > 64 MB' to view/edit."), Bytes.Num() / 1024.f / 1024.f);
		Entry.DataObject->Payload.Reset();
		Entry.bLoaded = false;
		return true;
	}

	Entry.DataObject = TStrongObjectPtr<UTurboStructLiteSaveDataObject>(NewObject<UTurboStructLiteSaveDataObject>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional));
	Entry.DataObject->ClearFlags(RF_Transactional);
	Entry.DataObject->SlotName = Entry.SlotName;
	Entry.DataObject->SubSlotIndex = Entry.Info.SubSlotIndex;
	Entry.DataObject->SourceFile = Entry.FilePath;
	Entry.DataObject->FileTimestamp = Entry.Timestamp;
	Entry.DataObject->DataSize = Entry.Info.DataSizeBytes;
	Entry.DataObject->UncompressedSize = Entry.Info.UncompressedSizeBytes;
	Entry.DataObject->Compression = FTurboStructLiteDatabaseParser::DescribeCompression(Entry.Info.Compression);
	Entry.DataObject->Encryption = FTurboStructLiteDatabaseParser::DescribeEncryption(Entry.Info.Encryption);
	Entry.DataObject->DebugMetadata = FTurboStructLiteDatabaseParser::FormatMetadataForDisplay(Entry.MetaString, Entry.ParsedMeta);
	Entry.DataObject->bParsed = Entry.ParseError.IsEmpty();
	Entry.DataObject->ParseError = Entry.ParseError.IsEmpty() ? FTurboStructLiteDatabaseParser::GetLastParseError() : Entry.ParseError;
	Entry.DataObject->Payload = Entry.DataBag;
	Entry.bLoaded = true;
	return Entry.bLoaded;
}

bool STurboStructLiteDatabaseWidget::ReloadEntry(FSaveEntry& Entry)
{
	Entry.bLoaded = false;
	Entry.DataObject.Reset();
	Entry.ParseError.Reset();
	const bool bOk = LoadEntryData(Entry);
	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();
	}
	UpdateTabLabel();
	return bOk;
}

void STurboStructLiteDatabaseWidget::SaveAll()
{
}

void STurboStructLiteDatabaseWidget::RefreshSelected()
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	if (Entries.Num() == 0)
	{
		RefreshData();
		return;
	}
	for (const TSharedPtr<FSaveEntry>& Entry : Entries)
	{
		if (Entry.IsValid())
		{
			ReloadEntry(*Entry);
		}
	}
	UpdateInspectorForSelection();
}

void STurboStructLiteDatabaseWidget::UpdateInspectorForSelection()
{
	if (InspectorSwitcher.IsValid())
	{
		InspectorSwitcher->SetActiveWidgetIndex(0);
	}

	TArray<UObject*> Objects;
	for (const TSharedPtr<FSaveEntry>& Entry : SelectedEntries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		if (Entry->DataObject.IsValid())
		{
			Objects.Add(Entry->DataObject.Get());
		}
	}

	if (Objects.Num() == 1)
	{
		if (BaseDetailsView.IsValid())
		{
			BaseDetailsView->SetObject(Objects[0]);
		}
		FadeSequence = FCurveSequence();
		FadeCurve = FadeSequence.AddCurve(0.f, 0.25f, ECurveEaseFunction::QuadInOut);
		FadeSequence.Play(this->AsShared());
	}
	else if (Objects.Num() > 1)
	{
		if (BaseDetailsView.IsValid())
		{
			BaseDetailsView->SetObjects(Objects);
		}
	}
	else
	{
		if (BaseDetailsView.IsValid())
		{
			BaseDetailsView->SetObject(nullptr);
		}
	}
}

void STurboStructLiteDatabaseWidget::ClearAllInspector()
{
	if (BaseDetailsView.IsValid())
	{
		BaseDetailsView->SetObject(nullptr);
	}
	if (InspectorSwitcher.IsValid())
	{
		InspectorSwitcher->SetActiveWidgetIndex(0);
	}
}

#undef LOCTEXT_NAMESPACE

#endif


