#!/usr/bin/env python3
"""
内存版 Elasticsearch 测试替身（仅用于集成验证）。

实现本项目实际使用的 ES 8.x REST/DSL 子集：
  - GET  /                         集群信息
  - GET  /_cluster/health          健康状态
  - HEAD /{index}                  索引存在性
  - PUT  /{index}                  创建索引（mapping 仅记录）
  - DELETE /{index}                删除索引
  - POST /{index}/_refresh         刷新（空操作，立即可见）
  - POST /_bulk                    批量写入
  - POST /{index}/_doc/{id}        写入文档
  - GET  /{index}/_doc/{id}        读取文档
  - POST /{index}/_update/{id}     更新文档
  - DELETE /{index}/_doc/{id}      删除文档
  - POST /{index}/_search          搜索（match/multi_match/term/bool/range、
                                   高亮、from/size、filter+terms 聚合）
  - GET  /__debug/requests         测试用：返回最近收到的搜索请求体

DSL 语义与本项目相关的部分保持一致：
  - match/multi_match 按 standard 分词近似（拉丁词项 + CJK 单字），OR 匹配
  - bool 的 must / filter / should(minimum_should_match)
  - range 的 gte/lt 闭开边界（日期按 yyyy-MM-dd 字符串比较）
  - filter 聚合 + 内嵌 terms 聚合（tags 多值每值计一次），
    桶按 doc_count 降序、同数按 key(UTF-8) 升序
"""
import json
import re
import threading
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CJK = re.compile(r"[一-鿿]")
LATIN = re.compile(r"[A-Za-z0-9][A-Za-z0-9.+#]*")


def tokenize(text):
    """近似 standard analyzer：拉丁词项小写化，CJK 逐字切分。"""
    if not isinstance(text, str):
        return []
    tokens = [m.group(0).lower() for m in LATIN.finditer(text)]
    tokens.extend(ch for ch in text if CJK.match(ch))
    return tokens


