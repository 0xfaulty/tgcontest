#include "dbscan.h"

#include <mlpack/methods/dbscan/dbscan.hpp>

Dbscan::Dbscan(
    FastTextEmbedder& embedder
    , double epsilon
    , size_t minPoints
)
    : Clustering(embedder)
    , Epsilon(epsilon)
    , MinPoints(minPoints)
{
}

Dbscan::Clusters Dbscan::Cluster(
    const std::vector<Document>& docs
) {
    const size_t docSize = docs.size();
    const size_t embSize = Embedder.GetEmbeddingSize();
    arma::mat data(embSize, docSize);
    for (size_t i = 0; i < docSize; ++i) {
        fasttext::Vector embedding = Embedder.GetSentenceEmbedding(docs[i]);

        arma::fcolvec fvec(embedding.data(), embSize, /*copy_aux_mem*/ false, /*strict*/ true);
        data.col(i) = arma::normalise(arma::conv_to<arma::colvec>::from(fvec));
    }
    mlpack::dbscan::DBSCAN<> clustering(Epsilon, MinPoints);

    arma::Row<size_t> assignments;
    const size_t clustersSize = docSize > 0 ? clustering.Cluster(data, assignments) : 0;
    Dbscan::Clusters clusters(clustersSize);
    for (size_t i = 0; i < docSize; ++i) {
        const size_t clusterId = assignments[i];
        if (clusterId == SIZE_MAX) { // outlier
            clusters.push_back({std::cref(docs[i])});
            continue;
        }
        clusters[clusterId].push_back(std::cref(docs[i]));
    }

    return clusters;
}
