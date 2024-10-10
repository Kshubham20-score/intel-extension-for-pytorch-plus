import unittest
import itertools
import torch
import intel_extension_for_pytorch as ipex
import intel_extension_for_pytorch._C as core
from torch.testing._internal.common_utils import TestCase
import copy
from intel_extension_for_pytorch.cpu._auto_kernel_selection import (
    _enable_tpp,
    _disable_tpp,
)


class Linear_with_bias(torch.nn.Module):
    def __init__(self):
        super(Linear_with_bias, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096)

    def forward(self, x):
        return self.mlp(x)


class Linear_without_bias(torch.nn.Module):
    def __init__(self):
        super(Linear_without_bias, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096, bias=False)

    def forward(self, x):
        return self.mlp(x)


class Linear_gelu(torch.nn.Module):
    def __init__(self):
        super(Linear_gelu, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096)

    def forward(self, x):
        return torch.nn.functional.gelu(self.mlp(x))


class Linear_silu(torch.nn.Module):
    def __init__(self):
        super(Linear_silu, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096, bias=False)

    def forward(self, x):
        return torch.nn.functional.silu(self.mlp(x))


class Linear_Gate_Up(torch.nn.Module):
    def __init__(self, in_feature, out_feature, bias_gate, bias_up):
        super(Linear_Gate_Up, self).__init__()
        self.gate_proj = torch.nn.Linear(in_feature, out_feature, bias=bias_gate)
        self.up_proj = torch.nn.Linear(in_feature, out_feature, bias=bias_up)

    def forward(self, x):
        return torch.nn.functional.silu(self.gate_proj(x)) * self.up_proj(x)


class Linear_relu(torch.nn.Module):
    def __init__(self):
        super(Linear_relu, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096, bias=False)

    def forward(self, x):
        return torch.nn.functional.relu(self.mlp(x))


class Linear_mul(torch.nn.Module):
    def __init__(self):
        super(Linear_mul, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096, bias=False)

    def forward(self, x):
        return self.mlp(x) * x


class Linear_add(torch.nn.Module):
    def __init__(self):
        super(Linear_add, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096, bias=False)

    def forward(self, x):
        return self.mlp(x) + x


class Linear_add_add(torch.nn.Module):
    def __init__(self):
        super(Linear_add_add, self).__init__()
        self.mlp = torch.nn.Linear(4096, 4096)

    def forward(self, x):
        return self.mlp(x) + x + x


class Linear_tpp_fallback_dnnl(torch.nn.Module):
    def __init__(self):
        super(Linear_tpp_fallback_dnnl, self).__init__()
        self.mlp = torch.nn.Linear(4097, 4097)

    def forward(self, x):
        return self.mlp(x)


