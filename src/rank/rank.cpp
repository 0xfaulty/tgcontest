#include <set>
#include "rank.h"
#include "../util.h"
#include "../clustering/in_cluster_ranging.h"

std::string ComputeClusterCategory(const NewsCluster& cluster) {
    std::unordered_map<std::string, size_t> categoryCount;
    for (const auto& doc : cluster) {
        std::string docCategory = doc.get().Category;
        if (categoryCount.find(docCategory) == categoryCount.end()) {
            categoryCount[docCategory] = 0;
        }
        categoryCount[docCategory] += 1;
    }
    std::vector<std::pair<std::string, size_t>> categoryCountVector(categoryCount.begin(), categoryCount.end());
    std::sort(categoryCountVector.begin(), categoryCountVector.end(), [](std::pair<std::string, size_t> a, std::pair<std::string, size_t> b) {
        return a.second > b.second;
    });

    return categoryCountVector[0].first;
}

double ComputeClusterWeight(const NewsCluster& cluster, const std::unordered_map<std::string, double>& agencyRating) {
    double output = 0.0;
    std::set<std::string> seenHosts;
    
    for (const auto& doc : cluster) {
        if (seenHosts.insert(GetHost(doc.get().Url)).second) {
            output += ComputeDocWeight(doc, agencyRating);
        }
    }
    return output;
}


std::unordered_map<std::string, std::vector<WeightedNewsCluster>> Rank(
    const std::vector<NewsCluster>& clusters,
    const std::unordered_map<std::string, double>& agencyRating
) {
    std::vector<std::string> categoryList = {"any", "society", "economy", "technology", "sports", "entartainment", "science", "other"};
    std::unordered_map<std::string, std::vector<WeightedNewsCluster>> output;
    std::vector<WeightedNewsCluster> weightedClusters;

    for (const auto& cluster : clusters) {
        const std::string clusterCategory = ComputeClusterCategory(cluster);
        const double weight = ComputeClusterWeight(cluster, agencyRating);
        const std::string title = cluster[0].get().Title;
        weightedClusters.emplace_back(cluster, clusterCategory, title, weight);
    }
    
    std::sort(weightedClusters.begin(), weightedClusters.end(), [](const WeightedNewsCluster a, const WeightedNewsCluster b) {
        return a.Weight > b.Weight;
    });

    for (const auto& category : categoryList) {
        std::vector<WeightedNewsCluster> categoryWeightedClusters;
        std::copy_if(
            weightedClusters.cbegin(),
            weightedClusters.cend(),
            std::back_inserter(categoryWeightedClusters),
            [&category](const WeightedNewsCluster a) {
                return ((a.Category == category) || (category == "any")) ? true : false;
            }
        ); 
        output[category] = std::move(categoryWeightedClusters);
    }

    return output;
}
