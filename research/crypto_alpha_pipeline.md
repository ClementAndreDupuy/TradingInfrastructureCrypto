
# Automated Crypto Alpha via Order Book Deep Learning (PyTorch)

## Executive Summary
End‑to‑end pipeline using live order‑book feeds and tick data. A graph‑based spatial encoder processes LOB structure while a temporal transformer models dynamics. Multi‑task heads predict multi‑horizon returns, trend direction, and adverse‑selection risk. Training uses cost‑aware losses and walk‑forward validation. Backtesting simulates realistic fills, slippage, and routing.

## Data
- Level‑N order book snapshots (bid/ask price, size)
- Trade ticks
- Millisecond timestamps
- Multi‑exchange normalized schema

## Feature Engineering
- Midprice
- Spread
- Microprice
- Order‑book imbalance
- Depth profiles
- Signed trade flow
- Short‑horizon volatility
- Rolling order‑flow imbalance

## Labels
- Multi‑horizon returns (10ms, 100ms, 1s)
- Trend class (up / flat / down)
- Adverse selection probability
- Execution‑aware PnL

## Model
Spatial encoder: Graph Neural Network on LOB levels  
Temporal encoder: Transformer on time sequence

Architecture:

LOB → GNN encoder  
LOB sequence → Transformer encoder  
Embeddings → Fusion layer  
Fusion → Multi‑task heads

Outputs:
- Return regression
- Direction classification
- Risk probability

## Training
- Multi‑task loss (MSE + cross‑entropy)
- Transaction‑cost penalty
- Walk‑forward cross validation
- Contrastive self‑supervised pretraining
- Adversarial noise augmentation

## Backtesting
- Market‑impact model
- Limit vs market execution simulation
- Smart order routing across exchanges
- Slippage and fees included

## Metrics
- Sharpe ratio
- Information ratio
- Hit rate
- Drawdown
- Slippage
- Turnover

## Deployment
- TorchScript / ONNX inference
- Quantized model
- Sub‑millisecond prediction
- Async pipeline (ingest → predict → execute)
- Live monitoring and drift detection
