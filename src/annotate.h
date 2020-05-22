#pragma once

#include "embedder.h"
#include "db_document.h"

#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <boost/program_options.hpp>
#include <fasttext.h>
#include <onmt/Tokenizer.h>
#include <tinyxml2/tinyxml2.h>

struct TDocument;
//class TFastTextEmbedder;

using TFTModelStorage = std::unordered_map<tg::ELanguage, fasttext::FastText>;

class TAnnotator {
public:
    TAnnotator(const boost::program_options::variables_map& vm, bool saveNotNews);

    std::vector<TDbDocument> AnnotateAll(const std::vector<std::string>& fileNames, bool fromJson) const;

    boost::optional<TDbDocument> AnnotateHtml(const std::string& path) const;
    boost::optional<TDbDocument> AnnotateHtml(const tinyxml2::XMLDocument& html, const std::string& fileName) const;

private:
    boost::optional<TDbDocument> AnnotateDocument(const TDocument& document) const;

    boost::optional<TDocument> ParseHtml(const std::string& path) const;
    boost::optional<TDocument> ParseHtml(const tinyxml2::XMLDocument& html, const std::string& fileName) const;

    std::string PreprocessText(const std::string& text) const;

private:
    std::unordered_set<tg::ELanguage> Languages;
    onmt::Tokenizer Tokenizer;

    fasttext::FastText LanguageDetector;
    TFTModelStorage VectorModels;
    TFTModelStorage CategoryDetectors;
    std::unordered_map<tg::ELanguage, std::unique_ptr<TFastTextEmbedder>> Embedders;

    size_t MinTextLength = 20;
    bool ParseLinks = false;
    bool SaveNotNews = false;
    bool SaveTexts = false;
};
