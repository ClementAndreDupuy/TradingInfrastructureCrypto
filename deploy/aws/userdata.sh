#!/bin/bash
set -euo pipefail
exec > >(tee /var/log/trading-bootstrap.log) 2>&1

echo "[bootstrap] Starting at $(date)"

# ── 1. System packages ────────────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq cmake libssl-dev libboost-all-dev \
    python3.11 python3-pip python3.11-venv git awscli \
    build-essential libwebsockets-dev

# ── 2. Python virtualenv ──────────────────────────────────────────────────────
python3.11 -m venv /opt/trading-venv
source /opt/trading-venv/bin/activate
pip install --quiet --upgrade pip
pip install --quiet torch --index-url https://download.pytorch.org/whl/cpu
pip install --quiet polars numpy scipy requests boto3

# ── 3. Clone repository ───────────────────────────────────────────────────────
# The deploy key should be pre-loaded from Secrets Manager or baked into the AMI
git clone https://github.com/C18andre/ThamesRiverTrading /opt/trading
cd /opt/trading

# ── 4. Build C++ hot path ─────────────────────────────────────────────────────
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_BINDINGS=OFF \
    -DCMAKE_CXX_FLAGS="-I$(pwd)/.."
make -j$(nproc)
cd /opt/trading

# ── 5. Load Binance API keys from Secrets Manager ─────────────────────────────
mkdir -p /etc/trading
chmod 700 /etc/trading

aws secretsmanager get-secret-value \
    --secret-id trading/binance_api_keys \
    --query SecretString \
    --output text | \
python3 -c "
import json, sys
d = json.load(sys.stdin)
with open('/etc/trading/env', 'w') as f:
    f.write(f'BINANCE_API_KEY={d[\"api_key\"]}\n')
    f.write(f'BINANCE_API_SECRET={d[\"api_secret\"]}\n')
    f.write('S3_BUCKET=${s3_bucket}\n')
    f.write('MODEL_DIR=/opt/trading/models\n')
    f.write('DATA_DIR=/opt/trading/data\n')
    f.write('TRAIN_TICKS=2000\n')
    f.write('EPOCHS=30\n')
    f.write('PROMOTE_IC_MIN=0.02\n')
"
chmod 600 /etc/trading/env

# ── 6. Install systemd service files ─────────────────────────────────────────
cp /opt/trading/deploy/systemd/*.service /etc/systemd/system/
cp /opt/trading/deploy/systemd/*.timer   /etc/systemd/system/
systemctl daemon-reload

# ── 7. Create directories ─────────────────────────────────────────────────────
mkdir -p /opt/trading/models /opt/trading/data /opt/trading/logs

# ── 8. Enable and start training timer and shadow session ─────────────────────
systemctl enable neural-alpha-train.timer neural-alpha-shadow.service
systemctl start neural-alpha-train.timer neural-alpha-shadow.service

# trading-engine.service is deliberately left disabled until shadow is validated

echo "[bootstrap] Complete at $(date)"
