# -*- coding: utf-8 -*-

import torch
from torch.testing._internal.common_utils import TestCase
import intel_extension_for_pytorch  # noqa
import pytest

cpu_device = torch.device("cpu")
dpcpp_device = torch.device("xpu")


class TestTorchMethod(TestCase):
    @pytest.mark.skipif(not torch.xpu.has_onemkl(), reason="not torch.xpu.has_onemkl()")
    def test_svd(self, dtype=torch.float):
        # Since U and V of an SVD is not unique, each vector can be multiplied by an arbitrary phase factor e^iϕ
        # while the SVD result is still correct. Different platforms, like Numpy, or inputs on different device types,
        # may produce different U and V tensors.

        a = torch.randn(5, 5)
        a_xpu = a.to("xpu")

        u, s, v = torch.svd(a)
        r_cpu = torch.mm(torch.mm(u, torch.diag(s)), v.t())

        u_xpu, s_xpu, v_xpu = torch.svd(a_xpu)
        r_xpu = torch.mm(torch.mm(u_xpu, torch.diag(s_xpu)), v_xpu.t())

        self.assertEqual(r_cpu, r_xpu.cpu())

    @pytest.mark.skipif(not torch.xpu.has_onemkl(), reason="not torch.xpu.has_onemkl()")
    def test_batch_svd(self, dtype=torch.float):
        # Since U and V of an SVD is not unique, each vector can be multiplied by an arbitrary phase factor e^iϕ
        # while the SVD result is still correct. Different platforms, like Numpy, or inputs on different device types,
        # may produce different U and V tensors.
        a = torch.randn(5, 5, 5)
        a_xpu = a.to("xpu")

        u, s, v = torch.svd(a)
        r_cpu = torch.matmul(torch.matmul(u, torch.diag_embed(s)), v.transpose(-2, -1))

        u_xpu, s_xpu, v_xpu = torch.svd(a_xpu)
        r_xpu = torch.matmul(
            torch.matmul(u_xpu, torch.diag_embed(s_xpu)), v_xpu.transpose(-2, -1)
        )

        self.assertEqual(r_cpu, r_xpu.cpu())

    @pytest.mark.skipif(not torch.xpu.has_onemkl(), reason="not torch.xpu.has_onemkl()")
    def test_svd_complex_float(self, dtype=torch.cfloat):
        # Since U and V of an SVD is not unique, each vector can be multiplied by an arbitrary phase factor e^iϕ
        # while the SVD result is still correct. Different platforms, like Numpy, or inputs on different device types,
        # may produce different U and V tensors.

        a = torch.randn(5, 5, dtype=torch.cfloat)
        a_xpu = a.to("xpu")

        u, s, v = torch.svd(a)
        r_cpu = torch.mm(torch.mm(u, torch.diag(s).cfloat()), v.t())

        u_xpu, s_xpu, v_xpu = torch.svd(a_xpu)
        r_xpu = torch.mm(torch.mm(u_xpu, torch.diag(s_xpu).cfloat()), v_xpu.t())

        self.assertEqual(r_cpu, r_xpu.cpu())

    @pytest.mark.skipif(not torch.xpu.has_onemkl(), reason="not torch.xpu.has_onemkl()")
    def test_linalg_svd_complex_float(self, dtype=torch.cfloat):
        # Since U and V of an SVD is not unique, each vector can be multiplied by an arbitrary phase factor e^iϕ
        # while the SVD result is still correct. Different platforms, like Numpy, or inputs on different device types,
        # may produce different U and V tensors.

        a = torch.randn(5, 5, dtype=torch.cfloat)
        a_xpu = a.to("xpu")

        u, s, v = torch.linalg.svd(a)
        r_cpu = torch.mm(torch.mm(u, torch.diag(s).cfloat()), v)

        u_xpu, s_xpu, v_xpu = torch.linalg.svd(a_xpu)
        r_xpu = torch.mm(torch.mm(u_xpu, torch.diag(s_xpu).cfloat()), v_xpu)

        self.assertEqual(r_cpu, r_xpu.cpu())

    @pytest.mark.skipif(not torch.xpu.has_onemkl(), reason="not torch.xpu.has_onemkl()")
    def test_batch_svd_complex_float(self, dtype=torch.cfloat):
        # Since U and V of an SVD is not unique, each vector can be multiplied by an arbitrary phase factor e^iϕ
        # while the SVD result is still correct. Different platforms, like Numpy, or inputs on different device types,
        # may produce different U and V tensors.
        a = torch.randn(5, 5, 5)
        a_xpu = a.to("xpu").to(torch.cfloat)

        u, s, v = torch.svd(a)
        r_cpu = torch.matmul(torch.matmul(u, torch.diag_embed(s)), v.transpose(-2, -1))

        u_xpu, s_xpu, v_xpu = torch.svd(a_xpu)
        u_xpu = u_xpu.to(torch.float32)
        v_xpu = v_xpu.to(torch.float32)
        r_xpu = torch.matmul(
            torch.matmul(u_xpu, torch.diag_embed(s_xpu)), v_xpu.transpose(-2, -1)
        )

        self.assertEqual(r_cpu, r_xpu.cpu())
