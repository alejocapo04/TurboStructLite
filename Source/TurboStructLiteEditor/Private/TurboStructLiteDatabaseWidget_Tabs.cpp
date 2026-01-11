#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"

TSharedRef<SDockTab> STurboStructLiteDatabaseWidget::SpawnSlotsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		[
			BuildSlotsPanel()
		];
}

TSharedRef<SDockTab> STurboStructLiteDatabaseWidget::SpawnInspectorTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		[
			BuildInspectorPanel()
		];
}

TSharedRef<SWidget> STurboStructLiteDatabaseWidget::BuildSlotsPanel()
{
	return SNew(SBorder)
		.Padding(8.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("SearchSlotsHint", "Search slots / subslots..."))
				.OnTextChanged(this, &STurboStructLiteDatabaseWidget::HandleSearchTextChanged)
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			.Padding(0.f, 8.f, 0.f, 0.f)
			[
				SAssignNew(TreeView, STreeView<TSharedPtr<FSaveTreeNode>>)
				.TreeItemsSource(&FilteredRootNodes)
				.OnGetChildren(this, &STurboStructLiteDatabaseWidget::HandleGetChildren)
				.OnGenerateRow(this, &STurboStructLiteDatabaseWidget::GenerateTreeRow)
				.OnSelectionChanged(this, &STurboStructLiteDatabaseWidget::OnTreeSelectionChanged)
				.OnContextMenuOpening(this, &STurboStructLiteDatabaseWidget::HandleTreeContextMenu)
				.SelectionMode(ESelectionMode::Multi)
			]
		];
}

TSharedRef<SWidget> STurboStructLiteDatabaseWidget::BuildInspectorPanel()
{
	return SNew(SBorder)
		.Padding(8.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshButton", "Refresh"))
					.OnClicked(this, &STurboStructLiteDatabaseWidget::OnRefreshClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SButton)
					.IsEnabled(this, &STurboStructLiteDatabaseWidget::HasSelection)
					.Text(LOCTEXT("ReloadButton", "Reload subslot"))
					.OnClicked(this, &STurboStructLiteDatabaseWidget::OnReloadClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(12.f, 0.f, 0.f, 0.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &STurboStructLiteDatabaseWidget::GetDirtyLabel)
					.ColorAndOpacity(FSlateColor(FLinearColor(0.1f, 0.8f, 0.1f, 1.f)))
					.Visibility(this, &STurboStructLiteDatabaseWidget::GetDirtyVisibility)
					.AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 8.f, 0.f, 4.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(this, &STurboStructLiteDatabaseWidget::GetSelectionSummary)
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bAllowLargeLoads ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { bAllowLargeLoads = (NewState == ECheckBoxState::Checked); })
					.Content()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AllowLargeLoads", "Allow loading payloads > 64 MB (may use lots of RAM)"))
						.AutoWrapText(true)
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.f)
			[
				SAssignNew(InspectorSwitcher, SWidgetSwitcher)
				+ SWidgetSwitcher::Slot()
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(BaseDetailsBorder, SBorder)
						.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
						.BorderBackgroundColor(this, &STurboStructLiteDatabaseWidget::GetDetailsBackgroundColor)
						[
							BaseDetailsView.ToSharedRef()
						]
					]
					+ SOverlay::Slot()
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::Get().GetBrush("WhiteBrush"))
						.BorderBackgroundColor(this, &STurboStructLiteDatabaseWidget::GetDetailsBackgroundColor)
						.Visibility(this, &STurboStructLiteDatabaseWidget::GetFadeVisibility)
						.IsEnabled(false)
					]
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE

#endif


