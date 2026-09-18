#ifndef ES_CLIENT_HPP
#define ES_CLIENT_HPP

#include "http_client.hpp"
#include "json.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>

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
 * 分面类型
 */
enum class FacetType {
    Terms,     // 词条分面：keyword 字段，多选值按 OR 组合
    DateRange  // 日期分面：date 字段，按闭开区间 [from, to) 过滤
};

/**
 * 分面定义：声明需要对哪个字段统计可选数量
 */
struct FacetDef {
    std::string field;  // 分面字段（须在允许的分面字段白名单内）
    int size = 10;      // 返回桶数量上限
};

/**
 * 词条分面选择：同一分面内多个选中值按 OR 解释
 */
struct FacetSelection {
    std::string field;               // 分面字段
    std::vector<std::string> values; // 选中的值（OR）
};

/**
 * 日期范围选择：闭开区间 [from, to)，格式 yyyy-MM-dd，空串表示该侧不限
 */
struct DateRangeSelection {
    std::string field; // 日期分面字段
    std::string from;  // 起始日期（含）
    std::string to;    // 结束日期（不含）
};

/**
 * 分面搜索请求
 *
 * 语义：
 * - 命中列表应用关键词与全部已选分面条件；
 * - 同一分面内多选按 OR，不同分面之间按 AND；
 * - 每个分面的桶在保留关键词和其他分面条件的同时排除自身过滤。
 */
struct FacetSearchRequest {
    std::string keyword;                            // 关键词，空表示不限（match_all）
    std::vector<std::string> keywordFields;         // 关键词检索字段，空则默认 title、content
    std::vector<FacetDef> facets;                   // 需要统计的分面
    std::vector<FacetSelection> selections;         // 词条分面已选值
    std::vector<DateRangeSelection> dateSelections; // 日期分面已选范围
    std::vector<std::string> highlightFields;       // 高亮字段（可空）
    int from = 0;                                   // 分页偏移
    int size = 10;                                  // 分页大小
};

/**
 * 分面桶
 */
struct FacetBucket {
    std::string key;       // 桶键名
    long count = 0;        // 该值在「排除本面过滤」条件下的可选数量
    bool selected = false; // 是否为当前已选值
};

/**
 * 单个分面的统计结果
 */
struct FacetResult {
    std::string field;
    FacetType type = FacetType::Terms;
    std::vector<FacetBucket> buckets; // 数量降序，同数按键名升序
};

/**
 * 分面搜索结果：命中列表 + 各分面统计
 */
struct FacetSearchResult {
    SearchResult search;             // 命中列表（应用关键词与全部已选条件）
    std::vector<FacetResult> facets; // 各分面统计（各自排除自身过滤）
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
     * 分面搜索：一次请求同时返回命中列表与各分面统计
     *
     * - 命中列表应用关键词与全部已选条件（同一分面内 OR，分面间 AND）；
     * - 每个分面的桶保留关键词与其他分面条件，仅排除自身过滤，
     *   因此已选分类不会把其他分类的计数压成零；
     * - 未出现在返回桶里的已选值以零计数保留；
     * - 非法字段、倒置日期范围、越界分页抛出 ESException，不会拼入查询。
     */
    FacetSearchResult facetSearch(const std::string& indexName,
                                  const FacetSearchRequest& request);

    /**
     * 通用搜索（自定义查询体）
     */
    SearchResult search(const std::string& indexName,
                        const json& queryBody);
    
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
};

} // namespace es

#endif // ES_CLIENT_HPP
