#pragma once

// ROOT exposes BIT as a process-wide preprocessor macro while DuckDB has
// identifiers named BIT. Every ROOT header used by the extension is parsed
// inside this boundary before the macro is removed for DuckDB headers.
#include "Rtypes.h"
#include "TBuffer.h"
#include "TBufferFile.h"
#include "TBasket.h"
#include "TBranch.h"
#include "TBranchElement.h"
#include "TClass.h"
#include "TFile.h"
#include "TKey.h"
#include "TLeaf.h"
#include "TROOT.h"
#include "TStreamerElement.h"
#include "TStreamerInfo.h"
#include "TStreamerInfoActions.h"
#include "TString.h"
#include "TSystem.h"
#include "TTree.h"
#include "TTreeCache.h"
#include "TVirtualCollectionProxy.h"

#ifdef BIT
#undef BIT
#endif
