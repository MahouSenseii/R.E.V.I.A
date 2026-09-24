"""A direct-forward replacement for the codebook predictor's inner generate loop.

Why this exists
---------------
Producing one 12 Hz audio frame costs one talker step plus a nested Hugging Face
``.generate()`` call that samples fifteen sub-tokens one at a time. Measured on a
62-character phrase: 672 sequential forward passes, 16.4 ms each, with 70.7% of the
total inside 42 of those nested ``.generate()`` invocations. A 0.6B model does not need
16 ms a step; almost all of it is generic generation machinery being rebuilt per call --
logits processors, stopping criteria, generation-config resolution, output dataclasses,
and a full hidden-state collection the caller then discards.

This module keeps the model, the weights, the tokenizer and the sampling behaviour, and
replaces only the loop. Hugging Face is still what loads and defines the model; what is
removed is generic ``.generate()`` from the hot path.

Equivalence
-----------
The sampling here reproduces what ``GenerationMixin`` does for this configuration, in
the same order: divide by temperature, keep the top ``k``, softmax, then one
``torch.multinomial`` draw. ``top_p`` is 1.0 in the upstream configuration and Hugging
Face omits the warper entirely at that value, so it is omitted here too rather than
applied as a no-op that would still consume a different amount of work.

Because the same operations are performed in the same order and the same number of
random draws are taken, a fixed seed produces the same tokens through either path. That
is the correctness test in ``tests_equivalence`` below, and it is a stronger check than
comparing audio: it holds the sampler exactly rather than approximately.
"""
from __future__ import annotations

import time
from typing import Any

import torch
from transformers.cache_utils import Cache


def _device_index(device: Any) -> int | None:
    """The CUDA ordinal for a device, or None when it is not a CUDA device."""
    text = str(device)
    if not text.startswith("cuda"):
        return None
    return int(text.split(":", 1)[1]) if ":" in text else 0


class InnerLoopStats:
    """Counters for one phrase, collected only when instrumentation is on."""

    __slots__ = ("predictor_forwards", "prefill_forwards", "forward_ms", "sample_ms")

    def __init__(self) -> None:
        self.predictor_forwards = 0
        self.prefill_forwards = 0
        self.forward_ms: list[float] = []
        self.sample_ms: list[float] = []

    def reset(self) -> None:
        self.predictor_forwards = 0
        self.prefill_forwards = 0
        self.forward_ms = []
        self.sample_ms = []

    def summary(self) -> dict[str, Any]:
        forwards = sorted(self.forward_ms)
        samples = sorted(self.sample_ms)

        def at(values: list[float], quantile: float) -> float:
            if not values:
                return -1.0
            index = min(len(values) - 1, int(len(values) * quantile))
            return values[index]

        return {
            "predictor_forwards": self.predictor_forwards,
            "prefill_forwards": self.prefill_forwards,
            "total_forwards": self.predictor_forwards + self.prefill_forwards,
            "forward_ms_mean": (sum(forwards) / len(forwards)) if forwards else -1.0,
            "forward_ms_p50": at(forwards, 0.5),
            "forward_ms_p90": at(forwards, 0.9),
            "forward_ms_total": sum(forwards),
            "sample_ms_total": sum(samples),
        }


