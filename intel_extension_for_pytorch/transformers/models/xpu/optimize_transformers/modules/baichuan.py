import torch
from typing import Optional, Tuple
from .transformer_modules.RoPE import LlamaRotaryEmbedding
from .transformer_modules.Norm import LlamaRMSNorm


from ._transformers import MAX_SEQ_LEN, MAX_OUT_SEQ_LEN
from .transformer_modules.BaseAttention import IPEXTransformerAttn
from .transformer_modules.Attention import (  # noqa F401
    IPEXTransformerAttnOptimizedFp16Baichuan,
)
from .transformer_modules.Mlp import (  # noqa F401
    IPEXTransformerBaseMLP,
    IPEXTransformerMLPOptimizedFp16,
)
from ._transformer_configuration import IPEXTransformerConfig, SupportedActivation
from .transformer_modules.QuantizedAttention import (  # noqa F401
    IPEXTransformerAttnOptimizedFp16,
    IPEXTransformerAttnOptimizedInt4,
)  # noqa
from .transformer_modules.NaiveAttention import IPEXTransformerAttnNaive  # noqa
from .transformer_modules.GroupedAttention import (  # noqa F401
    IPEXTransformerAttnOptimizedFp16Grouped,
)
from .transformer_modules.DecoderBlock import IPEXTransformerBlock
from .transformer_modules.Mlp import *  # noqa


