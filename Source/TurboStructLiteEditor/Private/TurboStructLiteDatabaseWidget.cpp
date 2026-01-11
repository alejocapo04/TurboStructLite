#include "TurboStructLiteDatabaseWidget.h"

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI

#include "TurboStructLiteBPLibrary.h"
#include "TurboStructLiteEditorTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "Animation/CurveSequence.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STreeView.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "InputCoreTypes.h"
#if TURBOSTRUCT_USE_STRUCTUTILS_DELEGATES
#include "StructUtilsDelegates.h"
#endif
#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Logging/LogMacros.h"

#define LOCTEXT_NAMESPACE "TurboStructLiteDatabaseWidget"
DEFINE_LOG_CATEGORY(LogTurboStructLiteDebug);

FString STurboStructLiteDatabaseWidget::GetSaveDirectory() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"), TEXT("TurboStructLite"));
}

FString STurboStructLiteDatabaseWidget::FormatLocalTimestamp(const FDateTime& Timestamp) const
{
	const FTimespan Bias = FDateTime::UtcNow() - FDateTime::Now();
	return (Timestamp - Bias).ToString(TEXT("%d/%m/%Y %H:%M:%S"));
}

void STurboStructLiteDatabaseWidget::ShowToast(const FText& Message, SNotificationItem::ECompletionState State) const
{
	FNotificationInfo Info(Message);
	Info.bUseThrobber = false;
	Info.FadeOutDuration = 0.25f;
	Info.ExpireDuration = 2.5f;
	Info.bFireAndForget = true;
	if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Notification->SetCompletionState(State);
	}
}

const FName STurboStructLiteDatabaseWidget::SlotsTabId(TEXT("TurboStructLiteDatabase.Slots"));
const FName STurboStructLiteDatabaseWidget::InspectorTabId(TEXT("TurboStructLiteDatabase.Inspector"));

void STurboStructLiteDatabaseWidget::Construct(const FArguments& InArgs)
{
	OwningTab = InArgs._OwningTab;

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs BaseDetailsArgs;
	BaseDetailsArgs.bHideSelectionTip = true;
	BaseDetailsArgs.bAllowSearch = true;
	BaseDetailsArgs.bShowOptions = false;
	BaseDetailsArgs.bShowScrollBar = true;
	BaseDetailsArgs.bAllowMultipleTopLevelObjects = true;
	BaseDetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	BaseDetailsView = PropertyModule.CreateDetailView(BaseDetailsArgs);
	BaseDetailsView->SetIsPropertyEditingEnabledDelegate(FIsPropertyEditingEnabled::CreateLambda([]() { return false; }));
	BaseDetailsView->OnFinishedChangingProperties().AddSP(this, &STurboStructLiteDatabaseWidget::HandlePropertyChanged);

	if (OwningTab.IsValid())
	{
		TabManager = FGlobalTabmanager::Get()->NewTabManager(OwningTab.Pin().ToSharedRef());
	}
	else
	{
		TabManager = FGlobalTabmanager::Get();
	}

	if (TabManager.IsValid() && !TabManager->HasTabSpawner(SlotsTabId))
	{
		TabManager->RegisterTabSpawner(SlotsTabId, FOnSpawnTab::CreateSP(this, &STurboStructLiteDatabaseWidget::SpawnSlotsTab))
			.SetDisplayName(LOCTEXT("SlotsTabTitle", "Slots"))
			.SetTooltipText(LOCTEXT("SlotsTabTooltip", "Browse TurboStructLite slots and subslots."))
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderTree"));
	}

	if (TabManager.IsValid() && !TabManager->HasTabSpawner(InspectorTabId))
	{
		TabManager->RegisterTabSpawner(InspectorTabId, FOnSpawnTab::CreateSP(this, &STurboStructLiteDatabaseWidget::SpawnInspectorTab))
			.SetDisplayName(LOCTEXT("InspectorTabTitle", "Inspector"))
			.SetTooltipText(LOCTEXT("InspectorTabTooltip", "Inspect the selected subslot."))
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
	}

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("TurboStructLiteDatabase_Layout_v1")
	->AddArea
	(
		FTabManager::NewPrimaryArea()
		->SetOrientation(Orient_Horizontal)
		->Split
		(
			FTabManager::NewStack()
			->SetSizeCoefficient(0.35f)
			->AddTab(SlotsTabId, ETabState::OpenedTab)
			->SetHideTabWell(false)
		)
		->Split
		(
			FTabManager::NewStack()
			->SetSizeCoefficient(0.65f)
			->AddTab(InspectorTabId, ETabState::OpenedTab)
			->SetHideTabWell(false)
		)
	);

	TSharedPtr<SWindow> ParentWindow;
	if (OwningTab.IsValid())
	{
		ParentWindow = OwningTab.Pin()->GetParentWindow();
	}
	else
	{
		ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
	}
	TSharedPtr<SWidget> Restored = TabManager.IsValid() ? TabManager->RestoreFrom(Layout, ParentWindow) : nullptr;
	if (!Restored.IsValid())
	{
		Restored = SNullWidget::NullWidget;
	}

	ChildSlot[Restored.ToSharedRef()];

#if TURBOSTRUCT_USE_STRUCTUTILS_DELEGATES
	if (!StructReinstancedHandle.IsValid())
	{
		StructReinstancedHandle = UE::StructUtils::Delegates::OnUserDefinedStructReinstanced.AddSP(this, &STurboStructLiteDatabaseWidget::HandleStructReinstanced);
	}
#endif

	FCoreUObjectDelegates::OnPreObjectPropertyChanged.AddSP(this, &STurboStructLiteDatabaseWidget::OnPreObjectPropertyChanged);

	RefreshData();
	UpdateTabLabel();

	FCoreDelegates::OnEnginePreExit.AddSP(this, &STurboStructLiteDatabaseWidget::HandleEditorClose);
}

