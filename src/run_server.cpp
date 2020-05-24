#include "run_server.h"

#include "annotator.h"
#include "hot_state.h"
#include "clusterer.h"
#include "config.pb.h"
#include "controller.h"
#include "server_clustering.h"
#include "util.h"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <rocksdb/db.h>

#include <fcntl.h>
#include <iostream>

using namespace drogon;

namespace {

    tg::TServerConfig ParseConfig(const std::string& fname) {
        const int fileDesc = open(fname.c_str(), O_RDONLY);
        ENSURE(fileDesc >= 0, "Could not open config file");

        google::protobuf::io::FileInputStream fileInput(fileDesc);

        tg::TServerConfig config;
        const bool succes = google::protobuf::TextFormat::Parse(&fileInput, &config);
        ENSURE(succes, "Invalid prototxt file");

        return config;
    }

    std::unique_ptr<rocksdb::DB> CreateDatabase(const tg::TServerConfig& config) {
        rocksdb::Options options;
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();
        options.create_if_missing = !config.db_fail_if_missing();

        rocksdb::DB* db;
        const rocksdb::Status s = rocksdb::DB::Open(options, config.db_path(), &db);
        ENSURE(s.ok(), "Failed to create database: " << s.getState());

        return std::unique_ptr<rocksdb::DB>(db);
    }

}

int RunServer(const std::string& fname, uint16_t port) {
    LOG_DEBUG("Loading server config");
    const auto config = ParseConfig(fname);

    LOG_DEBUG("Creating database");
    std::unique_ptr<rocksdb::DB> db = CreateDatabase(config);

    LOG_DEBUG("Creating annotator");
    std::unique_ptr<TAnnotator> annotator = std::make_unique<TAnnotator>(config.annotator_config_path());

    LOG_DEBUG("Creating clusterer");
    std::unique_ptr<TClusterer> clusterer = std::make_unique<TClusterer>(config.clusterer_config_path());

    TServerClustering serverClustering(std::move(clusterer), db.get());

    LOG_DEBUG("Launching server");
    app()
        .setLogLevel(trantor::Logger::kTrace)
        .addListener("0.0.0.0", port)
        .setThreadNum(config.threads());

    auto controllerPtr = std::make_shared<TController>();
    app().registerController(controllerPtr);


    LOG_DEBUG("Launching clustering");
    THotState<TClusterIndex> index;

    auto initContoller = [&, annotator=std::move(annotator)]() mutable {
        DrClassMap::getSingleInstance<TController>()->Init(&index, db.get(), std::move(annotator), config.skip_irrelevant_docs());
    };

    std::thread clusteringThread([&]() {
        bool firstRun = true;
        while (true) {
            TClusterIndex newIndex = serverClustering.MakeIndex();
            index.AtomicSet(std::make_shared<TClusterIndex>(std::move(newIndex)));

            if (firstRun) {
                initContoller();
                firstRun = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    app().run();

    return 0;
}