class DirectCodePredictor:
    """Runs the sub-token loop by calling the predictor's forward directly.

    One instance wraps one loaded model. It holds no per-phrase state beyond the
    optional counters, so it is safe to keep alongside a resident model for the life of
    the worker.
    """

    def __init__(
        self,
        code_predictor: Any,
        num_code_groups: int,
        top_k: int = 50,
        top_p: float = 1.0,
        temperature: float = 0.9,
        do_sample: bool = True,
        instrument: bool = False,
    ) -> None:
        self.code_predictor = code_predictor
        self.num_code_groups = num_code_groups
        self.top_k = top_k
        self.top_p = top_p
        self.temperature = temperature
        self.do_sample = do_sample
        self.instrument = instrument
        self.stats = InnerLoopStats()
        self.graph: GraphedInnerLoop | None = None
        self.talker_graph: Any = None
        self.graph_replays = 0
        self.eager_runs = 0

    # ---------------------------------------------------------------- sampling
    def _sample(self, logits: torch.Tensor) -> torch.Tensor:
        """One token per batch row, from the last position's logits.

        Mirrors the warper order Hugging Face builds for this configuration. The
        float() cast matters: sampling in bfloat16 quantises the probabilities coarsely
        enough to change which token is drawn, so the reference path's float32 softmax
        is reproduced rather than approximated.
        """
        if not self.do_sample:
            return torch.argmax(logits, dim=-1, keepdim=True)

        scores = logits.float()
        if self.temperature != 1.0:
            scores = scores / self.temperature
        if self.top_k is not None and self.top_k > 0:
            k = min(self.top_k, scores.size(-1))
            # Everything below the k-th best is removed before the softmax, which is
            # what Hugging Face's TopKLogitsWarper does with -inf.
            threshold = torch.topk(scores, k, dim=-1).values[..., -1, None]
            scores = scores.masked_fill(scores < threshold, float("-inf"))
        if self.top_p is not None and self.top_p < 1.0:
            ordered, indices = torch.sort(scores, descending=False, dim=-1)
            cumulative = ordered.softmax(dim=-1).cumsum(dim=-1)
            remove = cumulative <= (1 - self.top_p)
            remove = remove.scatter(-1, indices, remove)
            scores = scores.masked_fill(remove, float("-inf"))
        probabilities = torch.nn.functional.softmax(scores, dim=-1)
        return torch.multinomial(probabilities, num_samples=1)

    def graph_failure(self) -> str:
        """Why the graph is not in use, for the one line said at load."""
        if self.graph is None:
            return "not attempted"
        return self.graph.failure or "unknown"

    # ---------------------------------------------------------------- the loop
    @torch.no_grad()
    def generate_codes(self, prefill_embeds: torch.Tensor) -> torch.Tensor:
        """Fifteen sub-tokens for one frame, shaped like ``generate().sequences``.

        ``prefill_embeds`` is the (batch, 2, hidden) pair the upstream caller builds
        from the previous hidden state and the current token embedding. The predictor's
        own forward already resolves which embedding table and which head belong to a
        step from ``generation_steps``, so the loop only has to advance that counter and
        carry the cache.
        """
        from transformers.cache_utils import DynamicCache

        # The graph is captured for one sequence. Anything wider -- the batched reply
        # path -- runs the eager loop, which is correct at any width and already close
        # to real time there by amortising the same overhead across sequences.
        if (self.graph is not None and self.graph.graph is not None
                and prefill_embeds.shape[0] == 1
                and prefill_embeds.shape[1] == 2):
            self.graph_replays += 1
            if self.instrument:
                started = time.perf_counter()
                codes = self.graph.run(prefill_embeds)
                self.stats.predictor_forwards += self.num_code_groups - 1
                self.stats.forward_ms.append((time.perf_counter() - started) * 1000.0)
                return codes
            return self.graph.run(prefill_embeds)

        self.eager_runs += 1
        predictor = self.code_predictor
        steps = self.num_code_groups - 1
        cache = DynamicCache()

        prefill_length = prefill_embeds.shape[1]
        cache_position = torch.arange(
            prefill_length, device=prefill_embeds.device, dtype=torch.long)

        started = time.perf_counter() if self.instrument else 0.0
        outputs = predictor(
            inputs_embeds=prefill_embeds,
            past_key_values=cache,
            use_cache=True,
            cache_position=cache_position,
            # The caller discards the hidden states, so collecting every layer's
            # activations for every step is pure cost. This is the single largest thing
            # the reference path does that nothing reads.
            output_hidden_states=False,
            return_dict=True,
        )
        if self.instrument:
            self.stats.prefill_forwards += 1
            self.stats.forward_ms.append((time.perf_counter() - started) * 1000.0)

        sample_started = time.perf_counter() if self.instrument else 0.0
        token = self._sample(outputs.logits[:, -1, :])
        if self.instrument:
            self.stats.sample_ms.append((time.perf_counter() - sample_started) * 1000.0)

        # Collected on device and concatenated once. Appending to a Python list of
        # single-element tensors and stacking at the end keeps every value on CUDA;
        # reading each token back to the host would add a synchronisation per step, and
        # there are fifteen of them per audio frame.
        tokens = [token]
        cache_offset = prefill_length
        for step in range(1, steps):
            position = torch.tensor(
                [cache_offset], device=prefill_embeds.device, dtype=torch.long)
            started = time.perf_counter() if self.instrument else 0.0
            outputs = predictor(
                input_ids=token,
                past_key_values=cache,
                use_cache=True,
                cache_position=position,
                generation_steps=step,
                output_hidden_states=False,
                return_dict=True,
            )
            if self.instrument:
                self.stats.predictor_forwards += 1
                self.stats.forward_ms.append((time.perf_counter() - started) * 1000.0)

            sample_started = time.perf_counter() if self.instrument else 0.0
            token = self._sample(outputs.logits[:, -1, :])
            if self.instrument:
                self.stats.sample_ms.append(
                    (time.perf_counter() - sample_started) * 1000.0)
            tokens.append(token)
            cache_offset += 1

        return torch.cat(tokens, dim=-1)



