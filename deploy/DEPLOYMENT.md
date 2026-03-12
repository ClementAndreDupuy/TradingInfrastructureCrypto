# Deployment Guide — SOLUSDT on Binance (AWS)

## Overview

| Phase | Duration | Cost |
|---|---|---|
| Provision + first training | Day 0 (~1 hr) | ~$12 |
| Shadow trading | Days 1–5 | ~$60 |
| Live trading | Ongoing | ~$351/mo on-demand → ~$215/mo reserved |

Region: **ap-northeast-1 (Tokyo)** — lowest latency to Binance.
Instance: **c5n.2xlarge** (8 vCPU, 21 GB RAM, enhanced networking).

---

## Prerequisites (local machine)

Install tools:

```bash
brew install awscli terraform        # macOS
# or: apt install awscli  +  https://developer.hashicorp.com/terraform/install
```

Configure AWS credentials (needs AdministratorAccess or scoped EC2+S3+IAM+SecretsManager+CloudWatch):

```bash
aws configure
# AWS Access Key ID:     <your key>
# AWS Secret Access Key: <your secret>
# Default region:        ap-northeast-1
# Default output:        json
```

---

## Step 1 — Create SSH key pair for EC2 access

```bash
aws ec2 create-key-pair \
  --key-name thames-trading \
  --region ap-northeast-1 \
  --query KeyMaterial \
  --output text > ~/.ssh/thames-trading.pem
chmod 400 ~/.ssh/thames-trading.pem
```

---

## Step 2 — Create GitHub deploy key

```bash
# Generate an ed25519 key pair (no passphrase)
ssh-keygen -t ed25519 -C "trading-ec2-deploy" \
  -f ~/.ssh/thames-trading-deploy -N ""

# Print the public key — you will paste this into GitHub
cat ~/.ssh/thames-trading-deploy.pub
```

Add the public key to GitHub:
1. Go to **https://github.com/C18andre/ThamesRiverTrading/settings/keys**
2. Click **Add deploy key**
3. Title: `EC2 Trading Server`
4. Paste the public key
5. **Allow write access: NO** (read-only is enough)
6. Click **Add key**

---

## Step 3 — Provision infrastructure with Terraform

```bash
cd deploy/aws/terraform

terraform init

# Preview resources to be created
terraform plan \
  -var="key_pair_name=thames-trading" \
  -var="operator_cidr=$(curl -s ifconfig.me)/32"

# Apply (~3 min)
terraform apply \
  -var="key_pair_name=thames-trading" \
  -var="operator_cidr=$(curl -s ifconfig.me)/32"
```

Resources created:
- EC2 c5n.2xlarge + Elastic IP
- S3 bucket (model artifacts, tick data)
- IAM role + instance profile
- Security group (SSH from your IP only)
- Secrets Manager secrets (empty placeholders)
- CloudWatch log groups

Note the outputs — you will need:
- `instance_public_ip`
- `s3_bucket_name`
- `ssh_command`

---

## Step 4 — Upload secrets to Secrets Manager

```bash
# Binance API keys — use a key with Spot Trading only, NO withdrawal permission
aws secretsmanager put-secret-value \
  --region ap-northeast-1 \
  --secret-id trading/binance_api_keys \
  --secret-string '{"api_key":"YOUR_BINANCE_KEY","api_secret":"YOUR_BINANCE_SECRET"}'

# GitHub deploy key (private key)
aws secretsmanager put-secret-value \
  --region ap-northeast-1 \
  --secret-id trading/github_deploy_key \
  --secret-string file://~/.ssh/thames-trading-deploy
```

---

## Step 5 — Wait for EC2 bootstrap to complete

The userdata script runs automatically on first boot (~10–15 min).
It installs packages, clones the repo, builds the C++ engine, and starts the shadow session.

```bash
# SSH into the instance
ssh -i ~/.ssh/thames-trading.pem ubuntu@<INSTANCE_IP>

# Watch the bootstrap log
tail -f /var/log/trading-bootstrap.log

# You should see "[bootstrap] Complete at ..." at the end
```

---

## Step 6 — Trigger the first training run

The daily timer fires at 00:30 UTC. Trigger it manually now:

```bash
sudo systemctl start neural-alpha-train.service

# Watch progress
sudo journalctl -fu neural-alpha-train.service
```

Training produces:
- `/opt/trading/models/neural_alpha_latest.pt` — primary model (d_spatial=64, d_temporal=128)
- `/opt/trading/models/neural_alpha_secondary.pt` — secondary model (d_spatial=32, d_temporal=64)
- Both uploaded to S3 automatically

Training takes 20–40 min depending on tick count.

---

## Step 7 — Monitor shadow session (3–5 days)

Shadow starts automatically after bootstrap.

```bash
# Live logs
sudo journalctl -fu neural-alpha-shadow.service

# Signal stream
sudo tail -f /opt/trading/neural_alpha_shadow.jsonl | python3 -c "
import sys, json
for line in sys.stdin:
    d = json.loads(line.strip())
    if 'signal_bps' in d:
        print(f'signal={d[\"signal_bps\"]:+.1f}bps  pos={d[\"position\"]}  pnl={d.get(\"unrealised_pnl_bps\",0):+.1f}bps')
"
```

**Go/no-go checklist before enabling live:**
- [ ] IC (information coefficient) > 0.02 on shadow trades
- [ ] Fill rate > 30% of quoted orders
- [ ] No more than 2 consecutive loss days
- [ ] Kill switch test: `sudo kill -SIGTERM $(pgrep -f shadow_session)` restarts cleanly
- [ ] CloudWatch dashboards showing signal + position at `/trading/shadow`

---

## Step 8 — Enable live trading

```bash
sudo systemctl enable --now trading-engine.service

# Watch the live engine
sudo journalctl -fu trading-engine.service
```

To stop immediately:

```bash
sudo systemctl stop trading-engine.service
```

---

## Step 9 — Reduce cost: buy Reserved Instance

After the shadow week, once you decide to stay live, buy a 1-year Reserved Instance to cut the EC2 bill from ~$343/mo to ~$207/mo:

1. AWS Console → EC2 → Reserved Instances → Purchase
2. Instance type: `c5n.2xlarge`
3. Region: `ap-northeast-1`
4. Term: 1 year, No Upfront or Partial Upfront

---

## Cost Summary

| Resource | Monthly |
|---|---|
| EC2 c5n.2xlarge (on-demand) | ~$343 |
| EC2 c5n.2xlarge (1-yr reserved) | ~$207 |
| EBS gp3 50 GB | $4 |
| S3 (~10 GB + requests) | ~$2 |
| CloudWatch Logs | ~$2 |
| Secrets Manager (2 secrets) | $0.80 |
| **Total on-demand** | **~$352** |
| **Total reserved** | **~$216** |

---

## Teardown

```bash
# Stop live engine first
sudo systemctl stop trading-engine.service neural-alpha-shadow.service

# Destroy all AWS resources
cd deploy/aws/terraform
terraform destroy \
  -var="key_pair_name=thames-trading" \
  -var="operator_cidr=$(curl -s ifconfig.me)/32"
```

Note: S3 bucket must be emptied before Terraform can delete it:

```bash
aws s3 rm s3://<BUCKET_NAME> --recursive
```
