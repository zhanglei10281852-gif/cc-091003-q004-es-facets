#include "es_client.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace es {

namespace {

// 允许的分面字段白名单及类型：非法字段在校验阶段被拒绝，不会拼入查询
const std::map<std::string, FacetType>& facetFieldRegistry() {
    static const std::map<std::string, FacetType> kRegistry = {
        {"category", FacetType::Terms},
        {"author", FacetType::Terms},
        {"tags", FacetType::Terms},
        {"created_at", FacetType::DateRange},
    };
    return kRegistry;
}

// 允许作为关键词检索 / 高亮的文本字段
const std::set<std::string>& textFieldWhitelist() {
    static const std::set<std::string> kFields = {"title", "content"};
    return kFields;
}

constexpr int kMaxPageSize = 100;       // 单页最大条数
constexpr int kMaxResultWindow = 10000; // from + size 上限（与 ES max_result_window 对齐）
constexpr int kMaxFacetSize = 100;      // 单个分面返回桶数上限

FacetType facetTypeOf(const std::string& field) {
    return facetFieldRegistry().at(field);
}

bool isValidDateLiteral(const std::string& value) {
    static const std::regex kDateRe(R"(^\d{4}-\d{2}-\d{2}$)");
    return std::regex_match(value, kDateRe);
}

/**
 * 校验分面搜索请求。
 * 非法字段、倒置日期、越界分页在此抛出 ESException，绝不拼入查询。
 */
void validateFacetRequest(const FacetSearchRequest& req) {
    // ---- 分页 ----
    if (req.from < 0) {
        throw ESException("Invalid pagination: from must be >= 0, got " +
                          std::to_string(req.from));
    }
    if (req.size < 1 || req.size > kMaxPageSize) {
        throw ESException("Invalid pagination: size must be in [1, " +
                          std::to_string(kMaxPageSize) + "], got " +
                          std::to_string(req.size));
    }
    if (req.from + req.size > kMaxResultWindow) {
        throw ESException("Invalid pagination: from + size must be <= " +
                          std::to_string(kMaxResultWindow));
    }

    // ---- 分面定义 ----
    std::set<std::string> declared;
    for (const auto& def : req.facets) {
        if (facetFieldRegistry().count(def.field) == 0) {
            throw ESException("Illegal facet field: " + def.field);
        }
        if (!declared.insert(def.field).second) {
            throw ESException("Duplicate facet definition: " + def.field);
        }
        if (def.size < 1 || def.size > kMaxFacetSize) {
            throw ESException("Invalid facet size for " + def.field +
                              ": must be in [1, " + std::to_string(kMaxFacetSize) + "]");
        }
    }

    // ---- 关键词 / 高亮字段 ----
    for (const auto& field : req.keywordFields) {
        if (textFieldWhitelist().count(field) == 0) {
            throw ESException("Illegal keyword field: " + field);
        }
    }
    for (const auto& field : req.highlightFields) {
        if (textFieldWhitelist().count(field) == 0) {
            throw ESException("Illegal highlight field: " + field);
        }
    }

    // ---- 词条分面选择 ----
    for (const auto& sel : req.selections) {
        if (declared.count(sel.field) == 0) {
            throw ESException("Selection refers to undeclared facet: " + sel.field);
        }
        if (facetTypeOf(sel.field) != FacetType::Terms) {
            throw ESException("Facet " + sel.field +
                              " is a date facet; use a date range selection");
        }
    }

    // ---- 日期范围选择 ----
    std::set<std::string> dateSelectionSeen;
    for (const auto& ds : req.dateSelections) {
        if (declared.count(ds.field) == 0) {
            throw ESException("Date selection refers to undeclared facet: " + ds.field);
        }
        if (facetTypeOf(ds.field) != FacetType::DateRange) {
            throw ESException("Facet " + ds.field +
                              " is a terms facet; use a terms selection");
        }
        if (!dateSelectionSeen.insert(ds.field).second) {
            throw ESException("Duplicate date range selection for facet: " + ds.field);
        }
        if (ds.from.empty() && ds.to.empty()) {
            throw ESException("Date range selection for " + ds.field +
                              " needs at least one bound");
        }
        if (!ds.from.empty() && !isValidDateLiteral(ds.from)) {
            throw ESException("Invalid date literal (expect yyyy-MM-dd): " + ds.from);
        }
        if (!ds.to.empty() && !isValidDateLiteral(ds.to)) {
            throw ESException("Invalid date literal (expect yyyy-MM-dd): " + ds.to);
        }
        if (!ds.from.empty() && !ds.to.empty() && ds.from > ds.to) {
            throw ESException("Inverted date range for " + ds.field +
                              ": [" + ds.from + ", " + ds.to + ")");
        }
    }
}

/**
 * 构建单个分面的过滤条件；该分面无已选条件时返回 null json。
 */
json buildFacetFilter(const FacetDef& def, const FacetSearchRequest& req) {
    if (facetTypeOf(def.field) == FacetType::Terms) {
        // 同一分面内多选按 OR：terms 查询
        std::vector<std::string> values;
        for (const auto& sel : req.selections) {
            if (sel.field != def.field) {
                continue;
            }
            for (const auto& value : sel.values) {
                if (std::find(values.begin(), values.end(), value) == values.end()) {
                    values.push_back(value);
                }
            }
        }
        if (values.empty()) {
            return nullptr;
        }
        return {{"terms", {{def.field, values}}}};
    }

    // 日期分面：闭开区间 [from, to)
    for (const auto& ds : req.dateSelections) {
        if (ds.field != def.field) {
            continue;
        }
        json range = json::object();
        if (!ds.from.empty()) {
            range["gte"] = ds.from; // 下界闭
        }
        if (!ds.to.empty()) {
            range["lt"] = ds.to; // 上界开
        }
        return {{"range", {{def.field, range}}}};
    }
    return nullptr;
}

/**
 * 解析聚合响应为分面统计：
 * - 桶按数量降序、同数按键名升序稳定排序；
 * - 未出现在返回桶里的已选值以零计数保留。
 */
std::vector<FacetResult> parseFacetAggregations(const json& aggregations,
                                                const FacetSearchRequest& req) {
    std::vector<FacetResult> results;

    for (const auto& def : req.facets) {
        FacetResult facet;
        facet.field = def.field;
        facet.type = facetTypeOf(def.field);

        // 该分面的已选值（仅词条分面有离散已选值）
        std::set<std::string> selectedValues;
        for (const auto& sel : req.selections) {
            if (sel.field == def.field) {
                selectedValues.insert(sel.values.begin(), sel.values.end());
            }
        }

        const std::string aggName = "facet_" + def.field;
        if (aggregations.contains(aggName)) {
            const auto& filterAgg = aggregations[aggName];
            const auto& termsAgg = filterAgg.value("buckets", json::object());
            for (const auto& bucket : termsAgg.value("buckets", json::array())) {
                FacetBucket b;
                // 日期字段的 terms 聚合带 key_as_string（按 format 格式化）
                if (bucket.contains("key_as_string")) {
                    b.key = bucket["key_as_string"].get<std::string>();
                } else if (bucket["key"].is_string()) {
                    b.key = bucket["key"].get<std::string>();
                } else {
                    b.key = bucket["key"].dump();
                }
                b.count = bucket.value("doc_count", 0L);
                b.selected = selectedValues.count(b.key) > 0;
                facet.buckets.push_back(std::move(b));
            }
        }

        // 已选值零计数保留：返回桶里没有的已选值补 0
        for (const auto& value : selectedValues) {
            const auto it = std::find_if(facet.buckets.begin(), facet.buckets.end(),
                                         [&](const FacetBucket& b) { return b.key == value; });
            if (it == facet.buckets.end()) {
                facet.buckets.push_back({value, 0, true});
            }
        }

        // 数量降序，同数按键名升序（稳定）
        std::stable_sort(facet.buckets.begin(), facet.buckets.end(),
                         [](const FacetBucket& a, const FacetBucket& b) {
                             if (a.count != b.count) {
                                 return a.count > b.count;
                             }
                             return a.key < b.key;
                         });

        results.push_back(std::move(facet));
    }

    return results;
}

} // namespace