class GraphOwnedPredictorCache(Cache):
    """A key/value cache the captured graph owns outright.

    Hugging Face's ``StaticCache`` could not be captured here. It writes with
    ``index_copy_`` driven by a ``cache_position`` tensor and keeps bookkeeping a replay
    does not reproduce, which surfaced as ``index_copy_(): index out of bounds`` once the
    talker's loop drove the graph. Rather than bound-check around that, this cache is
    built for the one shape the predictor actually has.

    Three properties make it graph-safe:

    * **Fixed storage.** Every layer's keys and values are allocated once, before
      capture, and never reallocated. Addresses recorded during capture stay valid for
      every replay.
    * **Baked offsets.** ``write_offset`` and ``write_length`` are plain Python integers
      set by the loop before each forward, so the slice bounds are constants folded into
      the captured kernels. No tensor indexing, no ``index_copy_``, nothing whose value
      could drift between replays.
    * **A narrow view instead of a mask.** ``update`` returns exactly the valid prefix.
      With ``attention_mask=None`` SDPA derives causality from the query length -- true
      for the two-token prefill, false for a single decode step attending over its whole
      past -- so the correct thing happens without building a mask at all.

    The predictor always runs the same fifteen sub-steps over at most sixteen positions,
    so this deliberately does not generalise: a cache that handled arbitrary lengths
    would need dynamic bounds, which is the property that made the stock one uncapturable.
    """

    def __init__(
        self,
        num_layers: int,
        num_key_value_heads: int,
        head_dim: int,
        max_length: int,
        device: Any,
        dtype: Any,
    ) -> None:
        # Deliberately not calling Cache.__init__: the base builds a list of layer
        # objects for a growth model this cache does not have. Only the isinstance check
        # in the model and the update contract matter here.
        self.layers: list = []
        self.offloading = False
        self.max_length = max_length
        self.key_cache = [
            torch.zeros(1, num_key_value_heads, max_length, head_dim,
                        device=device, dtype=dtype)
            for _ in range(num_layers)
        ]
        self.value_cache = [
            torch.zeros(1, num_key_value_heads, max_length, head_dim,
                        device=device, dtype=dtype)
            for _ in range(num_layers)
        ]
        self.write_offset = 0
        self.write_length = 1

    def plan(self, offset: int, length: int) -> None:
        """Where the next forward writes. Called before the forward, never inside it."""
        if offset + length > self.max_length:
            raise ValueError(
                f"Predictor cache write {offset}+{length} exceeds {self.max_length}.")
        self.write_offset = offset
        self.write_length = length

    def update(self, key_states, value_states, layer_idx, cache_kwargs=None):
        start = self.write_offset
        end = start + self.write_length
        keys = self.key_cache[layer_idx]
        values = self.value_cache[layer_idx]
        keys[:, :, start:end, :].copy_(key_states)
        values[:, :, start:end, :].copy_(value_states)
        # Only the written prefix. Everything past `end` is stale from an earlier phrase
        # and must not be attended to; returning a narrow view is what excludes it,
        # which is why no mask is needed and why nothing has to be zeroed between runs.
        return keys[:, :, :end, :], values[:, :, :end, :]

    def get_seq_length(self, layer_idx: int = 0) -> int:
        return self.write_offset + self.write_length

    def get_max_cache_shape(self, layer_idx: int = 0) -> int:
        return self.max_length

    def reset(self) -> None:
        self.write_offset = 0
        self.write_length = 1

    def __len__(self) -> int:
        return len(self.key_cache)


