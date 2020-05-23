#include "controller.h"

#include "document.h"
#include "document.pb.h"

#include <boost/optional.hpp>
#include <nlohmann_json/json.hpp>
#include <tinyxml2/tinyxml2.h>

namespace {

    boost::optional<uint32_t> ParseTtlHeader(const std::string& value) try {
        static constexpr size_t PREFIX_LEN = std::char_traits<char>::length("max-age=");
        return static_cast<uint32_t>(std::stoi(value.substr(PREFIX_LEN)));
    } catch (const std::exception& e) {
        return boost::none;
    }

    boost::optional<uint32_t> ParsePeriod(const std::string& value) try {
        return static_cast<uint32_t>(std::stoi(value));
    } catch (const std::exception& e) {
        return boost::none;
    }

    boost::optional<std::string> ParseLang(const std::string& value) {
        static const std::vector<std::string> allowed = { "ru", "en" };
        if (std::find(allowed.cbegin(), allowed.cend(), value) != allowed.end()) {
            return value;
        }
        return boost::none;
    }

    boost::optional<tg::ECategory> ParseCategory(const std::string& value) {
        const tg::ECategory category = nlohmann::json(value).get<tg::ECategory>();
        return boost::make_optional(category != tg::NC_UNDEFINED, category);
    }

    void MakeSimpleResponse(std::function<void(const drogon::HttpResponsePtr&)>&& callback, drogon::HttpStatusCode code = drogon::k400BadRequest) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(code);
        callback(resp);
    }

}

void TController::Init(
    const THotState<TClustersIndex>* index,
    rocksdb::DB* db,
    std::unique_ptr<TAnnotator> annotator,
    bool skipIrrelevantDocs
) {
    Index = index;
    Db = db;
    Annotator = std::move(annotator);
    SkipIrrelevantDocs = skipIrrelevantDocs;
    Initialized.store(true, std::memory_order_release);
}

bool TController::IsNotReady(std::function<void(const drogon::HttpResponsePtr&)>&& callback) const {
    if (Initialized.load(std::memory_order_acquire)) {
        return false;
    }

    MakeSimpleResponse(std::move(callback), drogon::k503ServiceUnavailable);
    return true;
}

void TController::Put(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr&)> &&callback, const std::string& fname) const {
    if (IsNotReady(std::move(callback))) {
        return;
    }

    const boost::optional<uint32_t> ttl = ParseTtlHeader(req->getHeader("Cache-Control"));
    if (!ttl) {
        MakeSimpleResponse(std::move(callback), drogon::k400BadRequest);
        return;
    }

    tinyxml2::XMLDocument html;
    const tinyxml2::XMLError parseCode = html.Parse(req->bodyData(), req->bodyLength());
    if (parseCode != tinyxml2::XML_SUCCESS) {
        MakeSimpleResponse(std::move(callback), drogon::k400BadRequest);
        return;
    }


    const auto getCode = [this, &fname] () -> drogon::HttpStatusCode {
        std::string value;
        const auto mayExist = Db->KeyMayExist(rocksdb::ReadOptions(), fname, &value);
        return mayExist ? drogon::k204NoContent : drogon::k201Created;
    };

    // TODO: return 400 if bad XML
    const boost::optional<TDbDocument> dbDoc = Annotator->AnnotateHtml(html, fname);
    if (SkipIrrelevantDocs && !dbDoc) {
        MakeSimpleResponse(std::move(callback), getCode());
        return;
    }

    std::string serializedDoc;
    if (dbDoc) {
        const bool success = dbDoc->ToProtoString(&serializedDoc);
        if (!success) {
            MakeSimpleResponse(std::move(callback), drogon::k500InternalServerError);
            return;
        }
    }

    // TODO: possible races while the same fname is provided to multiple queries
    // TODO: use "value_found" flag and check DB instead of only bloom filter
    const drogon::HttpStatusCode code = getCode();
    const rocksdb::Status s = Db->Put(rocksdb::WriteOptions(), fname, serializedDoc);
    if (!s.ok()) {
        MakeSimpleResponse(std::move(callback), drogon::k500InternalServerError);
        return;
    }

    MakeSimpleResponse(std::move(callback), code);
}

void TController::Delete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr&)> &&callback, const std::string& fname) const {
    if (IsNotReady(std::move(callback))) {
        return;
    }

    // TODO: possible races while the same fname is provided to multiple queries
    // TODO: use "value_found" flag and check DB instead of only bloom filter
    std::string value;
    const bool mayExist = Db->KeyMayExist(rocksdb::ReadOptions(), fname, &value);
    if (mayExist) {
        const rocksdb::Status s = Db->Delete(rocksdb::WriteOptions(), fname);
        if (!s.ok()) {
            MakeSimpleResponse(std::move(callback), drogon::k500InternalServerError);
            return;
        }
    }

    MakeSimpleResponse(std::move(callback), mayExist ? drogon::k204NoContent : drogon::k404NotFound);
}

namespace {

    Json::Value ToJson(const TNewsCluster& cluster) {
        Json::Value articles(Json::arrayValue);
        for (const auto& document : cluster.GetDocuments()) {
            articles.append(document.get().FileName);
        }

        Json::Value json(Json::objectValue);
        json["title"] = cluster.GetTitle();
//        json["category"] = cluster.GetCategory();
//        json["articles"] = std::move(articles);
        return json;
    }

}

void TController::Threads(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr&)> &&callback) const {
    if (IsNotReady(std::move(callback))) {
        return;
    }

    const boost::optional<uint32_t> period = ParsePeriod(req->getParameter("period"));
    const boost::optional<std::string> lang = ParseLang(req->getParameter("lang_code"));
    const boost::optional<tg::ECategory> category = ParseCategory(req->getParameter("category"));

    if (!period || !lang || !category) {
        MakeSimpleResponse(std::move(callback), drogon::k400BadRequest);
        return;
    }
    const bool specificCategory = category != tg::NC_ANY;

    const std::shared_ptr<TClustersIndex> index = Index->AtomicGet();
    auto indexIt = std::lower_bound(index->cbegin(), index->cend(), period.value(), TNewsCluster::Compare); // TODO: uint32_t

    Json::Value threads(Json::arrayValue);
    for (; indexIt != index->cend(); ++indexIt) {
        if (specificCategory && category != indexIt->GetCategory()) {
            continue;
        }
        threads.append(ToJson(*indexIt));
    }

    Json::Value json(Json::objectValue);
    json["threads"] = std::move(threads);
    auto resp = drogon::HttpResponse::newHttpJsonResponse(json);
    callback(resp);
}


void TController::Get(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr&)> &&callback, const std::string& fname) const {
    if (IsNotReady(std::move(callback))) {
        return;
    }

    std::string serializedDoc;
    const rocksdb::Status s = Db->Get(rocksdb::ReadOptions(), fname, &serializedDoc);

    Json::Value ret;
    ret["fname"] = fname;
    ret["status"] = s.ok() ? "FOUND" : "NOT FOUND";

    if (s.ok()) {
        tg::TDocumentProto doc;
        const auto suc = doc.ParseFromString(serializedDoc);
        ret["parsed"] = suc;
        ret["title"] = doc.title();
        ret["lang"] = doc.language();
        ret["category"] = doc.category();
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(ret);
    callback(resp);
}