class MockES:
    def __init__(self):
        self.lock = threading.Lock()
        self.indices = {}          # index -> {id: doc}
        self.mappings = {}         # index -> mapping dict
        self.request_log = []      # 最近收到的 _search 请求体

    # ---------------- 文档/索引管理 ----------------

    def create_index(self, index, body):
        self.indices[index] = {}
        self.mappings[index] = body.get("mappings", {}) if body else {}

    def delete_index(self, index):
        self.indices.pop(index, None)
        self.mappings.pop(index, None)

    def put_doc(self, index, doc_id, doc):
        self.indices.setdefault(index, {})[doc_id] = doc
        return doc_id or ("auto-%d" % (len(self.indices[index]) + 1))

    def bulk(self, body_lines):
        items = []
        lines = [l for l in body_lines.splitlines() if l.strip()]
        for i in range(0, len(lines), 2):
            action = json.loads(lines[i])
            doc = json.loads(lines[i + 1])
            kind, meta = next(iter(action.items()))
            index = meta.get("_index")
            doc_id = meta.get("_id")
            if doc_id is None:
                doc_id = "auto-%d" % (len(self.indices.setdefault(index, {})) + 1)
            self.indices.setdefault(index, {})[doc_id] = doc
            items.append({kind: {"_index": index, "_id": doc_id,
                                 "status": 201, "result": "created"}})
        return items

    # ---------------- 查询求值 ----------------

    def _field_values(self, doc, field):
        val = doc.get(field)
        if isinstance(val, list):
            return val
        return [val] if val is not None else []

    def _match(self, doc, field, text):
        wanted = set(tokenize(text))
        if not wanted:
            return False
        got = set(tokenize(str(doc.get(field, ""))))
        return bool(wanted & got)

    def _eval_query(self, query, doc):
        if not query:
            return True
        if "match_all" in query:
            return True
        if "match" in query:
            (field, text), = query["match"].items()
            return self._match(doc, field, text)
        if "multi_match" in query:
            mm = query["multi_match"]
            wanted = set(tokenize(mm["query"]))
            if not wanted:
                return False
            for field in mm.get("fields", []):
                if wanted & set(tokenize(str(doc.get(field, "")))):
                    return True
            return False
        if "term" in query:
            (field, value), = query["term"].items()
            return value in self._field_values(doc, field)
        if "range" in query:
            (field, cond), = query["range"].items()
            val = doc.get(field)
            if val is None:
                return False
            if "gte" in cond and val < cond["gte"]:
                return False
            if "gt" in cond and val <= cond["gt"]:
                return False
            if "lte" in cond and val > cond["lte"]:
                return False
            # 闭开边界：lt 不含上界
            if "lt" in cond and val >= cond["lt"]:
                return False
            return True
        if "bool" in query:
            b = query["bool"]
            if not all(self._eval_query(c, doc) for c in b.get("must", [])):
                return False
            if not all(self._eval_query(c, doc) for c in b.get("filter", [])):
                return False
            if any(self._eval_query(c, doc) for c in b.get("must_not", [])):
                return False
            should = b.get("should", [])
            if should:
                min_match = b.get("minimum_should_match", 1)
                hits = sum(1 for c in should if self._eval_query(c, doc))
                if hits < min_match:
                    return False
            return True
        raise ValueError("unsupported query: %s" % json.dumps(query, ensure_ascii=False))

    def _score(self, query, doc):
        """简单确定性评分：query 词项命中文档的去重数。"""
        try:
            if "multi_match" in query:
                mm = query["multi_match"]
                wanted = set(tokenize(mm["query"]))
                got = set()
                for f in mm.get("fields", []):
                    got |= set(tokenize(str(doc.get(f, ""))))
                return float(len(wanted & got))
            if "bool" in query:
                scores = [self._score(c, doc) for c in query["bool"].get("must", [])]
                return float(sum(scores))
        except Exception:
            return 1.0
        return 1.0

    # ---------------- 高亮 ----------------

    @staticmethod
    def _highlight_field(text, keyword):
        if not keyword:
            return None
        pattern = re.compile(re.escape(keyword), re.IGNORECASE)
        fragments = []
        pos = 0
        for m in pattern.finditer(text):
            start = max(0, m.start() - 12)
            end = min(len(text), m.end() + 12)
            fragment = text[start:end]
            fragment = pattern.sub(lambda mm: "<em>%s</em>" % mm.group(0), fragment)
            fragments.append(fragment)
            pos = m.end()
            if len(fragments) >= 2:
                break
        return fragments or None

    # ---------------- 聚合 ----------------

    def _terms_agg(self, docs, spec):
        field = spec["terms"]["field"]
        size = spec["terms"].get("size", 10)
        counts = defaultdict(int)
        for doc in docs:
            for value in set(self._field_values(doc, field)):  # 每文档每值一次
                if value is not None:
                    counts[str(value)] += 1
        buckets = [{"key": k, "doc_count": c} for k, c in counts.items()]
        # doc_count 降序，同数按 key(UTF-8) 升序
        buckets.sort(key=lambda b: (-b["doc_count"], b["key"].encode("utf-8")))
        return {"buckets": buckets[:size]}

    def _run_agg(self, docs, agg):
        out = {}
        if "filter" in agg:
            scoped = [d for d in docs if self._eval_query(agg["filter"], d)]
            out["doc_count"] = len(scoped)
        else:
            scoped = docs
        for name, sub in agg.get("aggs", {}).items():
            if "terms" in sub:
                out[name] = self._terms_agg(scoped, sub)
        return out

    # ---------------- 搜索 ----------------

    def search(self, index, body):
        with self.lock:
            self.request_log.append(body)
            del self.request_log[:-32]

        docs = list(self.indices.get(index, {}).items())
        query = body.get("query", {"match_all": {}})
        # 聚合在 query 上下文内求值（post_filter 之前）
        query_matched = [(doc_id, doc) for doc_id, doc in docs
                         if self._eval_query(query, doc)]

        # post_filter 只过滤命中列表，不影响聚合（faceted search 的标准做法）
        if "post_filter" in body:
            matched = [(doc_id, doc) for doc_id, doc in query_matched
                       if self._eval_query(body["post_filter"], doc)]
        else:
            matched = query_matched

        matched.sort(key=lambda kv: (-self._score(query, kv[1]),
                                     int(kv[0]) if kv[0].isdigit() else kv[0]))
        total = len(matched)

        from_ = body.get("from", 0)
        size = body.get("size", 10)
        page = matched[from_:from_ + size]

        hits = []
        max_score = 0.0
        hl_cfg = body.get("highlight")
        for doc_id, doc in page:
            score = self._score(query, doc)
            max_score = max(max_score, score)
            hit = {"_index": index, "_id": doc_id, "_score": score,
                   "_source": doc}
            if hl_cfg:
                keyword = body.get("query", {})
                kw = ""
                if "multi_match" in query:
                    kw = query["multi_match"]["query"]
                elif "bool" in query:
                    for c in query["bool"].get("must", []):
                        if "multi_match" in c:
                            kw = c["multi_match"]["query"]
                highlight = {}
                for field in hl_cfg.get("fields", {}):
                    frags = self._highlight_field(str(doc.get(field, "")), kw)
                    if frags:
                        highlight[field] = frags
                if highlight:
                    hit["highlight"] = highlight
            hits.append(hit)

        resp = {
            "took": 3,
            "timed_out": False,
            "_shards": {"total": 1, "successful": 1, "skipped": 0, "failed": 0},
            "hits": {
                "total": {"value": total, "relation": "eq"},
                "max_score": max_score if matched else None,
                "hits": hits,
            },
        }

        if "aggs" in body:
            agg_docs = [doc for _, doc in query_matched]
            resp["aggregations"] = {
                name: self._run_agg(agg_docs, agg)
                for name, agg in body["aggs"].items()
            }
        return resp


