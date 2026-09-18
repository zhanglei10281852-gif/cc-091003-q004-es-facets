/**
 * 分面搜索集成测试
 *
 * 需要可访问的 Elasticsearch（ES_HOST / ES_PORT 环境变量，默认 localhost:9200）。
 * 覆盖场景：
 *   1. 多选组合（分面内 OR、分面间 AND、分面桶排除自身过滤）
 *   2. 零命中（筛选组合无结果，统计仍可用）
 *   3. 无关键词（空关键词浏览全库）
 *   4. 高亮与统计同时返回
 *   5. 已选值零计数保留
 *   6. 日期闭开区间边界 [gte, lt)
 *   7. 桶排序（数量降序、同数键名升序）
 *   8. 非法参数校验（非法字段 / 倒置日期 / 越界分页，不拼入查询）
 */
#include "es_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace es;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& message) {
    ++g_checks;
    if (cond) {
        std::cout << "    ✓ " << message << "\n";
    } else {
        ++g_failures;
        std::cout << "    ✗ FAIL: " << message << "\n";
    }
}

// 期望在校验阶段抛出 ESException（非法参数不得拼入查询）
void checkThrows(const std::function<void()>& fn, const std::string& message) {
    ++g_checks;
    try {
        fn();
        ++g_failures;
        std::cout << "    ✗ FAIL: " << message << "（期望抛出异常，实际未抛出）\n";
    } catch (const ESException&) {
        std::cout << "    ✓ " << message << "\n";
    }
}

// ==================== 测试数据 ====================

json testMappings() {
    return {
        {"properties", {
            {"title", {
                {"type", "text"},
                {"analyzer", "standard"},
                {"fields", {{"keyword", {{"type", "keyword"}}}}}
            }},
            {"content", {{"type", "text"}, {"analyzer", "standard"}}},
            {"author", {{"type", "keyword"}}},
            {"category", {{"type", "keyword"}}},
            {"tags", {{"type", "keyword"}}},
            {"created_at", {{"type", "date"}, {"format", "yyyy-MM-dd"}}}
        }}
    };
}

std::vector<json> testArticles() {
    return {
        {{"title", "人工智能基础"}, {"content", "人工智能与机器学习入门"},
         {"author", "张三"}, {"category", "技术"},
         {"tags", json::array({"AI", "机器学习"})}, {"created_at", "2024-01-15"}},
        {{"title", "深度学习实战"}, {"content", "深度学习与神经网络"},
         {"author", "张三"}, {"category", "技术"},
         {"tags", json::array({"AI", "深度学习"})}, {"created_at", "2024-02-01"}},
        {{"title", "Elasticsearch 实战"}, {"content", "搜索引擎与全文检索"},
         {"author", "李四"}, {"category", "技术"},
         {"tags", json::array({"Elasticsearch", "搜索"})}, {"created_at", "2024-02-20"}},
        {{"title", "C++17 详解"}, {"content", "C++ 新特性"},
         {"author", "李四"}, {"category", "编程语言"},
         {"tags", json::array({"C++", "编程"})}, {"created_at", "2024-03-10"}},
        {{"title", "微服务架构"}, {"content", "微服务与分布式系统"},
         {"author", "王五"}, {"category", "架构"},
         {"tags", json::array({"微服务", "分布式"})}, {"created_at", "2024-04-05"}},
        {{"title", "领域驱动设计"}, {"content", "架构设计方法论"},
         {"author", "张三"}, {"category", "架构"},
         {"tags", json::array({"架构", "DDD"})}, {"created_at", "2024-05-18"}},
    };
}

// ==================== 辅助函数 ====================

FacetSearchRequest baseRequest() {
    FacetSearchRequest req;
    req.facets = {{"category", 10}, {"author", 10}, {"tags", 20}, {"created_at", 20}};
    return req;
}

const FacetResult* findFacet(const FacetSearchResult& result, const std::string& field) {
    for (const auto& facet : result.facets) {
        if (facet.field == field) {
            return &facet;
        }
    }
    return nullptr;
}

const FacetBucket* findBucket(const FacetResult& facet, const std::string& key) {
    for (const auto& bucket : facet.buckets) {
        if (bucket.key == key) {
            return &bucket;
        }
    }
    return nullptr;
}

std::vector<std::string> bucketKeys(const FacetResult& facet) {
    std::vector<std::string> keys;
    for (const auto& bucket : facet.buckets) {
        keys.push_back(bucket.key);
    }
    return keys;
}

