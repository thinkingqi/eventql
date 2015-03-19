/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "IndexWriter.h"

using namespace fnord;

namespace cm {

RefPtr<IndexWriter> IndexWriter::openIndex(const String& index_path) {
  if (!FileUtil::exists(index_path) || !FileUtil::isDirectory(index_path)) {
    RAISEF(kIllegalArgumentError, "invalid index path: $0", index_path);
  }

  /* set up feature schema */
  FeatureSchema feature_schema;
  feature_schema.registerFeature("shop_id", 1, 1);
  feature_schema.registerFeature("category1", 2, 1);
  feature_schema.registerFeature("category2", 3, 1);
  feature_schema.registerFeature("category3", 4, 1);
  feature_schema.registerFeature("title~de", 5, 2);

  /* open mdb */
  auto db_path = FileUtil::joinPaths(index_path, "db");
  FileUtil::mkdir_p(db_path);
  auto db = mdb::MDB::open(db_path);
  db->setMaxSize(1000000 * 512000);

  /* open docs store */
  auto docs_path = FileUtil::joinPaths(index_path, "docs");
  FileUtil::mkdir_p(docs_path);
  RefPtr<DocStore> docs(new DocStore(docs_path));

  /* open lucene */
  auto fts_path = FileUtil::joinPaths(index_path, "fts");
  auto fts =
      fts::newLucene<fts::IndexWriter>(
          fts::FSDirectory::open(StringUtil::convertUTF8To16(fts_path)),
          fts::newLucene<fts::StandardAnalyzer>(
              fts::LuceneVersion::LUCENE_CURRENT),
          true,
          fts::IndexWriter::MaxFieldLengthLIMITED);

  return RefPtr<IndexWriter>(new IndexWriter(feature_schema, db, docs, fts));
}

IndexWriter::IndexWriter(
    FeatureSchema schema,
    RefPtr<mdb::MDB> db,
    RefPtr<DocStore> docs,
    std::shared_ptr<fts::IndexWriter> fts) :
    schema_(schema),
    db_(db),
    feature_idx_(new FeatureIndexWriter(&schema_)),
    docs_(docs),
    fts_(fts) {}


IndexWriter::~IndexWriter() {
  fts_->close();
}

void IndexWriter::updateDocument(const IndexRequest& index_request) {
  auto doc = docs_->updateDocument(index_request);
  updateDocumentFTS(doc);
}

void IndexWriter::commit() {
  fts_->commit();
}

void IndexWriter::updateDocumentFTS(RefPtr<Document> doc) {

  auto fts_doc = fts::newLucene<fts::Document>();
  fts_doc->add(
      fts::newLucene<fts::Field>(
          L"keywords",
          L"my fnordy document",
          fts::Field::STORE_NO,
          fts::Field::INDEX_ANALYZED));

  fts_->addDocument(fts_);
}

void IndexWriter::rebuildFTS() {
  fnord::iputs("rebuild fts...", 1);

  docs_->listDocuments([this] (const DocID& docid) -> bool {
    fnord::iputs("rebuild fts for $0", docid.docid);
    return true;
  });
}

RefPtr<mdb::MDB> IndexWriter::featureDB() {
  return db_;
}

} // namespace cm
