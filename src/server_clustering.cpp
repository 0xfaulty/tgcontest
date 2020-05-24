#include "server_clustering.h"

#include "util.h"

TServerClustering::TServerClustering(std::unique_ptr<TClusterer> clusterer, rocksdb::DB* db)
    : Clusterer(std::move(clusterer))
    , Db(db)
{
}

namespace {

    std::pair<std::vector<TDbDocument>, uint64_t> ReadDocs(rocksdb::DB* db) {
        rocksdb::ManagedSnapshot snapshot(db);

        rocksdb::ReadOptions ropt(/*cksum*/ true, /*cache*/ true);
        ropt.snapshot = snapshot.snapshot();

        std::vector<TDbDocument> docs;
        uint64_t timestamp = 0;

        std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(ropt));
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
            const std::string value = iter->value().ToString(); // TODO: use string_view
            if (value.empty()) {
                continue;
            }

            TDbDocument doc;
            const bool succes = TDbDocument::FromProtoString(value, &doc);
            if (!succes) {
                LOG_DEBUG("Bad document in db: " << iter->key().ToString());
                continue;
            }

            timestamp = std::max(timestamp, doc.PubTime);
            docs.push_back(std::move(doc));
        }

        return std::make_pair(std::move(docs), timestamp);
    }

    void RemoveStaleDocs(rocksdb::DB* db, std::vector<TDbDocument>& docs, uint64_t timestamp) {
        rocksdb::WriteOptions wopt;
        for (const auto& doc : docs) {
            if (doc.IsStale(timestamp)) {
                db->Delete(wopt, doc.FileName);
            }
        }
        docs.erase(std::remove_if(docs.begin(), docs.end(), [timestamp] (const auto& doc) { return doc.IsStale(timestamp); }), docs.end());
    }

}

TClusterIndex TServerClustering::MakeIndex() const {
    auto [docs, timestamp] = ReadDocs(Db);
    RemoveStaleDocs(Db, docs, timestamp);

    TClusterIndex index = Clusterer->Cluster(docs); // TODO: move?

    for (const auto& [lang, clusters] : index.Clusters) {
        LOG_DEBUG("Clustering output: " << ToString(lang) << " " << clusters.size() << " clusters");
    }

    return index;
};
