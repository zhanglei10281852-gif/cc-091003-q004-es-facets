// 分面搜索集成测试
//
// 对内存版 Elasticsearch 测试替身（tests/mock_es_server.py）做端到端验证：
// 真实 HTTP 链路（libcurl）→ 真实 DSL 拼包 → 服务端求值 → 响应解析。
//
// 覆盖：
//   1. 关键词 + 同组多选 OR + 跨分面 AND，命中/分面/高亮同一请求返回
//   2. 零命中时已选值零计数保留
//   3. 无关键词（match_all）+ 同数桶键名稳定排序
//   4. 日期闭开边界 [gte, lt)
//   5. 非法字段 / 倒置日期 / 越界分页在客户端被拒绝且不产生请求
//   6. 发往 ES 的 DSL 结构（命中含全部过滤，分面桶排除自身过滤）
//
// 用法: integration_test <es_host> <es_port> <sample_data.json>

#include "es_client.hpp"
#include "http_client.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace es;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, message)                                              \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::cout << "  [FAIL] " << (message) << "\n"                 \
                      << "         at " << __FILE__ << ":" << __LINE__    \
                      << "\n";                                            \
        }                                                                 \
    } while (0)

#define SECTION(title) std::cout << "\n== " << title << " ==\n"

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static const FacetResult* findFacet(const FacetedSearchResult& r,
                                    const std::string& field) {
    for (const auto& f : r.facets) {
        if (f.field == field) return &f;
    }
    return nullptr;
}

static const FacetBucket* findBucket(const FacetResult& f,
                                     const std::string& key) {
    for (const auto& b : f.buckets) {
        if (b.key == key) return &b;
    }
    return nullptr;
}

static json latestRequestBody(HttpClient& http, int port) {
    auto resp = http.get("http://127.0.0.1:" + std::to_string(port) +
                         "/__debug/requests");
    auto parsed = json::parse(resp.body);
    return parsed["requests"].back();
}

static long countRecordedRequests(HttpClient& http, int port) {
    auto resp = http.get("http://127.0.0.1:" + std::to_string(port) +
                         "/__debug/requests");
    return json::parse(resp.body)["requests"].size();
}

