# TurboStructLite

TurboStructLite is the free/open-source edition of **TurboStruct**: an asynchronous persistence system for **Unreal Engine 5** designed to run the save/load pipeline off the Game Thread to avoid gameplay hitches, with crash-safe atomic commits (anti-corruption) and schema evolution support.

**Pro Edition (Fab/Marketplace):**
https://www.fab.com/es-es/listings/915884dc-1a43-4b85-94dc-6a707a6facdf

---

## Documentation
- **Docs (overview & guides):** https://coda.io/d/TurboStruct-Documentation_doc8ElEMPPQ/Welcome-to-TurboStruct_suriCYLx#_luSuM9LA
- **API / Blueprint reference:** https://coda.io/d/TurboStruct-Documentation_doc8ElEMPPQ/Blueprint-API-Reference_suseOFJ_
- **Changelog / Releases:** https://github.com/alejocapo04/TurboStructLite/releases

> If you don’t have docs yet, a simple GitHub Wiki or `/docs` folder is enough to start.

---

## Features (Lite)
- Asynchronous save/load pipeline (off Game Thread)
- Crash-safe atomic commits (anti-corruption)
- Schema evolution support for backward-compatible loads
- Blueprint-friendly API + C++ implementation

---

## Installation
### Option A — As a project plugin
1. Download/clone this repository.
2. Copy it into:
   `YourProject/Plugins/TurboStructLite/`
3. Regenerate project files (if using C++) and compile.
4. Enable the plugin in **Edit → Plugins** (if required) and restart the editor.

### Option B — As a submodule (recommended for teams)
```bash
git submodule add https://github.com/alejocapo04/TurboStructLite.git Plugins/TurboStructLite
git submodule update --init --recursive