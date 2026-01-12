#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "Framework/Docking/TabManager.h"
#include "Widgets/Views/STreeView.h"

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"

void STurboStructLiteDatabaseWidget::RemoveTreeNode(const FSaveEntry& Entry)
{
	for (int32 i = RootNodes.Num() - 1; i >= 0; --i)
	{
		TSharedPtr<FSaveTreeNode> SlotNode = RootNodes[i];
		if (!SlotNode.IsValid() || SlotNode->SlotName != Entry.SlotName || SlotNode->Kind != Entry.Kind || SlotNode->UserIndex != Entry.UserIndex)
		{
			continue;
		}

		SlotNode->Children.RemoveAll([SubSlotIndex = Entry.Info.SubSlotIndex](const TSharedPtr<FSaveTreeNode>& Node)
		{
			return Node.IsValid() && Node->SubSlotIndex == SubSlotIndex;
		});

		if (SlotNode->Children.Num() == 0)
		{
			RootNodes.RemoveAt(i);
			AllSlotNodes.Remove(BuildSlotKey(Entry.SlotName, Entry.Kind, Entry.UserIndex));
		}
		break;
	}

	FilterTree();
}

void STurboStructLiteDatabaseWidget::FilterTree()
{
	if (SearchText.IsEmpty())
	{
		FilteredRootNodes = RootNodes;
	}
	else
	{
		const FString FilterLower = SearchText.ToLower();
		FilteredRootNodes.Reset();

		for (const TSharedPtr<FSaveTreeNode>& Slot : RootNodes)
		{
			if (!Slot.IsValid())
			{
				continue;
			}

			TSharedPtr<FSaveTreeNode> FilteredSlot = MakeShared<FSaveTreeNode>(*Slot);
			FilteredSlot->Children.Reset();

			for (const TSharedPtr<FSaveTreeNode>& Child : Slot->Children)
			{
				if (!Child.IsValid())
				{
					continue;
				}

				const FString Label = FString::Printf(TEXT("%s %d"), *Child->SlotName, Child->SubSlotIndex).ToLower();
				const FString Meta = Child->Entry.IsValid() ? BuildRowMeta(*Child->Entry).ToLower() : TEXT("");
				if (Label.Contains(FilterLower) || Meta.Contains(FilterLower))
				{
					FilteredSlot->Children.Add(Child);
				}
			}

			if (FilteredSlot->Children.Num() > 0 || Slot->SlotName.ToLower().Contains(FilterLower))
			{
				FilteredRootNodes.Add(FilteredSlot);
			}
		}
	}

	if (TreeView.IsValid())
	{
		TreeView->SetTreeItemsSource(&FilteredRootNodes);
		TreeView->RequestTreeRefresh();
	}
}

void STurboStructLiteDatabaseWidget::UpdateTabLabel()
{
	const bool bDirty = HasDirtyEntries();
	if (TSharedPtr<SDockTab> TabPtr = OwningTab.Pin())
	{
		const FText BaseLabel = LOCTEXT("TabTitleTurbo", "TurboStruct Database");
		TabPtr->SetLabel(bDirty ? FText::FromString(BaseLabel.ToString() + TEXT(" *")) : BaseLabel);
	}
}

FText STurboStructLiteDatabaseWidget::GetSelectionSummary() const
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	if (Entries.Num() == 0)
	{
		return LOCTEXT("NoSelection", "Select a subslot to inspect.");
	}
	if (Entries.Num() == 1)
	{
		const FSaveEntry& Entry = *Entries[0];
		const FString Summary = FString::Printf(TEXT("%s | SubSlot %d | Size %d bytes"),
			*Entry.SlotName,
			Entry.Info.SubSlotIndex,
			Entry.Info.UncompressedSizeBytes);
		return FText::FromString(Summary);
	}

return FText::Format(LOCTEXT("MultiSelectionSummary", "{0} subslots selected."), FText::AsNumber(Entries.Num()));
}

FText STurboStructLiteDatabaseWidget::GetSelectionSubSummary() const
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	if (Entries.Num() != 1)
	{
		return FText::GetEmpty();
	}

return FText::FromString(BuildRowMeta(*Entries[0]));
}

