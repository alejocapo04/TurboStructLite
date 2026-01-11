/* Copyright (C) 2025 Alejo ALDREY - All Rights Reserved
 * This content is downloadable from Epic Games Fab.
 */

#pragma once

/*
 * Descripcion (ES):
 * Header del modulo TurboStructLiteQuery. Maneja el ciclo de vida del motor de queries.
 *
 * Description (EN):
 * TurboStructLiteQuery module header. Handles lifecycle for the query engine.
 */

#include "Modules/ModuleManager.h"

class FTurboStructLiteQueryModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
