/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/dom/Attr.h"
#include "mozilla/dom/Comment.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/DocumentType.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/HTMLStyleElement.h"
#include "nsCOMPtr.h"
#include "nsGkAtoms.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsRange.h"
#include "nsString.h"
#include "nsTextNode.h"

namespace mozilla::dom {

static already_AddRefed<Document> CreateHTMLDocForTextContentUtf8Test() {
  nsCOMPtr<nsIURI> uri;
  NS_NewURI(getter_AddRefs(uri), "data:text/html,");

  RefPtr<BasePrincipal> principal =
      BasePrincipal::CreateContentPrincipal(uri, OriginAttributes());
  MOZ_RELEASE_ASSERT(principal);

  nsCOMPtr<Document> doc;
  MOZ_ALWAYS_SUCCEEDS(NS_NewDOMDocument(getter_AddRefs(doc),
                                       u""_ns,   // aNamespaceURI
                                       u""_ns,   // aQualifiedName
                                       nullptr,  // aDoctype
                                       uri, uri, principal,
                                       LoadedAsData::No,  // aLoadedAsData
                                       nullptr,           // aEventObject
                                       DocumentFlavor::HTML));
  MOZ_RELEASE_ASSERT(doc);
  return doc.forget();
}

// Thin MOZ_CAN_RUN_SCRIPT_BOUNDARY wrappers so TEST() bodies (which the
// can-run-script static analysis can't annotate) can call the
// MOZ_CAN_RUN_SCRIPT textContent/value setters. Taking the node by value
// keeps it alive via a genuine stack RefPtr for the duration of the call.
MOZ_CAN_RUN_SCRIPT_BOUNDARY
static void SetTextContentForTest(RefPtr<nsINode> aNode,
                                  const nsACString& aValue, ErrorResult& aRv) {
  aNode->SetTextContent(aValue, nullptr, aRv);
}

MOZ_CAN_RUN_SCRIPT_BOUNDARY
static void SetTextContentForTest(RefPtr<nsINode> aNode,
                                  const nsAString& aValue, ErrorResult& aRv) {
  aNode->SetTextContent(aValue, aRv);
}

MOZ_CAN_RUN_SCRIPT_BOUNDARY
static void SetAttrValueForTest(RefPtr<Attr> aAttr, const nsAString& aValue,
                                ErrorResult& aRv) {
  aAttr->SetValue(aValue, nullptr, aRv);
}

// Asserts that GetTextContent(nsACString&) agrees with
// NS_ConvertUTF16toUTF8(GetTextContent(nsAString&)), including voidness -
// the same invariant nsINode::AssertTextContentUtf8Matches enforces in
// DEBUG builds, checked here so opt builds get coverage too.
static void CheckGetTextContentUtf8Matches(nsINode* aNode,
                                           const char* aLabel) {
  IgnoredErrorResult rv;
  nsAutoString utf16;
  aNode->GetTextContent(utf16, rv);
  ASSERT_FALSE(rv.Failed()) << aLabel;

  nsAutoCString utf8;
  aNode->GetTextContent(utf8, rv);
  ASSERT_FALSE(rv.Failed()) << aLabel;

  if (utf16.IsVoid()) {
    EXPECT_TRUE(utf8.IsVoid()) << aLabel;
    return;
  }
  ASSERT_FALSE(utf8.IsVoid()) << aLabel;
  EXPECT_TRUE(NS_ConvertUTF16toUTF8(utf16).Equals(utf8)) << aLabel;
}

TEST(TextContentUtf8Test, GetTextContentMatchesAcrossNodeTypes)
{
  RefPtr<Document> doc = CreateHTMLDocForTextContentUtf8Test();

  RefPtr<Element> div = doc->CreateHTMLElement(nsGkAtoms::div);
  RefPtr<Element> span = doc->CreateHTMLElement(nsGkAtoms::span);
  IgnoredErrorResult rv;
  div->AppendChild(*span, rv);
  ASSERT_FALSE(rv.Failed());
  RefPtr<nsTextNode> text1 = doc->CreateTextNode(u"hello "_ns);
  RefPtr<nsTextNode> text2 = doc->CreateTextNode(u"café 日本"_ns);
  span->AppendChild(*text1, rv);
  ASSERT_FALSE(rv.Failed());
  div->AppendChild(*text2, rv);
  ASSERT_FALSE(rv.Failed());
  CheckGetTextContentUtf8Matches(div, "nested element");

  RefPtr<Element> empty = doc->CreateHTMLElement(nsGkAtoms::div);
  CheckGetTextContentUtf8Matches(empty, "empty element");

  RefPtr<nsTextNode> lone = doc->CreateTextNode(u"lone \U0001F600 text"_ns);
  CheckGetTextContentUtf8Matches(lone, "text node");

  RefPtr<Comment> comment = doc->CreateComment(u"a comment é"_ns);
  CheckGetTextContentUtf8Matches(comment, "comment node");

  RefPtr<DocumentType> doctype = NS_NewDOMDocumentType(
      doc->NodeInfoManager(), nsGkAtoms::html, u""_ns, u""_ns, u""_ns);
  CheckGetTextContentUtf8Matches(doctype, "doctype node");

  RefPtr<Attr> attr = doc->CreateAttribute(u"data-x"_ns, rv);
  ASSERT_FALSE(rv.Failed());
  SetAttrValueForTest(attr, u"attr é value"_ns, rv);
  ASSERT_FALSE(rv.Failed());
  CheckGetTextContentUtf8Matches(attr, "attr node");

  CheckGetTextContentUtf8Matches(doc, "document node");

  RefPtr<Element> styleElem = doc->CreateHTMLElement(nsGkAtoms::style);
  RefPtr<nsTextNode> styleText = doc->CreateTextNode(u"body { color: red }"_ns);
  styleElem->AppendChild(*styleText, rv);
  ASSERT_FALSE(rv.Failed());
  CheckGetTextContentUtf8Matches(styleElem, "style element");
}

TEST(TextContentUtf8Test, GetTextContentNullTextContentIsVoid)
{
  RefPtr<Document> doc = CreateHTMLDocForTextContentUtf8Test();

  RefPtr<DocumentType> doctype = NS_NewDOMDocumentType(
      doc->NodeInfoManager(), nsGkAtoms::html, u""_ns, u""_ns, u""_ns);
  IgnoredErrorResult rv;
  nsAutoCString utf8;
  doctype->GetTextContent(utf8, rv);
  ASSERT_FALSE(rv.Failed());
  EXPECT_TRUE(utf8.IsVoid());

  nsAutoCString docUtf8;
  doc->GetTextContent(docUtf8, rv);
  ASSERT_FALSE(rv.Failed());
  EXPECT_TRUE(docUtf8.IsVoid());
}

struct SetTextContentCase {
  const char* mUtf8;
  const char16_t* mUtf16;
  const char* mLabel;
};

TEST(TextContentUtf8Test, SetTextContentMatchesUtf16)
{
  RefPtr<Document> doc = CreateHTMLDocForTextContentUtf8Test();

  for (const auto& testCase : {
           SetTextContentCase{"ascii", u"ascii", "ascii"},
           SetTextContentCase{"caf\xC3\xA9", u"café", "latin1"},
           SetTextContentCase{"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", u"日本語",
                              "cjk (2b, different length)"},
           SetTextContentCase{"\xF0\x9F\x98\x80", u"\U0001F600",
                              "astral (surrogate pair)"},
           SetTextContentCase{"", u"", "empty"},
       }) {
    IgnoredErrorResult rv;

    RefPtr<Element> viaUtf8 = doc->CreateHTMLElement(nsGkAtoms::div);
    SetTextContentForTest(viaUtf8, nsDependentCString(testCase.mUtf8), rv);
    ASSERT_FALSE(rv.Failed()) << testCase.mLabel;

    RefPtr<Element> viaUtf16 = doc->CreateHTMLElement(nsGkAtoms::div);
    SetTextContentForTest(viaUtf16, nsDependentString(testCase.mUtf16), rv);
    ASSERT_FALSE(rv.Failed()) << testCase.mLabel;

    nsAutoString utf8Result, utf16Result;
    viaUtf8->GetTextContent(utf8Result, rv);
    viaUtf16->GetTextContent(utf16Result, rv);
    EXPECT_TRUE(utf8Result.Equals(utf16Result)) << testCase.mLabel;
  }
}

TEST(TextContentUtf8Test, SetTextContentReusesExistingTextChild)
{
  // The tryReuse path: setting non-empty content on a node that already has
  // a single reusable text child.
  RefPtr<Document> doc = CreateHTMLDocForTextContentUtf8Test();
  IgnoredErrorResult rv;
  RefPtr<Element> reuseTarget = doc->CreateHTMLElement(nsGkAtoms::div);
  RefPtr<nsTextNode> initial = doc->CreateTextNode(u"initial"_ns);
  nsIContent* initialRaw = initial.get();
  reuseTarget->AppendChild(*initial, rv);
  ASSERT_FALSE(rv.Failed());
  // The reuse path requires the text node be owned only by the DOM/frame
  // trees; drop our extra ref so that holds.
  initial = nullptr;

  SetTextContentForTest(reuseTarget, "replaced \xC3\xA9"_ns, rv);
  ASSERT_FALSE(rv.Failed());

  nsAutoCString result;
  reuseTarget->GetTextContent(result, rv);
  EXPECT_TRUE(result.Equals("replaced \xC3\xA9"_ns));
  EXPECT_EQ(reuseTarget->GetFirstChild(), initialRaw)
      << "should have reused the existing text child";
}

TEST(TextContentUtf8Test, SetDataUtf8UpdatesRangeOffsetsLikeUtf16)
{
  // The UTF-8 setter reports its replacement length in UTF-16 code units,
  // same as the UTF-16 setter (CharacterDataChangeInfo::mReplaceLength) - a
  // byte-count bug here would corrupt any live Range's offsets. Compare the
  // two paths directly rather than hard-coding an expected offset, since
  // that also exercises whatever clamping nsRange itself does on a
  // whole-buffer replacement, for content whose UTF-16 length (3) differs
  // from its UTF-8 byte length (9).
  RefPtr<Document> doc = CreateHTMLDocForTextContentUtf8Test();

  auto setAndGetRangeOffset = [&](auto aSetter) -> uint32_t {
    RefPtr<nsTextNode> text = doc->CreateTextNode(u"0123456789"_ns);
    RefPtr<Element> div = doc->CreateHTMLElement(nsGkAtoms::div);
    IgnoredErrorResult rv;
    div->AppendChild(*text, rv);
    EXPECT_FALSE(rv.Failed());
    RefPtr<nsRange> range = nsRange::Create(text, 7, text, 7, rv);
    EXPECT_FALSE(rv.Failed());
    aSetter(text.get(), rv);
    EXPECT_FALSE(rv.Failed());
    return range->StartOffset();
  };

  uint32_t viaUtf16 = setAndGetRangeOffset([](nsTextNode* aText, ErrorResult& aRv) {
    aText->SetData(nsLiteralString(u"日本語"), aRv);
  });
  uint32_t viaUtf8 = setAndGetRangeOffset([](nsTextNode* aText, ErrorResult& aRv) {
    aText->SetData("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"_ns, aRv);
  });

  EXPECT_EQ(viaUtf16, viaUtf8);
}

TEST(TextContentUtf8Test, SetTextContentNullIsNoOpOnDocumentType)
{
  RefPtr<Document> doc = CreateHTMLDocForTextContentUtf8Test();
  RefPtr<DocumentType> doctype = NS_NewDOMDocumentType(
      doc->NodeInfoManager(), nsGkAtoms::html, u""_ns, u""_ns, u""_ns);

  IgnoredErrorResult rv;
  SetTextContentForTest(doctype, "should be ignored"_ns, rv);
  EXPECT_FALSE(rv.Failed());

  nsAutoCString utf8;
  doctype->GetTextContent(utf8, rv);
  EXPECT_TRUE(utf8.IsVoid());
}

};  // namespace mozilla::dom