std::set<std::string> hitIds(const FacetSearchResult& result) {
    std::set<std::string> ids;
    for (const auto& hit : result.search.hits) {
        ids.insert(hit.id);
    }
    return ids;
}

// ==================== 用例 ====================

// 用例 1：多选组合——分面内 OR、分面间 AND、分面桶排除自身过滤
void testMultiSelectCombination(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 1] 多选组合：分面内 OR、分面间 AND\n";

    // 1a. 同一分面多选按 OR 解释
    {
        auto req = baseRequest();
        req.selections = {{"category", {"技术", "架构"}}};
        auto res = client.facetSearch(index, req);
        check(res.search.total == 5, "分类∈{技术,架构} 按 OR 命中 5 条");
    }

    // 1b. 跨分面 AND + 分面桶排除自身过滤
    {
        auto req = baseRequest();
        req.selections = {{"category", {"技术", "架构"}}, {"author", {"张三"}}};
        auto res = client.facetSearch(index, req);

        check(res.search.total == 3, "分类∈{技术,架构} AND 作者=张三 命中 3 条");

        bool allMatch = !res.search.hits.empty();
        for (const auto& hit : res.search.hits) {
            const auto category = hit.source["category"].get<std::string>();
            const auto author = hit.source["author"].get<std::string>();
            if ((category != "技术" && category != "架构") || author != "张三") {
                allMatch = false;
            }
        }
        check(allMatch, "命中文档全部满足已选条件");

        // category 分面：排除自身过滤，保留 作者=张三 → 张三的文档中 技术2 架构1
        const auto* category = findFacet(res, "category");
        check(category != nullptr, "返回 category 分面统计");
        if (category) {
            const auto* tech = findBucket(*category, "技术");
            const auto* arch = findBucket(*category, "架构");
            check(tech && tech->count == 2 && tech->selected,
                  "category 分面排除自身过滤：技术(2) 且标记已选");
            check(arch && arch->count == 1 && arch->selected,
                  "category 分面排除自身过滤：架构(1) 且标记已选");
            check(findBucket(*category, "编程语言") == nullptr,
                  "未选中且无匹配文档的值不出现（编程语言）");
        }

        // author 分面：排除自身过滤，保留 分类∈{技术,架构} → 张三3 李四1 王五1
        const auto* author = findFacet(res, "author");
        check(author != nullptr, "返回 author 分面统计");
        if (author) {
            check(author->buckets.size() == 3, "author 分面桶数量为 3");
            if (author->buckets.size() == 3) {
                check(author->buckets[0].key == "张三" && author->buckets[0].count == 3 &&
                          author->buckets[0].selected,
                      "author 分面排除自身过滤：张三(3) 且标记已选");
                check(author->buckets[1].key == "李四" && author->buckets[1].count == 1,
                      "author 分面：李四(1)");
                check(author->buckets[2].key == "王五" && author->buckets[2].count == 1,
                      "author 分面同数按键名排序：王五(1) 排在李四(1) 之后");
            }
        }
    }
}

// 用例 2：零命中——筛选组合无结果，分面统计仍可用
void testZeroHits(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 2] 零命中：筛选组合无结果，统计仍可用\n";

    auto req = baseRequest();
    req.selections = {{"category", {"编程语言"}}, {"author", {"王五"}}};
    auto res = client.facetSearch(index, req);

    check(res.search.total == 0, "总命中为 0");
    check(res.search.hits.empty(), "命中列表为空");

    // category 分面：排除自身过滤，保留 作者=王五 → 架构(1)；已选「编程语言」零计数保留
    const auto* category = findFacet(res, "category");
    check(category != nullptr, "零命中时仍返回 category 分面");
    if (category) {
        const auto* arch = findBucket(*category, "架构");
        const auto* lang = findBucket(*category, "编程语言");
        check(arch && arch->count == 1, "category 分面：架构(1)（王五的文章）");
        check(lang && lang->count == 0 && lang->selected,
              "已选值「编程语言」以零计数保留且标记已选");
    }

    // author 分面：排除自身过滤，保留 分类=编程语言 → 李四(1)；已选「王五」零计数保留
    const auto* author = findFacet(res, "author");
    check(author != nullptr, "零命中时仍返回 author 分面");
    if (author) {
        const auto* lisi = findBucket(*author, "李四");
        const auto* wangwu = findBucket(*author, "王五");
        check(lisi && lisi->count == 1, "author 分面：李四(1)（编程语言分类下）");
        check(wangwu && wangwu->count == 0 && wangwu->selected,
              "已选值「王五」以零计数保留且标记已选");
    }
}

