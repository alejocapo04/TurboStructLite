#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif

#if TURBOSTRUCT_USE_STRUCTUTILS_DELEGATES
#include "StructUtilsDelegates.h"
#endif

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"

void STurboStructLiteDatabaseWidget::ClearEntryForStructChange(FSaveEntry& Entry, const FString& Reason)
{
	Entry.ParsedMeta = FTurboStructLiteParsedProperty();
	FMemory::Memzero(&Entry.DataBag, sizeof(FInstancedPropertyBag));
	if (Entry.DataObject.IsValid())
	{
		if (Entry.DataObject->IsValidLowLevelFast())
		{
			FMemory::Memzero(&Entry.DataObject->Payload, sizeof(FInstancedPropertyBag));
			Entry.DataObject->bParsed = false;
			Entry.DataObject->ParseError = Reason;
		}
	}
	Entry.bLoaded = false;
	Entry.bDirty = false;
	Entry.ParseError = Reason;
}

void STurboStructLiteDatabaseWidget::OnPreObjectPropertyChanged(UObject* Object, const FEditPropertyChain& PropertyChain)
{
	auto IsUserDefinedStructObject = [](const UObject* InObject) -> bool
	{
		if (!InObject)
		{
			return false;
		}
		if (InObject->IsA<UUserDefinedStruct>())
		{
			return true;
		}
		return InObject->GetClass()->GetName() == TEXT("UserDefinedStructEditorData");
	};

	if (!IsUserDefinedStructObject(Object))
	{
		return;
	}

	bool bViewDisconnected = false;

	for (const TSharedPtr<FSaveTreeNode>& Slot : RootNodes)
	{
		if (!Slot.IsValid())
		{
			continue;
		}

		for (const TSharedPtr<FSaveTreeNode>& Child : Slot->Children)
		{
			if (!Child.IsValid() || !Child->Entry.IsValid())
			{
				continue;
			}

			FSaveEntry& Entry = *Child->Entry;
			if (Entry.bLoaded || Entry.DataObject.IsValid())
			{
				if (!bViewDisconnected && BaseDetailsView.IsValid())
				{
					ClearAllInspector();
					bViewDisconnected = true;
				}
				Entry.DataBag.Reset();
				if (Entry.DataObject.IsValid() && Entry.DataObject->IsValidLowLevelFast())
				{
					if (Entry.DataObject->Payload.IsValid())
					{
						Entry.DataObject->Payload.Reset();
					}
					Entry.DataObject->bParsed = false;
					Entry.DataObject->ClearFlags(RF_Transactional);
					Entry.DataObject->ParseError = TEXT("Waiting for recompile...");
				}
				Entry.bLoaded = false;
			}
		}
	}
}

void STurboStructLiteDatabaseWidget::HandleStructReinstanced(const UUserDefinedStruct& Struct)
{
	bool bShouldClose = false;

	for (const TSharedPtr<FSaveTreeNode>& Slot : RootNodes)
	{
		if (!Slot.IsValid())
		{
			continue;
		}

		for (const TSharedPtr<FSaveTreeNode>& Child : Slot->Children)
		{
			if (!Child.IsValid() || !Child->Entry.IsValid())
			{
				continue;
			}

			FSaveEntry& Entry = *Child->Entry;
			const UUserDefinedStruct* EntryStruct = Cast<UUserDefinedStruct>(Entry.ParsedMeta.TypeObject.Get());
			const bool bStructMatches = EntryStruct && (EntryStruct == &Struct || EntryStruct->PrimaryStruct == &Struct || Struct.PrimaryStruct == EntryStruct);
			const bool bNameMatches = Entry.ParsedMeta.TypeName.Equals(Struct.GetName(), ESearchCase::IgnoreCase);

			if (bStructMatches || bNameMatches)
			{
				bShouldClose = true;
				break;
			}
		}

		if (bShouldClose)
		{
			break;
		}
	}

	if (bShouldClose)
	{
		ClearAllInspector();

		SelectedNode.Reset();
		SelectedNodes.Reset();
		SelectedEntries.Reset();

		if (TSharedPtr<SDockTab> Tab = OwningTab.Pin())
		{
			Tab->RequestCloseTab();
		}
	}
}

#undef LOCTEXT_NAMESPACE

#endif


