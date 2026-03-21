# Neural Alpha Training and Model Review — 2026-03-21

## Scope

Reviewed the Python `research/neural_alpha/` stack with focus on:
- feature construction
- dataset normalisation and walk-forward safety
- model architecture
- multitask objective
- training stability

## Findings

1. **The existing architecture was directionally correct but overly complex at the head/loss boundary.**
   - Spatial attention over LOB levels plus a temporal transformer is consistent with modern LOB modeling.
   - The implementation mixed inference probabilities with training losses for the risk head, which is less numerically stable than training on logits directly.

2. **Evaluation normalisation had a cold-start issue.**
   - Test splits were normalised independently using only their own first observations.
   - This was leak-free, but the first holdout windows could receive unstable scaling unrelated to how a live model would actually transition from train to holdout.

3. **The original objective underweighted robustness to noisy labels and class imbalance.**
   - Financial return labels are heavy-tailed and noisy.
   - Risk labels are sparse and direction labels can be imbalanced by market regime.

## Industry comparison

Relative to common market microstructure models such as DeepLOB-style spatial/temporal encoders and newer attention-based LOB models, the repository was already aligned on the high-level modeling pattern, but lagged on several implementation details that are standard in production training loops:

- use **robust regression losses** for noisy return targets
- train binary tasks with **logit-space BCE**
- compensate for **class imbalance** in classification heads
- preserve **point-in-time normalisation continuity** across walk-forward splits
- keep inference outputs simple while using the more stable internal training representation

## Implemented changes

1. Switched the risk task to **logit-space training** while preserving probability outputs for runtime consumers.
2. Replaced raw MSE on returns with **SmoothL1 / Huber-style regression**.
3. Added **direction class weights** and **risk positive-class weighting** derived from the training loader.
4. Added **history-aware rolling normalisation** so holdout scaling can warm-start from the training tail without introducing leakage.
5. Simplified the model heads into one reusable `MLPHead` abstraction.
6. Enabled optional **AMP** on CUDA while keeping CPU behavior unchanged.
7. Disabled self-supervised pretraining by default to simplify the default training path; it remains available as an opt-in option.

## Expected impact

- More stable risk training under extreme logits
- Better robustness to heavy-tailed return noise
- Improved behavior in imbalanced regimes
- Cleaner, easier-to-maintain model code
- Holdout evaluation that better matches live deployment transitions