// 用例 3：无关键词——空关键词浏览全库
void testNoKeyword(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 3] 无关键词：浏览全库并统计\n";

    // 3a. 完全无关键词、无筛选
    {
        auto req = baseRequest();
        auto res = client.facetSearch(index, req);

        check(res.search.total == 6, "无关键词命中全部 6 篇");

        const auto* category = findFacet(res, "category");
        check(category != nullptr && category->buckets.size() == 3,
              "category 分面覆盖全部 3 个分类");
        if (category && category->buckets.size() == 3) {
            check(category->buckets[0].key == "技术" && category->buckets[0].count == 3,
                  "category 分面：技术(3)");
            check(category->buckets[1].key == "架构" && category->buckets[1].count == 2,
                  "category 分面：架构(2)");
            check(category->buckets[2].key == "编程语言" && category->buckets[2].count == 1,
                  "category 分面：编程语言(1)");
        }

        const auto* author = findFacet(res, "author");
        check(author != nullptr && author->buckets.size() == 3,
              "author 分面覆盖全部 3 位作者");
        if (author && author->buckets.size() == 3) {
            check(author->buckets[0].key == "张三" && author->buckets[0].count == 3,
                  "author 分面：张三(3)");
            check(author->buckets[1].key == "李四" && author->buckets[1].count == 2,
                  "author 分面：李四(2)");
            check(author->buckets[2].key == "王五" && author->buckets[2].count == 1,
                  "author 分面：王五(1)");
        }
    }

    // 3b. 无关键词但有筛选
    {
        auto req = baseRequest();
        req.selections = {{"category", {"技术"}}};
        auto res = client.facetSearch(index, req);
        check(res.search.total == 3, "无关键词 + 分类=技术 命中 3 条");
    }
}

// 用例 4：高亮与统计同时返回
void testHighlightWithFacets(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 4] 高亮与统计同时返回\n";

    auto req = baseRequest();
    req.keyword = "神经网络";
    req.selections = {{"category", {"技术"}}};
    req.highlightFields = {"title", "content"};
    auto res = client.facetSearch(index, req);

    check(res.search.total == 1, "关键词「神经网络」+ 分类=技术 命中 1 条");

    bool hasEmFragment = false;
    for (const auto& hit : res.search.hits) {
        for (const auto& item : hit.highlight.items()) {
            for (const auto& fragment : item.value()) {
                if (fragment.get<std::string>().find("<em>") != std::string::npos) {
                    hasEmFragment = true;
                }
            }
        }
    }
    check(hasEmFragment, "命中包含 <em> 高亮片段");

    const auto* category = findFacet(res, "category");
    check(category != nullptr, "高亮结果同时返回 category 分面");
    if (category) {
        const auto* tech = findBucket(*category, "技术");
        check(tech && tech->count == 1 && tech->selected,
              "category 分面：技术(1) 且标记已选");
    }

    const auto* author = findFacet(res, "author");
    check(author != nullptr, "高亮结果同时返回 author 分面");
    if (author) {
        const auto* zhangsan = findBucket(*author, "张三");
        check(zhangsan && zhangsan->count == 1, "author 分面：张三(1)");
    }
}

// 用例 5：已选值零计数保留（已选值在其他分面条件下无文档）
void testSelectedValueZeroCount(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 5] 已选值零计数保留\n";

    auto req = baseRequest();
    req.selections = {{"category", {"技术"}}, {"author", {"王五"}}};
    auto res = client.facetSearch(index, req);

    check(res.search.total == 0, "分类=技术 AND 作者=王五 无命中");

    const auto* category = findFacet(res, "category");
    check(category != nullptr, "返回 category 分面");
    if (category) {
        const auto* tech = findBucket(*category, "技术");
        const auto* arch = findBucket(*category, "架构");
        check(tech && tech->count == 0 && tech->selected,
              "已选「技术」在作者=王五条件下以零计数保留");
        check(arch && arch->count == 1 && !arch->selected,
              "category 分面仍提示可选：架构(1)");
    }

    const auto* author = findFacet(res, "author");
    check(author != nullptr, "返回 author 分面");
    if (author) {
        const auto* wangwu = findBucket(*author, "王五");
        check(wangwu && wangwu->count == 0 && wangwu->selected,
              "已选「王五」在分类=技术条件下以零计数保留");
        const auto* zhangsan = findBucket(*author, "张三");
        check(zhangsan && zhangsan->count == 2, "author 分面仍提示可选：张三(2)");
    }
}