void STurboStructLiteDatabaseWidget::HandleEditorClose()
{
	if (!HasDirtyEntries())
	{
		return;
	}

	bShutdownPromptShown = true;

	const EAppReturnType::Type Response = FMessageDialog::Open(
		EAppMsgType::YesNo,
		LOCTEXT("UnsavedPromptTurboExit", "TurboStructLite has unsaved changes. Save before exiting Editor?"));

	if (Response == EAppReturnType::Yes)
	{
		SaveAll();
	}
}

STurboStructLiteDatabaseWidget::~STurboStructLiteDatabaseWidget()
{
	FCoreUObjectDelegates::OnPreObjectPropertyChanged.RemoveAll(this);

	FCoreDelegates::OnEnginePreExit.RemoveAll(this);

	if (HasDirtyEntries() && !IsEngineExitRequested() && !bShutdownPromptShown)
	{
		const EAppReturnType::Type Response = FMessageDialog::Open(
			EAppMsgType::YesNo,
			LOCTEXT("UnsavedPromptTurbo", "There are unsaved TurboStructLite subslots. Save before closing?"));
		if (Response == EAppReturnType::Yes)
		{
			SaveAll();
		}
	}

	ClearAllInspector();

	SelectedNode.Reset();
	SelectedNodes.Empty();
	SelectedEntries.Empty();

	RootNodes.Empty();
	FilteredRootNodes.Empty();
	AllSlotNodes.Empty();

	if (TreeView.IsValid())
	{
		TreeView->ClearSelection();
		TreeView->SetTreeItemsSource(nullptr);
	}

	if (TabManager.IsValid())
	{
		if (TabManager->HasTabSpawner(SlotsTabId))
		{
			TabManager->UnregisterTabSpawner(SlotsTabId);
		}
		if (TabManager->HasTabSpawner(InspectorTabId))
		{
			TabManager->UnregisterTabSpawner(InspectorTabId);
		}
	}
#if TURBOSTRUCT_USE_STRUCTUTILS_DELEGATES
	if (StructReinstancedHandle.IsValid())
	{
		UE::StructUtils::Delegates::OnUserDefinedStructReinstanced.Remove(StructReinstancedHandle);
		StructReinstancedHandle.Reset();
	}
#endif

}

#undef LOCTEXT_NAMESPACE

#endif