FacetSearchResult ESClient::facetSearch(const std::string& indexName,
                                        const FacetSearchRequest& req) {
    // 校验失败直接抛异常，非法参数不会拼入查询
    validateFacetRequest(req);

    // ---- 关键词查询（空关键词 -> match_all）----
    json keywordQuery;
    if (req.keyword.empty()) {
        keywordQuery = {{"match_all", json::object()}};
    } else {
        const auto fields = req.keywordFields.empty()
                                ? std::vector<std::string>{"title", "content"}
                                : req.keywordFields;
        keywordQuery = {{"multi_match", {{"query", req.keyword}, {"fields", fields}}}};
    }

    // ---- 各分面过滤条件 ----
    std::map<std::string, json> filterByField; // field -> filter（可能为 null）
    json allFilters = json::array();
    for (const auto& def : req.facets) {
        json filter = buildFacetFilter(def, req);
        filterByField[def.field] = filter;
        if (!filter.is_null()) {
            allFilters.push_back(filter);
        }
    }

    json body;
    body["query"] = keywordQuery;
    body["from"] = req.from;
    body["size"] = req.size;

    // 命中列表应用全部已选条件（post_filter 不影响聚合统计）
    if (!allFilters.empty()) {
        body["post_filter"] = {{"bool", {{"filter", allFilters}}}};
    }

    // ---- 聚合：每个分面排除自身过滤、保留关键词与其他分面条件 ----
    json aggs = json::object();
    for (const auto& def : req.facets) {
        json otherFilters = json::array();
        for (const auto& entry : filterByField) {
            if (entry.first != def.field && !entry.second.is_null()) {
                otherFilters.push_back(entry.second);
            }
        }
        json filterClause = otherFilters.empty()
                                ? json{{"match_all", json::object()}}
                                : json{{"bool", {{"filter", otherFilters}}}};

        json terms = {{"field", def.field}, {"size", def.size}};
        if (facetTypeOf(def.field) == FacetType::DateRange) {
            terms["format"] = "yyyy-MM-dd";
        }

        aggs["facet_" + def.field] = {
            {"filter", filterClause},
            {"aggs", {{"buckets", {{"terms", terms}}}}}
        };
    }
    if (!aggs.empty()) {
        body["aggs"] = aggs;
    }

    // ---- 高亮（与统计同请求返回）----
    if (!req.highlightFields.empty()) {
        json highlightFields = json::object();
        for (const auto& field : req.highlightFields) {
            highlightFields[field] = json::object();
        }
        body["highlight"] = {
            {"pre_tags", {"<em>"}},
            {"post_tags", {"</em>"}},
            {"fields", highlightFields}
        };
    }

    log("Facet search on index: " + indexName);
    auto response = httpClient_.post(buildUrl("/" + indexName + "/_search"), body.dump());
    if (!response.isSuccess()) {
        throw ESException("Facet search failed: " + response.body);
    }

    const auto respJson = json::parse(response.body);

    FacetSearchResult result;
    result.search = parseSearchResponse(respJson);
    result.facets = parseFacetAggregations(respJson.value("aggregations", json::object()), req);
    return result;
}

} // namespace es
