#!/usr/bin/env python3
"""Qwen3 (Hugging Face の safetensors) を FreeOS Any の AI チャット用の 1 ファイル (.fqw) にする。
使い方: export_qwen3.py モデルのフォルダ 出力.fqw    (numpy だけ必要。PyTorch は不要)

形式 (すべてリトルエンディアン、各部分は 64 バイト境界):
  ヘッダー 256 バイト: "FQW3", 版, 設定の数値, 各部分の位置
  トークン: u32 位置[vocab+1] と、生のバイト列
  結合規則: (左, 右, 結果) の u32 x 3 を優先順に
  重み: 層ごとに norm (f32)、q/k/v/o・gate/up/down (Q4_0)、q_norm/k_norm (f32)。最後に final_norm (f32)、埋め込み (Q8_0)
  Q4_0: 32 個ごとに f16 の倍率 + 16 バイト (下位 4 ビットが前半 16 個、上位が後半)。値 = (q - 8) * 倍率
  Q8_0: 32 個ごとに f16 の倍率 + 32 バイト (符号付き)
設計は llm.asm (https://github.com/alesha-pro/llm.asm, MIT) の .asmbin を参考にした。"""
import json
import os
import struct
import sys

import numpy as np

src, out = sys.argv[1], sys.argv[2]
Q8 = '--q8' in sys.argv  # 層の重みも 8 ビットにする (大きいが正確)
cfg = json.load(open(os.path.join(src, 'config.json')))
H, L, NH, NKV, HD, FF, V = (cfg['hidden_size'], cfg['num_hidden_layers'], cfg['num_attention_heads'],
                            cfg['num_key_value_heads'], cfg['head_dim'], cfg['intermediate_size'], cfg['vocab_size'])


def load_safetensors(path):
    f = open(path, 'rb')
    n = struct.unpack('<Q', f.read(8))[0]
    header = json.loads(f.read(n))
    base = 8 + n
    data = np.memmap(path, dtype=np.uint8, mode='r')
    out = {}
    for k, v in header.items():
        if k == '__metadata__':
            continue
        a, b = v['data_offsets']
        raw = data[base + a:base + b]
        if v['dtype'] == 'BF16':
            arr = (raw.view(np.uint16).astype(np.uint32) << 16).view(np.float32)
        elif v['dtype'] == 'F32':
            arr = raw.view(np.float32)
        elif v['dtype'] == 'F16':
            arr = raw.view(np.float16).astype(np.float32)
        else:
            raise SystemExit('未対応の型 ' + v['dtype'])
        out[k] = arr.reshape(v['shape'])
    return out


# 1 ファイルか、分割 (model.safetensors.index.json) か
if os.path.exists(os.path.join(src, 'model.safetensors.index.json')):
    W = {}
    for shard in sorted(set(json.load(open(os.path.join(src, 'model.safetensors.index.json')))['weight_map'].values())):
        W.update(load_safetensors(os.path.join(src, shard)))
else:
    W = load_safetensors(os.path.join(src, 'model.safetensors'))


def q4_0(m):
    m = np.ascontiguousarray(m, dtype=np.float32).reshape(-1, 32)
    amax = np.abs(m).max(axis=1)
    idx = np.abs(m).argmax(axis=1)
    mx = m[np.arange(len(m)), idx]          # 絶対値が最大の値 (符号付き)
    d = mx / -8.0
    inv = np.where(d != 0, 1.0 / np.where(d != 0, d, 1), 0)
    q = np.clip(np.round(m * inv[:, None] + 8.0), 0, 15).astype(np.uint8)
    packed = (q[:, :16] | (q[:, 16:] << 4)).astype(np.uint8)
    out = np.zeros(len(m), dtype=[('d', '<f2'), ('q', 'u1', 16)])
    out['d'] = d.astype(np.float16); out['q'] = packed
    del amax
    return out.tobytes()


