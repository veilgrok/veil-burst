#!/usr/bin/env python3
"""Write a Llama-architecture GGUF with random F16 weights and a real tokenizer
(copied from a vocab-only GGUF in the llama.cpp repo), for speed benchmarks of
llama.cpp when real weights can't be downloaded. Quantize afterwards with
llama-quantize. Speed does not depend on weight values; text output is noise.

  mkgguf.py <llama.cpp dir> <out.gguf> d f L H Hkv
"""
import sys
import numpy as np

lc, out = sys.argv[1], sys.argv[2]
d, f, L, H, Hkv = map(int, sys.argv[3:8])
sys.path.insert(0, f"{lc}/gguf-py")
import gguf  # noqa: E402

voc = gguf.GGUFReader(f"{lc}/models/ggml-vocab-llama-spm.gguf")
w = gguf.GGUFWriter(out, "llama")
for name, field in voc.fields.items():
    if not name.startswith("tokenizer."):
        continue
    vt = field.types[0]
    val = field.contents()
    if vt == gguf.GGUFValueType.ARRAY:
        w.add_key_value(name, val, vt, sub_type=field.types[1])
    else:
        w.add_key_value(name, val, vt)
V = len(voc.fields["tokenizer.ggml.tokens"].contents())
hd = d // H
w.add_context_length(4096)
w.add_embedding_length(d)
w.add_block_count(L)
w.add_feed_forward_length(f)
w.add_head_count(H)
w.add_head_count_kv(Hkv)
w.add_rope_dimension_count(hd)
w.add_layer_norm_rms_eps(1e-5)
w.add_file_type(gguf.LlamaFileType.MOSTLY_F16)
rng = np.random.default_rng(0)


def rnd(*shape):
    return (rng.standard_normal(shape, dtype=np.float32) * 0.02).astype(np.float16)


ones = lambda n: np.ones(n, dtype=np.float32)
w.add_tensor("token_embd.weight", rnd(V, d))
for i in range(L):
    p = f"blk.{i}."
    w.add_tensor(p + "attn_norm.weight", ones(d))
    w.add_tensor(p + "attn_q.weight", rnd(d, d))
    w.add_tensor(p + "attn_k.weight", rnd(Hkv * hd, d))
    w.add_tensor(p + "attn_v.weight", rnd(Hkv * hd, d))
    w.add_tensor(p + "attn_output.weight", rnd(d, d))
    w.add_tensor(p + "ffn_norm.weight", ones(d))
    w.add_tensor(p + "ffn_gate.weight", rnd(f, d))
    w.add_tensor(p + "ffn_up.weight", rnd(f, d))
    w.add_tensor(p + "ffn_down.weight", rnd(d, f))
w.add_tensor("output_norm.weight", ones(d))
w.add_tensor("output.weight", rnd(V, d))
w.write_header_to_file()
w.write_kv_data_to_file()
w.write_tensors_to_file()
w.close()
print("wrote", out, "vocab", V)
