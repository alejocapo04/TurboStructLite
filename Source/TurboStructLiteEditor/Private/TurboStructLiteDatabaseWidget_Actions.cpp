#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLiteDatabaseParser.h"
#include "Animation/CurveSequence.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"
#include "Widgets/Views/STableRow.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"

FReply STurboStructLiteDatabaseWidget::OnRefreshClicked()
{
	RefreshData();
	ShowToast(LOCTEXT("Refreshed", "Saves reloaded."), SNotificationItem::CS_Success);
	return FReply::Handled();
}

FReply STurboStructLiteDatabaseWidget::OnReloadClicked()
{
	if (TSharedPtr<FSaveEntry> Entry = GetSelectedEntry())
	{
		if (ReloadEntry(*Entry))
		{
			ShowToast(LOCTEXT("ReloadOkTurbo", "Subslot reloaded from disk."), SNotificationItem::CS_Success);
			UpdateInspectorForSelection();
		}
		else
		{
			ShowToast(LOCTEXT("ReloadFailTurbo", "Could not reload subslot."), SNotificationItem::CS_Fail);
		}
	}

return FReply::Handled();
}

void STurboStructLiteDatabaseWidget::HandleSearchTextChanged(const FText& InFilter)
{
	SearchText = InFilter.ToString();
	FilterTree();
}

void STurboStructLiteDatabaseWidget::HandleGetChildren(TSharedPtr<FSaveTreeNode> InParent, TArray<TSharedPtr<FSaveTreeNode>>& OutChildren) const
{
	if (InParent.IsValid())
	{
		OutChildren.Append(InParent->Children);
	}
}

TSharedRef<ITableRow> STurboStructLiteDatabaseWidget::GenerateTreeRow(TSharedPtr<FSaveTreeNode> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const bool bIsSlot = Item->Type == FSaveTreeNode::ENodeType::Slot;
	const bool bIsSubSlotGroup = Item->Type == FSaveTreeNode::ENodeType::SubSlotGroup;
	const FSlateBrush* Icon = (bIsSlot || bIsSubSlotGroup)
		? FAppStyle::Get().GetBrush("ContentBrowser.AssetTreeFolderClosed")
		: FAppStyle::Get().GetBrush("ContentBrowser.ColumnViewAssetIcon");
	if (Icon == nullptr)
	{
		Icon = FAppStyle::Get().GetBrush("ClassIcon.GenericAsset");
	}

	FString Label = bIsSlot
		? Item->SlotName
		: FString::Printf(TEXT("SubSlot %d"), Item->SubSlotIndex);

	FString Meta;
	if (!bIsSlot && !bIsSubSlotGroup && Item->Entry.IsValid())
	{
		Meta = BuildRowMeta(*Item->Entry);
		if (Item->Entry->bDirty)
		{
			Label += TEXT("  *");
		}
	}

	const float IconSize = 16.f;

	if (bIsSlot || bIsSubSlotGroup)
	{
		return SNew(STableRow<TSharedPtr<FSaveTreeNode>>, OwnerTable)
		.Padding(FMargin(4.f, 2.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(SBox)
				.WidthOverride(IconSize)
				.HeightOverride(IconSize)
				[
					SNew(SImage)
					.Image(Icon)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
			]
		];
	}

	return SNew(STableRow<TSharedPtr<FSaveTreeNode>>, OwnerTable)
	.Padding(FMargin(4.f, 2.f))
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(SBox)
				.WidthOverride(IconSize)
				.HeightOverride(IconSize)
				[
					SNew(SImage)
					.Image(Icon)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Meta))
			.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
			.AutoWrapText(true)
		]
	];
}

void STurboStructLiteDatabaseWidget::OnTreeSelectionChanged(TSharedPtr<FSaveTreeNode> InItem, ESelectInfo::Type SelectInfo)
{
	SelectedNode = InItem;
	SelectedNodes.Reset();
	SelectedEntries.Reset();
	if (TreeView.IsValid())
	{
		SelectedNodes = TreeView->GetSelectedItems();
	}

	TArray<TSharedPtr<FSaveEntry>> LocalEntries;
	for (const TSharedPtr<FSaveTreeNode>& Node : SelectedNodes)
	{
		if (Node.IsValid() && Node->Type == FSaveTreeNode::ENodeType::SubSlot && Node->Entry.IsValid())
		{
			LocalEntries.Add(Node->Entry);
		}
	}
	SelectedEntries = LocalEntries;

	if (LocalEntries.Num() == 0)
	{
		ClearAllInspector();
		return;
	}

	for (const TSharedPtr<FSaveEntry>& Entry : LocalEntries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}
		Entry->bKeyPromptCancelled = false;
		LoadEntryData(*Entry);
	}
	UpdateInspectorForSelection();
}

FReply STurboStructLiteDatabaseWidget::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void STurboStructLiteDatabaseWidget::HandlePropertyChanged(const FPropertyChangedEvent& ChangeEvent)
{
	return;
}

#undef LOCTEXT_NAMESPACE

#endif