FText STurboStructLiteDatabaseWidget::GetDirtyLabel() const
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	bool bAnyDirty = false;
	for (const TSharedPtr<FSaveEntry>& Entry : Entries)
	{
		if (Entry.IsValid() && Entry->bDirty)
		{
			bAnyDirty = true;
			break;
		}
	}
	return bAnyDirty ? LOCTEXT("DirtyLabelTurbo", "Unsaved changes pending.") : FText::GetEmpty();
}

EVisibility STurboStructLiteDatabaseWidget::GetDirtyVisibility() const
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	for (const TSharedPtr<FSaveEntry>& Entry : Entries)
	{
		if (Entry.IsValid() && Entry->bDirty)
		{
			return EVisibility::Visible;
		}
	}

return EVisibility::Collapsed;
}

FSlateColor STurboStructLiteDatabaseWidget::GetDetailsBackgroundColor() const
{
	const float Alpha = FadeSequence.IsPlaying() ? (1.f - FadeCurve.GetLerp()) * 0.4f : 0.f;
	return FSlateColor(FLinearColor(1.f, 1.f, 1.f, Alpha));
}

EVisibility STurboStructLiteDatabaseWidget::GetFadeVisibility() const
{
	const bool bVisible = FadeSequence.IsPlaying() && FadeCurve.GetLerp() < 1.f;
	return bVisible ? EVisibility::Visible : EVisibility::Collapsed;
}

bool STurboStructLiteDatabaseWidget::HasDirtyEntries() const
{
	for (const TSharedPtr<FSaveTreeNode>& Slot : RootNodes)
	{
		if (!Slot.IsValid())
		{
			continue;
		}
		for (const TSharedPtr<FSaveTreeNode>& Child : Slot->Children)
		{
			if (Child.IsValid() && Child->Entry.IsValid() && Child->Entry->bDirty)
			{
				return true;
			}
		}
	}
	return false;
}

bool STurboStructLiteDatabaseWidget::HasSelection() const
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	return Entries.Num() > 0;
}

bool STurboStructLiteDatabaseWidget::IsSubSlotSelected() const
{
	TArray<TSharedPtr<FSaveEntry>> Entries;
	GetSelectedEntries(Entries);
	return Entries.Num() > 0;
}

TSharedPtr<FSaveEntry> STurboStructLiteDatabaseWidget::GetSelectedEntry() const
{
	if (SelectedNode.IsValid() && SelectedNode->Entry.IsValid())
	{
		return SelectedNode->Entry;
	}
	return nullptr;
}

void STurboStructLiteDatabaseWidget::GetSelectedEntries(TArray<TSharedPtr<FSaveEntry>>& OutEntries) const
{
	OutEntries.Reset();
	OutEntries.Append(SelectedEntries);
}

void STurboStructLiteDatabaseWidget::GetSelectedSlotNodes(TArray<TSharedPtr<FSaveTreeNode>>& OutSlotNodes) const
{
	OutSlotNodes.Reset();
	const TArray<TSharedPtr<FSaveTreeNode>>& Items = SelectedNodes.Num() > 0 ? SelectedNodes : (TreeView.IsValid() ? TreeView->GetSelectedItems() : TArray<TSharedPtr<FSaveTreeNode>>());
	for (const TSharedPtr<FSaveTreeNode>& Item : Items)
	{
		if (Item.IsValid() && Item->Type == FSaveTreeNode::ENodeType::Slot)
		{
			OutSlotNodes.Add(Item);
		}
	}
}

void STurboStructLiteDatabaseWidget::ReselectTreeSelection()
{
	if (!TreeView.IsValid())
	{
		return;
	}
	TArray<TSharedPtr<FSaveTreeNode>> ToSelect = SelectedNodes;
	TreeView->ClearSelection();
	for (const TSharedPtr<FSaveTreeNode>& Node : ToSelect)
	{
		if (Node.IsValid())
		{
			TreeView->SetItemSelection(Node, true, ESelectInfo::Direct);
		}
	}
}

FString STurboStructLiteDatabaseWidget::BuildRowMeta(const FSaveEntry& Entry) const
{
	const FString TimestampString = FormatLocalTimestamp(Entry.Timestamp);
	return FString::Printf(TEXT("Saved: %s | Data: %d bytes"), *TimestampString, Entry.Info.DataSizeBytes);
}

#undef LOCTEXT_NAMESPACE

#endif



