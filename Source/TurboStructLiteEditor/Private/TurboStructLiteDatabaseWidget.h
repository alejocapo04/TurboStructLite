/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Description (EN):
 * Main widget for the TurboStructLite editor. Provides a read-only viewer for save slots.
 */

#if !defined(TURBOSTRUCT_HAS_EDITOR_UI)
#define TURBOSTRUCT_HAS_EDITOR_UI 0
#endif

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "TurboStructLiteTypes.h"
#include "TurboStructLiteEditorTypes.h"
#include "TurboStructLiteDatabaseParser.h"
#include "Delegates/Delegate.h"
#include "Logging/LogMacros.h"
#include "Widgets/Notifications/SNotificationList.h"

#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#define TURBOSTRUCT_USE_STRUCTUTILS_DELEGATES 1
#else
#define TURBOSTRUCT_USE_STRUCTUTILS_DELEGATES 0
#endif

DECLARE_LOG_CATEGORY_EXTERN(LogTurboStructLiteDebug, Log, All);

class IDetailsView;
class SDockTab;
class UUserDefinedStruct;
class UStruct;
class UClass;
class UWorld;
class FPreviewScene;
class SWidgetSwitcher;

class STurboStructLiteDatabaseWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(STurboStructLiteDatabaseWidget) {}
		SLATE_ARGUMENT(TWeakPtr<SDockTab>, OwningTab)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~STurboStructLiteDatabaseWidget();

	static const FName SlotsTabId;
	static const FName InspectorTabId;

private:
	// === Tab and panel creation ===
	TSharedRef<SDockTab> SpawnSlotsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnInspectorTab(const FSpawnTabArgs& Args);
	TSharedRef<SWidget> BuildSlotsPanel();
	TSharedRef<SWidget> BuildInspectorPanel();

	// === Button and key handlers ===
	FReply OnRefreshClicked();
	FReply OnReloadClicked();
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	// === Tree and details callbacks ===
	void HandleSearchTextChanged(const FText& InFilter);
	void HandleGetChildren(TSharedPtr<FSaveTreeNode> InParent, TArray<TSharedPtr<FSaveTreeNode>>& OutChildren) const;
	TSharedRef<ITableRow> GenerateTreeRow(TSharedPtr<FSaveTreeNode> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnTreeSelectionChanged(TSharedPtr<FSaveTreeNode> InItem, ESelectInfo::Type SelectInfo);
	void HandlePropertyChanged(const FPropertyChangedEvent& ChangeEvent);
	void OnPreObjectPropertyChanged(UObject* Object, const FEditPropertyChain& PropertyChain);

	// === Data loading and refresh ===
	void RefreshData();
	bool LoadEntryData(FSaveEntry& Entry);
	bool ReloadEntry(FSaveEntry& Entry);
	void SaveAll();
	void RefreshSelected();

	// === Tree maintenance and filtering ===
	void RemoveTreeNode(const FSaveEntry& Entry);
	void FilterTree();
	void ReselectTreeSelection();
	void UpdateTabLabel();

	// === UI state helpers ===
	FText GetSelectionSummary() const;
	FText GetSelectionSubSummary() const;
	FText GetDirtyLabel() const;
	EVisibility GetDirtyVisibility() const;
	FSlateColor GetDetailsBackgroundColor() const;
	EVisibility GetFadeVisibility() const;
	bool HasDirtyEntries() const;
	bool HasSelection() const;
	bool IsSubSlotSelected() const;
	TSharedPtr<FSaveEntry> GetSelectedEntry() const;
	void GetSelectedEntries(TArray<TSharedPtr<FSaveEntry>>& OutEntries) const;
	void GetSelectedSlotNodes(TArray<TSharedPtr<FSaveTreeNode>>& OutSlotNodes) const;
	FString BuildRowMeta(const FSaveEntry& Entry) const;
	bool EnsureEncryptionKey(FSaveEntry& Entry);
	FString BuildSlotKey(const FString& SlotName, ESaveEntryKind Kind, int32 UserIndex) const;
	void UpdateInspectorForSelection();
	void ClearAllInspector();
	// Category: Editor UI.
	FString GetSaveDirectory() const;
	// Category: Editor UI.
	FString FormatLocalTimestamp(const FDateTime& Timestamp) const;
	// Category: Editor UI.
	void ShowToast(const FText& Message, SNotificationItem::ECompletionState State) const;

	// === Struct and reinstance handling ===
	void HandleStructReinstanced(const UUserDefinedStruct& Struct);
	void ClearEntryForStructChange(FSaveEntry& Entry, const FString& Reason);
	bool IsStructSafeRecursive(const UStruct* InStruct, TSet<const UStruct*>& Visited) const;

	// === Context menus ===
	TSharedPtr<SWidget> HandleTreeContextMenu();
	TSharedPtr<SWidget> BuildSlotContextMenu(const FSaveTreeNode& SlotNode);
	TSharedPtr<SWidget> BuildSubSlotContextMenu(const FSaveTreeNode& SubSlotNode);

	// === Shutdown handling ===
	void HandleEditorClose();

	// === Safety flags ===
	bool bAllowLargeLoads = false;

	// === UI references ===
	TSharedPtr<FTabManager> TabManager;
	TSharedPtr<STreeView<TSharedPtr<FSaveTreeNode>>> TreeView;
	TSharedPtr<IDetailsView> BaseDetailsView;
	TSharedPtr<SBorder> BaseDetailsBorder;
	TSharedPtr<SWidgetSwitcher> InspectorSwitcher;
	TWeakPtr<SDockTab> OwningTab;
	mutable FCurveSequence FadeSequence;
	mutable FCurveHandle FadeCurve;

	// === Tree data and selection ===
	TArray<TSharedPtr<FSaveTreeNode>> RootNodes;
	TArray<TSharedPtr<FSaveTreeNode>> FilteredRootNodes;
	TMap<FString, TSharedPtr<FSaveTreeNode>> AllSlotNodes;
	TSharedPtr<FSaveTreeNode> SelectedNode;
	TArray<TSharedPtr<FSaveTreeNode>> SelectedNodes;
	TArray<TSharedPtr<FSaveEntry>> SelectedEntries;
	FString SearchText;

	// === State and helpers ===
	FDelegateHandle StructReinstancedHandle;
	FDelegateHandle ObjectsReplacedHandle;
	bool bShutdownPromptShown = false;
};

#endif
