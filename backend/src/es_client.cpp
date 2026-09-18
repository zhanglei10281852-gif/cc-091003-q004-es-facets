#include "es_client.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <set>

namespace es {

// ==================== 构造与析构 ====================

ESClient::ESClient(const std::string& host, int port) {
    std::ostringstream oss;
    oss << "http://" << host << ":" << port;
    baseUrl_ = oss.str();
    httpClient_.setTimeout(30);
    httpClient_.setConnectTimeout(10);
}

ESClient::~ESClient() = default;

// ==================== 辅助方法 ====================

void ESClient::log(const std::string& message) {
    if (logCallback_) {
        logCallback_(message);
    }
}

std::string ESClient::buildUrl(const std::string& path) {
    return baseUrl_ + path;
}

void ESClient::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
}

// ==================== 集群操作 ====================

bool ESClient::ping() {
    try {
        auto response = httpClient_.get(buildUrl("/"));
        return response.isSuccess();
    } catch (const HttpException&) {
        return false;
    }
}

json ESClient::clusterHealth() {
    auto response = httpClient_.get(buildUrl("/_cluster/health"));
    if (!response.isSuccess()) {
        throw ESException("Failed to get cluster health: " + response.body);
    }
    return json::parse(response.body);
}

json ESClient::clusterInfo() {
    auto response = httpClient_.get(buildUrl("/"));
    if (!response.isSuccess()) {
        throw ESException("Failed to get cluster info: " + response.body);
    }
    return json::parse(response.body);
}

// ==================== 索引操作 ====================

bool ESClient::createIndex(const std::string& indexName,
                           const json& mappings,
                           const json& settings) {
    json body;
    if (!mappings.empty()) {
        body["mappings"] = mappings;
    }
    if (!settings.empty()) {
        body["settings"] = settings;
    }
    
    log("Creating index: " + indexName);
    auto response = httpClient_.put(buildUrl("/" + indexName), body.dump());
    
    if (!response.isSuccess()) {
        auto error = json::parse(response.body);
        throw ESException("Failed to create index: " + 
                         error.value("error", json::object()).value("reason", response.body));
    }
    
    log("Index created successfully: " + indexName);
    return true;
}

bool ESClient::deleteIndex(const std::string& indexName) {
    log("Deleting index: " + indexName);
    auto response = httpClient_.del(buildUrl("/" + indexName));
    
    if (!response.isSuccess() && !response.isNotFound()) {
        throw ESException("Failed to delete index: " + response.body);
    }
    
    log("Index deleted: " + indexName);
    return true;
}

bool ESClient::indexExists(const std::string& indexName) {
    auto response = httpClient_.head(buildUrl("/" + indexName));
    return response.isSuccess();
}

json ESClient::getIndex(const std::string& indexName) {
    auto response = httpClient_.get(buildUrl("/" + indexName));
    if (!response.isSuccess()) {
        throw ESException("Failed to get index: " + response.body);
    }
    return json::parse(response.body);
}

bool ESClient::refreshIndex(const std::string& indexName) {
    auto response = httpClient_.post(buildUrl("/" + indexName + "/_refresh"), "");
    return response.isSuccess();
}

// ==================== 文档操作 ====================

DocResult ESClient::indexDocument(const std::string& indexName,
                                  const json& doc,
                                  const std::string& id) {
    std::string url = "/" + indexName + "/_doc";
    if (!id.empty()) {
        url += "/" + id;
    }
    
    auto response = httpClient_.post(buildUrl(url), doc.dump());
    
    DocResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.id = respJson.value("_id", "");
        result.index = respJson.value("_index", "");
        result.result = respJson.value("result", "");
        result.version = respJson.value("_version", 0);
        result.success = true;
        log("Document indexed: " + result.id);
    } else {
        result.success = false;
        throw ESException("Failed to index document: " + response.body);
    }
    
    return result;
}