class GraphedInnerLoop:
    """The fifteen-step sub-token loop replayed as one CUDA graph.

    EXPERIMENTAL, AND NOT CURRENTLY SAFE TO ENABLE.

    In isolation this works and is worth a great deal: capture succeeds, replay responds
    to changed input, the random stream still advances, and one frame's inner loop drops
    from 171.9 ms to 13.1 ms on an RTX 5070 -- 13.1x. Driven by the talker's own
    generation loop it does not survive. Capturing at model load produced a graph that
    replayed correctly alone but hung the first time the full pipeline ran it; capturing
    lazily on the first real phrase instead produced
    ``index_copy_(): index out of bounds`` from the cache kernel. The prefill shape the
    live flow uses was confirmed to be exactly the captured one, so the fault is not a
    shape mismatch.
    
    The likely cause is that Hugging Face's StaticCache does index bookkeeping the graph
    cannot reproduce across replays. Fixing it properly means giving the graph a cache it
    owns outright -- preallocated key/value tensors written at baked offsets -- rather
    than borrowing one whose internal indexing is invisible from here.

    Left in place because the measurement is real and the remaining work is well
    defined, and left off by default because a voice that sometimes crashes the worker
    is worse than a voice that is slow.

    The loop qualifies because nothing in it depends on the host: the step count is
    fixed at ``num_code_groups - 1``, which weights each step uses is decided by a
    Python index resolved once while capturing, and the sampled token reaches the next
    step without ever leaving the device. Sampling is captured too, so one replay
    produces a whole audio frame's codes.

    Measured on an RTX 5070: 171.9 ms eager, 13.1 ms replayed. The gap is launch and
    dispatch overhead, not arithmetic -- a forward of this predictor costs the same at
    batch 32 as at batch 1, and a bare matmul of comparable size runs in 0.04 ms.

    Batch one only. The graph bakes in the shapes it was captured with, and the batched
    reply path deliberately runs several sequences at once; that path is already near
    real time by amortising the same overhead a different way, so it keeps the eager
    loop rather than needing a graph per batch size.
    """

    def __init__(
        self,
        predictor: Any,
        num_code_groups: int,
        hidden_size: int,
        sampler: Any,
        device: Any,
        dtype: Any,
    ) -> None:
        self.predictor = predictor
        self.steps = num_code_groups - 1
        self.hidden_size = hidden_size
        self.sampler = sampler
        self.device = device
        self.dtype = dtype
        self.graph = None
        self.static_embeds = None
        self.static_tokens = None
        self.positions: list = []
        self.cache = None
        self.capture_ms = -1.0
        self.failure = ""
        # Capture is attempted exactly once. A second attempt after a failure would
        # repeat the same cost on every phrase for the same answer.
        self.attempted = False

    def _make_cache(self) -> Any:
        config = self.predictor.config
        heads = getattr(config, "num_key_value_heads", config.num_attention_heads)
        head_dim = getattr(
            config, "head_dim", config.hidden_size // config.num_attention_heads)
        # Two positions for the prefill plus one per decode step is the entire range the
        # predictor ever uses.
        return GraphOwnedPredictorCache(
            num_layers=config.num_hidden_layers,
            num_key_value_heads=heads,
            head_dim=head_dim,
            max_length=self.steps + 1,
            device=self.device,
            dtype=self.dtype,
        )

    # Passing a dict short-circuits the model's mask construction entirely, and a None
    # entry leaves SDPA to derive causality from the query length. Building a mask here
    # would mean building it inside the captured region for no benefit.
    NO_MASK = {"full_attention": None}

    @torch.no_grad()
    def _loop(self, positions: list) -> torch.Tensor:
        # No reset: every write offset below is a constant baked at capture, each
        # position is written before it is read, and update() returns only the prefix
        # written so far. Nothing from a previous phrase is reachable.
        self.cache.plan(0, 2)
        outputs = self.predictor(
            inputs_embeds=self.static_embeds, past_key_values=self.cache,
            use_cache=True, cache_position=positions[0],
            attention_mask=self.NO_MASK,
            output_hidden_states=False, return_dict=True)
        token = self.sampler(outputs.logits[:, -1, :])
        tokens = [token]
        for step in range(1, self.steps):
            self.cache.plan(step + 1, 1)
            outputs = self.predictor(
                input_ids=token, past_key_values=self.cache, use_cache=True,
                cache_position=positions[step], generation_steps=step,
                attention_mask=self.NO_MASK,
                output_hidden_states=False, return_dict=True)
            token = self.sampler(outputs.logits[:, -1, :])
            tokens.append(token)
        return torch.cat(tokens, dim=-1)

    def capture(self) -> bool:
        """Captures once. False leaves the caller on the eager path, which always works."""
        if self.graph is not None:
            return True
        self.attempted = True
        if not str(self.device).startswith("cuda"):
            self.failure = "CUDA graphs need a CUDA device."
            return False
        index = _device_index(self.device)
        previous_device = torch.cuda.current_device() if index is not None else None
        try:
            if index is not None:
                torch.cuda.set_device(index)
            self.cache = self._make_cache()
            self.static_embeds = torch.zeros(
                1, 2, self.hidden_size, device=self.device, dtype=self.dtype)
            # Held on the instance, not in a local. A graph records the addresses of its
            # inputs; letting these fall out of scope frees them, the allocator hands the
            # memory to something else, and every replay then reads whatever now sits
            # there as its cache positions. That corrupts the rotary embedding from the
            # first decode step onward -- the prefill token still matched, which is what
            # made it look like a cache bug rather than a lifetime bug.
            self.positions = [torch.arange(2, device=self.device)] + [
                torch.tensor([1 + step], dtype=torch.long, device=self.device)
                for step in range(1, self.steps)]
            positions = self.positions

            # Warm on a side stream first. Capture records whatever the allocator and
            # any lazy initialisation do, and recording a first-time allocation would
            # bake a one-off into every replay.
            stream = torch.cuda.Stream()
            stream.wait_stream(torch.cuda.current_stream())
            with torch.cuda.stream(stream):
                for _ in range(5):
                    self._loop(positions)
            torch.cuda.current_stream().wait_stream(stream)
            torch.cuda.synchronize()

            started = time.perf_counter()
            graph = torch.cuda.CUDAGraph()
            with torch.no_grad():
                with torch.cuda.graph(graph):
                    self.static_tokens = self._loop(positions)
            torch.cuda.synchronize()
            if self.static_tokens is None:
                raise RuntimeError("capture produced no output tensor")
            self.capture_ms = (time.perf_counter() - started) * 1000.0
            self.graph = graph
            return True
        except Exception as exception:  # noqa: BLE001 - any failure means eager
            self.failure = f"{type(exception).__name__}: {exception}"
            self.graph = None
            self.static_tokens = None
            return False
        finally:
            if previous_device is not None:
                torch.cuda.set_device(previous_device)

    @torch.no_grad()
    def run(self, prefill_embeds: torch.Tensor) -> torch.Tensor:
        # Cloned because the next replay overwrites the same buffer, and the caller
        # keeps these codes until the whole phrase has been generated.
        with torch.cuda.device(_device_index(self.device)):
            self.static_embeds.copy_(prefill_embeds)
            self.graph.replay()
            return self.static_tokens.clone()


# ---------------------------------------------------------------------------
# Stage 2: the talker decode step
# ---------------------------------------------------------------------------


class FixedTalkerCache(Cache):
    """Fixed-width key/value storage for the talker, written at a stable position.

    The predictor's cache could bake its offsets as Python integers because it always
    runs exactly fifteen sub-steps. The talker cannot: its length is the voice-clone
    prompt plus however many frames the phrase turns out to need, which is not known
    until an end-of-sequence token arrives. Baking an offset per step would mean one
    graph per frame.

    So the width is fixed and the write position lives in a tensor whose address is
    stable. One graph then serves every decode step: the position is read at replay
    rather than recorded at capture. Measured prefill is 86 positions and a 250-character
    phrase reaches position 252, so 512 leaves roughly twice the longest phrase in hand;
    anything longer falls back to the eager path rather than wrapping.
    """

    def __init__(
        self,
        num_layers: int,
        num_key_value_heads: int,
        head_dim: int,
        max_length: int,
        device: Any,
        dtype: Any,
    ) -> None:
        # Not calling Cache.__init__ for the same reason as the predictor cache: the
        # base builds layer objects for a growth model this storage does not have.
        self.layers: list = []
        self.offloading = False
        self.max_length = max_length
        self.key_cache = [
            torch.zeros(1, num_key_value_heads, max_length, head_dim,
                        device=device, dtype=dtype)
            for _ in range(num_layers)
        ]
        self.value_cache = [
            torch.zeros(1, num_key_value_heads, max_length, head_dim,
                        device=device, dtype=dtype)
            for _ in range(num_layers)
        ]
        # Read at replay, so one graph covers every decode step.
        self.position = torch.zeros(1, dtype=torch.long, device=device)
        self.graph_mode = False
        self.write_start = 0
        self.write_length = 1
        self.length = 0

    def plan_prefill(self, length: int) -> None:
        self.graph_mode = False
        self.write_start = 0
        self.write_length = length
        self.length = length

    def plan_decode(self, position: int) -> None:
        self.graph_mode = True
        self.position.fill_(position)
        self.length = position + 1

    def update(self, key_states, value_states, layer_idx, cache_kwargs=None):
        keys = self.key_cache[layer_idx]
        values = self.value_cache[layer_idx]
        if self.graph_mode:
            # Stable index tensor: the address is fixed and the value is read at replay.
            keys.index_copy_(2, self.position, key_states)
            values.index_copy_(2, self.position, value_states)
        else:
            start, end = self.write_start, self.write_start + self.write_length
            keys[:, :, start:end, :].copy_(key_states)
            values[:, :, start:end, :].copy_(value_states)
        # Always the full width. The caller supplies a mask that admits only the
        # positions written so far, which is what keeps the shapes constant enough to
        # capture while still excluding everything stale.
        return keys, values

    def get_seq_length(self, layer_idx: int = 0) -> int:
        return self.length

    def get_max_cache_shape(self, layer_idx: int = 0) -> int:
        return self.max_length

    def reset(self) -> None:
        self.length = 0
        self.write_start = 0
        self.write_length = 1

    def __len__(self) -> int:
        return len(self.key_cache)


class GraphedTalkerDecode:
    """Replays the talker's 28-layer decode step as one CUDA graph.

    EXPERIMENTAL. Measured on an RTX 5070: 64.68 ms eager, 5.535 ms replayed -- 11.69x --
    for the same reason the predictor was slow, which is per-launch overhead rather than
    arithmetic.

    Only the decode step is graphed. Prefill is variable-length and happens once per
    phrase, so it runs eagerly into the same cache. Everything above the inner model --
    sampling, end-of-sequence detection, the token budget, the codebook predictor -- is
    untouched and still Hugging Face's. That matters: a corrupted hidden state that
    prevented end-of-sequence is exactly how the Stage 1 lifetime bug disguised itself as
    a hang, so this deliberately does not take responsibility for stopping.
    """

    def __init__(self, talker: Any, max_length: int = 512) -> None:
        self.talker = talker
        self.inner = talker.model
        config = talker.config
        self.max_length = max_length
        self.hidden_size = config.hidden_size
        heads = config.num_attention_heads
        self.kv_heads = getattr(config, "num_key_value_heads", heads)
        self.head_dim = getattr(config, "head_dim", config.hidden_size // heads)
        self.num_layers = config.num_hidden_layers
        parameter = next(self.inner.parameters())
        self.device = parameter.device
        self.dtype = parameter.dtype

        self.cache: Any = None
        self.graph = None
        self.original_forward = None
        self.capture_ms = -1.0
        self.failure = ""
        self.attempted = False
        # Held on the instance for the whole life of the graph. Letting any of these
        # fall out of scope frees memory the graph still writes to and reads from, which
        # is the Stage 1 bug and is silent until the output is subtly wrong.
        self.static_embeds = None
        self.static_position_ids = None
        self.static_cache_position = None
        self.static_hidden = None
        self.arange = None
        self.next_position = 0
        self.replays = 0
        self.eager_steps = 0
        self.installed = False

    # ------------------------------------------------------------------ masks
    def _decode_mask(self) -> torch.Tensor:
        # Built from the position tensor inside the captured region, so it follows the
        # position at replay instead of freezing the one seen at capture.
        return self.arange.view(1, 1, 1, -1) <= self.cache.position.view(1, 1, 1, 1)

    def _prefill_mask(self, length: int) -> torch.Tensor:
        rows = self.arange[:length].view(length, 1)
        allowed = (self.arange.view(1, -1) <= rows)
        return allowed.view(1, 1, length, self.max_length)

    # ---------------------------------------------------------------- capture
    def _decode_forward(self):
        return self.original_forward(
            input_ids=None,
            attention_mask=self._decode_mask(),
            position_ids=self.static_position_ids,
            past_key_values=self.cache,
            inputs_embeds=self.static_embeds,
            use_cache=True,
            cache_position=self.static_cache_position,
            output_hidden_states=False,
        )

    def capture(self) -> bool:
        if self.graph is not None:
            return True
        self.attempted = True
        if not str(self.device).startswith("cuda"):
            self.failure = "CUDA graphs need a CUDA device."
            return False
        index = _device_index(self.device)
        previous_device = torch.cuda.current_device() if index is not None else None
        try:
            # The capturing device must be current. torch.cuda.graph() records on
            # whatever device is current, so capturing a cuda:1 model while cuda:0 is
            # current yields an EMPTY graph: it warns rather than raising, replays
            # nothing, and leaves the previous contents in the output buffer.
            if index is not None:
                torch.cuda.set_device(index)
            # Grabbed before capture, not in install(): capture runs the real forward to
            # record it, so it needs the unrouted one already in hand.
            if self.original_forward is None:
                self.original_forward = self.inner.forward
            self.cache = FixedTalkerCache(
                num_layers=self.num_layers, num_key_value_heads=self.kv_heads,
                head_dim=self.head_dim, max_length=self.max_length,
                device=self.device, dtype=self.dtype)
            self.arange = torch.arange(self.max_length, device=self.device)
            self.static_embeds = torch.zeros(
                1, 1, self.hidden_size, device=self.device, dtype=self.dtype)
            self.static_position_ids = torch.zeros(
                3, 1, 1, dtype=torch.long, device=self.device)
            self.static_cache_position = torch.zeros(
                1, dtype=torch.long, device=self.device)
            self.cache.plan_decode(1)

            stream = torch.cuda.Stream()
            stream.wait_stream(torch.cuda.current_stream())
            with torch.cuda.stream(stream):
                for _ in range(5):
                    self._decode_forward()
            torch.cuda.current_stream().wait_stream(stream)
            torch.cuda.synchronize()

            started = time.perf_counter()
            graph = torch.cuda.CUDAGraph()
            with torch.no_grad():
                with torch.cuda.graph(graph):
                    self.static_hidden = self._decode_forward().last_hidden_state
            torch.cuda.synchronize()
            self.capture_ms = (time.perf_counter() - started) * 1000.0
            if self.static_hidden is None:
                raise RuntimeError("capture produced no output tensor")
            self.graph = graph
            return True
        except Exception as exception:  # noqa: BLE001 - any failure means eager
            self.failure = f"{type(exception).__name__}: {exception}"
            self.graph = None
            return False
        finally:
            if previous_device is not None:
                torch.cuda.set_device(previous_device)

    # ---------------------------------------------------------------- install
    def install(self) -> None:
        """Routes the inner model's forward through prefill-eager / decode-graph."""
        from transformers.modeling_outputs import BaseModelOutputWithPast

        # An explicit flag, not an identity test against the stored forward: attribute
        # access on a bound method produces a new object every time, so `is` comparisons
        # against it are always false and the routing silently never happened.
        if self.installed:
            return
        if self.original_forward is None:
            self.original_forward = self.inner.forward

        def routed(*args: Any, **kwargs: Any):
            embeds = kwargs.get("inputs_embeds")
            if embeds is None or self.graph is None:
                return self.original_forward(*args, **kwargs)

            # Batch one only. The cache and the graph are both shaped for a single
            # sequence, and the batched reply path deliberately runs several at once --
            # sending those through here would write keys of the wrong shape into
            # storage sized for one. The batched path is already near real time by
            # amortising the same overhead a different way, so it keeps the eager route.
            if embeds.shape[0] != 1:
                self.eager_steps += 1
                return self.original_forward(*args, **kwargs)

            length = embeds.shape[1]
            if length > 1:
                # Prefill: variable length, once per phrase, and it is what resets the
                # cache for the phrase that follows.
                if length > self.max_length:
                    self.next_position = -1
                    return self.original_forward(*args, **kwargs)
                self.cache.plan_prefill(length)
                kwargs = dict(kwargs)
                kwargs["past_key_values"] = self.cache
                kwargs["attention_mask"] = self._prefill_mask(length)
                outputs = self.original_forward(*args, **kwargs)
                self.next_position = length
                return outputs

            if self.next_position < 0 or self.next_position >= self.max_length:
                # Longer than this cache can hold. Eager rather than wrapping, which
                # would silently attend to another phrase's keys.
                self.eager_steps += 1
                return self.original_forward(*args, **kwargs)

            position_ids = kwargs.get("position_ids")
            self.static_embeds.copy_(embeds)
            if position_ids is not None:
                self.static_position_ids.copy_(position_ids)
            self.static_cache_position.fill_(self.next_position)
            self.cache.plan_decode(self.next_position)
            with torch.cuda.device(_device_index(self.device)):
                self.graph.replay()
            self.next_position += 1
            self.replays += 1
            # Cloned because the next replay overwrites this buffer, and the caller keeps
            # the last hidden state as `past_hidden` for the following frame.
            hidden = self.static_hidden.clone()
            # The outer generation loop collects hidden states per step and takes the
            # last layer out of each: `hid[0][-1][:, -1:]`. After the final norm that is
            # the same tensor as last_hidden_state, so a one-element tuple satisfies it
            # without the graph having to collect all twenty-eight layers.
            return BaseModelOutputWithPast(
                last_hidden_state=hidden,
                past_key_values=self.cache,
                hidden_states=(hidden,),
            )

        self.inner.forward = routed
        self.installed = True

    def uninstall(self) -> None:
        if self.installed and self.original_forward is not None:
            self.inner.forward = self.original_forward
            self.installed = False


class _SequencesOnly:
    """The one field the upstream caller reads off the reference result."""

    __slots__ = ("sequences",)

    def __init__(self, sequences: torch.Tensor) -> None:
        self.sequences = sequences


def install(
    model: Any,
    instrument: bool = False,
    use_cuda_graph: bool = False,
    use_talker_graph: bool = False,
) -> DirectCodePredictor:
    """Point the talker's inner loop at the direct decoder.

    Patches the bound ``generate`` on the predictor instance rather than editing the
    package, so the change is confined to one loaded model and ``uninstall`` puts the
    original back. The talker's own outer loop, the voice-clone conditioning, the
    vocoder and every weight are untouched.
    """
    predictor = model.model.talker.code_predictor
    if getattr(predictor, "_revia_original_generate", None) is not None:
        existing = predictor._revia_direct
        existing.instrument = instrument
        if use_cuda_graph and existing.graph is None:
            graph = GraphedInnerLoop(
                predictor=predictor,
                num_code_groups=model.model.talker.config.num_code_groups,
                hidden_size=model.model.talker.config.hidden_size,
                sampler=existing._sample,
                device=next(predictor.parameters()).device,
                dtype=next(predictor.parameters()).dtype,
            )
            if graph.capture():
                existing.graph = graph
        return existing

    talker_config = model.model.talker.config
    direct = DirectCodePredictor(
        code_predictor=predictor,
        num_code_groups=talker_config.num_code_groups,
        instrument=instrument,
    )

    def patched_generate(*args: Any, **kwargs: Any) -> _SequencesOnly:
        prefill = kwargs.get("inputs_embeds")
        if prefill is None:
            # Not the call this replaces. Hand it back to the original rather than
            # guessing, so any other use of the predictor keeps working.
            return predictor._revia_original_generate(*args, **kwargs)
        return _SequencesOnly(direct.generate_codes(prefill))

    if use_cuda_graph:
        graph = GraphedInnerLoop(
            predictor=predictor,
            num_code_groups=talker_config.num_code_groups,
            hidden_size=talker_config.hidden_size,
            sampler=direct._sample,
            device=next(predictor.parameters()).device,
            dtype=next(predictor.parameters()).dtype,
        )
        # Captured here, at install, which is a quiescent point: no generation is in
        # flight and the device can be synchronised first. Capturing from inside the
        # talker's own loop is not safe, and an earlier attempt to do so lazily is what
        # produced a graph that never terminated.
        if graph.capture():
            direct.graph = graph

    # Stage 2 is a separate switch on purpose. Stage 1 is the path that has been
    # measured end to end; a talker graph that fails must not take it down with it.
    if use_talker_graph:
        talker_graph = GraphedTalkerDecode(model.model.talker)
        if talker_graph.capture():
            talker_graph.install()
            direct.talker_graph = talker_graph
        else:
            direct.talker_graph = None

    predictor._revia_original_generate = predictor.generate
    predictor._revia_direct = direct
    predictor.generate = patched_generate
    return direct


def uninstall(model: Any) -> None:
    """Restores the stock Hugging Face path."""
    predictor = model.model.talker.code_predictor
    existing = getattr(predictor, "_revia_direct", None)
    talker_graph = getattr(existing, "talker_graph", None) if existing else None
    if talker_graph is not None:
        talker_graph.uninstall()
        existing.talker_graph = None
    original = getattr(predictor, "_revia_original_generate", None)
    if original is not None:
        predictor.generate = original
        predictor._revia_original_generate = None
        predictor._revia_direct = None
