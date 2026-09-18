#ifndef ES_CLIENT_HPP
#define ES_CLIENT_HPP

#include "http_client.hpp"
#include "json.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <set>

namespace es {

using json = nlohmann::json;

/**
 * 搜索命中结果
 */
struct SearchHit {
    std::string id;
    std::string index;
    double score;
    json source;
    json highlight;
};

/**
 * 搜索结果
 */
struct SearchResult {
    int total;
    double maxScore;
    std::vector<SearchHit> hits;
    int took;  // 耗时（毫秒）
    bool timedOut;
};

// ==================== 分面搜索 ====================

/**
 * 分面字段允许的字段类型
 */
enum class FacetFieldType {
    Keyword,  ///< 词项分面（category / author / tags 等 keyword 字段）
    Date      ///< 日期范围分面（created_at 等 date 字段）
};

/**
 * 分面定义
 *
 * - Keyword 类型：按 field 做 terms 聚合，selected 为当前已勾选的词项
 * - Date 类型：以 dateFrom/dateTo 表示闭开区间 [dateFrom, dateTo)
 *   （yyyy-MM-dd，空串表示该侧不限制）
 */
struct FacetSpec {
    std::string field;
    FacetFieldType type = FacetFieldType::Keyword;
    std::vector<std::string> selected;  ///< Keyword: 已选词项（同组 OR）
    int size = 20;                      ///< Keyword: 最多返回的桶数

    /* Date 分面专用：闭开边界 [dateFrom, dateTo)，空串表示不限制 */
    std::string dateFrom;
    std::string dateTo;

    /// 词项分面（category / author / tags …），selected 为当前已勾选的值
    static FacetSpec keyword(std::string field,
                             std::vector<std::string> selected = {},
                             int size = 20) {
        return FacetSpec{std::move(field), FacetFieldType::Keyword,
                         std::move(selected), size, "", ""};
    }