std::optional<json> ESClient::getDocument(const std::string& indexName,
                                          const std::string& id) {
    auto response = httpClient_.get(buildUrl("/" + indexName + "/_doc/" + id));
    
    if (response.isNotFound()) {
        return std::nullopt;
    }
    
    if (!response.isSuccess()) {
        throw ESException("Failed to get document: " + response.body);
    }
    
    auto respJson = json::parse(response.body);
    if (respJson.value("found", false)) {
        return respJson["_source"];
    }
    return std::nullopt;
}

DocResult ESClient::updateDocument(const std::string& indexName,
                                   const std::string& id,
                                   const json& doc) {
    json body = {{"doc", doc}};
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_update/" + id), 
        body.dump()
    );
    
    DocResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.id = respJson.value("_id", "");
        result.index = respJson.value("_index", "");
        result.result = respJson.value("result", "");
        result.version = respJson.value("_version", 0);
        result.success = true;
        log("Document updated: " + result.id);
    } else {
        result.success = false;
        throw ESException("Failed to update document: " + response.body);
    }
    
    return result;
}

bool ESClient::deleteDocument(const std::string& indexName,
                              const std::string& id) {
    auto response = httpClient_.del(buildUrl("/" + indexName + "/_doc/" + id));
    
    if (response.isSuccess()) {
        log("Document deleted: " + id);
        return true;
    }
    
    if (response.isNotFound()) {
        return false;
    }
    
    throw ESException("Failed to delete document: " + response.body);
}

BulkResult ESClient::bulkIndex(const std::string& indexName,
                               const std::vector<json>& docs,
                               const std::vector<std::string>& ids) {
    std::ostringstream body;
    
    for (size_t i = 0; i < docs.size(); ++i) {
        json action = {{"index", {{"_index", indexName}}}};
        if (i < ids.size() && !ids[i].empty()) {
            action["index"]["_id"] = ids[i];
        }
        body << action.dump() << "\n";
        body << docs[i].dump() << "\n";
    }
    
    auto response = httpClient_.post(buildUrl("/_bulk"), body.str());
    
    BulkResult result;
    if (response.isSuccess()) {
        auto respJson = json::parse(response.body);
        result.took = respJson.value("took", 0);
        result.errors = respJson.value("errors", false);
        result.successCount = 0;
        result.failCount = 0;
        
        for (const auto& item : respJson["items"]) {
            DocResult docResult;
            const auto& indexResult = item["index"];
            docResult.id = indexResult.value("_id", "");
            docResult.index = indexResult.value("_index", "");
            docResult.result = indexResult.value("result", "");
            docResult.version = indexResult.value("_version", 0);
            docResult.success = indexResult.value("status", 500) < 300;
            
            if (docResult.success) {
                result.successCount++;
            } else {
                result.failCount++;
            }
            result.items.push_back(docResult);
        }
        
        log("Bulk indexed " + std::to_string(result.successCount) + " documents");
    } else {
        throw ESException("Bulk index failed: " + response.body);
    }
    
    return result;
}

// ==================== 搜索操作 ====================

SearchResult ESClient::parseSearchResponse(const json& response) {
    SearchResult result;
    result.took = response.value("took", 0);
    result.timedOut = response.value("timed_out", false);
    
    const auto& hits = response["hits"];
    const auto& total = hits["total"];
    result.total = total.is_object() ? total.value("value", 0) : total.get<int>();
    result.maxScore = hits["max_score"].is_number()
                          ? hits["max_score"].get<double>()
                          : 0.0;

    for (const auto& hit : hits["hits"]) {
        SearchHit searchHit;
        searchHit.id = hit.value("_id", "");
        searchHit.index = hit.value("_index", "");
        searchHit.score = hit["_score"].is_number()
                              ? hit["_score"].get<double>()
                              : 0.0;
        searchHit.source = hit.value("_source", json::object());
        searchHit.highlight = hit.value("highlight", json::object());
        result.hits.push_back(searchHit);
    }
    
    return result;
}

