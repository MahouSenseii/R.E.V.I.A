"""Reconstructed LoCoMo retrieval experiment, not the report's missing harness.

Reads only an explicitly supplied public dataset. SQLite indexes are in memory;
results contain question/turn IDs and metrics, never conversation text. No model
is downloaded or started here. See retrievalBenchmark.md for provenance and limits.
"""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import http.client
import importlib.metadata
import json
import math
from pathlib import Path
import re
import shutil
import sqlite3
import sys
import time

import numpy as np


REVIA_STOP = set("""a an and are about can could did do does for how i in is it me my
of on or should that the this to user what when where which who why with would you your""".split())
NCM_STOP = set("""a an the and or but if then so of to in on at by for with from as is are was were be
been being am i me my mine we us our you your yours he him his she her hers it its
they them their this that these those what which who whom whose when where why how
do does did done doing have has had having not no yes can could would should will
shall may might must just very really also too there here about into over than out
up down off again once all any both each few more most other some such only own same
s t don doesn didn isn wasn aren weren won wouldn couldn shouldn ll ve re d m o y
user assistant oh ok okay yeah hey hi hello lol um uh like get got go going""".split())
ARCHIVE_STOP = set("""a an and any anything are about as at be been but by can could did do does for from get had
has have how i if in into is it its just me much my of on one or our so some such than that the their them then
there these they thing things this those to up us was we were what when where which who why will with would you your
again ago back bring brought chat chatted conversation conversations discuss discussed discussing earlier exactly
first mention mentioned recall remember said say saying scroll something speak spoke spoken start started talk talked
talking tell telling told afternoon before couple day days evening few hour hours last minute minutes month months
morning night several time times today tonight week weeks year years yesterday tomorrow monday tuesday wednesday
thursday friday saturday sunday mon tue wed thu fri sat sun two three four five six seven eight nine ten""".split())
METRICS = ("recall@5", "hit@5", "recall@6", "hit@6", "recall@12", "hit@12", "mrr@10", "mrr@100")
REPORT_VALUES = {
    "revia_bm25": {"recall@5": .517, "hit@6": .589},
    "revia_rrf": {"recall@5": .591, "hit@6": .686},
    "zscore": {"recall@5": .604, "hit@6": .698},
    "zscore_lift": {"recall@5": .605, "hit@6": .705},
    "ncm_v2": {"recall@5": .585, "hit@6": .678},
    "revia_rrf_rerank": {"recall@5": .662, "hit@6": .747},
    "ncm_v2_rerank": {"recall@5": .676, "hit@6": .763},
}


