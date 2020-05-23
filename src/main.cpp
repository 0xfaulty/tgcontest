#include "agency_rating.h"
#include "annotator.h"
#include "clustering/slink.h"
#include "document.h"
#include "rank.h"
#include "run_server.h"
#include "summarize.h"
#include "timer.h"
#include "util.h"

#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <fasttext.h>

namespace po = boost::program_options;

uint64_t GetIterTimestamp(const std::vector<TDbDocument>& documents, double percentile) {
    // In production ts.now() should be here.
    // In this case we have percentile of documents timestamps because of the small percent of wrong dates.
    if (documents.empty()) {
        return 0;
    }
    assert(std::is_sorted(documents.begin(), documents.end(), [](const TDbDocument& d1, const TDbDocument& d2) {
        return d1.FetchTime < d2.FetchTime;
    }));

    size_t index = std::floor(percentile * documents.size());
    return documents[index].FetchTime;
}


int main(int argc, char** argv) {
    std::cerr << "main" << std::endl;
    try {
        po::options_description desc("options");
        desc.add_options()
            ("mode", po::value<std::string>()->required(), "mode")
            ("input", po::value<std::string>()->required(), "input")
            ("server_config", po::value<std::string>()->default_value("configs/server.pbtxt"), "server_config")
            ("annotator_config", po::value<std::string>()->default_value("configs/annotator.pbtxt"), "annotator_config")
            ("clustering_type", po::value<std::string>()->default_value("slink"), "clustering_type")
            ("en_small_clustering_distance_threshold", po::value<float>()->default_value(0.015f), "en_clustering_distance_threshold")
            ("en_small_cluster_size", po::value<size_t>()->default_value(15), "en_small_cluster_size")
            ("en_medium_clustering_distance_threshold", po::value<float>()->default_value(0.01f), "en_medium_clustering_distance_threshold")
            ("en_medium_cluster_size", po::value<size_t>()->default_value(50), "en_medium_cluster_size")
            ("en_large_clustering_distance_threshold", po::value<float>()->default_value(0.005f), "en_large_clustering_distance_threshold")
            ("en_large_cluster_size", po::value<size_t>()->default_value(100), "en_large_cluster_size")
            ("ru_small_clustering_distance_threshold", po::value<float>()->default_value(0.015f), "ru_clustering_distance_threshold")
            ("ru_small_cluster_size", po::value<size_t>()->default_value(15), "ru_small_cluster_size")
            ("ru_medium_clustering_distance_threshold", po::value<float>()->default_value(0.01f), "ru_medium_clustering_distance_threshold")
            ("ru_medium_cluster_size", po::value<size_t>()->default_value(50), "ru_medium_cluster_size")
            ("ru_large_clustering_distance_threshold", po::value<float>()->default_value(0.005f), "ru_large_clustering_distance_threshold")
            ("ru_large_cluster_size", po::value<size_t>()->default_value(100), "ru_large_cluster_size")
            ("clustering_batch_size", po::value<size_t>()->default_value(10000), "clustering_batch_size")
            ("clustering_batch_intersection_size", po::value<size_t>()->default_value(2000), "clustering_batch_intersection_size")
            ("clustering_use_timestamp_moving", po::value<bool>()->default_value(false), "clustering_use_timestamp_moving")
            ("clustering_ban_threads_from_same_site", po::value<bool>()->default_value(true), "clustering_ban_threads_from_same_site")
            ("rating", po::value<std::string>()->default_value("models/pagerank_rating.txt"), "rating")
            ("alexa_rating", po::value<std::string>()->default_value("models/alexa_rating_2_fixed.txt"), "alexa_rating")
            ("ndocs", po::value<int>()->default_value(-1), "ndocs")
            ("from_json", po::bool_switch()->default_value(false), "from_json")
            ("save_not_news", po::bool_switch()->default_value(false), "save_not_news")
            ("languages", po::value<std::vector<std::string>>()->multitoken()->default_value(std::vector<std::string>{"ru", "en"}, "ru en"), "languages")
            ("iter_timestamp_percentile", po::value<double>()->default_value(0.99), "iter_timestamp_percentile")
            ("window_size", po::value<uint64_t>()->default_value(0), "window_size")
            ;

        po::positional_options_description p;
        p.add("mode", 1);
        p.add("input", 1);

        po::command_line_parser parser{argc, argv};
        parser.options(desc).positional(p);
        po::parsed_options parsed_options = parser.run();
        po::variables_map vm;
        po::store(parsed_options, vm);
        po::notify(vm);

        // Args check
        if (!vm.count("mode") || !vm.count("input")) {
            std::cerr << "Not enough arguments" << std::endl;
            return -1;
        }
        std::string mode = vm["mode"].as<std::string>();
        LOG_DEBUG("Mode: " << mode);
        std::vector<std::string> modes = {
            "languages",
            "news",
            "json",
            "categories",
            "threads",
            "top",
            "server"
        };
        if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
            std::cerr << "Unknown or unsupported mode!" << std::endl;
            return -1;
        }

        if (mode == "server") {
            const std::string serverConfig = vm["server_config"].as<std::string>();
            return RunServer(serverConfig);
        }

        // Load agency ratings
        LOG_DEBUG("Loading agency ratings...");
        const std::string ratingPath = vm["rating"].as<std::string>();
        TAgencyRating agencyRating(ratingPath);
        LOG_DEBUG("Agency ratings loaded");

        // Load alexa agency ratings
        LOG_DEBUG("Loading alexa agency ratings...");
        const std::string alexaRatingPath = vm["alexa_rating"].as<std::string>();
        TAlexaAgencyRating alexaAgencyRating(alexaRatingPath);
        LOG_DEBUG("Alexa agency ratings loaded");


        // Read file names
        LOG_DEBUG("Reading file names...");
        int nDocs = vm["ndocs"].as<int>();
        bool fromJson = vm["from_json"].as<bool>();
        std::vector<std::string> fileNames;
        if (!fromJson) {
            std::string sourceDir = vm["input"].as<std::string>();
            ReadFileNames(sourceDir, fileNames, nDocs);
            LOG_DEBUG("Files count: " << fileNames.size());
        } else {
            std::string fileName = vm["input"].as<std::string>();
            fileNames.push_back(fileName);
            LOG_DEBUG("JSON file as input");
        }

        // Parse files and annotate with classifiers
        const std::string annotatorConfig = vm["annotator_config"].as<std::string>();
        bool saveNotNews = vm["save_not_news"].as<bool>();
        TAnnotator annotator(annotatorConfig, saveNotNews, mode == "json");
        TTimer<std::chrono::high_resolution_clock, std::chrono::milliseconds> annotationTimer;
        std::vector<TDbDocument> docs = annotator.AnnotateAll(fileNames, fromJson);
        LOG_DEBUG("Annotation: " << annotationTimer.Elapsed() << " ms (" << docs.size() << " documents)");

        // Output
        if (mode == "languages") {
            nlohmann::json outputJson = nlohmann::json::array();
            std::map<std::string, std::vector<std::string>> langToFiles;
            for (const TDbDocument& doc : docs) {
                langToFiles[nlohmann::json(doc.Language)].push_back(CleanFileName(doc.FileName));
            }
            for (const auto& pair : langToFiles) {
                const std::string& language = pair.first;
                const std::vector<std::string>& files = pair.second;
                nlohmann::json object = {
                    {"lang_code", language},
                    {"articles", files}
                };
                outputJson.push_back(object);
            }
            std::cout << outputJson.dump(4) << std::endl;
            return 0;
        } else if (mode == "json") {
            nlohmann::json outputJson = nlohmann::json::array();
            for (const TDbDocument& doc : docs) {
                outputJson.push_back(doc.ToJson());
            }
            std::cout << outputJson.dump(4) << std::endl;
            return 0;
        } else if (mode == "news") {
            nlohmann::json articles = nlohmann::json::array();
            for (const TDbDocument& doc : docs) {
                articles.push_back(CleanFileName(doc.FileName));
            }
            nlohmann::json outputJson = nlohmann::json::object();
            outputJson["articles"] = articles;
            std::cout << outputJson.dump(4) << std::endl;
            return 0;
        } else if (mode == "categories") {
            nlohmann::json outputJson = nlohmann::json::array();

            std::vector<std::vector<std::string>> catToFiles(tg::ECategory_ARRAYSIZE);
            for (const TDbDocument& doc : docs) {
                tg::ECategory category = doc.Category;
                if (category == tg::NC_UNDEFINED || (category == tg::NC_NOT_NEWS && !saveNotNews)) {
                    continue;
                }
                catToFiles[static_cast<size_t>(category)].push_back(CleanFileName(doc.FileName));
                LOG_DEBUG(category << "\t" << doc.Title);
            }
            for (size_t i = 0; i < tg::ECategory_ARRAYSIZE; i++) {
                tg::ECategory category = static_cast<tg::ECategory>(i);
                if (category == tg::NC_UNDEFINED || category == tg::NC_ANY) {
                    continue;
                }
                if (!saveNotNews && category == tg::NC_NOT_NEWS) {
                    continue;
                }
                const std::vector<std::string>& files = catToFiles[i];
                nlohmann::json object = {
                    {"category", category},
                    {"articles", files}
                };
                outputJson.push_back(object);
            }
            std::cout << outputJson.dump(4) << std::endl;
            return 0;
        } else if (mode != "threads" && mode != "top") {
            assert(false);
        }

        // Clustering
        const std::set<std::string> clusteringLanguages = {"ru", "en"};
        std::stable_sort(docs.begin(), docs.end(),
            [](const TDbDocument& d1, const TDbDocument& d2) {
                if (d1.FetchTime == d2.FetchTime) {
                    if (d1.FileName.empty() && d2.FileName.empty()) {
                        return d1.Title.length() < d2.Title.length();
                    }
                    return d1.FileName < d2.FileName;
                }
                return d1.FetchTime < d2.FetchTime;
            }
        );
        const double iterTimestampPercentile = vm["iter_timestamp_percentile"].as<double>();
        uint64_t iterTimestamp = GetIterTimestamp(docs, iterTimestampPercentile);

        const std::string clusteringType = vm["clustering_type"].as<std::string>();
        assert(clusteringType == "slink");

        std::map<std::string, std::unique_ptr<TClustering>> clusterings;
        for (const std::string& language : clusteringLanguages) {
            TSlinkClustering::TConfig slinkConfig;
            slinkConfig.SmallClusterThreshold = vm[language + "_small_clustering_distance_threshold"].as<float>();
            slinkConfig.SmallClusterSize = vm[language + "_small_cluster_size"].as<size_t>();
            slinkConfig.MediumClusterThreshold = vm[language + "_medium_clustering_distance_threshold"].as<float>();
            slinkConfig.MediumClusterSize = vm[language + "_medium_cluster_size"].as<size_t>();
            slinkConfig.LargeClusterThreshold = vm[language + "_large_clustering_distance_threshold"].as<float>();
            slinkConfig.LargeClusterSize = vm[language + "_large_cluster_size"].as<size_t>();

            slinkConfig.BatchSize = vm["clustering_batch_size"].as<size_t>();
            slinkConfig.BatchIntersectionSize = vm["clustering_batch_intersection_size"].as<size_t>();

            slinkConfig.UseTimestampMoving = vm["clustering_use_timestamp_moving"].as<bool>();
            slinkConfig.BanThreadsFromSameSite = vm["clustering_ban_threads_from_same_site"].as<bool>();

            clusterings[language] = std::make_unique<TSlinkClustering>(slinkConfig);
        }

        std::map<std::string, std::vector<TDbDocument>> lang2Docs;
        while (!docs.empty()) {
            const TDbDocument& doc = docs.back();
            assert(doc.Language);
            const std::string& language = nlohmann::json(doc.Language);
            if (clusteringLanguages.find(language) != clusteringLanguages.end()) {
                lang2Docs[language].push_back(doc);
            }
            docs.pop_back();
        }
        docs.shrink_to_fit();
        docs.clear();

        TTimer<std::chrono::high_resolution_clock, std::chrono::milliseconds> clusteringTimer;
        TClusters clusters;
        for (const std::string& language : clusteringLanguages) {
            const TClusters langClusters = clusterings[language]->Cluster(lang2Docs[language]);
            std::copy_if(
                langClusters.cbegin(),
                langClusters.cend(),
                std::back_inserter(clusters),
                [](const TNewsCluster& cluster) {
                    return cluster.GetSize() > 0;
                }
            );
        }
        LOG_DEBUG("Clustering: " << clusteringTimer.Elapsed() << " ms (" << clusters.size() << " clusters)");

        //Summarization
        Summarize(clusters, agencyRating);
        if (mode == "threads") {
            nlohmann::json outputJson = nlohmann::json::array();
            for (const auto& cluster : clusters) {
                nlohmann::json files = nlohmann::json::array();
                for (const TDbDocument& doc : cluster.GetDocuments()) {
                    files.push_back(CleanFileName(doc.FileName));
                }
                nlohmann::json object = {
                    {"title", cluster.GetTitle()},
                    {"articles", files}
                };
                outputJson.push_back(object);

                if (cluster.GetSize() >= 2) {
                    LOG_DEBUG("\n         CLUSTER: " << cluster.GetTitle());
                    for (const TDbDocument& doc : cluster.GetDocuments()) {
                        LOG_DEBUG("  " << doc.Title << " (" << doc.Url << ")");
                    }
                }
            }
            std::cout << outputJson.dump(4) << std::endl;
            return 0;
        } else if (mode != "top") {
            assert(false);
        }

        // Ranking
        uint64_t window = vm["window_size"].as<uint64_t>();
        const auto tops = Rank(clusters, agencyRating, alexaAgencyRating, iterTimestamp, window);
        nlohmann::json outputJson = nlohmann::json::array();
        for (auto it = tops.begin(); it != tops.end(); ++it) {
            const auto category = static_cast<tg::ECategory>(std::distance(tops.begin(), it));
            if (category == tg::NC_UNDEFINED) {
                continue;
            }
            if (!saveNotNews && category == tg::NC_NOT_NEWS) {
                continue;
            }

            nlohmann::json rubricTop = {
                {"category", category},
                {"threads", nlohmann::json::array()}
            };
            for (const auto& cluster : *it) {
                nlohmann::json object = {
                    {"title", cluster.Title},
                    {"category", cluster.Category},
                    {"articles", nlohmann::json::array()},
                    {"article_weights", nlohmann::json::array()},
                    {"weight", cluster.WeightInfo.Weight},
                    {"importance", cluster.WeightInfo.Importance},
                    {"best_time", cluster.WeightInfo.BestTime},
                    {"age_penalty", cluster.WeightInfo.AgePenalty}
                };
                for (const TDbDocument& doc : cluster.Cluster.get().GetDocuments()) {
                    object["articles"].push_back(CleanFileName(doc.FileName));
                }
                for (const auto& weight : cluster.DocWeights) {
                    object["article_weights"].push_back(weight);
                }
                rubricTop["threads"].push_back(object);
            }
            outputJson.push_back(rubricTop);
        }
        std::cout << outputJson.dump(4) << std::endl;
        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }
}
