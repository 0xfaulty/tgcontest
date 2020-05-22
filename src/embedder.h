#pragma once

#include <Eigen/Core>
#include <torch/script.h>
#include <unordered_map>
#include <memory>

struct TDocument;

namespace fasttext {
    class FastText;
}

class TFastTextEmbedder {
public:
    enum AggregationMode {
        AM_Avg = 0,
        AM_Max = 1,
        AM_Min = 2,
        AM_Matrix = 3
    };

    TFastTextEmbedder(
        fasttext::FastText& model,
        AggregationMode mode = AM_Avg,
        size_t maxWords = 100,
        const std::string& modelPath = "");

    size_t GetEmbeddingSize() const;
    std::vector<float> CalcEmbedding(const std::string& title, const std::string& text) const;

private:
    fasttext::FastText& Model;
    AggregationMode Mode;
    size_t MaxWords;
    Eigen::MatrixXf Matrix;
    Eigen::VectorXf Bias;
    mutable torch::jit::script::Module TorchModel;
    std::string TorchModelPath;
};