class NewIPEXBaichuanBlock(IPEXTransformerBlock):
    def __init__(
        self,
        module,
        config,
        dtype="fp16",
        device="xpu",
        module_name="",
        impl_mode=None,
        tp_size=1,
        tp_group=None,
        **kwargs,
    ):
        super().__init__(module, config, dtype, device, module_name)
        self.ipex_config = self.build_ipex_transformer_config(
            config, device, dtype, impl_mode, tp_size, tp_group
        )
        self.attn = self.build_attention_from_config("Baichuan")
        self.mlp = self.build_mlp_from_config("Baichuan")
        self.input_layernorm = LlamaRMSNorm(
            self.ipex_config.embedding_dim, self.ipex_config.norm_eps
        )
        self.post_attn_layernorm = LlamaRMSNorm(
            self.ipex_config.embedding_dim, self.ipex_config.norm_eps
        )
        self.port_all_parameters_to_new_module()

    def build_ipex_transformer_config(
        self, config, device, dtype, impl_mode, tp_size, tp_group
    ) -> IPEXTransformerConfig:
        activation_function = self.config.hidden_act
        ipex_activation = None
        for act in SupportedActivation:
            if activation_function in act.value:
                ipex_activation = act
                break
        assert ipex_activation is not None, (
            "found unrecognized activation function,"
            "can not build ipex config from {}".format(activation_function)
        )

        assert dtype in [
            "fp16",
            "int4",
        ], "dtype tag {} passed to optimized_transformers is not supported!".format(
            dtype
        )

        return IPEXTransformerConfig(
            embedding_dim=self.config.hidden_size,
            intermediate_dim=self.config.intermediate_size,
            num_attention_head=self.config.num_attention_heads,
            # transformers==4.31.0
            num_key_value_head=self.config.num_attention_heads,
            max_positions=max(
                (
                    self.config.max_position_embeddings
                    if hasattr(self.config, "max_position_embeddings")
                    else self.config.model_max_length
                ),
                MAX_SEQ_LEN,
            ),
            max_out_positions=MAX_OUT_SEQ_LEN,
            rotary_embedding_class=LlamaRotaryEmbedding,
            rotary_dim=None,
            rotary_half=True,
            rotate_every_two=False,
            use_causal_mask=False,
            activation_function=self.config.hidden_act,
            ipex_act=ipex_activation,
            norm_eps=self.config.rms_norm_eps,
            residual_dropout=None,
            attn_dropout=None,
            enable_bias=False,
            residual_pdrop=None,
            scale_attention=True,
            is_decoder=False,
            do_norm_before=None,
            ln_elementwise_affine=None,
            positional_embedding_base=10000,
            device=self.device,
            dtype=dtype,
            impl=impl_mode,
            tp_size=tp_size,
            tp_group=tp_group,
        )

    def port_attn_parameter(self):
        # IPEXTransformerAttnOptimizedFp16Baichuan
        self.attn.load_parameter(
            self.module.self_attn.W_pack,
            self.module.self_attn.o_proj,
        )

    def port_mlp_parameter(self):
        # IPEXTransformerMLPOptimizedFp16SiluBaichuan
        self.mlp.load_parameter(
            self.module.mlp.gate_proj,
            self.module.mlp.down_proj,
            self.module.mlp.up_proj,
        )

    def port_norm_parameter(self):
        self.input_layernorm.weight = self.module.input_layernorm.weight
        self.post_attn_layernorm.weight = self.module.post_attention_layernorm.weight

    def transpose_parameter(self):
        self.attn.transpose_parameter()
        self.mlp.transpose_parameter()

    def port_all_parameters_to_new_module(self):
        super().port_all_parameters_to_new_module()
        if self.ipex_config.transpose:
            self.transpose_parameter()
        self.attn.cat_qkv()

    def forward(
        self,
        hidden_states: torch.Tensor,
        attention_mask: Optional[torch.Tensor] = None,
        position_ids: Optional[torch.LongTensor] = None,
        past_key_value: Optional[Tuple[torch.Tensor]] = None,
        output_attentions: Optional[bool] = False,
        use_cache: Optional[bool] = False,
    ) -> Tuple[
        torch.FloatTensor, Optional[Tuple[torch.FloatTensor, torch.FloatTensor]]
    ]:
        # hidden_states:  [bs*beam, seq, hidden_size]
        # position_ids:   [bs*beam, seq]
        # attention_mask: [bs*beam, head, q_seq, kv_seq]
        bs = IPEXTransformerAttn.batch_size
        dim = hidden_states.dim()
        if dim == 3:
            beam = hidden_states.shape[0] // bs
            seq = hidden_states.shape[1]
        elif dim == 4:
            beam = hidden_states.shape[1]
            seq = hidden_states.shape[2]
        else:
            print("Unsupported input shape")
            return

        IPEXTransformerAttn.beam_size = beam
        first_token = True if past_key_value is None else False

        hidden_size = hidden_states.shape[-1]
        hidden_shape = [bs, beam, seq, hidden_size]
        if first_token and beam > 1:
            # for 1st token, keep the original layout
            # reduce the duplicated info in beam dim
            # shape -> [bs*beam, seq, hidden_size]
            # layout -> [bs*beam, seq, hidden_size]
            hidden_states = hidden_states.view(hidden_shape)[:, 0, :, :].contiguous()
            if position_ids is not None:
                position_ids = position_ids.view(bs, beam, position_ids.shape[1])[
                    :, 0, :
                ].view(bs, position_ids.shape[1])
            if attention_mask is not None:
                attention_mask = attention_mask.view(
                    bs,
                    beam,
                    attention_mask.shape[1],
                    attention_mask.shape[2],
                    attention_mask.shape[3],
                )[:, 0, :, :, :].view(
                    bs,
                    attention_mask.shape[1],
                    attention_mask.shape[2],
                    attention_mask.shape[3],
                )
        else:
            # for 2nd to last token, we convert the layout
            # shape -> [bs*beam, seq, hidden_size]
            # convert layout form [bs*beam, seq, hidden_size] to [seq, bs*beam, hidden_size]
            hidden_states = hidden_states.transpose(0, 1).contiguous()

        residual = hidden_states
        hidden_states = self.input_layernorm(hidden_states)

        hidden_states, present_key_value, self_attn_weights = self.attn(
            hidden_states=hidden_states,
            attention_mask=attention_mask,
            position_ids=position_ids,
            layer_past=past_key_value,
            output_attentions=output_attentions,
            use_cache=use_cache,
            residual=residual,
            first_token=first_token,
        )

        residual = hidden_states
        hidden_states = self.post_attn_layernorm(hidden_states)
        hidden_states = self.mlp(hidden_states, residual)
        if first_token and beam > 1:
            # for 1st token, expand the result with beam
            hidden_states = hidden_states.view(bs, 1, seq, hidden_size).expand(
                [bs, beam, seq, hidden_size]
            )
        else:
            # for 2nd to last token, we convert the layout back
            # convert hidden_states form [seq, beam, hidden_size] back to [beam, seq, hidden_size]
            hidden_states = hidden_states.transpose(0, 1)
        outputs = (hidden_states,)
        if output_attentions:
            outputs += (self_attn_weights,)

        if use_cache:
            outputs += (present_key_value,)
        return outputs