def digest_file(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def load_locomo(path):
    categories = {1: "multi-hop", 2: "temporal", 3: "open-domain", 4: "single-hop"}
    haystacks, queries = [], []
    for conversation in json.loads(Path(path).read_text(encoding="utf-8")):
        history = conversation["conversation"]
        hay = {"id": conversation["sample_id"], "texts": [], "contents": [], "sessions": [], "times": []}
        positions = {}
        sessions = sorted(int(key[8:]) for key in history if re.fullmatch(r"session_\d+", key))
        for session in sessions:
            when = datetime.strptime(history[f"session_{session}_date_time"], "%I:%M %p on %d %B, %Y")
            for offset, turn in enumerate(history[f"session_{session}"]):
                text = turn["text"]
                if turn.get("blip_caption"):
                    text += f" [shares a photo of {turn['blip_caption']}]"
                positions[turn["dia_id"]] = len(hay["texts"])
                hay["texts"].append(f"{turn['speaker']}: {text}")
                hay["contents"].append(text)
                hay["sessions"].append(session - 1)
                hay["times"].append(int(when.replace(tzinfo=timezone.utc).timestamp()) + offset * 60)
        haystacks.append(hay)
        for index, question in enumerate(conversation["qa"]):
            if question["category"] not in categories:
                continue
            gold = sorted({positions[e] for e in question.get("evidence", []) if e in positions})
            if gold:
                queries.append({"id": f"{hay['id']}:{index}", "conversation": hay["id"],
                                "question": question["question"], "gold": gold,
                                "category": categories[question["category"]]})
    return haystacks, queries


def fts_query(text):
    words = dict.fromkeys(re.findall(r"[a-z0-9]+", text.lower()))
    return " OR ".join(f'"{word}"*' for word in words if len(word) >= 2 and word not in REVIA_STOP)


def archive_topic_terms(text):
    return list(dict.fromkeys(word for word in re.findall(r"[a-z0-9]+", text.lower())
                             if len(word) >= 2 and word not in ARCHIVE_STOP and not word.isdigit()))[:6]


class LexicalIndex:
    def __init__(self, texts):
        self.connection = sqlite3.connect(":memory:")
        self.count = len(texts)
        self.connection.execute("CREATE VIRTUAL TABLE archive USING fts5(content, tokenize='unicode61 remove_diacritics 2')")
        self.connection.execute("CREATE VIRTUAL TABLE memory USING fts5(summary, category, tokenize='unicode61 remove_diacritics 2')")
        self.connection.executemany("INSERT INTO archive(rowid,content) VALUES (?,?)", enumerate(texts, 1))
        self.connection.executemany("INSERT INTO memory(rowid,summary,category) VALUES (?,?,'conversation')", enumerate(texts, 1))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.connection.close()

    def search(self, query, archive=False, expression=None, newest=False):
        table, scoring = ("archive", "bm25(archive)") if archive else ("memory", "bm25(memory,6.0,1.0)")
        expression = fts_query(query) if expression is None else expression
        scores = np.zeros(self.count, dtype=np.float32)
        if not expression:
            return [], scores
        # Archive ties follow insertion order; curated ties are newer first.
        ties = "rowid ASC" if archive and not newest else "rowid DESC"
        rows = self.connection.execute(
            f"SELECT rowid,{scoring} FROM {table} WHERE {table} MATCH ? ORDER BY {scoring},{ties}",
            (expression,)).fetchall()
        ranking = [rowid - 1 for rowid, _ in rows]
        for rowid, score in rows:
            scores[rowid - 1] = -score
        return ranking, scores


def ncm_tokens(text):
    words = []
    for word in re.findall(r"[a-z0-9]+", text.lower()):
        if word in NCM_STOP:
            continue
        if len(word) > 4 and word.endswith("ies"):
            word = word[:-3] + "y"
        elif len(word) > 3 and word.endswith("s") and not word.endswith("ss"):
            word = word[:-1]
        words.append(word)
    return words


class NcmLexical:
    def __init__(self, texts):
        counts = [Counter(ncm_tokens(text)) for text in texts]
        self.lengths = np.array([sum(row.values()) for row in counts], dtype=np.float32)
        self.count = len(texts)
        self.postings = {}
        for index, row in enumerate(counts):
            for term, frequency in row.items():
                self.postings.setdefault(term, []).append((index, frequency))

    def search(self, query):
        result = np.zeros(self.count, dtype=np.float32)
        norm = 1.2 * (0.25 + .75 * self.lengths / max(float(self.lengths.mean()), 1e-9))
        for term in sorted(set(ncm_tokens(query))):
            rows = self.postings.get(term, [])
            if not rows:
                continue
            docs = np.array([row[0] for row in rows])
            counts = np.array([row[1] for row in rows], dtype=np.float32)
            weight = math.log(1 + (self.count - len(rows) + .5) / (len(rows) + .5))
            result[docs] += weight * counts * 2.2 / (counts + norm[docs])
        return result


def rrf_rank(lexical, cosine, candidate_limit):
    semantic = [int(i) for i in np.argsort(-cosine, kind="stable") if cosine[i] >= .35][:candidate_limit]
    scores = {}
    for weight, ranking in ((1.0, lexical[:candidate_limit]), (1.25, semantic)):
        for rank, index in enumerate(ranking):
            scores[index] = scores.get(index, 0.0) + weight / (61.0 + rank)
    return sorted(scores, key=lambda index: (-scores[index], -index))


def zscore(values):
    values = np.asarray(values, dtype=np.float32)
    deviation = values.std()
    return (values - values.mean()) / deviation if deviation > 1e-9 else np.zeros_like(values)


def backward_lift(scores, sessions):
    scores = np.asarray(scores, dtype=np.float32)
    session_ids = np.asarray(sessions)
    lift = np.zeros_like(scores)
    anchors = np.flatnonzero(scores > 0)
    if len(scores) > 1024:
        anchors = np.argpartition(-scores, 1024)[:1024]
        anchors = anchors[scores[anchors] > 0]
    for distance in (1, 2, 3):
        valid = anchors[anchors >= distance]
        valid = valid[session_ids[valid] == session_ids[valid - distance]]
        np.maximum.at(lift, valid - distance, scores[valid] * math.exp(-distance))
    return scores + .75 * lift


def metrics(ranking, gold):
    ranking = [int(i) for i in ranking]
    if len(set(ranking)) != len(ranking) or not gold:
        raise ValueError("Metrics require unique ranked IDs and at least one evidence ID.")
    evidence = set(gold)
    output = {}
    for k in (5, 6, 12):
        found = evidence.intersection(ranking[:k])
        output[f"recall@{k}"] = len(found) / len(evidence)
        output[f"hit@{k}"] = float(bool(found))
    for cutoff in (10, 100):
        output[f"mrr@{cutoff}"] = next((1.0 / (i + 1) for i, value in enumerate(ranking[:cutoff])
                                       if value in evidence), 0.0)
    return output


def paired_bootstrap(baseline, candidate, metric, count, seed):
    if len(baseline) != len(candidate) or not baseline:
        raise ValueError("Paired bootstrap requires equal, nonempty samples.")
    for left, right in zip(baseline, candidate):
        if (left.get("id"), left["conversation"]) != (right.get("id"), right["conversation"]):
            raise ValueError("Paired rows must name the same question in the same order.")
    differences = np.asarray([b[metric] - a[metric] for a, b in zip(baseline, candidate)])
    rng = np.random.default_rng(seed)
    means = np.empty(count)
    # Batches avoid allocating count * corpus-size doubles at once.
    for start in range(0, count, 256):
        batch = min(256, count - start)
        means[start:start + batch] = differences[rng.integers(0, len(differences), (batch, len(differences)))].mean(axis=1)
    groups = sorted({row["conversation"] for row in baseline})
    sums = np.asarray([sum(value for row, value in zip(baseline, differences) if row["conversation"] == group) for group in groups])
    sizes = np.asarray([sum(row["conversation"] == group for row in baseline) for group in groups])
    sampled = rng.integers(0, len(groups), (count, len(groups)))
    clustered = sums[sampled].sum(axis=1) / sizes[sampled].sum(axis=1)
    p = min(1.0, max(1.0 / count, 2 * min(float((means <= 0).mean()), float((means >= 0).mean()))))
    return {"delta": float(differences.mean()), "question_ci95": np.percentile(means, [2.5, 97.5]).tolist(),
            "conversation_ci95": np.percentile(clustered, [2.5, 97.5]).tolist(), "question_two_sided_p": p}


def aggregate(rows):
    return {key: float(np.mean([row[key] for row in rows])) for key in METRICS} | {"questions": len(rows)}


def embeddings(texts, kind, options, model_hash):
    prefix = "search_query: " if kind.endswith("queries") else "search_document: "
    text_hash = hashlib.sha256(json.dumps(texts, ensure_ascii=False).encode()).hexdigest()
    identity = {"texts_sha256": text_hash, "model_sha256": model_hash, "prefix": prefix}
    cache = options.cache / f"{kind}-{text_hash[:16]}-{model_hash[:16]}.npz"
    options.cache.mkdir(parents=True, exist_ok=True)
    if cache.exists():
        with np.load(cache, allow_pickle=False) as stored:
            if json.loads(str(stored["identity"])) != identity:
                raise ValueError("Embedding cache identity does not match.")
            vectors = stored["vectors"]
            producer = json.loads(str(stored["producer"]))
    else:
        if not options.embedding_port and not options.embedding_inprocess:
            raise ValueError(f"No embedding cache {cache}; supply an already running --embedding-port.")
        values = []
        if options.embedding_inprocess:
            import llama_cpp
            if getattr(options, "local_embedder", None) is None:
                options.local_embedder = llama_cpp.Llama(
                    model_path=str(options.embedding_model.resolve()), embedding=True,
                    pooling_type=llama_cpp.LLAMA_POOLING_TYPE_MEAN, n_gpu_layers=0,
                    offload_kqv=False, op_offload=False,
                    n_ctx=2048, n_batch=2048, n_ubatch=2048,
                    n_threads=options.threads, n_threads_batch=options.threads, verbose=False)
            producer = {"backend": "llama-cpp-python", "version": llama_cpp.__version__,
                        "pooling": "mean", "gpu_layers": 0, "ctx": 2048, "batch": 2048,
                        "ubatch": 2048, "threads": options.threads,
                        "library_sha256": digest_file(llama_cpp.llama_cpp._lib._name)}
            for start in range(0, len(texts), 32):
                values.append(np.asarray(options.local_embedder.embed(
                    [prefix + text for text in texts[start:start + 32]], normalize=False, truncate=False), dtype=np.float32))
                if start % 320 == 0 or start + 32 >= len(texts):
                    print(f"Embedded {kind}: {min(start + 32, len(texts))}/{len(texts)}", flush=True)
        else:
            producer = {"backend": "loopback-http", "port": options.embedding_port,
                        "server_version": "caller must record separately"}
            for index, text in enumerate(texts):
                connection = http.client.HTTPConnection("127.0.0.1", options.embedding_port, timeout=60)
                try:
                    connection.request("POST", "/v1/embeddings", json.dumps({
                        "model": options.embedding_model.name, "input": prefix + text}),
                        {"Content-Type": "application/json"})
                    response = connection.getresponse()
                    payload = response.read()
                    if response.status != 200:
                        raise RuntimeError(f"Embedding failed at {kind} {index}: HTTP {response.status}")
                    values.append(np.asarray([json.loads(payload)["data"][0]["embedding"]], dtype=np.float32))
                finally:
                    connection.close()
                if (index + 1) % 100 == 0:
                    print(f"Embedded {kind}: {index + 1}/{len(texts)}", flush=True)
        vectors = np.concatenate(values, axis=0)
        if vectors.ndim != 2 or len(vectors) != len(texts) or not np.isfinite(vectors).all():
            raise ValueError("Invalid embedding vectors.")
        np.savez_compressed(cache, identity=json.dumps(identity), producer=json.dumps(producer), vectors=vectors)
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    if vectors.ndim != 2 or len(vectors) != len(texts) or not np.isfinite(vectors).all() or np.any(norms <= 0):
        raise ValueError("Invalid cached embedding vectors.")
    options.embedding_producers[kind] = producer | {"cache_sha256": digest_file(cache)}
    return vectors / norms


def reranker(directory, threads):
    import torch
    from sentence_transformers import CrossEncoder
    torch.set_num_threads(threads)
    # No remote code, downloads or pickle model weights are allowed by this tool.
    if not directory.is_dir() or not (directory / "model.safetensors").is_file():
        raise ValueError("--reranker must be a local directory containing model.safetensors.")
    return CrossEncoder(str(directory.resolve()), device="cpu", local_files_only=True, trust_remote_code=False,
                        model_kwargs={"use_safetensors": True})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New output directory; refuses overwrite.")
    parser.add_argument("--cache", type=Path, default=Path(".revia-ai/evidence/retrieval-cache"))
    parser.add_argument("--embedding-model", type=Path)
    parser.add_argument("--embedding-port", type=int)
    parser.add_argument("--embedding-inprocess", action="store_true",
                        help="Use installed llama-cpp-python on CPU; no server, socket or child worker.")
    parser.add_argument("--prepare-embeddings-only", action="store_true",
                        help="Fill the model/text-bound vector cache, without ranking or reranking.")
    parser.add_argument("--include-archive-policy", action="store_true",
                        help="Separately evaluate current six-term archive queries, cap48/cap120, and topic-string reranking.")
    parser.add_argument("--lexical-only", action="store_true")
    parser.add_argument("--reranker", type=Path, help="Local reference cross-encoder directory.")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--candidate-limit", type=int, default=40,
                        help="Reconstruction assumption: max(4*k,20) with k=10; runtime k=6 also reported.")
    parser.add_argument("--limit", type=int, help="Smoke run only; not comparable to the full report.")
    parser.add_argument("--bootstrap", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=0)
    options = parser.parse_args(argv)
    if options.output.exists():
        parser.error("--output must be a new directory.")
    if options.candidate_limit < 30 or options.threads < 1 or options.bootstrap < 1:
        parser.error("Candidate limit must be at least 30; threads and bootstrap must be positive.")
    if options.embedding_port and not 1 <= options.embedding_port <= 65535:
        parser.error("Embedding port must be between 1 and 65535.")
    if not options.lexical_only and not options.embedding_model:
        parser.error("--embedding-model is required unless --lexical-only.")
    if options.embedding_inprocess and options.embedding_port:
        parser.error("Choose exactly one embedding transport.")
    if options.prepare_embeddings_only and (options.lexical_only or options.reranker):
        parser.error("--prepare-embeddings-only cannot use --lexical-only or --reranker.")
    started = time.perf_counter()
    hays, queries = load_locomo(options.dataset)
    total_queries = len(queries)
    if options.limit is not None:
        if options.limit < 1:
            parser.error("--limit must be positive.")
        queries = queries[:options.limit]
        selected = {query["conversation"] for query in queries}
        hays = [hay for hay in hays if hay["id"] in selected]
    options.output.mkdir(parents=True)
    shutil.copyfile(__file__, options.output / "runner.py")
    rows, latencies = {}, {}
    encoder = reranker(options.reranker, options.threads) if options.reranker else None
    model_hash = digest_file(options.embedding_model) if not options.lexical_only else None
    context = {hay["id"]: hay for hay in hays}
    docs, query_vectors = None, None
    archive_vectors = None
    options.embedding_producers = {}
    if not options.lexical_only:
        try:
            docs = embeddings([text for hay in hays for text in hay["texts"]], "documents", options, model_hash)
            query_vectors = embeddings([query["question"] for query in queries], "queries", options, model_hash)
            if options.include_archive_policy:
                archive_vectors = embeddings(
                    [" ".join(archive_topic_terms(query["question"])) for query in queries],
                    "archive-queries", options, model_hash)
        finally:
            if getattr(options, "local_embedder", None) is not None:
                options.local_embedder.close()
        if options.prepare_embeddings_only:
            write_json(options.output / "embedding-preparation.json", {
                "dataset_sha256": digest_file(options.dataset), "model_sha256": model_hash,
                "producer": options.embedding_producers, "documents": len(docs),
                "queries": len(query_vectors), "archive_queries": len(archive_vectors) if archive_vectors is not None else 0,
                "elapsed_seconds": time.perf_counter() - started,
                "script_sha256": digest_file(options.output / "runner.py")})
            return 0
    offset = 0
    for hay in hays:
        hay["fts"] = LexicalIndex(hay["texts"])
        if options.include_archive_policy:
            hay["archive_fts"] = LexicalIndex(hay["contents"])
        hay["bm25"] = NcmLexical(hay["texts"])
        if docs is not None:
            hay["vectors"] = docs[offset:offset + len(hay["texts"])]
            offset += len(hay["texts"])
    metadata = {
        "schema_version": 1, "evidence": "RECONSTRUCTED EXPERIMENT; not the original report harness or production runtime",
        "dataset_sha256": digest_file(options.dataset), "dataset_source":
        "https://github.com/snap-research/locomo/tree/3eb6f2c585f5e1699204e3c3bdf7adc5c28cb376",
        "ncm_reference": "https://github.com/iam-saiteja/NCM/tree/98651bb605898f20936c094d9c1169c51f8c00ef",
        "script_sha256": digest_file(options.output / "runner.py"), "embedding_model_sha256": model_hash,
        "embedding_producers": options.embedding_producers,
        "reranker_sha256": digest_file(options.reranker / "model.safetensors") if encoder else None,
        "reranker_threads": options.threads if encoder else None, "reranker_batch_size": 30 if encoder else None,
        "lexical_only": options.lexical_only, "candidate_limit": options.candidate_limit,
        "include_archive_policy": options.include_archive_policy,
        "turns": sum(len(hay["texts"]) for hay in hays), "questions": len(queries), "full_dataset_questions": total_queries,
        "bootstrap_resamples": options.bootstrap, "bootstrap_seed": options.seed,
        "python": sys.version, "numpy": np.__version__, "sqlite": sqlite3.sqlite_version,
        "packages": {name: importlib.metadata.version(name) for name in
                     ("torch", "sentence-transformers", "transformers") if encoder},
        "limits": [
            "Original report scripts, raw rows, vectors and significance files were not supplied.",
            "No production archive admission, scope, sensitive-content filtering, curation or revision path is measured.",
            "Temporal query parsing omitted, as in the report; all turns use their public dataset session order.",
            "FTS archive topic terms use the curated tokenizer; the report does not specify its exact topic extraction.",
            "Curated FTS category is fixed to conversation for this public raw-turn experiment.",
            "The report does not specify k used for candidate caps; the default assumes k=10 and also reports runtime k=6.",
            "NCM's referenced metric uses MRR@100 while the report says MRR@10; both are reported explicitly.",
            "Per-query latency excludes embedding and bootstrap; rerank CPU inference is timed separately.",
            "Full runs share cross-encoder scores across the union of top-30 shortlists; each rerank latency includes the whole union and is an upper bound, not a single-shortlist measurement.",
            "Answer accuracy is not measured or projected by this tool.",
            "Archive-policy variants use six extracted topic terms, 12 ranked results/cap48 baseline and cap120 rerank. The 2400-character prompt rendering budget and admission policy are not simulated.",
            "Archive-policy lexical FTS indexes content without the speaker prefix, matching production. Public LoCoMo speaker names remain the embedding/reranker role-prefix adaptation.",
        ],
    }
    write_json(options.output / "metadata.json", metadata)
    try:
        with (options.output / "queries.jsonl").open("w", encoding="utf-8") as output:
            for position, query in enumerate(queries):
                hay = context[query["conversation"]]
                timings, rankings = {}, {}
                tick = time.perf_counter()
                rankings["revia_bm25"], _ = hay["fts"].search(query["question"], archive=True)
                timings["revia_bm25"] = time.perf_counter() - tick
                if options.include_archive_policy:
                    tick = time.perf_counter()
                    terms = archive_topic_terms(query["question"])
                    topic = " ".join(terms)
                    expression = " OR ".join(f'"{term}"*' for term in terms)
                    rankings["archive_topic_bm25"], _ = hay["archive_fts"].search(
                        query["question"], archive=True, expression=expression)
                    timings["archive_topic_bm25"] = time.perf_counter() - tick
                if docs is not None:
                    tick = time.perf_counter()
                    lexical, fts_scores = hay["fts"].search(query["question"])
                    lexical_seconds = time.perf_counter() - tick
                    tick = time.perf_counter()
                    cosine = hay["vectors"] @ query_vectors[position]
                    cosine_seconds = time.perf_counter() - tick
                    preparation_seconds = lexical_seconds + cosine_seconds
                    for name, cap in (("revia_rrf", options.candidate_limit), ("revia_rrf_cap20", 20),
                                      ("revia_rrf_runtime6", 24), ("revia_rrf_pool30", 120)):
                        tick = time.perf_counter()
                        rankings[name] = rrf_rank(lexical, cosine, cap)
                        timings[name] = preparation_seconds + time.perf_counter() - tick
                    tick = time.perf_counter()
                    scores = zscore(cosine) + .75 * zscore(fts_scores)
                    rankings["zscore"] = np.argsort(-scores, kind="stable").tolist()
                    timings["zscore"] = preparation_seconds + time.perf_counter() - tick
                    tick = time.perf_counter()
                    rankings["zscore_lift"] = np.argsort(-backward_lift(scores, hay["sessions"]), kind="stable").tolist()
                    timings["zscore_lift"] = timings["zscore"] + time.perf_counter() - tick
                    tick = time.perf_counter()
                    ncm_scores = zscore(cosine) + .75 * zscore(hay["bm25"].search(query["question"]))
                    rankings["ncm_v2"] = np.argsort(-backward_lift(ncm_scores, hay["sessions"]), kind="stable").tolist()
                    timings["ncm_v2"] = cosine_seconds + time.perf_counter() - tick
                    if options.include_archive_policy:
                        tick = time.perf_counter()
                        archive_lexical, _ = hay["archive_fts"].search(
                            query["question"], archive=True, expression=expression, newest=True)
                        topic_cosine = hay["vectors"] @ archive_vectors[position]
                        archive_preparation_seconds = time.perf_counter() - tick
                        for name, cap in (("archive_topic_rrf", 48), ("archive_topic_pool30", 120)):
                            tick = time.perf_counter()
                            rankings[name] = rrf_rank(archive_lexical, topic_cosine, cap) if terms else []
                            timings[name] = archive_preparation_seconds + time.perf_counter() - tick
                if encoder:
                    bases = ["revia_bm25"] if docs is None else ["revia_rrf", "ncm_v2", "revia_rrf_pool30"]
                    union = list(dict.fromkeys(index for base in bases for index in rankings[base][:30]))
                    tick = time.perf_counter()
                    scores_by_id = {}
                    if union:
                        scores = encoder.predict([(query["question"], hay["texts"][index]) for index in union],
                                                 batch_size=30, show_progress_bar=False, convert_to_numpy=True)
                        if np.asarray(scores).shape != (len(union),) or not np.isfinite(scores).all():
                            raise ValueError("Reranker returned invalid scores.")
                        scores_by_id = dict(zip(union, scores))
                    union_seconds = time.perf_counter() - tick
                    for base in bases:
                        head = rankings[base][:30]
                        rankings[base + "_rerank"] = sorted(head, key=lambda index: -scores_by_id[index]) + rankings[base][30:]
                        timings[base + "_rerank"] = timings[base] + union_seconds
                    if options.include_archive_policy and docs is not None:
                        tick = time.perf_counter()
                        head = rankings["archive_topic_pool30"][:30]
                        if head:
                            scores = encoder.predict([(topic, hay["texts"][index]) for index in head],
                                                     batch_size=30, show_progress_bar=False, convert_to_numpy=True)
                            if np.asarray(scores).shape != (len(head),) or not np.isfinite(scores).all():
                                raise ValueError("Archive reranker returned invalid scores.")
                            reordered = [head[index] for index in np.argsort(-scores, kind="stable")]
                        else:
                            reordered = []
                        rankings["archive_topic_rrf_rerank"] = reordered + rankings["archive_topic_pool30"][30:]
                        timings["archive_topic_rrf_rerank"] = timings["archive_topic_pool30"] + time.perf_counter() - tick
                record = {key: query[key] for key in ("id", "conversation", "category", "gold")}
                if encoder:
                    record["rerank_union_candidates"] = len(union)
                    record["rerank_union_milliseconds"] = union_seconds * 1000
                record["systems"] = {}
                for name, ranking in rankings.items():
                    scored = metrics(ranking, query["gold"])
                    row = {key: query[key] for key in ("id", "conversation", "category")} | scored
                    rows.setdefault(name, []).append(row)
                    latencies.setdefault(name, []).append(timings[name] * 1000)
                    record["systems"][name] = scored | {"top100": ranking[:100], "milliseconds": timings[name] * 1000}
                output.write(json.dumps(record, allow_nan=False) + "\n")
                output.flush()
                if (position + 1) % 50 == 0 or position == len(queries) - 1:
                    print(f"Scored {position + 1}/{len(queries)} questions ({time.perf_counter() - started:.1f}s)", flush=True)
    finally:
        for hay in hays:
            hay["fts"].connection.close()
            if "archive_fts" in hay:
                hay["archive_fts"].connection.close()
    summary = {}
    for name, values in rows.items():
        summary[name] = {"all": aggregate(values), "categories": {
            category: aggregate([row for row in values if row["category"] == category])
            for category in sorted({row["category"] for row in values})},
            "mean_ms": float(np.mean(latencies[name])), "p95_ms": float(np.percentile(latencies[name], 95))}
        if name in REPORT_VALUES:
            summary[name]["delta_from_report_rounded"] = {
                key: summary[name]["all"][key] - value for key, value in REPORT_VALUES[name].items()}
    comparisons = {}
    for baseline, candidate in (("revia_bm25", "revia_rrf"), ("revia_rrf", "zscore_lift"),
                                ("revia_rrf", "ncm_v2"), ("revia_rrf", "revia_rrf_rerank"),
                                ("revia_rrf", "ncm_v2_rerank"), ("revia_bm25", "revia_bm25_rerank"),
                                ("revia_bm25", "revia_rrf_runtime6"),
                                ("revia_rrf_runtime6", "revia_rrf_pool30_rerank"),
                                ("revia_rrf_rerank", "ncm_v2_rerank"),
                                ("archive_topic_bm25", "archive_topic_rrf"),
                                ("archive_topic_bm25", "archive_topic_rrf_rerank"),
                                ("archive_topic_rrf", "archive_topic_rrf_rerank")):
        if baseline in rows and candidate in rows:
            comparisons[f"{candidate} - {baseline}"] = {
                metric: paired_bootstrap(rows[baseline], rows[candidate], metric, options.bootstrap, options.seed)
                for metric in METRICS}
    write_json(options.output / "summary.json", summary)
    write_json(options.output / "significance.json", comparisons)
    metadata["completed"] = True
    metadata["elapsed_seconds"] = time.perf_counter() - started
    write_json(options.output / "metadata.json", metadata)
    for name, value in summary.items():
        scores = value["all"]
        print(f"{name:25s} R@5 {scores['recall@5']:.6f} Hit@6 {scores['hit@6']:.6f} "
              f"MRR@10 {scores['mrr@10']:.6f} MRR@100 {scores['mrr@100']:.6f} {value['mean_ms']:.2f}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
