#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"

TSharedPtr<SWidget> STurboStructLiteDatabaseWidget::HandleTreeContextMenu()
{
	if (!SelectedNode.IsValid())
	{
		return nullptr;
	}

	if (SelectedNode->Type == FSaveTreeNode::ENodeType::Slot)
	{
		return BuildSlotContextMenu(*SelectedNode);
	}
	if (SelectedNode->Type == FSaveTreeNode::ENodeType::SubSlotGroup)
	{
		return BuildSubSlotContextMenu(*SelectedNode);
	}
	return BuildSubSlotContextMenu(*SelectedNode);
}

TSharedPtr<SWidget> STurboStructLiteDatabaseWidget::BuildSlotContextMenu(const FSaveTreeNode& SlotNode)
{
	FMenuBuilder MenuBuilder(true, nullptr);
	(void)SlotNode;

	MenuBuilder.AddMenuEntry(
		LOCTEXT("RefreshSlots", "Refresh Slots"),
		LOCTEXT("RefreshSlotsTT", "Reload slot list from disk."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &STurboStructLiteDatabaseWidget::RefreshData)));

	return MenuBuilder.MakeWidget();
}

TSharedPtr<SWidget> STurboStructLiteDatabaseWidget::BuildSubSlotContextMenu(const FSaveTreeNode& SubSlotNode)
{
	FMenuBuilder MenuBuilder(true, nullptr);
	(void)SubSlotNode;

	MenuBuilder.AddMenuEntry(
		LOCTEXT("ReloadSelection", "Reload Selection"),
		LOCTEXT("ReloadSelectionTT", "Reload selected subslots from disk."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &STurboStructLiteDatabaseWidget::RefreshSelected)));

	return MenuBuilder.MakeWidget();
}

bool STurboStructLiteDatabaseWidget::EnsureEncryptionKey(FSaveEntry& Entry)
{
	const ETurboStructLiteEncryption Encryption = Entry.Info.Encryption;
	if (Encryption != ETurboStructLiteEncryption::AES)
	{
		return true;
	}
	if (!Entry.EncryptionKey.IsEmpty())
	{
		return true;
	}

	TSharedPtr<FString> EnteredKey = MakeShared<FString>();
	TSharedRef<SEditableTextBox> TextBox = SNew(SEditableTextBox)
		.IsPassword(true)
		.OnTextChanged_Lambda([EnteredKey](const FText& NewText) { *EnteredKey = NewText.ToString(); });

	TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(LOCTEXT("EnterEncryptionKey", "Enter Encryption Key"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TWeakPtr<SWindow> WeakDialog = Dialog;

	Dialog->SetContent(
		SNew(SBorder)
		.Padding(8.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("EncryptionKeyPrompt", "This subslot is encrypted. Enter the AES key to view it."))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				TextBox
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 8.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("EncryptionOk", "Accept"))
					.OnClicked_Lambda([WeakDialog]() { if (TSharedPtr<SWindow> PinnedDialog = WeakDialog.Pin()) { PinnedDialog->RequestDestroyWindow(); } return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("EncryptionCancel", "Cancel"))
					.OnClicked_Lambda([WeakDialog, EnteredKey]() { *EnteredKey = FString(); if (TSharedPtr<SWindow> PinnedDialog = WeakDialog.Pin()) { PinnedDialog->RequestDestroyWindow(); } return FReply::Handled(); })
				]
			]
		]
	);

	TSharedPtr<SWindow> ParentWindow;
	if (OwningTab.IsValid())
	{
		ParentWindow = OwningTab.Pin()->GetParentWindow();
	}
	if (!ParentWindow.IsValid())
	{
		ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
	}
	FSlateApplication::Get().AddModalWindow(Dialog, ParentWindow);

	if (EnteredKey->IsEmpty())
	{
		Entry.bKeyPromptCancelled = true;
		return false;
	}

	Entry.EncryptionKey = *EnteredKey;
	return true;
}

#undef LOCTEXT_NAMESPACE

#endif


