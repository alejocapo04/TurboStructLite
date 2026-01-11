#include "TurboStructLiteEditorModule.h"
#include "Modules/ModuleManager.h"

#if !defined(TURBOSTRUCT_HAS_EDITOR_UI)
#define TURBOSTRUCT_HAS_EDITOR_UI 0
#endif

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI
#include "TurboStructLiteDatabaseWidget.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateBrush.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#endif

#define LOCTEXT_NAMESPACE "TurboStructLiteEditorModule"

const FName FTurboStructLiteEditorModule::DatabaseTabName(TEXT("TurboStructLiteDatabase"));
TSharedPtr<FSlateStyleSet> FTurboStructLiteEditorModule::StyleSet;

void FTurboStructLiteEditorModule::StartupModule()
{
#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI
	InitializeStyle();

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(DatabaseTabName, FOnSpawnTab::CreateRaw(this, &FTurboStructLiteEditorModule::SpawnDatabaseTab))
		.SetDisplayName(LOCTEXT("TurboStructLiteDatabaseTabTitle", "TurboStructLite Database"))
		.SetTooltipText(LOCTEXT("TurboStructLiteDatabaseTabTooltip", "Explore TurboStructLite saves."))
		.SetIcon(FSlateIcon(StyleSet->GetStyleSetName(), "TurboStructLiteEditor.SaveIcon"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
#endif
}

void FTurboStructLiteEditorModule::ShutdownModule()
{
#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DatabaseTabName);
	ShutdownStyle();
#endif
}

TSharedRef<SDockTab> FTurboStructLiteEditorModule::SpawnDatabaseTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> DockTab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab);

#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI
	DockTab->SetContent(
		SNew(STurboStructLiteDatabaseWidget)
		.OwningTab(DockTab)
	);
#endif

	return DockTab;
}

void FTurboStructLiteEditorModule::InitializeStyle()
{
#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI
	if (StyleSet.IsValid())
	{
		return;
	}

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TurboStructLite"));
	const FString ResourceDir = Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Resources")) : FPaths::ProjectContentDir();

	StyleSet = MakeShared<FSlateStyleSet>("TurboStructLiteEditorStyle");
	StyleSet->SetContentRoot(ResourceDir);
	StyleSet->SetCoreContentRoot(ResourceDir);
	StyleSet->Set("TurboStructLiteEditor.SaveIcon", new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("SaveIcon"), TEXT(".png")), FVector2D(40.f, 40.f)));
	StyleSet->Set("ClassIcon.TurboSaveManagerLite", new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("S_TurboSaveManager"), TEXT(".png")), FVector2D(16.f, 16.f)));
	StyleSet->Set("ClassThumbnail.TurboSaveManagerLite", new FSlateImageBrush(StyleSet->RootToContentDir(TEXT("S_TurboSaveManager"), TEXT(".png")), FVector2D(64.f, 64.f)));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
#endif
}

void FTurboStructLiteEditorModule::ShutdownStyle()
{
#if WITH_EDITOR && TURBOSTRUCT_HAS_EDITOR_UI
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
#endif
}

IMPLEMENT_MODULE(FTurboStructLiteEditorModule, TurboStructLiteEditor)

#undef LOCTEXT_NAMESPACE


