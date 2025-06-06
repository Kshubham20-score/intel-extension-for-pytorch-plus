import torch
import math
import torch.nn as nn
from .._transformer_configuration import IPEXTransformerConfig
from .CacheUtils import CacheFormat


class PositionalEmbedding(nn.Module):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__()
        self.config = config
        self.dtype = dtype
        self.dynamic_cache_stride = config.dynamic_cache_stride

    def forward(
        self,
        query,
        key,
        position_ids,
        layer_id,
        beam_size,
        kv_seq_len,
        *args,
    ):
        return query, key


class GPTJRotaryEmbeddingRef(PositionalEmbedding):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config=config, dtype=dtype)
        self.rotary_dim = config.rotary_dim
        self.base = config.positional_embedding_base
        self.device = config.device
        pos_embd_dim = self.rotary_dim or self.embed_dim
        self.embed_positions = self.create_sinusoidal_positions(
            config.max_positions, pos_embd_dim
        )

    def create_sinusoidal_positions(self, num_pos: int, dim: int) -> torch.Tensor:
        inv_freq = 1.0 / (10000 ** (torch.arange(0, dim, 2, dtype=self.dtype) / dim))
        sinusoid_inp = torch.einsum(
            "i , j -> i j", torch.arange(num_pos, dtype=torch.float), inv_freq
        ).float()
        res = torch.cat((torch.sin(sinusoid_inp), torch.cos(sinusoid_inp)), dim=1)
        return res

    def _get_embed_positions(self, position_ids):
        embed_positions = self.embed_positions
        if embed_positions.device != position_ids.device:
            embed_positions = embed_positions.to(position_ids.device)
            self.embed_positions = embed_positions
        return embed_positions.repeat(position_ids.shape[0], 1, 1)

    def rotate_every_two(self, x: torch.Tensor) -> torch.Tensor:
        x1 = x[:, :, :, ::2]
        x2 = x[:, :, :, 1::2]
        x = torch.stack((-x2, x1), dim=-1)
        return x.flatten(-2)  # in einsum notation: rearrange(x, '... d j -> ... (d j)')

    def apply_rotary_pos_emb(
        self, tensor: torch.Tensor, sin: torch.Tensor, cos: torch.Tensor
    ) -> torch.Tensor:
        sin = torch.repeat_interleave(sin[:, :, None, :], 2, 3)
        cos = torch.repeat_interleave(cos[:, :, None, :], 2, 3)
        return (tensor * cos) + (self.rotate_every_two(tensor) * sin)

    def forward(self, query, key, position_ids, layer_id, beam_size, kv_seq_len):
        # position_ids [bs*beam, seq_len]
        first_token = position_ids.shape[-1] > 1
        # beam search first_token
        # query, key shape [bs*beam, seq, hidden_size], layout [bs*beam, seq, hidden_size]
        # greedy search/ beam search 2nd token
        # query, key shape [seq, bs*beam, hidden_size], layout [seq, bs*beam, hidden_size]
        if beam_size == 1 or not first_token:
            query = query.transpose(0, 1).contiguous()
            key = key.transpose(0, 1).contiguous()
            position_ids = position_ids.transpose(0, 1).contiguous()
        seq_len = key.size(1)
        if seq_len >= self.dynamic_cache_stride:
            new_cache_length = seq_len + self.dynamic_cache_stride
            self.embed_positions = self.create_sinusoidal_positions(
                new_cache_length, self.rotary_dim
            )
            self.dynamic_cache_stride = new_cache_length
        embed_positions = self._get_embed_positions(position_ids)
        repeated_position_ids = position_ids.unsqueeze(-1).repeat(
            1, 1, embed_positions.shape[-1]
        )
        sincos = torch.gather(embed_positions, 1, repeated_position_ids)
        sin, cos = torch.split(sincos, sincos.shape[-1] // 2, dim=-1)

        if self.rotary_dim is not None:
            k_rot = key[:, :, :, : self.rotary_dim]
            k_pass = key[:, :, :, self.rotary_dim :]
            q_rot = query[:, :, :, : self.rotary_dim]
            q_pass = query[:, :, :, self.rotary_dim :]
            k_rot = self.apply_rotary_pos_emb(k_rot, sin, cos)
            q_rot = self.apply_rotary_pos_emb(q_rot, sin, cos)
            key = torch.cat([k_rot, k_pass], dim=-1)
            query = torch.cat([q_rot, q_pass], dim=-1)
        else:
            key = self.apply_rotary_pos_emb(key, sin, cos)
            query = self.apply_rotary_pos_emb(query, sin, cos)

        if beam_size == 1 or not first_token:
            query = query.transpose(0, 1).contiguous()
            key = key.transpose(0, 1).contiguous()
        return query, key


class GPTJRotaryEmbedding(PositionalEmbedding):
    cos_cached = None
    sin_cached = None
    max_position_embedding = 2048

    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config=config, dtype=dtype)
        self.rotary_dim = config.rotary_dim
        self.max_position_embedding = config.max_positions
        self.base = config.positional_embedding_base
        self.device = config.device
        if (
            GPTJRotaryEmbedding.cos_cached is None
            or GPTJRotaryEmbedding.sin_cached is None
        ):
            self.create_sin_cos_cache(GPTJRotaryEmbedding.max_position_embedding)

    def create_sin_cos_cache(self, pos):
        inv_freq = 1.0 / (
            self.base
            ** (
                torch.arange(0, self.rotary_dim, 2).float().to(self.device)
                / self.rotary_dim
            )
        )
        t = torch.arange(pos, dtype=torch.float, device=self.device)
        sinusoid_inp = torch.einsum("i , j -> i j", t, inv_freq).float()
        embed_positions = torch.cat(
            (torch.sin(sinusoid_inp), torch.cos(sinusoid_inp)), dim=1
        )

        sin, cos = torch.split(embed_positions, embed_positions.shape[-1] // 2, dim=-1)
        sin = torch.repeat_interleave(sin, 2, 1).to(self.dtype).to(self.device)
        cos = torch.repeat_interleave(cos, 2, 1).to(self.dtype).to(self.device)
        GPTJRotaryEmbedding.sin_cached = sin
        GPTJRotaryEmbedding.cos_cached = cos

    def rotate_every_two(self, x: torch.Tensor) -> torch.Tensor:
        # the original rotary_every_two funtion used in the model
        x1 = x[:, :, :, ::2]
        x2 = x[:, :, :, 1::2]
        x = torch.stack((-x2, x1), dim=-1)
        return x.flatten(-2)  # in einsum notation: rearrange(x, '... d j -> ... (d j)')

    def apply_rotary_pos_emb(self, query, key, sin, cos):
        torch.ops.torch_ipex.apply_rotary_embedding_two_qk(
            query, key, sin, cos, query, key
        )

    def get_sin_cos(self, position_ids, layer_id, beam_size, kv_seq_len, cache_format):
        # position_ids [bs*beam, seq_len]
        if GPTJRotaryEmbedding.max_position_embedding < kv_seq_len:
            new_cache_length = kv_seq_len + self.dynamic_cache_stride
            self.create_sin_cos_cache(new_cache_length)
            GPTJRotaryEmbedding.max_position_embedding = new_cache_length
        if layer_id == 0:
            GPTJRotaryEmbedding.position_ids = position_ids
            GPTJRotaryEmbedding.sin = self.sin_cached[
                GPTJRotaryEmbedding.position_ids
            ].unsqueeze(2)
            GPTJRotaryEmbedding.cos = self.cos_cached[
                GPTJRotaryEmbedding.position_ids
            ].unsqueeze(2)
            if cache_format == CacheFormat.FBNH:
                GPTJRotaryEmbedding.sin = GPTJRotaryEmbedding.sin.permute(1, 0, 2, 3)
                GPTJRotaryEmbedding.cos = GPTJRotaryEmbedding.cos.permute(1, 0, 2, 3)

        # 1st token
        # GPTJRotaryEmbedding.sin is in shape of [bs*beam, seq, num_head, head_dim]
        # 2nd to last token or greedy
        # GPTJRotaryEmbedding.sin is in shape of [seq, bs*beam, num_head, head_dim]
        return GPTJRotaryEmbedding.sin, GPTJRotaryEmbedding.cos

    def forward(
        self,
        query,
        key,
        position_ids,
        layer_id,
        beam_size,
        kv_seq_len,
        cache_format=CacheFormat.BFNH,
    ):
        sin, cos = self.get_sin_cos(
            position_ids, layer_id, beam_size, kv_seq_len, cache_format
        )
        if self.rotary_dim is not None:
            self.apply_rotary_pos_emb(
                query[:, :, :, : self.rotary_dim],
                key[:, :, :, : self.rotary_dim],
                sin,
                cos,
            )
        else:
            self.apply_rotary_pos_emb(query, key, sin, cos)
        return query, key


class GLMRotaryEmbedding(GPTJRotaryEmbedding):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        config.rotary_dim = config.embedding_dim // config.num_attention_head // 2
        super().__init__(config, dtype)

    def create_sin_cos_cache(self, pos):
        super().create_sin_cos_cache(pos)

    def apply_rotary_pos_emb(
        self,
        query: torch.Tensor,
        key: torch.Tensor,
        sin: torch.Tensor,
        cos: torch.Tensor,
    ):
        if query.shape == key.shape:
            cos = cos.expand(query.shape)
            sin = sin.expand(query.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_two_qk(
                query, key, sin, cos, query, key
            )
        else:
            cos_q = cos.expand(query.shape)
            sin_q = sin.expand(query.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_two(query, sin_q, cos_q, query)
            cos_k = cos.expand(key.shape)
            sin_k = sin.expand(key.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_two(key, sin_k, cos_k, key)

    def apply_rotary_pos_emb_ref(
        self, x: torch.Tensor, rope_cache: torch.Tensor
    ) -> torch.Tensor:
        # x: [sq, b, np, hn]
        sq, b, np, hn = x.size(0), x.size(1), x.size(2), x.size(3)
        rot_dim = rope_cache.shape[-2] * 2
        x, x_pass = x[..., :rot_dim], x[..., rot_dim:]
        # truncate to support variable sizes
        rope_cache = rope_cache[:sq]
        xshaped = x.reshape(sq, -1, np, rot_dim // 2, 2)
        rope_cache = rope_cache.view(sq, -1, 1, xshaped.size(3), 2)
        x_out2 = torch.stack(
            [
                xshaped[..., 0] * rope_cache[..., 0]
                - xshaped[..., 1] * rope_cache[..., 1],
                xshaped[..., 1] * rope_cache[..., 0]
                + xshaped[..., 0] * rope_cache[..., 1],
            ],
            -1,
        )
        x_out2 = x_out2.flatten(3)
        return torch.cat((x_out2, x_pass), dim=-1)

    def forward(
        self,
        query,
        key,
        position_ids,
        layer_id=None,
        beam_size=None,
        kv_seq_len=None,
        cache_format=CacheFormat.BFNH,
    ):
        sin, cos = self.get_sin_cos(
            position_ids, layer_id, beam_size, kv_seq_len, cache_format
        )
        rot_dim = cos.shape[-1]
        self.apply_rotary_pos_emb(query[..., :rot_dim], key[..., :rot_dim], sin, cos)
        return query, key


class LlamaRotaryEmbeddingBase(torch.nn.Module):
    def __init__(self, dim, max_position_embeddings=2048, base=10000, device=None):
        super().__init__()
        self.dim = dim
        self.max_position_embeddings = max_position_embeddings
        self.base = base
        inv_freq = 1.0 / (
            self.base ** (torch.arange(0, self.dim, 2).float().to(device) / self.dim)
        )
        self.register_buffer("inv_freq", inv_freq, persistent=False)

        # Build here to make `torch.jit.trace` work.
        self._set_cos_sin_cache(
            seq_len=max_position_embeddings,
            device=self.inv_freq.device,
            dtype=torch.get_default_dtype(),
        )

    def _set_cos_sin_cache(self, seq_len, device, dtype):
        self.max_seq_len_cached = seq_len
        t = torch.arange(
            self.max_seq_len_cached, device=device, dtype=self.inv_freq.dtype
        )

        freqs = torch.einsum("i,j->ij", t, self.inv_freq)
        # Different from paper, but it uses a different permutation in order to obtain the same calculation
        emb = torch.cat((freqs, freqs), dim=-1)
        self.register_buffer(
            "cos_cached", emb.cos()[None, None, :, :].to(dtype), persistent=False
        )
        self.register_buffer(
            "sin_cached", emb.sin()[None, None, :, :].to(dtype), persistent=False
        )

    def forward(self, x, seq_len=None):
        # x: [bs, num_attention_heads, seq_len, head_size]
        if seq_len > self.max_seq_len_cached:
            self._set_cos_sin_cache(seq_len=seq_len, device=x.device, dtype=x.dtype)

        return (
            self.cos_cached[:, :, :seq_len, ...].to(dtype=x.dtype),
            self.sin_cached[:, :, :seq_len, ...].to(dtype=x.dtype),
        )


class LlamaLinearScalingRotaryEmbedding(LlamaRotaryEmbeddingBase):
    """LlamaRotaryEmbedding extended with linear scaling. Credits to the Reddit user /u/kaiokendev"""

    def __init__(
        self,
        dim,
        max_position_embeddings=2048,
        base=10000,
        device=None,
        scaling_factor=1.0,
    ):
        self.scaling_factor = scaling_factor
        super().__init__(dim, max_position_embeddings, base, device)

    def _set_cos_sin_cache(self, seq_len, device, dtype):
        self.max_seq_len_cached = seq_len
        t = torch.arange(
            self.max_seq_len_cached, device=device, dtype=self.inv_freq.dtype
        )
        t = t / self.scaling_factor

        freqs = torch.einsum("i,j->ij", t, self.inv_freq)
        # Different from paper, but it uses a different permutation in order to obtain the same calculation
        emb = torch.cat((freqs, freqs), dim=-1)
        self.register_buffer(
            "cos_cached", emb.cos()[None, None, :, :].to(dtype), persistent=False
        )
        self.register_buffer(
            "sin_cached", emb.sin()[None, None, :, :].to(dtype), persistent=False
        )


class LlamaDynamicNTKScalingRotaryEmbedding(LlamaRotaryEmbeddingBase):
    """LlamaRotaryEmbedding extended with Dynamic NTK scaling. Credits to the Reddit users /u/bloc97 and /u/emozilla"""

    def __init__(
        self,
        dim,
        max_position_embeddings=2048,
        base=10000,
        device=None,
        scaling_factor=1.0,
    ):
        self.scaling_factor = scaling_factor
        super().__init__(dim, max_position_embeddings, base, device)

    def _set_cos_sin_cache(self, seq_len, device, dtype):
        self.max_seq_len_cached = seq_len

        if seq_len > self.max_position_embeddings:
            base = self.base * (
                (self.scaling_factor * seq_len / self.max_position_embeddings)
                - (self.scaling_factor - 1)
            ) ** (self.dim / (self.dim - 2))
            inv_freq = 1.0 / (
                base ** (torch.arange(0, self.dim, 2).float().to(device) / self.dim)
            )
            self.register_buffer("inv_freq", inv_freq, persistent=False)

        t = torch.arange(
            self.max_seq_len_cached, device=device, dtype=self.inv_freq.dtype
        )

        freqs = torch.einsum("i,j->ij", t, self.inv_freq)
        # Different from paper, but it uses a different permutation in order to obtain the same calculation
        emb = torch.cat((freqs, freqs), dim=-1)
        self.register_buffer(
            "cos_cached", emb.cos()[None, None, :, :].to(dtype), persistent=False
        )
        self.register_buffer(
            "sin_cached", emb.sin()[None, None, :, :].to(dtype), persistent=False
        )


class LlamaRotaryEmbeddingRef(torch.nn.Module):
    def __init__(self, config: IPEXTransformerConfig):
        super().__init__()
        self.config = config
        self.head_dim = int(config.embed_dim / config.num_attention_heads)
        self.max_position_embeddings = config.max_positions
        self.device = config.device
        self.dtype = config.dtype
        if self.config.rope_scaling is None:
            self.rotary_emb = LlamaRotaryEmbeddingBase(
                self.head_dim,
                max_position_embeddings=self.max_position_embeddings,
                device=self.device,
            )
        else:
            scaling_type = self.config.rope_scaling["type"]
            scaling_factor = self.config.rope_scaling["factor"]
            if scaling_type == "linear":
                self.rotary_emb = LlamaLinearScalingRotaryEmbedding(
                    self.head_dim,
                    max_position_embeddings=self.max_position_embeddings,
                    scaling_factor=scaling_factor,
                )
            elif scaling_type == "dynamic":
                self.rotary_emb = LlamaDynamicNTKScalingRotaryEmbedding(
                    self.head_dim,
                    max_position_embeddings=self.max_position_embeddings,
                    scaling_factor=scaling_factor,
                )
            else:
                raise ValueError(f"Unknown RoPE scaling type {scaling_type}")

        import os

        col_major = os.environ.get("COL_MAJOR", "OFF").upper() in [
            "1",
            "Y",
            "ON",
            "YES",
            "TRUE",
        ]
        self.row_major = not col_major

    def rotate_half(self, x):
        """Rotates half the hidden dims of the input."""
        x1 = x[..., : x.shape[-1] // 2]
        x2 = x[..., x.shape[-1] // 2 :]
        return torch.cat((-x2, x1), dim=-1)

    def apply_rotary_pos_emb(self, q, k, cos, sin, position_ids):
        # The first two dimensions of cos and sin are always 1, so we can `squeeze` them.
        cos = cos.squeeze(1).squeeze(0)  # [seq_len, dim]
        sin = sin.squeeze(1).squeeze(0)  # [seq_len, dim]
        cos = cos[position_ids].unsqueeze(1)  # [bs, 1, seq_len, dim]
        sin = sin[position_ids].unsqueeze(1)  # [bs, 1, seq_len, dim]
        rotate_q = self.rotate_half(q)
        rotate_k = self.rotate_half(k)
        q_embed = (q * cos) + (rotate_q * sin)
        k_embed = (k * cos) + (rotate_k * sin)
        return q_embed, k_embed

    def forward(self, query, key, position_ids, layer_id, beam_size, kv_seq_len):
        # position_ids [bs*beam, seq_len]
        # cos [1, 1, kv_seq_len, head_dim]
        # sin [1, 1, kv_seq_len, head_dim]
        first_token = position_ids.shape[-1] > 1
        if self.row_major:
            if first_token and beam_size > 1:
                # from [bs*beam, seq, num_head, head_dim]
                # to [bs*beam, num_head, seq, head_dim]
                query_states = query.permute(0, 2, 1, 3).contiguous()
                key_states = key.permute(0, 2, 1, 3).contiguous()
            else:
                # from [seq, bs*beam, num_head, head_dim]
                # to [bs*beam, num_head, seq, head_dim]
                query_states = query.permute(1, 2, 0, 3).contiguous()
                key_states = key.permute(1, 2, 0, 3).contiguous()
        else:
            # from [bs*beam, seq, num_head, head_dim]
            # to [bs*beam, num_head, seq, head_dim]
            query_states = query.permute(0, 2, 1, 3).contiguous()
            key_states = key.permute(0, 2, 1, 3).contiguous()

        cos, sin = self.rotary_emb(key_states, seq_len=kv_seq_len)
        query_states, key_states = self.apply_rotary_pos_emb(
            query_states, key_states, cos, sin, position_ids
        )

        if self.row_major:
            if first_token and beam_size > 1:
                # frpm [bs*beam, num_head, seq, head_dim]
                # to [bs*beam, seq, num_head, head_dim]
                query_states = query_states.permute(0, 2, 1, 3).contiguous()
                key_states = key_states.permute(0, 2, 1, 3).contiguous()
            else:
                # from [bs*beam, num_head, seq, head_dim]
                # to [seq, bs*beam, num_head, head_dim]
                query_states = query_states.permute(2, 0, 1, 3).contiguous()
                key_states = key_states.permute(2, 0, 1, 3).contiguous()
        else:
            # frpm [bs*beam, num_head, seq, head_dim]
            # to [bs*beam, seq, num_head, head_dim]
            query_states = query_states.permute(0, 2, 1, 3).contiguous()
            key_states = key_states.permute(0, 2, 1, 3).contiguous()
        query.copy_(query_states)
        key.copy_(key_states)
        return query, key


class LlamaRotaryEmbedding(PositionalEmbedding):
    cos_cached = None
    sin_cached = None
    max_position_embedding = 2048

    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config, dtype)
        self.dim = (
            config.head_dim
            if config.head_dim is not None
            else int(
                config.embedding_dim
                / config.num_attention_head
                * config.partial_rotary_factor
            )
        )

        LlamaRotaryEmbedding.max_position_embedding = config.max_positions
        self.base = config.positional_embedding_base
        self.device = config.device
        self.dtype = dtype
        self.rope_type = "default"
        if config.rope_scaling is not None:
            self.rope_type = config.rope_scaling.get(
                "rope_type", config.rope_scaling.get("type")
            )
        if self.rope_type == "su" or self.rope_type == "longrope":
            self.short_factor = config.rope_scaling["short_factor"]
            self.long_factor = config.rope_scaling["long_factor"]
            self.max_seq_len = config.max_positions
            LlamaRotaryEmbedding.max_position_embedding = (
                config.original_max_position_embeddings
            )
            self.original_max_position_embeddings = (
                config.original_max_position_embeddings
            )
            self.create_sin_cos_cache_long_scaled_rope(
                self.original_max_position_embeddings
            )
        elif (
            LlamaRotaryEmbedding.sin_cached is None
            or LlamaRotaryEmbedding.cos_cached is None
        ):
            inv_freq = 1.0 / (
                self.base
                ** (torch.arange(0, self.dim, 2).float().to(self.device) / self.dim)
            )
            if self.rope_type == "llama3":
                inv_freq = self.compute_llama3_parameters(inv_freq, config)
            self.register_buffer("inv_freq", inv_freq, persistent=False)
            self.create_sin_cos_cache(LlamaRotaryEmbedding.max_position_embedding)

    def compute_llama3_parameters(self, inv_freq, config: IPEXTransformerConfig):
        factor = config.rope_scaling["factor"]
        low_freq_factor = config.rope_scaling["low_freq_factor"]
        high_freq_factor = config.rope_scaling["high_freq_factor"]
        old_context_len = config.rope_scaling["original_max_position_embeddings"]

        low_freq_wavelen = old_context_len / low_freq_factor
        high_freq_wavelen = old_context_len / high_freq_factor

        wavelen = 2 * math.pi / inv_freq
        # wavelen < high_freq_wavelen: do nothing
        # wavelen > low_freq_wavelen: divide by factor
        inv_freq_llama = torch.where(
            wavelen > low_freq_wavelen, inv_freq / factor, inv_freq
        )
        # otherwise: interpolate between the two, using a smooth factor
        smooth_factor = (old_context_len / wavelen - low_freq_factor) / (
            high_freq_factor - low_freq_factor
        )
        smoothed_inv_freq = (
            1 - smooth_factor
        ) * inv_freq_llama / factor + smooth_factor * inv_freq_llama
        is_medium_freq = ~(wavelen < high_freq_wavelen) * ~(wavelen > low_freq_wavelen)
        inv_freq = torch.where(is_medium_freq, smoothed_inv_freq, inv_freq_llama)
        return inv_freq

    def _calc_mscale(self, scale: torch.Tensor) -> torch.Tensor:
        if scale <= 1.0:
            return 1.0
        return math.sqrt(
            1 + math.log(scale) / math.log(self.original_max_position_embeddings)
        )

    def create_sin_cos_cache_long_scaled_rope(self, seq_len):
        scale = self.max_seq_len / self.original_max_position_embeddings
        scaling_factor = self._calc_mscale(scale)
        if seq_len > self.original_max_position_embeddings:
            ext_factors = torch.tensor(
                self.long_factor, dtype=torch.float32, device=self.device
            )
            if self.config.rope_scaling.get("long_mscale", None) is not None:
                long_mscale = self.config.rope_scaling["long_mscale"]
                scaling_factor = (
                    long_mscale if long_mscale > 0 else self._calc_mscale(scale)
                )
        else:
            ext_factors = torch.tensor(
                self.short_factor, dtype=torch.float32, device=self.device
            )
            if self.config.rope_scaling.get("short_mscale", None) is not None:
                short_mscale = self.config.rope_scaling["short_mscale"]
                scaling_factor = short_mscale if short_mscale > 0 else 1.0
        inv_freq_shape = (
            torch.arange(0, self.dim, 2, dtype=torch.int64, device=self.device).float()
            / self.dim
        )
        inv_freq = 1.0 / (ext_factors * self.base**inv_freq_shape)
        self.register_buffer("inv_freq", inv_freq, persistent=False)

        t = torch.arange(seq_len, device=self.device, dtype=self.inv_freq.dtype)
        freqs = torch.einsum("i,j->ij", t, self.inv_freq)
        emb = torch.cat((freqs, freqs), dim=-1)

        LlamaRotaryEmbedding.cos_cached = emb.cos().to(self.dtype) * scaling_factor
        LlamaRotaryEmbedding.sin_cached = emb.sin().to(self.dtype) * scaling_factor

    def create_sin_cos_cache(self, pos):
        t = torch.arange(pos, device=self.device, dtype=self.inv_freq.dtype)
        freqs = torch.einsum("i,j->ij", t, self.inv_freq)
        # Different from paper, but it uses a different permutation in order to obtain the same calculation
        emb = torch.cat((freqs, freqs), dim=-1)
        LlamaRotaryEmbedding.cos_cached = emb.cos().to(self.dtype)
        LlamaRotaryEmbedding.sin_cached = emb.sin().to(self.dtype)

    def apply_rotary_pos_emb(
        self,
        query: torch.Tensor,
        key: torch.Tensor,
        sin: torch.Tensor,
        cos: torch.Tensor,
    ):
        if query.shape == key.shape:
            cos = cos.expand(query.shape)
            sin = sin.expand(query.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half_qk(
                query, key, sin, cos, query, key
            )
        else:
            cos_q = cos.expand(query.shape)
            sin_q = sin.expand(query.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half(query, sin_q, cos_q, query)
            cos_k = cos.expand(key.shape)
            sin_k = sin.expand(key.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half(key, sin_k, cos_k, key)

    def apply_rotary_pos_emb_ref(
        self,
        query: torch.Tensor,
        key: torch.Tensor,
        sin: torch.Tensor,
        cos: torch.Tensor,
    ):
        cos = cos.expand(query.shape).contiguous()
        sin = sin.expand(query.shape).contiguous()
        rotate_q = self.rotate_half(query)
        rotate_k = self.rotate_half(key)
        q_embed = (query * cos) + (rotate_q * sin)
        k_embed = (key * cos) + (rotate_k * sin)
        query.copy_(q_embed)
        key.copy_(k_embed)

    def update_cache_if_needed(self, kv_seq_len):
        if kv_seq_len >= LlamaRotaryEmbedding.max_position_embedding:
            new_cache_length = kv_seq_len + self.dynamic_cache_stride
            if self.rope_type == "su" or self.rope_type == "longrope":
                self.create_sin_cos_cache_long_scaled_rope(new_cache_length)
            else:
                self.create_sin_cos_cache(new_cache_length)
            LlamaRotaryEmbedding.max_position_embedding = new_cache_length

    def get_sin_cos(self, position_ids, layer_id, beam_size, kv_seq_len, cache_format):
        self.update_cache_if_needed(kv_seq_len)
        if layer_id == 0:
            LlamaRotaryEmbedding.position_ids = position_ids
            LlamaRotaryEmbedding.sin = self.sin_cached[
                LlamaRotaryEmbedding.position_ids
            ].unsqueeze(2)
            LlamaRotaryEmbedding.cos = self.cos_cached[
                LlamaRotaryEmbedding.position_ids
            ].unsqueeze(2)
            if cache_format == CacheFormat.FBNH:
                LlamaRotaryEmbedding.sin = LlamaRotaryEmbedding.sin.permute(1, 0, 2, 3)
                LlamaRotaryEmbedding.cos = LlamaRotaryEmbedding.cos.permute(1, 0, 2, 3)

            LlamaRotaryEmbedding.sin = LlamaRotaryEmbedding.sin.contiguous()
            LlamaRotaryEmbedding.cos = LlamaRotaryEmbedding.cos.contiguous()

        # 1st token
        # LlamaRotaryEmbedding.sin is in shape of [bs*beam, seq, 1, head_dim]
        # 2nd to last token or greedy
        # GPTJRotaryEmbedding.sin is in shape of [seq, bs*beam, 1, head_dim]
        return LlamaRotaryEmbedding.sin, LlamaRotaryEmbedding.cos

    def rotate_half(self, x):
        """Rotates half the hidden dims of the input."""
        x1 = x[..., : x.shape[-1] // 2]
        x2 = x[..., x.shape[-1] // 2 :]
        return torch.cat((-x2, x1), dim=-1)

    def forward(
        self,
        query,
        key,
        position_ids,
        layer_id,
        beam_size,
        kv_seq_len,
        cache_format=CacheFormat.BFNH,
    ):
        sin, cos = self.get_sin_cos(
            position_ids, layer_id, beam_size, kv_seq_len, cache_format
        )
        self.apply_rotary_pos_emb(query, key, sin, cos)
        return query, key


def apply_rotary_pos_emb(query, key, rotary_pos_emb_list):
    cur_len = query.shape[1]
    if len(rotary_pos_emb_list) == 1:
        rotary_pos_emb = rotary_pos_emb_list[0]
        rotary_pos_emb = [i[:, -cur_len:, :, :] for i in rotary_pos_emb]
        rotary_pos_emb = (rotary_pos_emb,) * 2
        q_pos_emb, k_pos_emb = rotary_pos_emb
        # Slice the pos emb for current inference
        torch.ops.torch_ipex.apply_rotary_embedding_half(
            query, q_pos_emb[1], q_pos_emb[0], query
        )
        torch.ops.torch_ipex.apply_rotary_embedding_half(
            key, k_pos_emb[1], k_pos_emb[0], key
        )
    else:
        query_list = []
        key_list = []
        for i, rotary_pos_emb in enumerate(rotary_pos_emb_list):
            rotary_pos_emb = [i[:, -cur_len:, :, :] for i in rotary_pos_emb]
            rotary_pos_emb = (rotary_pos_emb,) * 2
            q_pos_emb, k_pos_emb = rotary_pos_emb
            # Slice the pos emb for current inference
            q_i, k_i = query[i : i + 1, :, :], key[i : i + 1, :, :]
            torch.ops.torch_ipex.apply_rotary_embedding_half(
                q_i, q_pos_emb[1], q_pos_emb[0], q_i
            )
            torch.ops.torch_ipex.apply_rotary_embedding_half(
                k_i, k_pos_emb[1], k_pos_emb[0], k_i
            )
            query_list += [q_i]
            key_list += [k_i]
        query = torch.cat(query_list, dim=0)
        key = torch.cat(key_list, dim=0)
    return query, key


class ChatGLMRotaryEmbedding(PositionalEmbedding):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config, dtype)

    def apply_rotary_pos_emb(
        self,
        query: torch.Tensor,
        key: torch.Tensor,
        rotary_pos_emb: torch.Tensor,
    ):
        sin = rotary_pos_emb[..., 1]
        cos = rotary_pos_emb[..., 0]

        if query.shape == key.shape:
            cos = cos.expand(query.shape)
            sin = sin.expand(query.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_two_qk(
                query, key, sin, cos, query, key
            )
        else:
            cos_q = cos.expand(query.shape)
            sin_q = sin.expand(query.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_two(query, sin_q, cos_q, query)
            cos_k = cos.expand(key.shape)
            sin_k = sin.expand(key.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_two(key, sin_k, cos_k, key)

    def apply_rotary_pos_emb_ref(
        self, x: torch.Tensor, rope_cache: torch.Tensor
    ) -> torch.Tensor:
        # x: [sq, b, np, hn]
        sq, b, np, hn = x.size(0), x.size(1), x.size(2), x.size(3)
        rot_dim = rope_cache.shape[-2] * 2
        x, x_pass = x[..., :rot_dim], x[..., rot_dim:]
        # truncate to support variable sizes
        rope_cache = rope_cache[:sq]
        xshaped = x.reshape(sq, -1, np, rot_dim // 2, 2)
        rope_cache = rope_cache.view(sq, -1, 1, xshaped.size(3), 2)
        x_out2 = torch.stack(
            [
                xshaped[..., 0] * rope_cache[..., 0]
                - xshaped[..., 1] * rope_cache[..., 1],
                xshaped[..., 1] * rope_cache[..., 0]
                + xshaped[..., 0] * rope_cache[..., 1],
            ],
            -1,
        )
        x_out2 = x_out2.flatten(3)
        return torch.cat((x_out2, x_pass), dim=-1)

    def forward(
        self,
        query,
        key,
        rotary_pos_emb,
        layer_id=None,
        beam_idx=None,
        seqlen=None,
        cache_format=CacheFormat.BFNH,
    ):
        rot_dim = rotary_pos_emb.shape[-2]
        self.apply_rotary_pos_emb(
            query[..., :rot_dim], key[..., :rot_dim], rotary_pos_emb
        )
        return query, key


class Phi3RotaryEmbedding(LlamaRotaryEmbedding):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config, dtype)

    def apply_rotary_pos_emb(self, q, k, sin, cos, position_ids=None, unsqueeze_dim=1):
        if q.shape == k.shape:
            cos = cos.expand(q.shape)
            sin = sin.expand(q.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half_qk(q, k, sin, cos, q, k)
        else:
            # TODO Optimize space?
            rotary_dim = cos.shape[-1]
            q_rot, q_pass = q[..., :rotary_dim], q[..., rotary_dim:]
            k_rot, k_pass = k[..., :rotary_dim], k[..., rotary_dim:]

            q_embed = torch.cat(
                [(q_rot * cos) + (self.rotate_half(q_rot) * sin), q_pass], dim=-1
            )
            k_embed = torch.cat(
                [(k_rot * cos) + (self.rotate_half(k_rot) * sin), k_pass], dim=-1
            )
            return q_embed, k_embed

    def forward(
        self,
        query,
        key,
        position_ids,
        layer_id,
        beam_size,
        kv_seq_len,
        cache_format=CacheFormat.BFNH,
    ):
        sin, cos = self.get_sin_cos(
            position_ids, layer_id, beam_size, kv_seq_len, cache_format
        )
        if query.shape == key.shape:
            self.apply_rotary_pos_emb(query, key, sin, cos)
        else:
            q_embed, k_embed = self.apply_rotary_pos_emb(query, key, sin, cos)
            query.copy_(q_embed)
            key.copy_(k_embed)
        return query, key


class Phi3SmallRotaryEmbedding(LlamaRotaryEmbedding):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config, dtype)


class Qwen2RotaryEmbedding(LlamaRotaryEmbedding):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__(config, dtype)


class MixtralRotaryEmbedding(nn.Module):
    def __init__(self, config: IPEXTransformerConfig, dtype):
        super().__init__()
        self.dim = int(config.embedding_dim / config.num_attention_head)
        self.max_position_embeddings = config.max_positions
        self.base = config.positional_embedding_base
        device = config.device
        inv_freq = 1.0 / (
            self.base
            ** (
                torch.arange(0, self.dim, 2, dtype=torch.int64).float().to(device)
                / self.dim
            )
        )
        self.register_buffer("inv_freq", inv_freq, persistent=False)

        # Build here to make `torch.jit.trace` work.
        self._set_cos_sin_cache(
            seq_len=self.max_position_embeddings,
            device=self.inv_freq.device,
            dtype=torch.get_default_dtype(),
        )

    def _set_cos_sin_cache(self, seq_len, device, dtype):
        self.max_seq_len_cached = seq_len
        t = torch.arange(
            self.max_seq_len_cached, device=device, dtype=torch.int64
        ).type_as(self.inv_freq)

        freqs = torch.outer(t, self.inv_freq)
        # Different from paper, but it uses a different permutation in order to obtain the same calculation
        emb = torch.cat((freqs, freqs), dim=-1)
        self.register_buffer("cos_cached", emb.cos().to(dtype), persistent=False)
        self.register_buffer("sin_cached", emb.sin().to(dtype), persistent=False)

    def get_cos_sin(self, x, seq_len=None):
        # x: [bs, num_attention_heads, seq_len, head_size]
        if seq_len > self.max_seq_len_cached:
            self._set_cos_sin_cache(seq_len=seq_len, device=x.device, dtype=x.dtype)

        return (
            self.cos_cached[:seq_len].to(dtype=x.dtype),
            self.sin_cached[:seq_len].to(dtype=x.dtype),
        )

    def rotate_half(self, x):
        """Rotates half the hidden dims of the input."""
        x1 = x[..., : x.shape[-1] // 2]
        x2 = x[..., x.shape[-1] // 2 :]
        return torch.cat((-x2, x1), dim=-1)

    def apply_rotary_pos_emb_ref(self, q, k, cos, sin, position_ids, unsqueeze_dim=1):
        cos = cos[position_ids].unsqueeze(unsqueeze_dim)
        sin = sin[position_ids].unsqueeze(unsqueeze_dim)
        q_embed = (q * cos) + (self.rotate_half(q) * sin)
        k_embed = (k * cos) + (self.rotate_half(k) * sin)
        return q_embed, k_embed

    # self.cos_cache/self.sin_cache shape: [seq_len, dim]
    # q/k shape: [bs, seq_len, num_head, head_dim]
    def apply_rotary_pos_emb(self, q, k, cos, sin, position_ids, unsqueeze_dim=1):
        cos = cos[position_ids].unsqueeze(unsqueeze_dim)
        sin = sin[position_ids].unsqueeze(unsqueeze_dim)

        q_embed = torch.empty_like(q)
        k_embed = torch.empty_like(k)
        if q.shape == k.shape:
            cos = cos.expand(q.shape)
            sin = sin.expand(q.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half_qk(
                q, k, sin, cos, q_embed, k_embed
            )
        else:
            cos_q = cos.expand(q.shape)
            sin_q = sin.expand(q.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half(q, sin_q, cos_q, q_embed)
            cos_k = cos.expand(k.shape)
            sin_k = sin.expand(k.shape)
            torch.ops.torch_ipex.apply_rotary_embedding_half(k, sin_k, cos_k, k_embed)

        return q_embed, k_embed

    def forward(
        self,
        query,
        key,
        position_ids,
        layer_id,
        beam_size,
        kv_seq_len,
        cache_format=CacheFormat.BFNH,
    ):
        cos, sin = self.get_cos_sin(key, seq_len=kv_seq_len)
        query, key = self.apply_rotary_pos_emb(
            query, key, cos, sin, position_ids, unsqueeze_dim=-2
        )
        return query, key
