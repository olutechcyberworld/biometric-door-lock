#pragma once
#include <vector>
namespace dl { namespace detect {
struct result_t { int category; float score; std::vector<int> box; std::vector<int> keypoint; };
}}