// 用例 6：日期闭开区间边界 [gte, lt)
void testDateRangeClosedOpen(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 6] 日期闭开区间 [gte, lt)\n";

    // 6a. [2024-02-01, 2024-03-01)：含下界 02-01，不含上界 03-01
    {
        auto req = baseRequest();
        req.dateSelections = {{"created_at", "2024-02-01", "2024-03-01"}};
        auto res = client.facetSearch(index, req);

        check(res.search.total == 2, "[2024-02-01, 2024-03-01) 命中 2 条");
        check(hitIds(res) == std::set<std::string>({"t2", "t3"}),
              "下界闭：2024-02-01 当天的文章(t2)被包含");

        // 日期分面排除自身过滤 → 全部 6 个日期桶都在
        const auto* dateFacet = findFacet(res, "created_at");
        check(dateFacet != nullptr && dateFacet->buckets.size() == 6,
              "日期分面排除自身过滤：返回全部 6 个日期桶");
        if (dateFacet) {
            const auto* may = findBucket(*dateFacet, "2024-05-18");
            check(may && may->count == 1,
                  "范围外的 2024-05-18 仍出现在日期分面中（排除自身过滤）");
        }
    }

    // 6b. [2024-01-01, 2024-02-01)：上界开，排除 02-01 当天
    {
        auto req = baseRequest();
        req.dateSelections = {{"created_at", "2024-01-01", "2024-02-01"}};
        auto res = client.facetSearch(index, req);

        check(res.search.total == 1, "[2024-01-01, 2024-02-01) 命中 1 条");
        check(hitIds(res) == std::set<std::string>({"t1"}),
              "上界开：2024-02-01 当天的文章(t2)被排除");
    }

    // 6c. 单边范围：>= 2024-05-01
    {
        auto req = baseRequest();
        req.dateSelections = {{"created_at", "2024-05-01", ""}};
        auto res = client.facetSearch(index, req);

        check(res.search.total == 1, ">=2024-05-01 命中 1 条");
        check(hitIds(res) == std::set<std::string>({"t6"}), "命中 t6");
    }
}

// 用例 7：桶排序——数量降序，同数按键名升序
void testBucketOrdering(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 7] 桶排序：数量降序，同数键名升序\n";

    auto req = baseRequest();
    auto res = client.facetSearch(index, req);

    const auto* tags = findFacet(res, "tags");
    check(tags != nullptr, "返回 tags 分面");
    if (tags) {
        // AI(2) 居首；其余 10 个标签各 1 次，按键名（UTF-8 字节序）升序
        const std::vector<std::string> expected = {
            "AI", "C++", "DDD", "Elasticsearch",
            "分布式", "微服务", "搜索", "机器学习", "架构", "深度学习", "编程"
        };
        const auto actual = bucketKeys(*tags);
        check(actual == expected, "tags 分面桶顺序：AI(2) 在前，同数按键名升序");
        if (actual != expected) {
            std::cout << "      实际顺序: ";
            for (const auto& key : actual) {
                std::cout << key << " ";
            }
            std::cout << "\n";
        }
    }
}