def q8_0(m):
    m = np.ascontiguousarray(m, dtype=np.float32).reshape(-1, 32)
    d = np.abs(m).max(axis=1) / 127.0
    inv = np.where(d != 0, 1.0 / np.where(d != 0, d, 1), 0)
    q = np.clip(np.round(m * inv[:, None]), -127, 127).astype(np.int8)
    out = np.zeros(len(m), dtype=[('d', '<f2'), ('q', 'i1', 32)])
    out['d'] = d.astype(np.float16); out['q'] = q
    return out.tobytes()


# ---- トークナイザー (byte-level BPE): 文字表現を生のバイト列に戻す ----
tok = json.load(open(os.path.join(src, 'tokenizer.json')))
bs = list(range(ord('!'), ord('~') + 1)) + list(range(ord('¡'), ord('¬') + 1)) + list(range(ord('®'), ord('ÿ') + 1))
cs = bs[:]
n = 0
for b in range(256):
    if b not in bs:
        bs.append(b); cs.append(256 + n); n += 1
unmap = {chr(c): b for b, c in zip(bs, cs)}
vocab = tok['model']['vocab']
pieces = [b''] * V
for s, i in vocab.items():
    pieces[i] = bytes(unmap[ch] for ch in s)
for t in tok['added_tokens']:
    if t['id'] < V:
        pieces[t['id']] = t['content'].encode('utf-8')
merges = []
for m in tok['model']['merges']:
    a, b = m.split(' ', 1) if isinstance(m, str) else m
    merges.append((vocab[a], vocab[b], vocab[a + b]))

parts = []
pos = 256


def add(b):
    global pos
    pad = (-pos) % 64
    parts.append(b'\0' * pad); pos += pad
    start = pos
    parts.append(b); pos += len(b)
    return start


offs = [0]
blob = b''.join(pieces)
for p in pieces:
    offs.append(offs[-1] + len(p))
tok_off = add(np.array(offs, dtype='<u4').tobytes() + blob)
merge_off = add(np.array(merges, dtype='<u4').tobytes())


def g(name):
    return W[name]


weights_off = None
for li in range(L):
    p = f'model.layers.{li}.'
    a = add(g(p + 'input_layernorm.weight').astype('<f4').tobytes())
    if weights_off is None:
        weights_off = a
    qz = q8_0 if Q8 else q4_0
    add(qz(g(p + 'self_attn.q_proj.weight')))
    add(qz(g(p + 'self_attn.k_proj.weight')))
    add(qz(g(p + 'self_attn.v_proj.weight')))
    add(g(p + 'self_attn.q_norm.weight').astype('<f4').tobytes())
    add(g(p + 'self_attn.k_norm.weight').astype('<f4').tobytes())
    add(qz(g(p + 'self_attn.o_proj.weight')))
    add(g(p + 'post_attention_layernorm.weight').astype('<f4').tobytes())
    add(qz(g(p + 'mlp.gate_proj.weight')))
    add(qz(g(p + 'mlp.up_proj.weight')))
    add(qz(g(p + 'mlp.down_proj.weight')))
    print(f'layer {li + 1}/{L}', end='\r', flush=True)
final_off = add(g('model.norm.weight').astype('<f4').tobytes())
emb_off = add(q8_0(g('model.embed_tokens.weight')))

hdr = struct.pack('<4sI11I', b'FQW3', 1, H, L, NH, NKV, HD, FF, V, len(merges), int(cfg['rope_theta']),
                  len(blob), 1 if Q8 else 0)  # 最後: 層の重みの形式 0=Q4_0 1=Q8_0
hdr += struct.pack('<f', cfg['rms_norm_eps'])
hdr += struct.pack('<6Q', tok_off, merge_off, weights_off, final_off, emb_off, pos)
hdr = hdr.ljust(256, b'\0')
with open(out, 'wb') as f:
    f.write(hdr)
    for p in parts:
        f.write(p)
print(f'\n{out}: {pos / 1048576:.1f} MiB, vocab {V}, merges {len(merges)}')
