/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Header del modulo de editor de TurboStruct. Declara el modulo que registra pestañas, estilos
 * Slate y la UI de base de datos de guardados en el editor.
 *
 * Description (EN):
 * Header for the TurboStruct editor module. Declares the module that registers tabs, Slate styles,
 * and the save database UI inside the editor.
 */

#include "Modules/ModuleManager.h"

class FTurboStructLiteEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static const FName DatabaseTabName;

private:
	TSharedRef<class SDockTab> SpawnDatabaseTab(const class FSpawnTabArgs& Args);
	void InitializeStyle();
	void ShutdownStyle();

private:
	static TSharedPtr<class FSlateStyleSet> StyleSet;
};
