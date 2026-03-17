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

# ── 3. Clone private repository via deploy key from Secrets Manager ──────────
mkdir -p /root/.ssh
chmod 700 /root/.ssh

aws secretsmanager get-secret-value \
    --secret-id trading/github_deploy_key \
    --query SecretString \
    --output text > /root/.ssh/github_deploy_key
chmod 600 /root/.ssh/github_deploy_key

cat >> /root/.ssh/config <<'SSHCFG'
Host github.com
    HostName github.com
    User git
    IdentityFile /root/.ssh/github_deploy_key
    StrictHostKeyChecking no
SSHCFG
chmod 600 /root/.ssh/config

git clone git@github.com:C18andre/ThamesRiverTrading.git /opt/trading
cd /opt/trading

# ── 4. Build C++ hot path ─────────────────────────────────────────────────────
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_BINDINGS=OFF \
    -DCMAKE_CXX_FLAGS="-I$(pwd)/.."
make -j$(nproc)
cd /opt/trading

# ── 5. Load exchange API keys from Secrets Manager ────────────────────────────
mkdir -p /etc/trading
chmod 700 /etc/trading

BINANCE_SECRET_JSON="$(aws secretsmanager get-secret-value --secret-id trading/binance_api_keys --query SecretString --output text)"
KRAKEN_SECRET_JSON="$(aws secretsmanager get-secret-value --secret-id trading/kraken_api_keys --query SecretString --output text)"
OKX_SECRET_JSON="$(aws secretsmanager get-secret-value --secret-id trading/okx_api_keys --query SecretString --output text)"
COINBASE_SECRET_JSON="$(aws secretsmanager get-secret-value --secret-id trading/coinbase_api_keys --query SecretString --output text)"

export BINANCE_SECRET_JSON KRAKEN_SECRET_JSON OKX_SECRET_JSON COINBASE_SECRET_JSON
python3 - <<'PY'
import json
import os

def read_secret(name: str) -> dict:
    return json.loads(os.environ[name])

binance = read_secret("BINANCE_SECRET_JSON")
kraken = read_secret("KRAKEN_SECRET_JSON")
okx = read_secret("OKX_SECRET_JSON")
coinbase = read_secret("COINBASE_SECRET_JSON")

with open('/etc/trading/env', 'w') as f:
    f.write(f'BINANCE_API_KEY={binance["api_key"]}\n')
    f.write(f'BINANCE_API_SECRET={binance["api_secret"]}\n')
    f.write(f'KRAKEN_API_KEY={kraken["api_key"]}\n')
    f.write(f'KRAKEN_API_SECRET={kraken["api_secret"]}\n')
    f.write(f'OKX_API_KEY={okx["api_key"]}\n')
    f.write(f'OKX_API_SECRET={okx["api_secret"]}\n')
    f.write(f'COINBASE_API_KEY={coinbase["api_key"]}\n')
    f.write(f'COINBASE_API_SECRET={coinbase["api_secret"]}\n')
    f.write('S3_BUCKET=${s3_bucket}\n')
    f.write('MODEL_DIR=/opt/trading/models\n')
    f.write('DATA_DIR=/opt/trading/data\n')
    f.write('TRAIN_TICKS=2000\n')
    f.write('EPOCHS=30\n')
    f.write('PROMOTE_IC_MIN=0.02\n')
PY
chmod 600 /etc/trading/env

# ── 6. Install systemd service files ─────────────────────────────────────────
cp /opt/trading/deploy/systemd/*.service /etc/systemd/system/
cp /opt/trading/deploy/systemd/*.timer   /etc/systemd/system/
systemctl daemon-reload

# ── 7. Create directories ─────────────────────────────────────────────────────
mkdir -p /opt/trading/models /opt/trading/data /opt/trading/logs

# ── 8. Enable and start training timer, shadow session, and SLO engine ────────
systemctl enable neural-alpha-train.timer neural-alpha-shadow.service slo-engine.timer
systemctl start neural-alpha-train.timer neural-alpha-shadow.service slo-engine.timer

# trading-engine.service is deliberately left disabled until shadow is validated

echo "[bootstrap] Complete at $(date)"