class TestTPPlinear(TestCase):
    def test_tpp_linear_fallback_flag(self):
        x1 = torch.rand(1, 1, 4097)
        x2 = copy.deepcopy(x1)
        dtypes = [torch.float, torch.bfloat16]
        if core.onednn_has_fp16_support():
            dtypes.append(torch.float16)
        for dtype in dtypes:
            model = Linear_tpp_fallback_dnnl().eval()

            with torch.no_grad(), torch.cpu.amp.autocast(
                enabled=True if dtype in [torch.bfloat16, torch.float16] else False,
                dtype=dtype,
            ):
                ref_out = model(x1)

            model = ipex.optimize(model, dtype=dtype)
            with torch.no_grad(), torch.cpu.amp.autocast(
                enabled=True if dtype in [torch.bfloat16, torch.float16] else False,
                dtype=dtype,
            ):
                model = torch.jit.script(model)
                model = torch.jit.freeze(model)
                out = model(x2)
            self.assertEqual(out, ref_out)

    def test_tpp_linear_fallback(self):
        x1 = torch.rand(1, 1, 4097)
        x2 = copy.deepcopy(x1)
        dtypes = [torch.float, torch.bfloat16]
        if core.onednn_has_fp16_support():
            dtypes.append(torch.float16)
        for dtype in dtypes:
            model = Linear_tpp_fallback_dnnl().eval()

            with torch.no_grad(), torch.cpu.amp.autocast(
                enabled=True if dtype in [torch.bfloat16, torch.float16] else False,
                dtype=dtype,
            ):
                ref_out = model(x1)

            _enable_tpp()
            model = ipex.optimize(model, dtype=dtype)
            with torch.no_grad(), torch.cpu.amp.autocast(
                enabled=True if dtype in [torch.bfloat16, torch.float16] else False,
                dtype=dtype,
            ):
                out = model(x2)
            self.assertEqual(out, ref_out)
            _disable_tpp()

    def test_tpp_linear(self):
        x1 = torch.rand(1, 1, 4096)
        x2 = copy.deepcopy(x1)
        dtypes = [torch.float, torch.bfloat16]
        if core.onednn_has_fp16_support():
            dtypes.append(torch.float16)
        for dtype in dtypes:
            model = Linear_with_bias().eval()
            model_nb = Linear_without_bias().eval()
            if dtype is torch.bfloat16:
                x1 = x1.to(torch.bfloat16)
                x2 = x2.to(torch.bfloat16)
                model = model.to(torch.bfloat16)
                model_nb = model_nb.to(torch.bfloat16)
            elif dtype == torch.float16:
                x1 = x1.to(torch.float16)
                x2 = x2.to(torch.float16)
                model = model.to(torch.float16)
                model_nb = model_nb.to(torch.float16)
            ref_out = model(x1)
            ref_out_nb = model_nb(x1)

            _enable_tpp()
            model = ipex.optimize(model, dtype=dtype)
            model_nb = ipex.optimize(model_nb, dtype=dtype)
            out = model(x2)
            out_nb = model_nb(x2)
            self.assertEqual(out, ref_out)
            self.assertEqual(out_nb, ref_out_nb)
            _disable_tpp()

    def test_tpp_fused_gate_up_proj(self):
        in_feature = 64
        out_feature = 32

        x = torch.randn(1, 4, in_feature)
        x_tpp = copy.deepcopy(x)
        dtypes = [torch.float, torch.bfloat16]
        if core.isa_has_amx_fp16_support():
            dtypes.append(torch.float16)
        with torch.no_grad():
            for dtype, bias_gate, bias_up in itertools.product(
                dtypes,
                [False, True],
                [False, True],
            ):
                model = Linear_Gate_Up(
                    in_feature, out_feature, bias_gate, bias_up
                ).eval()
                if dtype == torch.bfloat16:
                    x = x.to(torch.bfloat16)
                    x_tpp = x_tpp.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype == torch.float16:
                    x = x.to(torch.float16)
                    x_tpp = x_tpp.to(torch.float16)
                    model = model.to(torch.float16)

                ref_out = model(x)

                _enable_tpp()
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_fused_gate_up_proj(
                    x_tpp,
                    model.gate_proj.weight,
                    model.gate_proj.bias,
                    model.up_proj.weight,
                    model.up_proj.bias,
                )

                out_linear_silu = torch.ops.torch_ipex.tpp_linear_silu(
                    x_tpp, model.gate_proj.weight, model.gate_proj.bias
                )
                out_tpp_ref = torch.ops.torch_ipex.tpp_linear_mul(
                    x_tpp, out_linear_silu, model.up_proj.weight, model.up_proj.bias
                )
                atol = None
                rtol = None
                if dtype is torch.float16:
                    atol = 1e-3
                    rtol = 1e-3
                self.assertEqual(out, out_tpp_ref)
                self.assertEqual(out, ref_out, atol=atol, rtol=rtol)
                _disable_tpp()

    def test_tpp_linear_gelu(self):
        x1 = torch.rand(1, 4, 4096)
        x2 = copy.deepcopy(x1)
        with torch.no_grad():
            dtypes = [torch.float, torch.bfloat16]
            if core.isa_has_amx_fp16_support():
                dtypes.append(torch.float16)
            for dtype in dtypes:
                model = Linear_gelu().eval()
                if dtype is torch.bfloat16:
                    x1 = x1.to(torch.bfloat16)
                    x2 = x2.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype is torch.float16:
                    x1 = x1.to(torch.float16)
                    x2 = x2.to(torch.float16)
                    model = model.to(torch.float16)
                ref_out = model(x1)

                _enable_tpp()
                atol = None
                rtol = None
                if dtype == torch.float:
                    atol = 3e-5
                    rtol = 1.3e-6
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_linear_gelu(
                    x2, model.mlp.weight, model.mlp.bias
                )
                self.assertEqual(out, ref_out, atol=atol, rtol=rtol)
                _disable_tpp()

    def test_tpp_linear_silu(self):
        x1 = torch.rand(1, 4, 4096)
        x2 = copy.deepcopy(x1)
        with torch.no_grad():
            dtypes = [torch.float, torch.bfloat16]
            if core.isa_has_amx_fp16_support():
                dtypes.append(torch.float16)
            for dtype in dtypes:
                model = Linear_silu().eval()
                if dtype is torch.bfloat16:
                    x1 = x1.to(torch.bfloat16)
                    x2 = x2.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype is torch.float16:
                    x1 = x1.to(torch.float16)
                    x2 = x2.to(torch.float16)
                    model = model.to(torch.float16)
                ref_out = model(x1)

                _enable_tpp()
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_linear_silu(
                    x2, model.mlp.weight, x2.new_empty(0)
                )
                atol = None
                rtol = None
                if dtype == torch.float:
                    atol = 2e-5
                    rtol = 1.3e-6
                elif dtype == torch.float16:
                    atol = 1e-4
                    rtol = 2e-3
                self.assertEqual(out, ref_out, atol=atol, rtol=rtol)
                _disable_tpp()

    def test_tpp_linear_relu(self):
        x1 = torch.rand(1, 4, 4096)
        x2 = copy.deepcopy(x1)
        with torch.no_grad():
            dtypes = [torch.float, torch.bfloat16]
            if core.isa_has_amx_fp16_support():
                dtypes.append(torch.float16)
            for dtype in dtypes:
                model = Linear_relu().eval()
                if dtype is torch.bfloat16:
                    x1 = x1.to(torch.bfloat16)
                    x2 = x2.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype is torch.float16:
                    x1 = x1.to(torch.float16)
                    x2 = x2.to(torch.float16)
                    model = model.to(torch.float16)
                ref_out = model(x1)

                _enable_tpp()
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_linear_relu(
                    x2, model.mlp.weight, x2.new_empty(0)
                )
                self.assertEqual(out, ref_out)
                _disable_tpp()

    def test_tpp_linear_mul(self):
        x1 = torch.rand(1, 4, 4096)
        x2 = copy.deepcopy(x1)
        with torch.no_grad():
            dtypes = [torch.float, torch.bfloat16]
            if core.isa_has_amx_fp16_support():
                dtypes.append(torch.float16)
            for dtype in dtypes:
                model = Linear_mul().eval()
                if dtype is torch.bfloat16:
                    x1 = x1.to(torch.bfloat16)
                    x2 = x2.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype is torch.float16:
                    x1 = x1.to(torch.float16)
                    x2 = x2.to(torch.float16)
                    model = model.to(torch.float16)
                ref_out = model(x1)

                _enable_tpp()
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_linear_mul(
                    x2, x2, model.mlp.weight, x2.new_empty(0)
                )
                atol = 1e-3 if dtype == torch.float16 else None
                rtol = 1e-3 if dtype == torch.float16 else None
                self.assertEqual(out, ref_out, atol=atol, rtol=rtol)
                _disable_tpp()

    def test_tpp_linear_add(self):
        x1 = torch.rand(1, 4, 4096)
        x2 = copy.deepcopy(x1)
        with torch.no_grad():
            dtypes = [torch.float, torch.bfloat16]
            if core.isa_has_amx_fp16_support():
                dtypes.append(torch.float16)
            for dtype in dtypes:
                model = Linear_add().eval()
                if dtype is torch.bfloat16:
                    x1 = x1.to(torch.bfloat16)
                    x2 = x2.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype is torch.float16:
                    x1 = x1.to(torch.float16)
                    x2 = x2.to(torch.float16)
                    model = model.to(torch.float16)
                ref_out = model(x1)

                _enable_tpp()
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_linear_add(
                    x2, x2, model.mlp.weight, x2.new_empty(0), 1.0
                )
                atol = None
                rtol = None
                if dtype is torch.float16:
                    atol = 1e-3
                    rtol = 1e-3
                self.assertEqual(out, ref_out, atol=atol, rtol=rtol)
                _disable_tpp()

    def test_tpp_linear_add2(self):
        x1 = torch.rand(1, 4, 4096)
        x2 = copy.deepcopy(x1)
        with torch.no_grad():
            dtypes = [torch.float, torch.bfloat16]
            if core.isa_has_amx_fp16_support():
                dtypes.append(torch.float16)
            for dtype in dtypes:
                model = Linear_add_add().eval()
                if dtype is torch.bfloat16:
                    x1 = x1.to(torch.bfloat16)
                    x2 = x2.to(torch.bfloat16)
                    model = model.to(torch.bfloat16)
                elif dtype is torch.float16:
                    x1 = x1.to(torch.float16)
                    x2 = x2.to(torch.float16)
                    model = model.to(torch.float16)
                ref_out = model(x1)

                _enable_tpp()
                model = ipex.optimize(model, dtype=dtype)
                out = torch.ops.torch_ipex.tpp_linear_add_add(
                    x2, x2, x2, model.mlp.weight, model.mlp.bias, 1.0
                )
                atol = 1e-3 if dtype == torch.float16 else None
                rtol = 0.01 if dtype == torch.float16 else None
                self.assertEqual(out, ref_out, atol=atol, rtol=rtol)
                _disable_tpp()


if __name__ == "__main__":
    test = unittest.main()