SearchResult ESClient::matchSearch(const std::string& indexName,
                                   const std::string& field,
                                   const std::string& query,
                                   int from,
                                   int size) {
    json body = {
        {"query", {
            {"match", {{field, query}}}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::multiMatchSearch(const std::string& indexName,
                                        const std::vector<std::string>& fields,
                                        const std::string& query,
                                        int from,
                                        int size) {
    json body = {
        {"query", {
            {"multi_match", {
                {"query", query},
                {"fields", fields}
            }}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::termSearch(const std::string& indexName,
                                  const std::string& field,
                                  const std::string& value,
                                  int from,
                                  int size) {
    json body = {
        {"query", {
            {"term", {{field, value}}}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::boolSearch(const std::string& indexName,
                                  const json& must,
                                  const json& should,
                                  const json& mustNot,
                                  const json& filter,
                                  int from,
                                  int size) {
    json boolQuery;
    if (!must.empty()) boolQuery["must"] = must;
    if (!should.empty()) boolQuery["should"] = should;
    if (!mustNot.empty()) boolQuery["must_not"] = mustNot;
    if (!filter.empty()) boolQuery["filter"] = filter;
    
    json body = {
        {"query", {{"bool", boolQuery}}},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::searchWithHighlight(const std::string& indexName,
                                           const json& query,
                                           const std::vector<std::string>& highlightFields,
                                           int from,
                                           int size) {
    json fields;
    for (const auto& field : highlightFields) {
        fields[field] = json::object();
    }
    
    json body = {
        {"query", query},
        {"highlight", {
            {"pre_tags", {"<em>"}},
            {"post_tags", {"</em>"}},
            {"fields", fields}
        }},
        {"from", from},
        {"size", size}
    };
    
    return search(indexName, body);
}

SearchResult ESClient::search(const std::string& indexName,
                              const json& queryBody) {
    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_search"),
        queryBody.dump()
    );

    if (!response.isSuccess()) {
        throw ESException("Search failed: " + response.body);
    }

    return parseSearchResponse(json::parse(response.body));
}

// ==================== 分面搜索 ====================

bool ESClient::isValidFieldName(const std::string& name) {
    // 拒绝空名、路径穿越、ES 字段名中不允许出现的字符，避免把非法字段拼进查询
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    static const std::string forbidden = "#:\\*?\"<>| ,/";
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 ||
            forbidden.find(c) != std::string::npos) {
            return false;
        }
    }
    if (name.front() == '.' || name.back() == '.') {
        return false;
    }
    return true;
}

bool ESClient::isValidCalendarDate(const std::string& s) {
    // 严格 yyyy-MM-dd，且必须是真实的日历日期（拒绝 2024-02-30 之类）
    if (s.size() != 10) return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    if (s[4] != '-' || s[7] != '-') return false;

    int year  = std::stoi(s.substr(0, 4));
    int month = std::stoi(s.substr(5, 2));
    int day   = std::stoi(s.substr(8, 2));
    if (month < 1 || month > 12 || day < 1) return false;

    static const int daysInMonth[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = daysInMonth[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) maxDay = 29;
    return day <= maxDay;
}

bool ESClient::isFacetActive(const FacetSpec& facet) {
    if (facet.type == FacetFieldType::Date) {
        return !facet.dateFrom.empty() || !facet.dateTo.empty();
    }
    return !facet.selected.empty();
}

std::string ESClient::facetAggName(const std::string& field) {
    // 字段名已校验为合法标识符风格，聚合名可安全派生
    return "facet_" + field;
}

json ESClient::buildKeywordClause(const std::string& keyword,
                                  const std::vector<std::string>& fields) const {
    if (keyword.empty()) {
        return json();
    }
    std::vector<std::string> matchFields = fields;
    if (matchFields.empty()) {
        matchFields = {"title", "content"};
    }
    return json{{"multi_match", {
        {"query", keyword},
        {"fields", matchFields}
    }}};
}

json ESClient::buildFacetFilter(const FacetSpec& facet) const {
    if (facet.type == FacetFieldType::Date) {
        // 明确的闭开区间 [dateFrom, dateTo)：gte 含下界，lt 不含上界
        json range;
        if (!facet.dateFrom.empty()) range["gte"] = facet.dateFrom;
        if (!facet.dateTo.empty())   range["lt"]  = facet.dateTo;
        return json{{"range", {{facet.field, range}}}};
    }

    std::vector<json> terms;
    terms.reserve(facet.selected.size());
    for (const auto& value : facet.selected) {
        terms.push_back(json{{"term", {{facet.field, value}}}});
    }
    if (terms.size() == 1) {
        return terms.front();
    }
    return json{{"bool", {{"should", terms}, {"minimum_should_match", 1}}}};
}

json ESClient::buildScopeQuery(const FacetedSearchRequest& request,
                               const std::string& excludeField) const {
    // 仅由“其他分面”的已选条件组成的过滤查询（不含关键词）。
    // excludeField 对应分面自身的过滤被排除：
    //   - excludeField == "" 时用于命中列表的 post_filter（全部分面条件）；
    //   - 否则用于该分面的 filter 聚合（保留其他分面、排除自身）。
    // 关键词放在顶层 query 中，对命中与所有聚合统一生效，故不在这里。
    json filterClauses = json::array();
    for (const auto& facet : request.facets) {
        if (isFacetActive(facet) && facet.field != excludeField) {
            filterClauses.push_back(buildFacetFilter(facet));
        }
    }

    if (filterClauses.empty()) {
        return json{{"match_all", json::object()}};
    }
    if (filterClauses.size() == 1) {
        return filterClauses[0];
    }
    return json{{"bool", {{"filter", std::move(filterClauses)}}}};
}

json ESClient::buildFacetAggregation(const FacetSpec& facet,
                                     const FacetedSearchRequest& request) const {
    // 桶的作用域 = 关键词 + 其他分面过滤（排除自身），从而给出“下一步可选数量”
    json agg;
    json scope = buildScopeQuery(request, facet.field);

    if (facet.type == FacetFieldType::Date) {
        // 日期分面本身是范围过滤，聚合只需要返回作用域内的命中文档数，
        // 前端据此展示当前范围下其他条件是否仍有命中。
        agg["filter"] = scope;
        return agg;
    }

    agg["filter"] = scope;
    // 关键词不写进这里：它在顶层 query 中，对 filter 聚合同样生效（ES 的
    // filter 聚合在外层查询上下文内求值）。这里的 filter 只保留“其他分面”。
    // ES 默认按 doc_count 降序、相同计数按 key 升序，这里显式写死
    agg["aggs"][facetAggName(facet.field)] = {
        {"terms", {
            {"field", facet.field},
            {"size", facet.size},
            {"order", json::array({
                json{{"_count", "desc"}},
                json{{"_key", "asc"}}
            })}
        }}
    };
    return agg;
}

void ESClient::validateFacetedRequest(const FacetedSearchRequest& request) const {
    // 只允许在已建 mapping 的文本字段上做关键词检索
    static const std::set<std::string> allowedKeywordFields =
        {"title", "content"};
    // 允许作为分面的字段及其类型（与 articles 索引 mapping 对应）
    static const std::set<std::string> keywordFacetFields =
        {"category", "author", "tags"};
    static const std::set<std::string> dateFacetFields =
        {"created_at"};

    if (!request.keywordFields.empty()) {
        for (const auto& field : request.keywordFields) {
            if (!isValidFieldName(field) || !allowedKeywordFields.count(field)) {
                throw ValidationException("非法的关键词字段名: " + field);
            }
        }
    }

    std::set<std::string> seenFacets;
    for (const auto& facet : request.facets) {
        if (!isValidFieldName(facet.field)) {
            throw ValidationException("非法的分面字段名: " + facet.field);
        }
        bool typeValid = facet.type == FacetFieldType::Date
                             ? static_cast<bool>(dateFacetFields.count(facet.field))
                             : static_cast<bool>(keywordFacetFields.count(facet.field));
        if (!typeValid) {
            throw ValidationException(
                "非法的分面字段或字段类型不匹配: " + facet.field);
        }
        if (!seenFacets.insert(facet.field).second) {
            throw ValidationException("分面字段重复: " + facet.field);
        }
        if (facet.type == FacetFieldType::Keyword && facet.size < 1) {
            throw ValidationException("词项分面 size 必须 >= 1: " + facet.field);
        }
        if (facet.type == FacetFieldType::Date) {
            if (!facet.dateFrom.empty() && !isValidCalendarDate(facet.dateFrom)) {
                throw ValidationException(
                    "日期下界非法（应为 yyyy-MM-dd）: " + facet.dateFrom);
            }
            if (!facet.dateTo.empty() && !isValidCalendarDate(facet.dateTo)) {
                throw ValidationException(
                    "日期上界非法（应为 yyyy-MM-dd）: " + facet.dateTo);
            }
            if (!facet.dateFrom.empty() && !facet.dateTo.empty() &&
                facet.dateFrom >= facet.dateTo) {
                // 闭开区间要求 from < to，等值或倒置都没有意义
                throw ValidationException(
                    "日期区间倒置，要求 from < to（闭开区间 [" +
                    facet.dateFrom + ", " + facet.dateTo + ")）");
            }
        } else {
            std::set<std::string> dedup;
            for (const auto& value : facet.selected) {
                if (value.empty()) {
                    throw ValidationException(
                        "分面 " + facet.field + " 存在空的已选值");
                }
                if (!dedup.insert(value).second) {
                    throw ValidationException(
                        "分面 " + facet.field + " 存在重复的已选值: " + value);
                }
            }
        }
    }

    if (request.from < 0) {
        throw ValidationException("分页 from 不能为负");
    }
    if (request.size <= 0) {
        throw ValidationException("分页 size 必须为正数");
    }
    if (static_cast<long>(request.from) + request.size > request.maxResultWindow) {
        throw ValidationException(
            "分页超出允许范围: from + size = " +
            std::to_string(request.from + request.size) +
            " > " + std::to_string(request.maxResultWindow));
    }
}

FacetedSearchResult ESClient::parseFacetedResponse(
        const json& response,
        const FacetedSearchRequest& request) const {
    FacetedSearchResult result;
    result.took = response.value("took", 0);
    result.timedOut = response.value("timed_out", false);

    const auto& hitsNode = response["hits"];
    const auto& total = hitsNode["total"];
    result.total = total.is_object() ? total.value("value", 0)
                                     : total.get<int>();
    // ES 在无命中时返回 max_score: null
    result.maxScore = hitsNode["max_score"].is_number()
                          ? hitsNode["max_score"].get<double>()
                          : 0.0;

    for (const auto& hit : hitsNode["hits"]) {
        SearchHit searchHit;
        searchHit.id = hit.value("_id", "");
        searchHit.index = hit.value("_index", "");
        searchHit.score = hit["_score"].is_number()
                              ? hit["_score"].get<double>()
                              : 0.0;
        searchHit.source = hit.value("_source", json::object());
        searchHit.highlight = hit.value("highlight", json::object());
        result.hits.push_back(std::move(searchHit));
    }

    // 响应中的聚合键固定为 aggregations（请求体里才可简写为 aggs）
    static const json emptyObject = json::object();
    const json& aggregations =
        response.contains("aggregations") && response["aggregations"].is_object()
            ? response["aggregations"]
            : emptyObject;
    for (const auto& facet : request.facets) {
        FacetResult facetResult;
        facetResult.field = facet.field;
        facetResult.type = facet.type;

        const std::string aggName = facetAggName(facet.field);
        if (facet.type == FacetFieldType::Keyword) {
            std::set<std::string> returnedKeys;
            if (aggregations.contains(aggName) &&
                aggregations[aggName].contains(aggName)) {
                for (const auto& bucket :
                     aggregations[aggName][aggName]["buckets"]) {
                    FacetBucket b;
                    b.key = bucket.value("key", "");
                    b.docCount = bucket.value("doc_count", 0L);
                    returnedKeys.insert(b.key);
                    facetResult.buckets.push_back(std::move(b));
                }
            }
            // 已选但 ES 未返回的桶（doc_count=0 时 terms 聚合默认不返回），
            // 以零计数补齐
            for (const auto& selected : facet.selected) {
                if (!returnedKeys.count(selected)) {
                    FacetBucket b;
                    b.key = selected;
                    b.docCount = 0;
                    facetResult.buckets.push_back(std::move(b));
                }
            }
            // 稳定排序：docCount 降序，同数按 key 升序
            std::sort(facetResult.buckets.begin(), facetResult.buckets.end(),
                      [](const FacetBucket& a, const FacetBucket& b) {
                          if (a.docCount != b.docCount)
                              return a.docCount > b.docCount;
                          return a.key < b.key;
                      });
            std::set<std::string> selectedSet(facet.selected.begin(),
                                              facet.selected.end());
            for (auto& b : facetResult.buckets) {
                b.selected = selectedSet.count(b.key) > 0;
            }
        } else {
            // 日期分面：以作用域文档数作为单桶暴露
            FacetBucket b;
            b.key = facet.dateFrom + ".." + facet.dateTo;
            b.docCount = aggregations.contains(aggName)
                             ? aggregations[aggName].value("doc_count", 0L)
                             : 0L;
            b.selected = isFacetActive(facet);
            facetResult.buckets.push_back(std::move(b));
        }
        result.facets.push_back(std::move(facetResult));
    }

    return result;
}

FacetedSearchResult ESClient::facetedSearch(const std::string& indexName,
                                            const FacetedSearchRequest& request) {
    validateFacetedRequest(request);

    // 顶层 query 只承载关键词：它同时约束命中列表与所有聚合；
    // 无关键词时为 match_all（分面浏览/无关键词场景）。
    json body = {
        {"query", request.keyword.empty()
                      ? json{{"match_all", json::object()}}
                      : buildKeywordClause(request.keyword,
                                           request.keywordFields)},
        {"from", request.from},
        {"size", request.size},
        {"track_total_hits", true}
    };

    // post_filter 承载全部分面条件，只过滤命中列表、不影响聚合；
    // 各分面聚合内再用 filter 排除自身、保留其他分面。
    bool anyActiveFacet = false;
    for (const auto& facet : request.facets) {
        if (isFacetActive(facet)) { anyActiveFacet = true; break; }
    }
    if (anyActiveFacet) {
        body["post_filter"] = buildScopeQuery(request, "");
    }

    if (request.highlight) {
        body["highlight"] = {
            {"pre_tags", {"<em>"}},
            {"post_tags", {"</em>"}},
            {"fields", {
                {"title", json::object()},
                {"content", json::object()}
            }}
        };
    }

    if (!request.facets.empty()) {
        json aggregations = json::object();
        for (const auto& facet : request.facets) {
            aggregations[facetAggName(facet.field)] =
                buildFacetAggregation(facet, request);
        }
        body["aggs"] = std::move(aggregations);
    }

    auto response = httpClient_.post(
        buildUrl("/" + indexName + "/_search"),
        body.dump()
    );
    if (!response.isSuccess()) {
        throw ESException("Faceted search failed: " + response.body);
    }

    return parseFacetedResponse(json::parse(response.body), request);
}

} // namespace es