    /// 日期范围分面，闭开区间 [from, to)，空串表示该侧不限制
    static FacetSpec date(std::string field,
                          std::string from = "",
                          std::string to = "") {
        return FacetSpec{std::move(field), FacetFieldType::Date,
                         {}, 0, std::move(from), std::move(to)};
    }
};

/**
 * 分面桶
 */
struct FacetBucket {
    std::string key;
    long docCount = 0;
    bool selected = false;  ///< 该值是否在当前筛选中（已选但计数为 0 时仍保留）
};

/**
 * 单个分面的统计结果
 */
struct FacetResult {
    std::string field;
    FacetFieldType type = FacetFieldType::Keyword;
    std::vector<FacetBucket> buckets;
};

/**
 * 分面搜索请求
 *
 * keyword 非空时在 keywordFields（默认 title/content）上做 multi_match；
 * 各分面的已选值同组 OR、跨组 AND，与关键词共同约束命中列表；
 * 每个分面的桶统计排除自身过滤、保留关键词与其他分面过滤（OR 多选下的
 * 标准做法）。
 */
struct FacetedSearchRequest {
    std::string keyword;                     ///< 搜索词，为空表示不限制
    std::vector<std::string> keywordFields;  ///< 关键词匹配字段，空则用 title/content
    std::vector<FacetSpec> facets;
    int from = 0;
    int size = 10;
    bool highlight = true;                   ///< 是否返回 title/content 高亮
    int maxResultWindow = 10000;             ///< 允许的 from + size 上限
};

/**
 * 分面搜索结果：一次请求同时拿到命中列表（含高亮）与各分面统计
 */
struct FacetedSearchResult {
    int total = 0;
    double maxScore = 0.0;
    std::vector<SearchHit> hits;
    std::vector<FacetResult> facets;
    int took = 0;
    bool timedOut = false;
};

/**
 * 文档操作结果
 */
struct DocResult {
    std::string id;
    std::string index;
    std::string result;  // created, updated, deleted
    int version;
    bool success;
};

/**
 * 批量操作结果
 */
struct BulkResult {
    int took;
    bool errors;
    std::vector<DocResult> items;
    int successCount;
    int failCount;
};

/**
 * Elasticsearch 客户端异常
 */
class ESException : public std::runtime_error {
public:
    explicit ESException(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * 请求参数校验异常（非法字段、日期、分页等；这类请求不会被拼入查询发往 ES）
 */
class ValidationException : public ESException {
public:
    explicit ValidationException(const std::string& message)
        : ESException(message) {}
};

/**
 * Elasticsearch 客户端类
 */
class ESClient {
public:
    /**
     * 构造函数
     * @param host ES 主机地址
     * @param port ES 端口
     */
    explicit ESClient(const std::string& host = "localhost", int port = 9200);
    ~ESClient();
    
    // ==================== 集群操作 ====================
    
    /**
     * 检查 ES 连接是否正常
     */
    bool ping();
    
    /**
     * 获取集群健康状态
     */
    json clusterHealth();
    
    /**
     * 获取集群信息
     */
    json clusterInfo();
    
    // ==================== 索引操作 ====================
    
    /**
     * 创建索引
     * @param indexName 索引名称
     * @param mappings 映射配置（可选）
     * @param settings 索引设置（可选）
     */
    bool createIndex(const std::string& indexName,
                     const json& mappings = json::object(),
                     const json& settings = json::object());
    
    /**
     * 删除索引
     */
    bool deleteIndex(const std::string& indexName);
    
    /**
     * 检查索引是否存在
     */
    bool indexExists(const std::string& indexName);
    
    /**
     * 获取索引信息
     */
    json getIndex(const std::string& indexName);
    
    /**
     * 刷新索引（使文档可搜索）
     */
    bool refreshIndex(const std::string& indexName);
    
    // ==================== 文档操作 ====================
    
    /**
     * 索引文档（添加或更新）
     * @param indexName 索引名称
     * @param doc 文档内容
     * @param id 文档 ID（可选，不指定则自动生成）
     */
    DocResult indexDocument(const std::string& indexName,
                            const json& doc,
                            const std::string& id = "");
    
    /**
     * 获取文档
     */
    std::optional<json> getDocument(const std::string& indexName,
                                    const std::string& id);
    
    /**
     * 更新文档
     */
    DocResult updateDocument(const std::string& indexName,
                             const std::string& id,
                             const json& doc);
    
    /**
     * 删除文档
     */
    bool deleteDocument(const std::string& indexName,
                        const std::string& id);
    
    /**
     * 批量索引文档
     */
    BulkResult bulkIndex(const std::string& indexName,
                         const std::vector<json>& docs,
                         const std::vector<std::string>& ids = {});
    
    // ==================== 搜索操作 ====================
    
    /**
     * Match 查询（分词匹配）
     */
    SearchResult matchSearch(const std::string& indexName,
                             const std::string& field,
                             const std::string& query,
                             int from = 0,
                             int size = 10);
    
    /**
     * Multi-Match 查询（多字段匹配）
     */
    SearchResult multiMatchSearch(const std::string& indexName,
                                  const std::vector<std::string>& fields,
                                  const std::string& query,
                                  int from = 0,
                                  int size = 10);
    
    /**
     * Term 查询（精确匹配）
     */
    SearchResult termSearch(const std::string& indexName,
                            const std::string& field,
                            const std::string& value,
                            int from = 0,
                            int size = 10);
    
    /**
     * Bool 组合查询
     */
    SearchResult boolSearch(const std::string& indexName,
                            const json& must = json::array(),
                            const json& should = json::array(),
                            const json& mustNot = json::array(),
                            const json& filter = json::array(),
                            int from = 0,
                            int size = 10);
    
    /**
     * 带高亮的搜索
     */
    SearchResult searchWithHighlight(const std::string& indexName,
                                     const json& query,
                                     const std::vector<std::string>& highlightFields,
                                     int from = 0,
                                     int size = 10);
    
    /**
     * 通用搜索（自定义查询体）
     */
    SearchResult search(const std::string& indexName,
                        const json& queryBody);

    /**
     * 分面搜索：一次请求返回命中列表（可带高亮）与各分面桶统计。
     *
     * 语义：
     *   - 同一分面内多选按 OR 解释，不同分面之间按 AND 组合；
     *   - 命中列表应用关键词与全部分面条件；
     *   - 每个分面的桶保留关键词与其他分面条件、排除自身过滤；
     *   - 日期分面使用闭开边界 [gte, lte)；
     *   - 桶按 docCount 降序、同数按键名升序稳定排序；
     *   - 已选但未出现在返回桶中的值以 0 计数保留。
     *
     * @throws ValidationException 字段非法、日期倒置/格式错误、分页越界等
     */
    FacetedSearchResult facetedSearch(const std::string& indexName,
                                      const FacetedSearchRequest& request);
    
    // ==================== 日志回调 ====================
    
    using LogCallback = std::function<void(const std::string&)>;
    
    /**
     * 设置日志回调
     */
    void setLogCallback(LogCallback callback);

private:
    std::string baseUrl_;
    HttpClient httpClient_;
    LogCallback logCallback_;
    
    void log(const std::string& message);
    std::string buildUrl(const std::string& path);
    SearchResult parseSearchResponse(const json& response);

    // ---- 分面搜索内部工具 ----
    static bool isValidFieldName(const std::string& name);
    static bool isValidCalendarDate(const std::string& s);
    static bool isFacetActive(const FacetSpec& facet);
    static std::string facetAggName(const std::string& field);
    json buildKeywordClause(const std::string& keyword,
                            const std::vector<std::string>& fields) const;
    json buildFacetFilter(const FacetSpec& facet) const;
    json buildScopeQuery(const FacetedSearchRequest& request,
                         const std::string& excludeField) const;
    json buildFacetAggregation(const FacetSpec& facet,
                               const FacetedSearchRequest& request) const;
    void validateFacetedRequest(const FacetedSearchRequest& request) const;
    FacetedSearchResult parseFacetedResponse(const json& response,
                                             const FacetedSearchRequest& request) const;
};

} // namespace es

#endif // ES_CLIENT_HPP
