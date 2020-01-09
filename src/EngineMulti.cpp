/* Copyright 2020 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#pragma warning(disable : 4611) // interaction between '_setjmp' and C++ object destruction is non-portable

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"
#include "utils/Log.h"

#include "AppColors.h"
#include "wingui/TreeModel.h"
#include "EngineBase.h"
#include "EngineFzUtil.h"
#include "EngineManager.h"
#include "ParseBKM.h"
#include "EngineMulti.h"

struct EnginePage {
    int pageNoInEngine = 0;
    EngineBase* engine = nullptr;
};

Kind kindEngineMulti = "enginePdfMulti";

class EngineMultiImpl : public EngineBase {
  public:
    EngineMultiImpl();
    virtual ~EngineMultiImpl();
    EngineBase* Clone() override;

    RectD PageMediabox(int pageNo) override;
    RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override;

    RenderedBitmap* RenderPage(RenderPageArgs& args) override;

    RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    std::string_view GetFileData() override;
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override;
    bool SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots = false);
    WCHAR* ExtractPageText(int pageNo, RectI** coordsOut = nullptr) override;

    bool HasClipOptimizations(int pageNo) override;
    WCHAR* GetProperty(DocumentProperty prop) override;

    void UpdateUserAnnotations(Vec<PageAnnotation>* list) override;

    bool BenchLoadPage(int pageNo) override;

    Vec<PageElement*>* GetElements(int pageNo) override;
    PageElement* GetElementAtPos(int pageNo, PointD pt) override;
    RenderedBitmap* GetImageForPageElement(PageElement*) override;

    PageDestination* GetNamedDest(const WCHAR* name) override;
    TocTree* GetToc() override;

    WCHAR* GetPageLabel(int pageNo) const override;
    int GetPageByLabel(const WCHAR* label) const override;

    bool Load(const WCHAR* fileName, PasswordUI* pwdUI);

    static EngineBase* CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI);

    EngineBase* PageToEngine(int& pageNo) const;
    VbkmFile vbkm;
    Vec<EnginePage> pageToEngine;

    TocTree* tocTree = nullptr;
};

EngineBase* EngineMultiImpl::PageToEngine(int& pageNo) const {
    EnginePage& ep = pageToEngine[pageNo - 1];
    pageNo = ep.pageNoInEngine;
    return ep.engine;
}

EngineMultiImpl::EngineMultiImpl() {
    kind = kindEngineMulti;
    defaultFileExt = L".vbkm";
    fileDPI = 72.0f;
    supportsAnnotations = false;
    supportsAnnotationsForSaving = false;
}

EngineMultiImpl::~EngineMultiImpl() {
    delete tocTree;
}

EngineBase* EngineMultiImpl::Clone() {
    CrashIf(true);
    return nullptr;
}

RectD EngineMultiImpl::PageMediabox(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->PageMediabox(pageNo);
}

RectD EngineMultiImpl::PageContentBox(int pageNo, RenderTarget target) {
    EngineBase* e = PageToEngine(pageNo);
    return e->PageContentBox(pageNo, target);
}

RenderedBitmap* EngineMultiImpl::RenderPage(RenderPageArgs& args) {
    EngineBase* e = PageToEngine(args.pageNo);
    return e->RenderPage(args);
}

RectD EngineMultiImpl::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse) {
    EngineBase* e = PageToEngine(pageNo);
    return e->Transform(rect, pageNo, zoom, rotation, inverse);
}

std::string_view EngineMultiImpl::GetFileData() {
    return {};
}

bool EngineMultiImpl::SaveFileAs(const char* copyFileName, bool includeUserAnnots) {
    return false;
}

bool EngineMultiImpl::SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots) {
    return false;
}

WCHAR* EngineMultiImpl::ExtractPageText(int pageNo, RectI** coordsOut) {
    EngineBase* e = PageToEngine(pageNo);
    return e->ExtractPageText(pageNo, coordsOut);
}

bool EngineMultiImpl::HasClipOptimizations(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->HasClipOptimizations(pageNo);
}

WCHAR* EngineMultiImpl::GetProperty(DocumentProperty prop) {
    return nullptr;
}

void EngineMultiImpl::UpdateUserAnnotations(Vec<PageAnnotation>* list) {
    // TODO: support user annotations
}

bool EngineMultiImpl::BenchLoadPage(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->BenchLoadPage(pageNo);
}

Vec<PageElement*>* EngineMultiImpl::GetElements(int pageNo) {
    EngineBase* e = PageToEngine(pageNo);
    return e->GetElements(pageNo);
}

PageElement* EngineMultiImpl::GetElementAtPos(int pageNo, PointD pt) {
    EngineBase* e = PageToEngine(pageNo);
    return e->GetElementAtPos(pageNo, pt);
}

RenderedBitmap* EngineMultiImpl::GetImageForPageElement(PageElement* pel) {
    EngineBase* e = PageToEngine(pel->pageNo);
    return e->GetImageForPageElement(pel);
}

PageDestination* EngineMultiImpl::GetNamedDest(const WCHAR* name) {
    int n = 0;
    for (auto&& f : vbkm.vbkms) {
        auto e = f->engine;
        if (!e) {
            continue;
        }
        auto dest = e->GetNamedDest(name);
        if (dest) {
            // TODO: add n to page number in returned destination
            return dest;
        }
        n += e->PageCount();
    }
    return nullptr;
}

static void updateTocItemsPageNo(TocItem* i, int nPageNoAdd) {
    if (nPageNoAdd == 0) {
        return;
    }
    if (!i) {
        return;
    }
    auto curr = i;
    while (curr) {
        if (curr->dest) {
            curr->dest->pageNo += nPageNoAdd;
        }
        updateTocItemsPageNo(curr->child, nPageNoAdd);
        curr->pageNo += nPageNoAdd;
        curr = curr->next;
    }
}

TocTree* EngineMultiImpl::GetToc() {
    CrashIf(!tocTree);
    return tocTree;
}

WCHAR* EngineMultiImpl::GetPageLabel(int pageNo) const {
    if (pageNo < 1 || pageNo >= pageCount) {
        return nullptr;
    }

    EngineBase* e = PageToEngine(pageNo);
    return e->GetPageLabel(pageNo);
}

int EngineMultiImpl::GetPageByLabel(const WCHAR* label) const {
    int n = 0;
    for (auto&& f : vbkm.vbkms) {
        auto e = f->engine;
        if (!e) {
            continue;
        }
        auto pageNo = e->GetPageByLabel(label);
        if (pageNo != -1) {
            return n + pageNo;
        }
        n += e->PageCount();
    }
    return -1;
}

static void CollectTocItemsRecur(TocItem* ti, Vec<TocItem*>& v) {
    while (ti) {
        v.push_back(ti);
        CollectTocItemsRecur(ti->child, v);
        ti = ti->next;
    }
}

static bool cmpByPageNo(TocItem* ti1, TocItem* ti2) {
    return ti1->pageNo < ti2->pageNo;
}

void CalcEndPageNo(TocItem* root, int nPages) {
    Vec<TocItem*> tocItems;
    CollectTocItemsRecur(root, tocItems);
    size_t n = tocItems.size();
    if (n < 1) {
        return;
    }
    std::sort(tocItems.begin(), tocItems.end(), cmpByPageNo);
    TocItem* prev = tocItems[0];
    for (size_t i = 1; i < n; i++) {
        TocItem* next = tocItems[i];
        prev->endPageNo = next->pageNo - 1;
        if (prev->endPageNo < prev->pageNo) {
            prev->endPageNo = prev->pageNo;
        }
        prev = next;
    }
    prev->endPageNo = nPages;
}

static void MarkAsInvisibleRecur(TocItem* ti, bool markInvisible, Vec<bool>& visible) {
    while (ti) {
        if (markInvisible) {
            for (int i = ti->pageNo; i < ti->endPageNo; i++) {
                visible[i - 1] = false;
            }
        }
        bool childMarkInvisible = markInvisible;
        if (!childMarkInvisible) {
            childMarkInvisible = ti->isUnchecked;
        }
        MarkAsInvisibleRecur(ti->child, childMarkInvisible, visible);
        ti = ti->next;
    }
}

static void MarkAsVisibleRecur(TocItem* ti, bool markVisible, Vec<bool>& visible) {
    if (!markVisible) {
        return;
    }
    while (ti) {
        for (int i = ti->pageNo; i < ti->endPageNo; i++) {
            visible[i - 1] = true;
        }
        MarkAsInvisibleRecur(ti->child, ti->isUnchecked, visible);
        ti = ti->next;
    }
}

static void CalcRemovedPages(TocItem* root, Vec<bool>& visible) {
    int nPages = (int)visible.size();
    CalcEndPageNo(root, nPages);
    // in the first pass we mark the pages under unchecked nodes as invisible
    MarkAsInvisibleRecur(root, root->isUnchecked, visible);

    // in the second pass we mark back pages that are visible
    // from nodes that are not unchecked
    MarkAsVisibleRecur(root, !root->isUnchecked, visible);
}

static void removeUncheckedRecur(TocItem* ti) {
    while (ti) {
        if (ti->child && ti->child->isUnchecked) {
            ti->child = nullptr;
        } else {
            removeUncheckedRecur(ti->child);
        }
        ti = ti->next;
    }
}

bool EngineMultiImpl::Load(const WCHAR* fileName, PasswordUI* pwdUI) {
    AutoFree filePath = strconv::WstrToUtf8(fileName);
    bool ok = LoadVbkmFile(filePath.get(), vbkm);

    // create a TocTree combining all the files and hiding nodes that are unchecked
    // create a mapping between "virtual page" (from combined documents) to
    // a page in a given engine
    int nOpened = 0;
    int nTotalPages = 0;
    TocItem* tocCombinedRoot = nullptr;

    for (auto&& vbkm : vbkm.vbkms) {
        CrashIf(vbkm->filePath.empty());
        AutoFreeWstr path = strconv::Utf8ToWstr(vbkm->filePath.as_view());
        vbkm->engine = EngineManager::CreateEngine(path, pwdUI);
        if (!vbkm->engine) {
            return false;
        }
        AutoDelete<TocTree> tree = CloneTocTree(vbkm->toc, false);
        EngineBase* engine = vbkm->engine;
        if (!vbkm->engine) {
            return false;
        }
        int nPages = vbkm->engine->PageCount();

        Vec<bool> visiblePages;
        for (int i = 0; i < nPages; i++) {
            visiblePages.Append(true);
        }
        CalcRemovedPages(tree->root, visiblePages);

        int nPage = 0;
        for (int i = 0; i < nPages; i++) {
            if (!visiblePages[i]) {
                continue;
            }
            EnginePage ep{i + 1, engine};
            pageToEngine.push_back(ep);
            nPage++;
        }
        nOpened++;
        updateTocItemsPageNo(tree->root, nTotalPages);
        removeUncheckedRecur(tree->root);
        nTotalPages += nPage;
        TocItem* root = CloneTocItemRecur(tree->root, true);
        if (root != nullptr) {
            if (tocCombinedRoot) {
                tocCombinedRoot->AddSibling(root);
            } else {
                tocCombinedRoot = root;
            }
        }
    }
    if (nOpened == 0) {
        delete tocCombinedRoot;
        return false;
    }
    tocTree = new TocTree(tocCombinedRoot);
    pageCount = nTotalPages;
    return true;
}

EngineBase* EngineMultiImpl::CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    if (str::IsEmpty(fileName)) {
        return nullptr;
    }
    EngineMultiImpl* engine = new EngineMultiImpl();
    if (!engine->Load(fileName, pwdUI)) {
        delete engine;
        return nullptr;
    }
    engine->fileName = str::Dup(fileName);
    return engine;
}

bool IsEngineMultiSupportedFile(const WCHAR* fileName, bool sniff) {
    if (sniff) {
        // we don't support sniffing
        return false;
    }
    return str::EndsWithI(fileName, L".vbkm");
}

EngineBase* CreateEngineMultiFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    return EngineMultiImpl::CreateFromFile(fileName, pwdUI);
}