STATE = MockES()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, payload):
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def _parts(self):
        return [p for p in self.path.split("?")[0].split("/") if p]

    def do_GET(self):
        parts = self._parts()
        if not parts:
            return self._send(200, {"cluster_name": "mock-es",
                                    "version": {"number": "8.11.0"}})
        if parts == ["_cluster", "health"]:
            return self._send(200, {"status": "green", "number_of_nodes": 1})
        if parts == ["__debug", "requests"]:
            with STATE.lock:
                return self._send(200, {"requests": list(STATE.request_log)})
        if len(parts) == 3 and parts[1] == "_doc":
            doc = STATE.indices.get(parts[0], {}).get(parts[2])
            if doc is None:
                return self._send(404, {"found": False})
            return self._send(200, {"found": True, "_index": parts[0],
                                    "_id": parts[2], "_source": doc})
        if len(parts) == 1:
            if parts[0] in STATE.indices:
                return self._send(200, {parts[0]: {"mappings":
                                                   STATE.mappings.get(parts[0], {})}})
        return self._send(404, {"error": "not found: %s" % self.path})

    def do_HEAD(self):
        parts = self._parts()
        self.send_response(200 if len(parts) == 1 and parts[0] in STATE.indices
                           else 404)
        self.end_headers()

    def do_PUT(self):
        parts = self._parts()
        if len(parts) == 1:
            with STATE.lock:
                STATE.create_index(parts[0], self._body())
            return self._send(200, {"acknowledged": True})
        return self._send(404, {"error": "not found"})

    def do_DELETE(self):
        parts = self._parts()
        if len(parts) == 1:
            with STATE.lock:
                STATE.delete_index(parts[0])
            return self._send(200, {"acknowledged": True})
        if len(parts) == 3 and parts[1] == "_doc":
            with STATE.lock:
                existed = STATE.indices.get(parts[0], {}).pop(parts[2], None)
            return self._send(200, {"result": "deleted" if existed else "not_found"})
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        parts = self._parts()
        if parts == ["_bulk"]:
            length = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(length).decode("utf-8") if length else ""
            items = STATE.bulk(raw)
            return self._send(200, {"took": 5, "errors": False, "items": items})
        if len(parts) == 3 and parts[1] == "_update":
            index, doc_id = parts[0], parts[2]
            doc = STATE.indices.setdefault(index, {}).get(doc_id, {})
            doc.update(self._body().get("doc", {}))
            STATE.indices.setdefault(index, {})[doc_id] = doc
            return self._send(200, {"_id": doc_id, "result": "updated",
                                    "_version": 2})
        if len(parts) == 3 and parts[1] == "_doc":
            doc_id = STATE.put_doc(parts[0], parts[2], self._body())
            return self._send(201, {"_id": doc_id, "_index": parts[0],
                                    "result": "created", "_version": 1})
        if len(parts) == 2 and parts[1] == "_refresh":
            return self._send(200, {"_shards": {"successful": 1}})
        if len(parts) == 2 and parts[1] == "_search":
            try:
                return self._send(200, STATE.search(parts[0], self._body()))
            except Exception as exc:  # 让客户端看到 4xx，便于测试断言
                return self._send(400, {"error": {"type": "mock_error",
                                                  "reason": str(exc)}})
        return self._send(404, {"error": "not found: %s" % self.path})


def main():
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9200
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("mock-es listening on 127.0.0.1:%d" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