// 用例 8：非法参数校验——非法字段、倒置日期、越界分页不得拼入查询
void testValidation(ESClient& client, const std::string& index) {
    std::cout << "\n[用例 8] 非法参数校验（不拼入查询）\n";

    checkThrows([&] {
        auto req = baseRequest();
        req.facets.push_back({"unknown_field", 10});
        client.facetSearch(index, req);
    }, "非法分面字段被拒绝");

    checkThrows([&] {
        FacetSearchRequest req;
        req.facets = {{"title", 10}};
        client.facetSearch(index, req);
    }, "text 字段不能作为分面字段");

    checkThrows([&] {
        FacetSearchRequest req;
        req.facets = {{"category", 10}};
        req.selections = {{"author", {"张三"}}};
        client.facetSearch(index, req);
    }, "未声明分面上的选择被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.selections = {{"created_at", {"2024-01-15"}}};
        client.facetSearch(index, req);
    }, "日期分面不能使用词条选择");

    checkThrows([&] {
        auto req = baseRequest();
        req.dateSelections = {{"category", "2024-01-01", "2024-02-01"}};
        client.facetSearch(index, req);
    }, "词条分面不能使用日期范围选择");

    checkThrows([&] {
        auto req = baseRequest();
        req.dateSelections = {{"created_at", "2024-05-01", "2024-01-01"}};
        client.facetSearch(index, req);
    }, "倒置日期范围被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.dateSelections = {{"created_at", "2024/01/01", ""}};
        client.facetSearch(index, req);
    }, "非法日期格式被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.dateSelections = {{"created_at", "", ""}};
        client.facetSearch(index, req);
    }, "上下界均为空的日期范围被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.from = -1;
        client.facetSearch(index, req);
    }, "from < 0 被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.size = 0;
        client.facetSearch(index, req);
    }, "size = 0 被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.size = 500;
        client.facetSearch(index, req);
    }, "size 超出上限被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.from = 9990;
        req.size = 20;
        client.facetSearch(index, req);
    }, "from + size 超出结果窗口被拒绝");

    checkThrows([&] {
        auto req = baseRequest();
        req.keywordFields = {"content", "password"};
        client.facetSearch(index, req);
    }, "非法关键词字段被拒绝");
}

} // namespace

int main() {
    const char* esHost = std::getenv("ES_HOST");
    const char* esPort = std::getenv("ES_PORT");
    const std::string host = esHost ? esHost : "localhost";
    const int port = esPort ? std::stoi(esPort) : 9200;

    std::cout << "========================================\n";
    std::cout << "  分面搜索集成测试\n";
    std::cout << "========================================\n";
    std::cout << "连接 Elasticsearch: " << host << ":" << port << "\n";

    ESClient client(host, port);

    std::cout << "等待 Elasticsearch 就绪";
    int retries = 30;
    while (!client.ping() && retries > 0) {
        std::cout << "." << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        --retries;
    }
    std::cout << "\n";
    if (retries == 0) {
        std::cerr << "无法连接到 Elasticsearch，请确保服务已启动\n";
        return 1;
    }

    const std::string index = "articles_facet_test";

    // 准备测试索引
    try {
        if (client.indexExists(index)) {
            client.deleteIndex(index);
        }
        client.createIndex(index, testMappings(),
                           {{"number_of_shards", 1}, {"number_of_replicas", 0}});
        auto bulk = client.bulkIndex(index, testArticles(),
                                     {"t1", "t2", "t3", "t4", "t5", "t6"});
        if (bulk.failCount > 0) {
            std::cerr << "导入测试数据失败\n";
            return 1;
        }
        client.refreshIndex(index);
        std::cout << "测试索引 '" << index << "' 就绪（6 篇文档）\n";
    } catch (const std::exception& e) {
        std::cerr << "准备测试索引失败: " << e.what() << "\n";
        return 1;
    }

    // 逐个执行用例，单个用例的未预期异常不影响其他用例
    const std::vector<std::pair<std::string, std::function<void()>>> suites = {
        {"多选组合", [&] { testMultiSelectCombination(client, index); }},
        {"零命中", [&] { testZeroHits(client, index); }},
        {"无关键词", [&] { testNoKeyword(client, index); }},
        {"高亮与统计同时返回", [&] { testHighlightWithFacets(client, index); }},
        {"已选值零计数保留", [&] { testSelectedValueZeroCount(client, index); }},
        {"日期闭开区间", [&] { testDateRangeClosedOpen(client, index); }},
        {"桶排序", [&] { testBucketOrdering(client, index); }},
        {"非法参数校验", [&] { testValidation(client, index); }},
    };

    for (const auto& suite : suites) {
        try {
            suite.second();
        } catch (const std::exception& e) {
            ++g_checks;
            ++g_failures;
            std::cout << "    ✗ FAIL: 用例「" << suite.first
                      << "」抛出未预期异常: " << e.what() << "\n";
        }
    }

    // 清理
    try {
        client.deleteIndex(index);
    } catch (const std::exception&) {
        // 清理失败不影响测试结果
    }

    std::cout << "\n========================================\n";
    std::cout << "共 " << g_checks << " 项断言，失败 " << g_failures << " 项\n";
    if (g_failures == 0) {
        std::cout << "全部通过 ✓\n";
        return 0;
    }
    std::cout << "存在失败 ✗\n";
    return 1;
}