static void loadData(ESClient& client, const std::string& index,
                     const std::string& dataFile) {
    if (client.indexExists(index)) client.deleteIndex(index);

    json mappings = {
        {"properties", {
            {"title",    {{"type", "text"}, {"analyzer", "standard"},
                          {"fields", {{"keyword", {{"type", "keyword"}}}}}}},
            {"content",  {{"type", "text"}, {"analyzer", "standard"}}},
            {"author",   {{"type", "keyword"}}},
            {"category", {{"type", "keyword"}}},
            {"tags",     {{"type", "keyword"}}},
            {"created_at", {{"type", "date"}, {"format", "yyyy-MM-dd"}}}
        }}
    };
    json settings = {{"number_of_shards", 1}, {"number_of_replicas", 0}};
    client.createIndex(index, mappings, settings);

    auto docs = json::parse(readFile(dataFile));
    std::vector<json> articles = docs.get<std::vector<json>>();
    std::vector<std::string> ids;
    for (size_t i = 1; i <= articles.size(); ++i) {
        ids.push_back(std::to_string(i));
    }
    auto bulk = client.bulkIndex(index, articles, ids);
    CHECK(bulk.errors == false, "批量导入无错误");
    CHECK(bulk.successCount == static_cast<int>(articles.size()),
          "导入 5 篇文章");
    client.refreshIndex(index);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0]
                  << " <host> <port> <sample_data.json>\n";
        return 2;
    }
    const std::string host = argv[1];
    const int port = std::stoi(argv[2]);
    const std::string dataFile = argv[3];
    const std::string index = "articles_test";

    std::cout << "分面搜索集成测试 -> " << host << ":" << port << "\n";

    try {
        ESClient client(host, port);
        HttpClient httpProbe;
        loadData(client, index, dataFile);

        // ------------------------------------------------------------
        SECTION("1. 多选组合：关键词 + 同组 OR + 跨组 AND + 高亮 + 分面同回");
        // ------------------------------------------------------------
        // 关键词 "学习"（standard 单字切分）在 title/content 上仅命中文档 2
        FacetedSearchRequest req;
        req.keyword = "学习";  // 命中文档 1、2
        req.facets = {
            FacetSpec::keyword("category", {"技术"}),
            FacetSpec::keyword("author"),
            FacetSpec::keyword("tags", {"深度学习", "神经网络"}),
            FacetSpec::date("created_at", "2024-01-01", "2024-12-31")
        };
        auto r1 = client.facetedSearch(index, req);

        // 命中列表应用全部条件：关键词 AND 分类=技术 AND 标签∈{深度学习,神经网络}
        // 仅文档 2（深度学习入门指南）
        CHECK(r1.total == 1, "总命中为 1（全条件 AND）");
        CHECK(r1.hits.size() == 1, "当前页返回 1 条");
        CHECK(r1.hits[0].id == "2", "命中文档 id=2");
        CHECK(r1.hits[0].source["category"] == "技术", "命中分类为技术");

        // 高亮与统计在同一响应中返回
        CHECK(r1.hits[0].highlight.contains("content"),
              "命中带 content 高亮");
        bool highlighted = false;
        for (const auto& frag : r1.hits[0].highlight["content"]) {
            if (frag.get<std::string>().find("<em>学习</em>") !=
                std::string::npos) {
                highlighted = true;
            }
        }
        CHECK(highlighted, "高亮片段包含 <em>学习</em>");
        CHECK(r1.facets.size() == 4, "返回 4 个分面");

        // category 桶排除了自身分类过滤，仅受关键词+标签约束 → 只有 技术:1
        const auto* cat = findFacet(r1, "category");
        CHECK(cat && cat->buckets.size() == 1, "category 桶数量为 1");
        if (cat) {
            const auto* b = findBucket(*cat, "技术");
            CHECK(b && b->docCount == 1 && b->selected,
                  "category=技术 计数 1 且标记已选");
        }

        // tags 桶排除标签自身过滤（关键词+分类=技术 → 文档1、2）
        const auto* tags = findFacet(r1, "tags");
        CHECK(tags, "存在 tags 分面");
        if (tags) {
            const auto* dl = findBucket(*tags, "深度学习");
            const auto* nn = findBucket(*tags, "神经网络");
            const auto* tf = findBucket(*tags, "TensorFlow");
            CHECK(dl && dl->docCount == 1 && dl->selected,
                  "已选 tags=深度学习 计数 1 且标记已选");
            CHECK(nn && nn->docCount == 1 && nn->selected,
                  "已选 tags=神经网络 计数 1 且标记已选");
            CHECK(tf && tf->docCount == 1 && !tf->selected,
                  "未选 tags=TensorFlow 计数 1（排除自身过滤后可见）");
        }

        // author 桶受全部其他条件约束 → 李四:1
        const auto* authors = findFacet(r1, "author");
        const auto* ls = authors ? findBucket(*authors, "李四") : nullptr;
        CHECK(ls && ls->docCount == 1, "author=李四 计数 1");

        // 日期分面：闭开区间下作用域文档数
        const auto* dt = findFacet(r1, "created_at");
        CHECK(dt && dt->buckets.size() == 1 &&
              dt->buckets[0].docCount == 1 && dt->buckets[0].selected,
              "日期分面给出作用域文档数 1 且标记已选");

        // ------------------------------------------------------------
        SECTION("2. 零命中：已选值仍以零计数保留");
        // ------------------------------------------------------------
        FacetedSearchRequest req2;
        req2.keyword = "机器学习";  // 仅文档 2 的正文含该词
        req2.facets = {
            FacetSpec::keyword("category", {"编程语言"})
        };
        auto r2 = client.facetedSearch(index, req2);
        CHECK(r2.total == 0, "矛盾条件总命中为 0");
        CHECK(r2.hits.empty(), "零命中时 hits 为空");

        const auto* cat2 = findFacet(r2, "category");
        CHECK(cat2, "零命中仍返回 category 分面");
        if (cat2) {
            const auto* tech = findBucket(*cat2, "技术");
            const auto* arch = findBucket(*cat2, "架构");
            const auto* plang = findBucket(*cat2, "编程语言");
            // 桶排除自身分类过滤，仅保留关键词：standard 分词 OR 下关键词
            // 命中文档 1(计算机/科学 共享 机/学)、2、5(机制 共享 机)
            // → 技术:2、架构:1；已选的 编程语言 零计数保留，提示可切换
            CHECK(tech && tech->docCount == 2,
                  "category=技术 计数 2（排除自身过滤，提示可切换）");
            CHECK(arch && arch->docCount == 1,
                  "category=架构 计数 1（排除自身过滤后仍可见）");
            CHECK(plang && plang->docCount == 0 && plang->selected,
                  "已选 category=编程语言 零计数保留且标记已选");
            CHECK(cat2->buckets.size() == 3, "category 共 3 个桶（含零计数已选）");
            CHECK(cat2->buckets[2].key == "编程语言",
                  "零计数已选值排在有计数桶之后");
        }

        // 已选了一个全库都不存在的标签：命中为 0，但该值仍零计数保留
        FacetedSearchRequest req2b;
        req2b.keyword = "机器";
        req2b.facets = {FacetSpec::keyword("tags", {"不存在的标签"})};
        auto r2b = client.facetedSearch(index, req2b);
        CHECK(r2b.total == 0, "不存在标签使总命中为 0");
        const auto* tags2 = findFacet(r2b, "tags");
        if (tags2) {
            const auto* missing = findBucket(*tags2, "不存在的标签");
            CHECK(missing && missing->docCount == 0 && missing->selected,
                  "已选但不存在的标签零计数保留");
        }

        // ------------------------------------------------------------
        SECTION("3. 无关键词 + 桶计数降序/同数键名稳定排序");
        // ------------------------------------------------------------
        FacetedSearchRequest req3;  // keyword 为空
        req3.facets = {
            FacetSpec::keyword("category", {"技术", "编程语言"}),
            FacetSpec::keyword("tags")
        };
        auto r3 = client.facetedSearch(index, req3);
        CHECK(r3.total == 4, "无关键词时命中分类 技术|编程语言 共 4 篇");
        CHECK(r3.hits.size() == 4, "返回 4 条命中");

        const auto* cat3 = findFacet(r3, "category");
        if (cat3) {
            // 桶排除自身过滤 → 全库统计：技术3、编程语言1、架构1
            const auto* arch = findBucket(*cat3, "架构");
            const auto* plang = findBucket(*cat3, "编程语言");
            CHECK(arch && arch->docCount == 1, "category=架构 计数 1");
            CHECK(plang && plang->docCount == 1 && plang->selected,
                  "category=编程语言 计数 1 已选");
            CHECK(cat3->buckets[0].key == "技术" &&
                      cat3->buckets[0].docCount == 3,
                  "计数降序：技术(3) 排第一");
            // 同数 1：架构(E6 9E B6) < 编程语言(E7 BC 96) 按 UTF-8 字节序
            CHECK(cat3->buckets[1].key == "架构" &&
                      cat3->buckets[2].key == "编程语言",
                  "同数按键名 UTF-8 升序：架构 在 编程语言 前");
        }
        const auto* tags3 = findFacet(r3, "tags");
        if (tags3) {
            // tags 桶保留“其他分面”条件（category ∈ {技术,编程语言}，4 篇），
            // 排除标签自身 → 12 个标签计数全为 1，按 UTF-8 字节序稳定排序。
            // 架构类文档 5 的标签（微服务/架构/分布式）不应出现，
            // 正好验证桶统计保留了其他分面过滤。
            const std::vector<std::string> expected = {
                "AI", "C++", "C++17", "Elasticsearch", "TensorFlow",
                "人工智能", "全文检索", "搜索引擎", "机器学习",
                "深度学习", "神经网络", "编程"};
            std::vector<std::string> actual;
            for (const auto& b : tags3->buckets) actual.push_back(b.key);
            CHECK(actual == expected,
                  "tags 桶保留其他分面过滤（无架构类标签）且同数键名稳定排序");
        }

        // ------------------------------------------------------------
        SECTION("4. 日期闭开边界 [from, to)");
        // ------------------------------------------------------------
        FacetedSearchRequest req4;
        req4.facets = {
            FacetSpec::date("created_at", "2024-03-10", "2024-04-05")
        };
        auto r4 = client.facetedSearch(index, req4);
        // 文档 3 = 2024-03-10 含下界；文档 4 = 2024-04-05 不含上界
        CHECK(r4.total == 1, "闭开区间命中 1 篇");
        CHECK(r4.hits[0].id == "3", "恰好命中下界当天文档 id=3");

        FacetedSearchRequest req4b;
        req4b.facets = {FacetSpec::date("created_at", "", "2024-02-20")};
        auto r4b = client.facetedSearch(index, req4b);
        CHECK(r4b.total == 1 && r4b.hits[0].id == "1",
              "仅上界开区间：排除 02-20 当天，仅留 01-15 文档");

        // ------------------------------------------------------------
        SECTION("5. 非法输入在客户端被拒绝，不拼入查询发往 ES");
        // ------------------------------------------------------------
        long before = countRecordedRequests(httpProbe, port);

        auto expectValidation = [&](FacetedSearchRequest bad,
                                    const std::string& label) {
            bool threw = false;
            try {
                client.facetedSearch(index, bad);
            } catch (const ValidationException&) {
                threw = true;
            } catch (const std::exception& e) {
                std::cout << "  [FAIL] " << label
                          << " 抛出了非校验异常: " << e.what() << "\n";
                ++g_failures;
            }
            ++g_checks;
            if (!threw) {
                ++g_failures;
                std::cout << "  [FAIL] " << label << " 未被拒绝\n";
            }
        };

        FacetedSearchRequest bad;
        bad.facets = {FacetSpec::keyword("not_a_field; DROP", {"x"})};
        expectValidation(bad, "非法字段名");

        bad = FacetedSearchRequest{};
        bad.facets = {FacetSpec::keyword("title", {"x"})};
        expectValidation(bad, "text 字段不能做词项分面");

        bad = FacetedSearchRequest{};
        bad.facets = {FacetSpec::date("created_at", "2024-13-01", "2024-01-01")};
        expectValidation(bad, "非法日历日期");

        bad = FacetedSearchRequest{};
        bad.facets = {FacetSpec::date("created_at", "2024-02-30", "")};
        expectValidation(bad, "不存在的日期 2024-02-30");

        bad = FacetedSearchRequest{};
        bad.facets = {FacetSpec::date("created_at", "2024-05-01", "2024-05-01")};
        expectValidation(bad, "起止相等（闭开区间为空）");

        bad = FacetedSearchRequest{};
        bad.facets = {FacetSpec::date("created_at", "2024-05-01", "2024-01-01")};
        expectValidation(bad, "日期倒置");

        bad = FacetedSearchRequest{};
        bad.from = -1;
        expectValidation(bad, "负分页 from");

        bad = FacetedSearchRequest{};
        bad.size = 0;
        expectValidation(bad, "size 为 0");

        bad = FacetedSearchRequest{};
        bad.from = 9999;
        bad.size = 2;  // 10001 > 10000
        expectValidation(bad, "from+size 超出 result window");

        bad = FacetedSearchRequest{};
        bad.keywordFields = {"content; echo pwned"};
        expectValidation(bad, "非法关键词字段");

        long after = countRecordedRequests(httpProbe, port);
        CHECK(after == before, "所有非法请求均未发往 ES");

        // 合法的边界分页应放行
        FacetedSearchRequest edge;
        edge.from = 9995;
        edge.size = 5;  // 恰好 10000
        bool edgeOk = true;
        try {
            client.facetedSearch(index, edge);
        } catch (const std::exception&) {
            edgeOk = false;
        }
        CHECK(edgeOk, "from+size 恰好 10000 放行");

        // ------------------------------------------------------------
        SECTION("6. 发往 ES 的 DSL 结构");
        // ------------------------------------------------------------
        FacetedSearchRequest req6;
        req6.keyword = "学习";
        req6.facets = {
            FacetSpec::keyword("category", {"技术"}),
            FacetSpec::keyword("tags", {"深度学习", "神经网络"}),
            FacetSpec::date("created_at", "2024-01-01", "2024-06-01")
        };
        client.facetedSearch(index, req6);
        json sent = latestRequestBody(httpProbe, port);

        // 顶层 query 只承载关键词
        CHECK(sent["query"].contains("multi_match"),
              "顶层 query 为 multi_match 关键词");
        CHECK(sent["query"]["multi_match"]["query"] == "学习",
              "关键词位于顶层 query（对命中与所有聚合统一生效）");
        CHECK(sent["highlight"]["fields"].contains("content"),
              "请求体包含高亮配置");

        // post_filter 承载全部分面条件：3 个过滤，跨分面 AND
        CHECK(sent["post_filter"].contains("bool") &&
                      sent["post_filter"]["bool"].contains("filter"),
              "命中列表的分面条件放在 post_filter");
        const auto& qfilters = sent["post_filter"]["bool"]["filter"];
        CHECK(qfilters.is_array() && qfilters.size() == 3,
              "post_filter 含 3 个分面过滤（跨分面 AND）");
        bool hasCat = false, hasTagsOr = false, hasRange = false;
        for (const auto& f : qfilters) {
            if (f.contains("term") && f["term"].contains("category")) hasCat = true;
            if (f.contains("bool") && f["bool"].contains("should")) {
                hasTagsOr = f["bool"]["minimum_should_match"] == 1 &&
                            f["bool"]["should"].size() == 2;
            }
            if (f.contains("range") && f["range"].contains("created_at")) {
                const auto& r = f["range"]["created_at"];
                hasRange = r.contains("gte") && r.contains("lt") &&
                           !r.contains("lte");
            }
        }
        CHECK(hasCat, "post_filter 含 category 词项过滤");
        CHECK(hasTagsOr, "同组多选编码为 should(2) + minimum_should_match=1（OR）");
        CHECK(hasRange, "日期范围使用 gte + lt 闭开边界（无 lte）");

        // category 分面聚合：只保留其他分面（tags、日期），不含自身分类过滤；
        // 关键词无需重复写入——filter 聚合在外层 query（关键词）上下文内求值
        const auto& catAgg = sent["aggs"]["facet_category"];
        CHECK(!catAgg["filter"].contains("multi_match"),
              "分面聚合不重复关键词（由顶层 query 提供）");
        const auto& catAggFilter = catAgg["filter"]["bool"]["filter"];
        bool catExcluded = true, tagsKept = false, dateKept = false;
        for (const auto& f : catAggFilter) {
            if (f.contains("term") && f["term"].contains("category"))
                catExcluded = false;
            if (f.contains("bool") && f["bool"].contains("should")) tagsKept = true;
            if (f.contains("range")) dateKept = true;
        }
        CHECK(catExcluded, "category 桶统计排除自身分类过滤");
        CHECK(tagsKept && dateKept,
              "category 桶统计保留其他分面（tags、日期）过滤");

        // tags 分面聚合：保留 category 与日期，排除标签自身 OR
        const auto& tagsAggFilter =
            sent["aggs"]["facet_tags"]["filter"]["bool"]["filter"];
        bool tagsSelfExcluded = true, catInTags = false, dateInTags = false;
        for (const auto& f : tagsAggFilter) {
            if (f.contains("bool") && f["bool"].contains("should"))
                tagsSelfExcluded = false;
            if (f.contains("term") && f["term"].contains("category"))
                catInTags = true;
            if (f.contains("range")) dateInTags = true;
        }
        CHECK(tagsSelfExcluded, "tags 桶统计排除自身标签 OR 过滤");
        CHECK(catInTags && dateInTags,
              "tags 桶统计保留其他分面（category、日期）过滤");

        CHECK(sent["aggs"]["facet_tags"]["aggs"]["facet_tags"]["terms"]
                      ["order"][0]["_count"] == "desc",
              "terms 聚合显式 _count desc");
        CHECK(sent["aggs"]["facet_tags"]["aggs"]["facet_tags"]["terms"]
                      ["order"][1]["_key"] == "asc",
              "terms 聚合显式 _key asc 保证同数稳定");
        CHECK(sent["track_total_hits"] == true, "开启 track_total_hits");

        // ------------------------------------------------------------
        std::cout << "\n----------------------------------------\n";
        std::cout << "检查项: " << g_checks << "，失败: " << g_failures << "\n";
        if (g_failures == 0) {
            std::cout << "全部通过 ✓\n";
            return 0;
        }
        std::cout << "存在失败 ✗\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "测试发生异常: " << e.what() << "\n";
        return 1;
    }
}
