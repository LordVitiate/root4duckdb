#ifndef ROOT4DUCKDB_CORE_ROOT_HEADERS_HPP
#define ROOT4DUCKDB_CORE_ROOT_HEADERS_HPP

// Parse ROOT headers before removing BIT, which conflicts with DuckDB identifiers.
#include "Rtypes.h"
#include "TArrayD.h"
#include "TAxis.h"
#include "TH1.h"
#include "TProfile.h"
#include "TProfile2D.h"
#include "TProfile3D.h"
#include "TObject.h"
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

#endif
