from __future__ import annotations

import http.client
import importlib.util
import json
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path


SERVICE = Path(__file__).resolve().parents[1] / "Tools" / "qwen_tts_service.py"
SPEC = importlib.util.spec_from_file_location("revia_qwen_tts_service", SERVICE)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FakeArgs:
    input_mode = "complete"


class FakeRuntime:
    model_kind = ""
    device = "cpu"
    device_name = "CPU"
    dtype_name = "float32"
    attention_backend = "auto"
    cuda_math_mode = "default"
    model = None
    loaded_at = 0.0
    last_used = 0.0
    clone_prompts: dict = {}
    args = FakeArgs()

    def vocalizations(self, request: dict) -> dict:
        return {"succeeded": True, "received": request.get("sentinel")}


class QwenHandlerTests(unittest.TestCase):
    def test_vocalization_endpoint_passes_the_parsed_request(self) -> None:
        token = "test-token"
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0), MODULE.make_handler(FakeRuntime(), token))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_address[1], timeout=5)
            body = json.dumps({"sentinel": "request-object"})
            connection.request(
                "POST", "/v1/vocalizations", body,
                {"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
            response = connection.getresponse()
            result = json.loads(response.read().decode("utf-8"))
            self.assertEqual(response.status, 200)
            self.assertEqual(result["received"], "request-object")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


class _FakeMatmul:
    allow_tf32 = False


class _FakeCudaBackend:
    matmul = _FakeMatmul()


class _FakeCudnnBackend:
    allow_tf32 = False


class _FakeBackends:
    cuda = _FakeCudaBackend()
    cudnn = _FakeCudnnBackend()


class _FakeCuda:
    def __init__(self, major: int) -> None:
        self.major = major

    def get_device_capability(self, index: int) -> tuple[int, int]:
        del index
        return self.major, 0


class _FakeTorch:
    def __init__(self, major: int) -> None:
        self.cuda = _FakeCuda(major)
        self.backends = _FakeBackends()
        self.precision = "highest"

    def set_float32_matmul_precision(self, precision: str) -> None:
        self.precision = precision


class CudaMathPolicyTests(unittest.TestCase):
    def test_ampere_enables_tf32_for_residual_float32_inference(self) -> None:
        torch = _FakeTorch(8)

        enabled = MODULE._enable_ampere_tf32(torch, "cuda:0")

        self.assertTrue(enabled)
        self.assertEqual(torch.precision, "high")
        self.assertTrue(torch.backends.cuda.matmul.allow_tf32)
        self.assertTrue(torch.backends.cudnn.allow_tf32)

    def test_cpu_and_turing_keep_the_default_math_path(self) -> None:
        cpu = _FakeTorch(8)
        turing = _FakeTorch(7)

        self.assertFalse(MODULE._enable_ampere_tf32(cpu, "cpu"))
        self.assertFalse(MODULE._enable_ampere_tf32(turing, "cuda:0"))
        self.assertEqual(cpu.precision, "highest")
        self.assertEqual(turing.precision, "highest")



class BatchFitTests(unittest.TestCase):
    """The VRAM gate that decides whether a batch may be attempted."""

    def _runtime(self, dtype_name, free_mib, reserved_mib, allocated_mib):
        runtime = MODULE.QwenRuntime.__new__(MODULE.QwenRuntime)
        runtime.device = "cuda:0"
        runtime.dtype_name = dtype_name

        class Args:
            minimum_free_vram_mib = 4600

        runtime.args = Args()
        self._free = free_mib * 1024 * 1024
        self._reserved = reserved_mib * 1024 * 1024
        self._allocated = allocated_mib * 1024 * 1024
        return runtime

    def _patched_fits(self, runtime, characters, phrases):
        """Runs the real estimate against a stubbed allocator."""
        import types

        fake_cuda = types.SimpleNamespace(
            mem_get_info=lambda index: (self._free, 12 * 1024 * 1024 * 1024),
            memory_reserved=lambda index: self._reserved,
            memory_allocated=lambda index: self._allocated,
        )
        fake_torch = types.SimpleNamespace(cuda=fake_cuda)
        import sys

        saved = sys.modules.get("torch")
        sys.modules["torch"] = fake_torch
        try:
            return runtime._batch_fits(characters, phrases)
        finally:
            if saved is not None:
                sys.modules["torch"] = saved
            else:
                del sys.modules["torch"]

    def test_reusable_cached_memory_counts_as_available(self):
        # The regression that mattered: PyTorch keeps the first batch's arena, the
        # driver reports it as taken, and every later batch refused itself. Blocks the
        # allocator holds but is not using are available for this allocation.
        runtime = self._runtime("bfloat16", free_mib=1600,
                                reserved_mib=4000, allocated_mib=2100)
        fits, reason, projected = self._patched_fits(runtime, characters=195, phrases=5)
        self.assertTrue(fits, f"a batch was refused despite reusable cache: {reason}")

    def test_driver_free_alone_would_have_refused(self):
        # Same card with nothing cached: this one genuinely does not fit, and refusing
        # is correct rather than optimistic.
        runtime = self._runtime("bfloat16", free_mib=1600,
                                reserved_mib=2100, allocated_mib=2100)
        fits, reason, projected = self._patched_fits(runtime, characters=195, phrases=5)
        self.assertFalse(fits)
        self.assertIn("exceeds", reason)

    def test_estimate_charges_per_phrase_as_well_as_per_character(self):
        # Five short phrases cost more than their character count suggests, which is
        # what the earlier per-character-only estimate got wrong by over 2x.
        runtime = self._runtime("bfloat16", free_mib=40000,
                                reserved_mib=0, allocated_mib=0)
        _, _, few_long = self._patched_fits(runtime, characters=200, phrases=1)
        _, _, many_short = self._patched_fits(runtime, characters=200, phrases=5)
        self.assertGreater(many_short, few_long,
                           "phrase count did not affect the estimate")

    def test_float32_doubles_the_estimate(self):
        # Turing runs float32, which holds twice the activation width for the same work.
        bf16 = self._runtime("bfloat16", free_mib=40000, reserved_mib=0, allocated_mib=0)
        fp32 = self._runtime("float32", free_mib=40000, reserved_mib=0, allocated_mib=0)
        _, _, small = self._patched_fits(bf16, characters=480, phrases=6)
        _, _, large = self._patched_fits(fp32, characters=480, phrases=6)
        self.assertAlmostEqual(large, small * 2, delta=2)


class BackendStateTests(unittest.TestCase):
    """What the worker says about which inference path it is on.

    This is the reporting that the 2026-09-02 session did not have. Every request that
    session ran on stock generation while the configuration said otherwise, and nothing
    said so, because "configured" and "running" were never separate facts.
    """

    def _runtime(self, low_latency, cuda_graph, talker_graph):
        import types

        runtime = MODULE.QwenRuntime.__new__(MODULE.QwenRuntime)
        runtime.args = types.SimpleNamespace(
            low_latency=low_latency,
            cuda_graph=cuda_graph,
            talker_graph=talker_graph,
        )
        runtime.backend = "standard"
        runtime.low_latency_detail = ""
        runtime.low_latency_direct = None
        runtime.model = None
        return runtime

    @staticmethod
    def _installed(runtime, predictor_captured, talker_captured):
        """The shape install() leaves behind, with capture having taken or not."""
        import types

        runtime.backend = "low_latency"
        runtime.model = object()
        graph = types.SimpleNamespace(graph=object()) if predictor_captured else None
        talker = types.SimpleNamespace(
            graph=object() if talker_captured else None)
        runtime.low_latency_direct = types.SimpleNamespace(
            graph=graph, talker_graph=talker)

    def test_a_requested_graph_is_not_reported_active_before_capture(self):
        # The load-time state of a perfectly healthy worker: the module installed, and
        # capture is deferred to the first eligible phrase. Reporting a graph here
        # would be a claim about something that has not happened.
        runtime = self._runtime(True, True, True)
        self._installed(runtime, predictor_captured=False, talker_captured=False)
        state = runtime._backend_state()
        self.assertTrue(state["low_latency_requested"])
        self.assertTrue(state["low_latency_installed"])
        self.assertTrue(state["cuda_graph_requested"])
        self.assertFalse(state["cuda_graph"],
                         "an uncaptured graph was reported as active")
        self.assertFalse(state["talker_graph"])

    def test_a_captured_graph_is_reported_active(self):
        runtime = self._runtime(True, True, True)
        self._installed(runtime, predictor_captured=True, talker_captured=True)
        state = runtime._backend_state()
        self.assertEqual(state["backend"], "low_latency")
        self.assertTrue(state["cuda_graph"])
        self.assertTrue(state["talker_graph"])

    def test_stage_two_can_fail_without_taking_stage_one_down(self):
        runtime = self._runtime(True, True, True)
        self._installed(runtime, predictor_captured=True, talker_captured=False)
        state = runtime._backend_state()
        self.assertTrue(state["cuda_graph"])
        self.assertFalse(state["talker_graph"])
        self.assertTrue(state["talker_graph_requested"],
                        "the request was lost along with the capture")

    def test_a_failed_install_reports_stock_generation_and_says_why(self):
        runtime = self._runtime(True, True, True)
        runtime.model = object()
        runtime.low_latency_detail = "unavailable: no module named qwen_lowlatency"
        state = runtime._backend_state()
        self.assertEqual(state["backend"], "standard")
        self.assertFalse(state["low_latency_installed"])
        self.assertEqual(state["low_latency_state"], "unavailable")
        self.assertTrue(state["low_latency_requested"],
                        "a failed install erased the fact that it was asked for")
        self.assertIn("unavailable", state["low_latency_detail"])

    def test_before_any_model_loads_nothing_is_claimed_either_way(self):
        # A worker polled at startup has not attempted the install yet. Reporting that
        # as a failure sends someone looking for a broken module that is fine.
        runtime = self._runtime(True, True, True)
        state = runtime._backend_state()
        self.assertEqual(state["low_latency_state"], "not-loaded")
        self.assertFalse(state["low_latency_installed"])
        self.assertFalse(state["cuda_graph"])

    def test_nothing_requested_reports_nothing_active(self):
        runtime = self._runtime(False, False, False)
        state = runtime._backend_state()
        self.assertFalse(state["low_latency_requested"])
        self.assertEqual(state["low_latency_state"], "off")
        self.assertFalse(state["cuda_graph"])
        self.assertFalse(state["talker_graph"])


class LowLatencySamplerTests(unittest.TestCase):
    """The sampler the direct-forward loop uses in place of Hugging Face's warpers."""

    @classmethod
    def setUpClass(cls):
        try:
            import torch  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("torch is not installed")
        import importlib.util as iu

        path = Path(__file__).resolve().parents[1] / "Tools" / "qwen_lowlatency.py"
        spec = iu.spec_from_file_location("revia_qwen_lowlatency", path)
        cls.module = iu.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def _predictor(self, **kwargs):
        return self.module.DirectCodePredictor(
            code_predictor=None, num_code_groups=16, **kwargs)

    def test_top_k_excludes_everything_below_the_kth_best(self):
        import torch

        direct = self._predictor(top_k=2, temperature=1.0)
        # Only the top two may ever be drawn, however many times it is asked.
        logits = torch.tensor([[10.0, 9.0, -20.0, -30.0]])
        drawn = {int(direct._sample(logits)) for _ in range(200)}
        self.assertTrue(drawn <= {0, 1}, f"top-k admitted a masked token: {drawn}")

    def test_greedy_when_sampling_is_off(self):
        import torch

        direct = self._predictor(do_sample=False)
        logits = torch.tensor([[1.0, 5.0, 2.0]])
        self.assertEqual(int(direct._sample(logits)), 1)

    def test_temperature_flattens_the_distribution(self):
        import torch

        torch.manual_seed(0)
        sharp = self._predictor(top_k=0, temperature=0.05)
        flat = self._predictor(top_k=0, temperature=5.0)
        logits = torch.tensor([[2.0, 1.5, 1.0, 0.5]])
        sharp_draws = {int(sharp._sample(logits)) for _ in range(100)}
        flat_draws = {int(flat._sample(logits)) for _ in range(100)}
        self.assertLessEqual(len(sharp_draws), len(flat_draws))

    def test_sampling_happens_in_float32(self):
        # bfloat16 quantises probabilities coarsely enough to change which token is
        # drawn, so the reference path's float32 softmax is reproduced, not approximated.
        import torch

        direct = self._predictor(top_k=50, temperature=0.9)
        logits = torch.tensor([[1.0, 2.0, 3.0]], dtype=torch.bfloat16)
        token = direct._sample(logits)
        self.assertEqual(token.dtype, torch.int64)
        self.assertTrue(0 <= int(token) < 3)


class GraphInputLifetimeTests(unittest.TestCase):
    """Guards the bug that made captured replays read freed memory.

    A CUDA graph records the addresses of its inputs. The cache-position tensors were
    built as a local inside capture(), so they were freed the moment it returned, the
    allocator reused the memory, and every replay afterwards read whatever now sat there
    as its positions. The prefill token still matched, which made it look like a cache
    fault rather than a lifetime fault, and it cost a long detour.

    There is no GPU in this test. What is checked is the property that made the bug
    possible: every tensor the captured loop consumes must be reachable from the object
    that owns the graph, for as long as the graph exists.
    """

    @classmethod
    def setUpClass(cls):
        try:
            import torch  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("torch is not installed")
        import importlib.util as iu

        path = Path(__file__).resolve().parents[1] / "Tools" / "qwen_lowlatency.py"
        spec = iu.spec_from_file_location("revia_qwen_lowlatency_graph", path)
        cls.module = iu.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_capture_stores_positions_on_the_instance(self):
        import inspect

        source = inspect.getsource(self.module.GraphedInnerLoop.capture)
        self.assertIn("self.positions", source,
                      "cache positions must outlive capture(), not be a local")
        self.assertNotIn("        positions = [torch.arange", source,
                         "positions were rebuilt as a bare local, which frees them")

    def test_graph_holder_declares_every_captured_input(self):
        holder = self.module.GraphedInnerLoop.__new__(self.module.GraphedInnerLoop)
        self.module.GraphedInnerLoop.__init__(
            holder, predictor=None, num_code_groups=16, hidden_size=1024,
            sampler=None, device="cpu", dtype=None)
        for field in ("static_embeds", "static_tokens", "positions", "cache"):
            self.assertTrue(hasattr(holder, field),
                            f"{field} must be an owned attribute, not a local")

    def test_owned_cache_returns_only_the_written_prefix(self):
        # The narrow view is what excludes a previous phrase's keys without a mask and
        # without zeroing. If it ever returned the whole buffer, stale positions would
        # be attended to and nothing would obviously break.
        import torch

        cache = self.module.GraphOwnedPredictorCache(
            num_layers=1, num_key_value_heads=2, head_dim=4, max_length=16,
            device="cpu", dtype=torch.float32)
        cache.plan(0, 2)
        keys, values = cache.update(torch.ones(1, 2, 2, 4), torch.ones(1, 2, 2, 4), 0)
        self.assertEqual(keys.shape[2], 2)
        cache.plan(2, 1)
        keys, values = cache.update(torch.ones(1, 2, 1, 4), torch.ones(1, 2, 1, 4), 0)
        self.assertEqual(keys.shape[2], 3, "decode must attend over prefill plus itself")
        self.assertEqual(values.shape[2], 3)

    def test_owned_cache_refuses_a_write_past_its_storage(self):
        import torch

        cache = self.module.GraphOwnedPredictorCache(
            num_layers=1, num_key_value_heads=2, head_dim=4, max_length=4,
            device="cpu", dtype=torch.float32)
        with self.assertRaises(ValueError):
            cache.plan(4, 1)


class TalkerGraphInstallTests(unittest.TestCase):
    """Guards the bug that made the talker routing silently never install.

    install() used to skip itself when `self.inner.forward is not self.original_forward`.
    Attribute access on a bound method builds a new object every time, so that identity
    test was always true and the routing never happened. Nothing failed -- generation
    simply stayed on the eager path, and a whole benchmark matrix reported the graphed
    talker as no faster than the ungraphed one before the counters showed zero replays.

    The lesson is that a silent no-op is worse than a crash, so installation state is now
    an explicit flag and the counters exist to prove routing happened.
    """

    @classmethod
    def setUpClass(cls):
        try:
            import torch  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("torch is not installed")
        import importlib.util as iu

        path = Path(__file__).resolve().parents[1] / "Tools" / "qwen_lowlatency.py"
        spec = iu.spec_from_file_location("revia_qwen_lowlatency_talker", path)
        cls.module = iu.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def test_install_state_is_a_flag_not_a_bound_method_identity(self):
        import inspect

        source = inspect.getsource(self.module.GraphedTalkerDecode.install)
        self.assertIn("self.installed", source,
                      "installation must be tracked by an explicit flag")
        self.assertNotIn("self.inner.forward is not self.original_forward", source,
                         "identity tests against a bound method are always true")

    def test_replay_counters_exist_so_a_no_op_is_visible(self):
        holder = self.module.GraphedTalkerDecode.__new__(
            self.module.GraphedTalkerDecode)
        for field in ("replays", "eager_steps", "installed", "next_position"):
            setattr(holder, field, 0)
            self.assertTrue(hasattr(holder, field))

    def test_fixed_talker_cache_writes_where_it_is_told(self):
        import torch

        cache = self.module.FixedTalkerCache(
            num_layers=1, num_key_value_heads=2, head_dim=4, max_length=8,
            device="cpu", dtype=torch.float32)
        # Prefill writes a range and reports its length.
        cache.plan_prefill(3)
        keys, _ = cache.update(torch.ones(1, 2, 3, 4), torch.ones(1, 2, 3, 4), 0)
        self.assertEqual(cache.get_seq_length(), 3)
        self.assertEqual(keys.shape[2], 8, "the width stays fixed so shapes can be captured")
        self.assertTrue(bool((keys[:, :, :3, :] == 1).all()))
        self.assertTrue(bool((keys[:, :, 3:, :] == 0).all()),
                        "prefill must not write past its own length")
        # Decode writes exactly one position, the one held in the tensor.
        cache.plan_decode(3)
        cache.update(torch.full((1, 2, 1, 4), 5.0), torch.full((1, 2, 1, 4), 5.0), 0)
        self.assertTrue(bool((cache.key_cache[0][:, :, 3, :] == 5).all()))
        self.assertEqual(cache.get_seq_length(), 4)

    def test_fixed_talker_cache_position_is_a_tensor(self):
        # It has to be read at replay, not baked at capture, because the talker's step
        # count is not known until end-of-sequence.
        import torch

        cache = self.module.FixedTalkerCache(
            num_layers=1, num_key_value_heads=2, head_dim=4, max_length=8,
            device="cpu", dtype=torch.float32)
        self.assertIsInstance(cache.position, torch.Tensor)


if __name__ == "__main__":
    unittest.main()
